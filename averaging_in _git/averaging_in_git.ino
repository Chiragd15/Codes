#include <MHZ19.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <DHT.h>
#include <U8g2lib.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <ErriezMHZ19B.h>

WiFiClientSecure secureClient;


// ============ configurstion ============
#define FW_VERSION    "1.1.14"   
// Local Python server (HTTP)

const char* OTA_MANIFEST_URL = "https://aiflux-bucket.s3.ap-south-1.amazonaws.com/laphu-ota-update-bins/manifest.json";
// const char* OTA_MANIFEST_URL = "http://192.168.0.248:8000/manifest.json";

// UART pins for MH-Z19B (match your wiring)
#define MHZ19B_TX_PIN 17
#define MHZ19B_RX_PIN 16
#define MHZ19B_BAUD 9600

HardwareSerial mhzSerial(2);
ErriezMHZ19B mhz19b(&mhzSerial);

// === OLED Display Settings ===
// Initialize for SH1106 128x64 hardware I2C, no reset pin
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// === MH-Z19 UART Configuration (TX->16, RX->17) ===
HardwareSerial mySerial(2);
MHZ19 myMHZ19;
////

// === DHT22 Sensor Settings ===
#define DHT22_PIN 23
DHT dht22(DHT22_PIN, DHT22);

// === MQTT Credentials ===
const char* mqtt_server = "mqtt.aiflux.tech";
const int mqtt_port = 1883;
const char* mqtt_user = "aiflux_user";
const char* mqtt_password = "Aiflux@2024";
const char* mqtt_topic = "23aff839-62a1-402e-98a9-fe1f548aecc8/data";

// ADD or UPDATE this line to use the correct API endpoint (remove ":3000")
const char* envMetricUrl = "https://laphu-api.aiflux.tech/api/current/environmental-metric/23aff839-62a1-402e-98a9-fe1f548aecc8";
const char* apiTempUrl   = "https://laphu-api.aiflux.tech/api/data/current/temperature?growhouse_id=23aff839-62a1-402e-98a9-fe1f548aecc8";
const char* apiHumiUrl   = "https://laphu-api.aiflux.tech/api/data/current/humidity?growhouse_id=23aff839-62a1-402e-98a9-fe1f548aecc8";
const char* apiCO2Url    = "https://laphu-api.aiflux.tech/api/data/current/co2?growhouse_id=23aff839-62a1-402e-98a9-fe1f548aecc8";



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
const int relayTempPin = 26;    // Relay 1 for Temp Control
const int relayHumiPin = 25;    // Relay 2 for Humidity Control
const int relayCO2Pin = 27;            // Set your chosen relay GPIO pin (example: GPIO27)

// const float TEMP_THRESHOLD = 25.0;    // Temperature threshold in °C
// const float HUMI_THRESHOLD = 80.0;    // Humidity threshold in %
// const int CO2_THRESHOLD = 1000;        // CO₂ threshold in ppm


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
  
  // Initialize secure client for HTTPS without cert validation
  secureClient.setInsecure();

  // Initialize MH-Z19B serial communication
  mhzSerial.begin(MHZ19B_BAUD, SERIAL_8N1, MHZ19B_RX_PIN, MHZ19B_TX_PIN);

// Wait for sensor detection
  while (!mhz19b.detect()) {
    Serial.println("Detecting MH-Z19B sensor...");
    delay(2000);
  }
  Serial.println("MH-Z19B detected");

// Allow warmup
  delay(10000);  // 10 seconds warm-up recommended

