#include <7Semi_CO2TH.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>



// ============ configurstion ============
#define FW_VERSION    "1.1.14"   
// Local Python server (HTTP)

const char* OTA_MANIFEST_URL = "https://aiflux-bucket.s3.ap-south-1.amazonaws.com/laphu-ota-update-bins/manifest.json";
// const char* OTA_MANIFEST_URL = "http://192.168.0.248:8000/manifest.json";


// === OLED Display Settings ===
// Initialize for SH1106 128x64 hardware I2C, no reset pin
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);


// === MQTT Credentials ===
const char* mqtt_server = "mqtt.aiflux.tech";
const int mqtt_port = 1883;
const char* mqtt_user = "aiflux_user";
const char* mqtt_password = "Aiflux@2024";
const char* mqtt_topic = "23aff839-62a1-402e-98a9-fe1f548aecc8/data";

// Publish cadence & OTA check cadence
const unsigned long PUBLISH_INTERVAL_MS   = 30000;                       // 30s
const unsigned long OTA_CHECK_INTERVAL_MS = 3UL * 60UL * 1000UL;  // 3 minute

// TLS (not used for HTTP, useful later for HTTPS when Vishal implements SSL)
 #define OTA_TLS_INSECURE 1
 #if !OTA_TLS_INSECURE
 static const char* ROOT_CA_PEM = R"PEM(
 -----BEGIN CERTIFICATE-----
 ...YOUR ROOT CA PEM HERE...
 -----END CERTIFICATE-----
 )PEM";
 #endif

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastPublishTime = 0;
const unsigned long interval = 30000; // 30 seconds

unsigned long lastOtaCheck = 0;


// ----- version compare (-1 a<b, 0 eq, +1 a>b) -----
int cmpVersion(const String& a, const String& b) {
  int ai[3] = {0,0,0}, bi[3] = {0,0,0};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i=0;i<3;i++) {
    if (ai[i] < bi[i]) return -1;
    if (ai[i] > bi[i]) return  1;
  }
  return 0;
}

// ----- HTTP/HTTPS auto-select -----
bool httpBeginAuto(HTTPClient& http, const String& url, WiFiClientSecure& secure, WiFiClient& plain) {
  if (url.startsWith("https://")) {
#if OTA_TLS_INSECURE
    secure.setInsecure();
#else
    secure.setCACert(ROOT_CA_PEM);
#endif
    return http.begin(secure, url);
  } else {
    return http.begin(plain, url);
  }
} 

// perform OTA from bin URL, instructions provided in whatsapp. check...
bool performOTA(const String& binUrl, const String& md5) {
  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;

  Serial.println(String("[OTA] Download: ") + binUrl);

  if (!httpBeginAuto(http, binUrl, secure, plain)) {
    Serial.println("[OTA] http.begin() failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] GET failed: %s (%d)\n", http.errorToString(code).c_str(), code);
    http.end();
    return false;
  }

  int len = http.getSize();
  WiFiClient * stream = http.getStreamPtr();
  if (len <= 0) {
    Serial.println("[OTA] Invalid content length");
    http.end();
    return false;
  }

  if (!Update.begin(len)) {
    Serial.printf("[OTA] Update.begin failed (%s)\n", Update.errorString());
    http.end();
    return false;
  }

  if (md5.length() == 32) Update.setMD5(md5.c_str());

  uint8_t buf[2048];
  size_t written = 0;
  unsigned long lastLog = 0;

  while (http.connected() && (len > 0 || len == -1)) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, (avail > sizeof(buf)) ? sizeof(buf) : avail);
      if (n <= 0) break;
      if (Update.write(buf, n) != (size_t)n) {
        Serial.printf("[OTA] Write failed (%s)\n", Update.errorString());
        Update.abort();
        http.end();
        return false;
      }
      written += n;
    }

    unsigned long now = millis();
    if (now - lastLog > 500) {
      int pct = (len > 0) ? (int)((written * 100) / (size_t)len) : -1;
      if (pct >= 0) Serial.printf("[OTA] Progress: %d%%\n", pct);
      lastLog = now;
    }
    delay(1);
  }

  if (!Update.end()) {
    Serial.printf("[OTA] End failed (%s)\n", Update.errorString());
    http.end();
    return false;
  }
  if (!Update.isFinished()) {
    Serial.println("[OTA] Not finished");
    http.end();
    return false;
  }

  http.end();
  Serial.println("[OTA] Update OK, rebooting...");
  delay(800);
  ESP.restart();
  return true;
}

