#include <Arduino.h>
#include <ErriezMHZ19B.h>

// ESP32 UART pins connected to MH-Z19B
#define MHZ19B_TX_PIN 17  // ESP32 TX pin to MH-Z19B RX
#define MHZ19B_RX_PIN 16  // ESP32 RX pin to MH-Z19B TX
#define BAUDRATE 9600

// Create HardwareSerial object for UART2 on ESP32
HardwareSerial mhzSerial(2);

// Create MH-Z19B object
ErriezMHZ19B mhz19b(&mhzSerial);






// comment all and the void setup after first calibration
// Comment/uncomment the line below to enable manual zero calibration during setup
// #define DO_ZERO_CALIBRATION 1

// void setup() 
// {
//   Serial.begin(115200);
//   Serial.println("Erriez MH-Z19B CO2 sensor example");

//   // Initialize serial with MH-Z19 baud rate and ESP32 pins
//   mhzSerial.begin(BAUDRATE, SERIAL_8N1, MHZ19B_RX_PIN, MHZ19B_TX_PIN);

//   // Detect sensor, wait until connected
//   while (!mhz19b.detect())
//   {
//     Serial.println("Detecting MH-Z19B sensor...");
//     delay(2000);
//   }

//   Serial.println("Sensor detected.");

//   // Wait 3 minutes for sensor warmup (recommended)
//   Serial.println("Warm-up (3 min recommended for calibration)...");
//   delay(180000); // 3 min warmup for zero calibration accuracy

//   // Disable auto calibration (recommended for mushroom rooms)
//   mhz19b.setAutoCalibration(false);

// #if DO_ZERO_CALIBRATION
//   // Manual zero-point calibration; do this only in fresh outdoor air (400 ppm) AFTER warmup
//   Serial.println("Performing manual zero-point calibration (in fresh outdoor air)...");
//   mhz19b.startZeroCalibration();
//   // Wait 10 seconds for calibration command to process
//   delay(10000);
//   Serial.println("Zero-point calibration complete.");
// #endif

//   Serial.println("Sensor ready.");
// }

// void setup after first zero calibration
void setup() 
{
  Serial.begin(115200);
  mhzSerial.begin(BAUDRATE, SERIAL_8N1, MHZ19B_RX_PIN, MHZ19B_TX_PIN);

  while (!mhz19b.detect())
  {
    Serial.println("Detecting MH-Z19B sensor...");
    delay(2000);
  }

  Serial.println("Sensor detected.");

 // delay(180000); // (optional) warm-up

  mhz19b.setAutoCalibration(false); // Leave ABC disabled for mushroom rooms

  Serial.println("Sensor ready.");
}


void loop() 
{
  int16_t result;

  // Wait until sensor is ready to provide CO2 reading
  if (mhz19b.isReady())
  {
    result = mhz19b.readCO2();

    if (result < 0)
    {
      switch (result)
      {
        case MHZ19B_RESULT_ERR_CRC:
          Serial.println("CRC error");
          break;
        case MHZ19B_RESULT_ERR_TIMEOUT:
          Serial.println("RX timeout");
          break;
        default:
          Serial.print("Error: ");
          Serial.println(result);
          break;
      }
    } 
    else
    {
      Serial.print("CO2 (ppm): ");
      Serial.println(result);
    }
  }

  delay(2000); // Read every 2 seconds
}
