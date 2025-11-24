/***************************************************************
 * @file    BasicRead.ino
 * @brief   Minimal demo for the 7Semi CO2TH I²C driver.
 *
 * Features demonstrated:
 * - Auto-detect I²C address (Begin with address = 0; scans 0x08..0x77)
 * - Start continuous measurement
 * - Read and print CO₂ (ppm), Temperature (°C), Humidity (%RH), Status (u16)
 *
 * Requirements:
 * - Arduino core with Wire
 * - 7Semi driver files: 7Semi_CO2TH.h / 7Semi_CO2TH.cpp
 *
 * Connections (typical):
 * - SDA -> board SDA
 * - SCL -> board SCL
 * - VDD -> 3.3V / 5V
 * - GND -> GND
 *
 * Author  : 7Semi
 * License : MIT
 * Version : 1.0.2
 * Date    : 15 October 2025
 ***************************************************************/
 
#include <7Semi_CO2TH.h>
 
/** - One driver instance */
CO2TH_7Semi CO2TH;
 
/** - Read period (ms): match sensor frame interval */
static const unsigned long kPeriodMs = 1200;
 
void setup() {
  /** - Serial + I2C */
  Serial.begin(115200);
  while (!Serial)
    ;
  // Ardiuno UNO
  err_t e = CO2TH.Begin();
  // err_t e = CO2TH.Begin(/*sda*/ 21, /*scl*/ 22, /*freq*/ 400000, /*port*/ 0);
  if (e != NO_ERROR) {
    Serial.print(F("Begin failed, err="));
    Serial.println((int)e);
    while (true) { delay(1000); }
  }
 
  /** - Start continuous measurement */
  e = CO2TH.StartContinuousMeasurement();
  if (e != NO_ERROR) {
    Serial.print(F("Start failed, err="));
    Serial.println((int)e);
    while (true) { delay(1000); }
  }
 
  /** - Give the sensor one frame to produce the first result */
  delay(kPeriodMs);
}
 
void loop() {
  static unsigned long last = 0;
  const unsigned long now = millis();
  if (now - last < kPeriodMs) return;
  last = now;
 
  /** - Read and print values */
  int16_t co2 = 0;
  float temp = 0.0f;
  float rh = 0.0f;
  uint16_t st = 0;
 
  err_t e = CO2TH.ReadMeasurement(co2, temp, rh, st);
  if (e != NO_ERROR) {
    Serial.print(F("Read failed, err="));
    Serial.println((int)e);
    return;
  }
 
  Serial.print(F("CO2 = "));
  Serial.print(co2);
  Serial.print(F(" ppm, T = "));
  Serial.print(temp, 2);
  Serial.print(F(" °C, RH = "));
  Serial.println(rh, 2);
}
 