// check manifest and update if newer
bool checkForUpdate() {
  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;

  Serial.println(String("[OTA] Manifest: ") + OTA_MANIFEST_URL);
  if (!httpBeginAuto(http, OTA_MANIFEST_URL, secure, plain)) {
    Serial.println("[OTA] manifest http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Manifest GET failed: %d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[OTA] JSON parse error: %s\n", err.c_str());
    return false;
  }

  String latest = doc["version"] | "";
  String binUrl = doc["bin_url"] | "";
  String md5    = doc["md5"]     | "";

  if (latest.length() == 0 || binUrl.length() == 0) {
    Serial.println("[OTA] Manifest missing fields");
    return false;
  }

  Serial.printf("[OTA] Current: %s, Latest: %s\n", FW_VERSION, latest.c_str());
  if (cmpVersion(FW_VERSION, latest) >= 0) {
    Serial.println("[OTA] Up-to-date");
    return false;
  }

  return performOTA(binUrl, md5);
}


// Relay pins and thresholds
const int relayCO2Pin = 26;   // Relay for CO2 control
const int relayHumiPin = 25;  // Relay for Humidity control
const float CO2_THRESHOLD = 450;     // CO2 threshold in ppm
const float HUMI_THRESHOLD = 80.0;   // Humidity threshold in %RH

CO2TH_7Semi CO2TH;     // 7Semi sensor driver object
static const unsigned long kPeriodMs = 1200;  // Sensor frame interval


// === Initialize OLED Display ===
void setup_display() {
  display.begin();
  display.clearBuffer();

  // Select font size here (change only this line for different font sizes)
  display.setFont(u8g2_font_ncenB14_tr); // Try ncenB08, ncenB10, ncenB12, ncenB14, ncenB18, ncenB24

  const char* line1 = "AIflux";
  const char* line2 = "Innovations";

  // Calculate text widths for both lines
  int16_t width1 = display.getStrWidth(line1);
  int16_t width2 = display.getStrWidth(line2);

  // Horizontally center each line
  int16_t x1 = (128 - width1) / 2;
  int16_t x2 = (128 - width2) / 2;

  // Vertically place lines (adjust if changing font size)
  int16_t y1 = 30;  // Line1 baseline (upper half)
  int16_t y2 = 55;  // Line2 baseline (lower half)

  // Draw both lines centered
  display.drawStr(x1, y1, line1);
  display.drawStr(x2, y2, line2);

  display.sendBuffer();
  delay(1000); // Display for 3 seconds
}



// === Reconnect to MQTT Broker ===
// void reconnect_mqtt() {
//   while (!client.connected()) {
//     Serial.print("Attempting MQTT connection...");
//     if (client.connect("ESP32_CO2_Client", mqtt_user, mqtt_password)) {
//       Serial.println("connected");
//     } else {
//       Serial.print("failed, rc=");
//       Serial.print(client.state());
//       Serial.println(" retrying in 5 seconds");
//       delay(5000);
//     }
//   }
// }
void reconnect_mqtt() {
  // Loop until we're reconnected  
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect with client ID, username, password
    if (client.connect("ESP32_CO2_Client", mqtt_user, mqtt_password)) {
      Serial.println("connected");
      // (Optional) Re-subscribe to topics here if required
      // client.subscribe("your/topic");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);

      // Optional: If repeated failures, consider a watchdog restart or limit retries
      // This prevents infinite loop in case of hardware/network failure
      // Implement a retry counter if needed:
      // static int retryCount = 0;
      // retryCount++;
      // if (retryCount > 10) {
      //     ESP.restart();
      // }
    }
  }
}



