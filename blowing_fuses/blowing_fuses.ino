#include "esp_efuse.h"
#include "esp_efuse_table.h"


// // Relay pins and thresholds
// const int relayTempPin = 26;    // Relay 1 for Temp Control
// const int relayHumiPin = 25;    // Relay 2 for Humidity Control
// const float TEMP_THRESHOLD = 25.0;    // Temperature threshold in °C
// const float HUMI_THRESHOLD = 80.0;    // Humidity threshold in %


//after this

bool LockChip() {  // Blows security fuses to lock the chip. Returns true if all fuses were blown successfully.
  bool fuseErr = false;

  // Disable UART download mode
if (esp_efuse_write_field_bit(ESP_EFUSE_UART_DOWNLOAD_DIS) != 0) {
    Serial.println("Error: blowing UART_DOWNLOAD_DIS returned an error.");
    fuseErr = true;
}

// Disable download mode encryption
if (esp_efuse_write_field_bit(ESP_EFUSE_DISABLE_DL_ENCRYPT) != 0) {
    Serial.println("Error: blowing DISABLE_DL_ENCRYPT returned an error.");
    fuseErr = true;
}

// Disable instruction cache for download mode
if (esp_efuse_write_field_bit(ESP_EFUSE_DISABLE_DL_CACHE) != 0) {
    Serial.println("Error: blowing DISABLE_DL_CACHE returned an error.");
    fuseErr = true;
}

// Disable force download mode
if (esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_UART_DOWNLOAD_DIS) != 0) {
    Serial.println("Error: blowing WR_DIS_UART_DOWNLOAD_DIS returned an error.");
    fuseErr = true;
}

// Disable physical JTAG pins (and USB-JTAG as same macro)
if (esp_efuse_write_field_bit(ESP_EFUSE_DISABLE_JTAG) != 0) {
    Serial.println("Error: blowing DISABLE_JTAG returned an error.");
    fuseErr = true;
}

// Skip `ESP_EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` and `ESP_EFUSE_SOFT_DIS_JTAG` if they do not exist in your SDK

  return !fuseErr;
}

// above before oled display



// below in void setup

void setup() {
  Serial.begin(115200);
  delay(1200);  // Wait for Serial Monitor connection

  Serial.println("Type LOCKCHIP and ENTER to permanently blow security eFuses...");
  delay(800);  // Give time for user to open Serial Monitor

  // Manual trigger: run just once, on YOUR command.
  if (Serial.available()) Serial.readStringUntil('\n');  // Clear input buffer
  while (Serial.available() == 0) {
    // Wait until you type a command and press Enter
    // (add status blink here if you want)
    delay(10);
  }
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "LOCKCHIP") {
    if (LockChip()) {
      Serial.println("\nSUCCESS: eFuses blown and chip is now locked!");
    } else {
      Serial.println("\nERROR: Failed (or already burned).");
    }
    delay(3000);  // Let user read the message
    ESP.restart();
  } else {
    Serial.println("\nEfuse NOT burned. Setup will continue.");
  }

  // After this, continue with your normal setup() code:
  // (WiFi, OTA, MQTT, sensor init, etc.)
}