// Disable automatic baseline calibration for mushroom room
  mhz19b.setAutoCalibration(false);


  // Relay pin setup (NO relays -> HIGH = OFF, LOW = ON)
  pinMode(relayTempPin, OUTPUT);
  pinMode(relayHumiPin, OUTPUT);
  pinMode(relayCO2Pin, OUTPUT);
  
  digitalWrite(relayTempPin, HIGH); // Default OFF
  digitalWrite(relayHumiPin, HIGH); // Default OFF
  digitalWrite(relayCO2Pin, HIGH);  // Off (assuming relay is active LOW)

  // Setup WiFi via WiFiManager
  WiFiManager wm;
  if (!wm.autoConnect("AIFLUX-ESP32-Setup", "12345678")) {
    Serial.println("Failed to connect to WiFi. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi connected!");
  Serial.println(WiFi.localIP());

  // Initialize DHT22
  dht22.begin();
  Serial.println("DHT22 sensor initialized");

  setup_display();

  client.setServer(mqtt_server, mqtt_port);

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

  // Only one timing check block here:
  if (millis() - lastPublishTime >= interval) {

    // Read actual sensors
    float actualTemp = dht22.readTemperature();
    float actualHumi = dht22.readHumidity();
    int actualCO2 = -1;
    if (mhz19b.isReady()) {
      actualCO2 = mhz19b.readCO2();
    }

    // Publish actual sensor values to MQTT if valid
    if (!isnan(actualTemp) && !isnan(actualHumi) && actualCO2 > 0) {
      char sensorPayload[256];
      snprintf(sensorPayload, sizeof(sensorPayload),
        "{\"temperature\":%.2f,\"humidity\":%.2f,\"co2\":%d}",
        actualTemp, actualHumi, actualCO2);
      client.publish(mqtt_topic, sensorPayload);

      Serial.printf("Published actual sensors to MQTT: Temp %.2f, Humi %.2f, CO2 %d\n",
                    actualTemp, actualHumi, actualCO2);
    } else {
      Serial.println("Skipping sensor MQTT publish due to invalid readings");
    }



    // Step 1: Get thresholds from environmental metric API
    float minTemp = NAN, maxTemp = NAN, minHumi = NAN, maxHumi = NAN, minCO2 = NAN, maxCO2 = NAN;

    HTTPClient envHttp;
    envHttp.begin(secureClient, envMetricUrl);
    int envCode = envHttp.GET();
    if (envCode == 200) {
      String envPayload = envHttp.getString();
      StaticJsonDocument<256> envDoc;
      DeserializationError err = deserializeJson(envDoc, envPayload);
      if (!err && envDoc.containsKey("getCurrentEnviromentalMetric")) {
        JsonObject metric = envDoc["getCurrentEnviromentalMetric"];
        maxTemp = metric["maxTemp"];
        minTemp = metric["minTemp"];
        maxHumi = metric["maxHumidity"];
        minHumi = metric["minHumidity"];
        maxCO2  = metric["maxCo2"];
        minCO2  = metric["minCo2"];
      }
    }
    envHttp.end();

    // Step 2: Get actual sensor values from individual APIs
    float tempVal = NAN, humiVal = NAN; 
    int co2Val = -1;

    // Temperature
    HTTPClient httpTemp;
    httpTemp.begin(secureClient, apiTempUrl);
    int tempCode = httpTemp.GET();
    if (tempCode == 200) {
      String tempPayload = httpTemp.getString();
      StaticJsonDocument<128> tempDoc;
      DeserializationError tempErr = deserializeJson(tempDoc, tempPayload);
      if (!tempErr && tempDoc.containsKey("data")) {
        JsonObject obj = tempDoc["data"];
        if (obj.containsKey("value")) tempVal = obj["value"];
      }
    }
    httpTemp.end();

    // Humidity
    HTTPClient httpHumi;
    httpHumi.begin(secureClient, apiHumiUrl);
    int humiCode = httpHumi.GET();
    if (humiCode == 200) {
      String humiPayload = httpHumi.getString();
      StaticJsonDocument<128> humiDoc;
      DeserializationError humiErr = deserializeJson(humiDoc, humiPayload);
      if (!humiErr && humiDoc.containsKey("data")) {
        JsonObject obj = humiDoc["data"];
        if (obj.containsKey("value")) humiVal = obj["value"];
      }
    }
    httpHumi.end();

    // CO2
    HTTPClient httpCO2;
    httpCO2.begin(secureClient, apiCO2Url);  // use secureClient here for HTTPS
    int co2Code = httpCO2.GET();
    Serial.print("CO2 HTTP Code: ");
    Serial.println(co2Code);

    String co2Payload = httpCO2.getString();
    Serial.println("CO2 API response:");
    Serial.println(co2Payload);

    StaticJsonDocument<256> co2Doc;
    DeserializationError co2Err = deserializeJson(co2Doc, co2Payload);
    if (!co2Err && co2Doc.containsKey("data")) {
      JsonObject obj = co2Doc["data"];
      if (obj.containsKey("value")) co2Val = obj["value"];
    }
    httpCO2.end();




    Serial.printf("[DEBUG] minTemp: %.2f, maxTemp: %.2f\n", minTemp, maxTemp);
    Serial.printf("[DEBUG] minHumi: %.2f, maxHumi: %.2f\n", minHumi, maxHumi);
    Serial.printf("[DEBUG] minCO2: %.2f, maxCO2: %.2f\n", minCO2, maxCO2);
    Serial.printf("[DEBUG] tempVal: %.2f, humiVal: %.2f, co2Val: %d\n", tempVal, humiVal, co2Val);


    // Step 3: Relay control logic based on thresholds
    bool validAll = !isnan(tempVal) && !isnan(humiVal) && co2Val >= 0 &&
                    !isnan(minTemp) && !isnan(maxTemp) && !isnan(minHumi) && !isnan(maxHumi) && 
                    !isnan(minCO2) && !isnan(maxCO2);

    if (validAll) {
      if (tempVal < minTemp || tempVal > maxTemp) {
        digitalWrite(relayTempPin, HIGH);
        Serial.println("Relay Temp: ON");
      } else {
        digitalWrite(relayTempPin, LOW);
        Serial.println("Relay Temp: OFF");
      }
      if (humiVal < minHumi || humiVal > maxHumi) {
        digitalWrite(relayHumiPin, HIGH);
        Serial.println("Relay Humi: ON");
      } else {
        digitalWrite(relayHumiPin, LOW);
        Serial.println("Relay Humi: OFF");
      }
      if (co2Val < minCO2 || co2Val > maxCO2) {
        digitalWrite(relayCO2Pin, HIGH);
        Serial.println("Relay CO2: ON");
      } else {
        digitalWrite(relayCO2Pin, LOW);
        Serial.println("Relay CO2: OFF");
      }

      // OLED and MQTT code here (unchanged)

      display.setFont(u8g2_font_ncenB10_tr);
      display.clearBuffer();
      char buf[25];
      snprintf(buf, sizeof(buf), "Temp: %.1f C", tempVal); display.drawStr(0,18,buf);
      snprintf(buf, sizeof(buf), "CO2: %d ppm", co2Val); display.drawStr(0,37,buf);
      snprintf(buf, sizeof(buf), "Humi: %.1f %%", humiVal); display.drawStr(0,56,buf);
      display.sendBuffer();

      // char mqttPayload[256];
      // snprintf(mqttPayload, sizeof(mqttPayload),
      //   "{\"temperature\":%.2f,\"humidity\":%.2f,\"co2\":%d,"
      //   "\"relay_temp\":\"%s\",\"relay_humi\":\"%s\",\"relay_co2\":\"%s\"}",
      //   tempVal, humiVal, co2Val,
      //   (digitalRead(relayTempPin) == HIGH) ? "ON" : "OFF",
      //   (digitalRead(relayHumiPin) == HIGH) ? "ON" : "OFF",
      //   (digitalRead(relayCO2Pin) == HIGH) ? "ON" : "OFF");
      // client.publish(mqtt_topic, mqttPayload);

    } else {
      Serial.println("[API] Data/thresholds invalid/skipped control");
      display.clearBuffer();
      display.setFont(u8g2_font_6x10_tr);
      display.drawStr(0, 22, "API missing value");
      display.drawStr(0, 36, "Relays not updated");
      display.sendBuffer();
    }

    lastPublishTime = millis();
  }

  delay(100);
}