// === Setup ===
void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22); // Ensure both libraries use the same I²C bus instance and correct pins.
  setup_display();
  
  err_t e = CO2TH.Begin(21, 22, 400000, 0); // If you need to set I2C pins, use CO2TH.Begin(21, 22, 400000, 0);
  if (e != NO_ERROR) {
    Serial.print(F("Begin failed, err="));
    Serial.println((int)e);
    while (1) { delay(1000); }
  }

  e = CO2TH.StartContinuousMeasurement();
  if (e != NO_ERROR) {
      Serial.print(F("Start failed, err="));
      Serial.println((int)e);
      while (1) { delay(1000); }
  }
  delay(kPeriodMs); // Wait for first reading frame


  // Relay pin setup (HIGH = OFF, LOW = ON)
  pinMode(relayCO2Pin, OUTPUT);    // CO2 relay pin setup
  pinMode(relayHumiPin, OUTPUT);   // Humidity relay pin setup
  digitalWrite(relayCO2Pin, HIGH); // Default OFF
  digitalWrite(relayHumiPin, HIGH); // Default OFF


  // Setup WiFi via WiFiManager
  WiFiManager wm;
  if (!wm.autoConnect("AIFLUX-ESP32-Setup", "12345678")) {
    Serial.println("Failed to connect to WiFi. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi connected!");
  Serial.println(WiFi.localIP());


  // Show firmware version
  Serial.printf("Firmware version: v%s\n", FW_VERSION);

  // Optional: keep LAN OTA enabled too
  ArduinoOTA.setHostname("esp32-ota");
  ArduinoOTA.setPassword("esp32-ota");
  ArduinoOTA.begin();

  // MQTT
  client.setServer(mqtt_server, mqtt_port);


  // Check for update on boot
  Serial.println("Checking OTA on boot...");
  checkForUpdate();
  lastOtaCheck = millis();

  Serial.println("Setup complete.");
}


// === Main Loop ===
void loop() {
  ArduinoOTA.handle();
  if (!client.connected()) {
    reconnect_mqtt();
  }
  client.loop();

// Periodic cloud OTA check
  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
    Serial.println("Periodic OTA check...");
    checkForUpdate();
    lastOtaCheck = millis();
  }


  if (millis() - lastPublishTime >= interval) {

    // --- Read sensor values ---
    int16_t co2 = 0;
    float tempC = 0.0f, humi = 0.0f;
    uint16_t st = 0;
    err_t e = CO2TH.ReadMeasurement(co2, tempC, humi, st);
    if (e != NO_ERROR) {
        Serial.print(F("Read failed, err=")); Serial.println((int)e);

        // If error, turn OFF both relays, display error, and publish error JSON
        digitalWrite(relayCO2Pin, HIGH);
        digitalWrite(relayHumiPin, HIGH);
        display.clearBuffer();
        display.setFont(u8g2_font_ncenB10_tr);
        display.drawStr(0, 18, "Sensor error!");
        display.sendBuffer();

        char errorPayload[128];
        snprintf(errorPayload, sizeof(errorPayload),
            "{\"error\":\"ReadFailure\"}");
        client.publish(mqtt_topic, errorPayload);
        lastPublishTime = millis();
        return;
    }

    // --- Serial Monitor Output ---
    Serial.print("CO2 = "); Serial.print(co2); Serial.print(" ppm, ");
    Serial.print("Temp = "); Serial.print(tempC, 2); Serial.print(" C, ");
    Serial.print("Humidity = "); Serial.println(humi, 2);

    // --- Relay Logic ---
    if (co2 >= CO2_THRESHOLD) {
        digitalWrite(relayCO2Pin, LOW);    // Relay ON (active low logic for most modules)
        Serial.println("Relay CO2: ON");
    } else {
        digitalWrite(relayCO2Pin, HIGH);   // Relay OFF
        Serial.println("Relay CO2: OFF");
    }
    if (humi >= HUMI_THRESHOLD) {
        digitalWrite(relayHumiPin, LOW);   // Relay ON
        Serial.println("Relay Humi: ON");
    } else {
        digitalWrite(relayHumiPin, HIGH);  // Relay OFF
        Serial.println("Relay Humi: OFF");
    }

    // --- MQTT Publish ---
    char payload[256];
    snprintf(payload, sizeof(payload),
         "{\"temperature\":%.2f,\"humidity\":%.2f,\"co2\":%d,"
         "\"relay_co2\":\"%s\",\"relay_humi\":\"%s\"}",
         tempC, humi, co2,
         (digitalRead(relayCO2Pin) == LOW) ? "ON" : "OFF",
         (digitalRead(relayHumiPin) == LOW) ? "ON" : "OFF"
    );

    if (client.publish(mqtt_topic, payload)) {
        Serial.print("Published JSON: "); Serial.println(payload);
    } else {
        Serial.println("Publish failed");
    }

    // --- OLED Display Output ---
    display.setFont(u8g2_font_ncenB10_tr);
    display.clearBuffer();
    char buf[24];
    snprintf(buf, sizeof(buf), "Temp: %.1f C", tempC);
    display.drawStr(0, 18, buf);
    snprintf(buf, sizeof(buf), "CO2: %d ppm", co2);
    display.drawStr(0, 37, buf);
    snprintf(buf, sizeof(buf), "Humidity: %.1f %%", humi);
    display.drawStr(0, 56, buf);
    display.sendBuffer();

    lastPublishTime = millis();
}

  delay(100);
}
