// Enable debug prints to serial monitor
#define MY_DEBUG
#define DEBUG

#define MY_RADIO_RF24
#define MY_BAUD_RATE 115200
#define MY_OTA_FIRMWARE_FEATURE

#include <SPI.h>
#include <MySensors.h>
#include <Wire.h>
#include <SI7021.h>
#ifndef MY_OTA_FIRMWARE_FEATURE
#include "drivers/SPIFlash/SPIFlash.cpp"
#endif
#include <sha204_library.h>
#include <RunningAverage.h>

#define SKETCH_NAME "Sensebender Micro"
#define SKETCH_MAJOR_VER "0"
#define SKETCH_MINOR_VER "2"

// Child sensor ID's
#define CHILD_ID_TEMP  1
#define CHILD_ID_HUM   2
// Uncomment the line below, to transmit battery voltage as a normal sensor value
#define CHILD_ID_BATT  199

#define AVERAGES 2

// How many milliseconds between each measurement
#define MEASURE_INTERVAL 60000 // 60s
// How many milliseconds should we wait for OTA?
#define OTA_WAIT_PERIOD 300
// FORCE_TRANSMIT_INTERVAL, this number of times of wakeup, the sensor is forced to report all values to the controller
#define FORCE_TRANSMIT_INTERVAL 30 
// When MEASURE_INTERVAL is 60000 and FORCE_TRANSMIT_INTERVAL is 30, we force a transmission every 30 minutes.
// Between the forced transmissions a tranmission will only occur if the measured value differs from the previous measurement

// THRESHOLD defines how much the value should have changed since last time it was transmitted.
#define HUMI_TRANSMIT_THRESHOLD 0.3
#define TEMP_TRANSMIT_THRESHOLD 0.3

// Pin definitions
#define TEST_PIN       A0
#define LED_PIN        A2
#define ATSHA204_PIN   17 // A3

const int sha204Pin = ATSHA204_PIN;
atsha204Class sha204(sha204Pin);

SI7021 humiditySensor;
SPIFlash flash(8, 0x1F65);

// Sensor messages
MyMessage msgTemp(CHILD_ID_TEMP, V_TEMP);
MyMessage msgHum(CHILD_ID_HUM, V_HUM);
#ifdef CHILD_ID_BATT
MyMessage msgBatt(CHILD_ID_BATT, V_VOLTAGE);
#endif

// Global settings
int measureCount = 0;
int sendBattery = 0;
boolean highfreq = true;
boolean transmission_occured = false;

// Storage of old measurements
float lastTemperature = -100;
int lastHumidity = -100;
long lastBattery = -100;

RunningAverage raHum(AVERAGES);

// TODO: Move this to header file?
void sendMeasurements(bool force);
void testMode();
void sendBattLevel(bool force);
long readVcc();

/********************************
 * Setup
 ********************************/
void setup() {
#ifdef DEBUG
  Serial.println("setup");
#endif
 
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.print(SKETCH_NAME SKETCH_MAJOR_VER "." SKETCH_MINOR_VER);
  Serial.flush();

  // First check if we should boot into test mode
  pinMode(TEST_PIN, INPUT);
  digitalWrite(TEST_PIN, HIGH); // Enable pullup
  if (!digitalRead(TEST_PIN)) testMode();

  // Make sure that ATSHA204 is not floating
  pinMode(ATSHA204_PIN, INPUT);
  digitalWrite(ATSHA204_PIN, HIGH);
  
  digitalWrite(TEST_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); 
  humiditySensor.begin();
  digitalWrite(LED_PIN, LOW);

  Serial.flush();
  Serial.println(" - Online!");
  
  raHum.clear();
  // Send initial values
  sendMeasurements(false);
  sendBattLevel(false);
  
#ifdef MY_OTA_FIRMWARE_FEATURE  
  Serial.println("OTA FW update enabled");
#endif
}

/******************************
 * Presentation
 ******************************/
void presentation()  {
#ifdef DEBUG
  Serial.println("presentation");
#endif
  sendSketchInfo(SKETCH_NAME, SKETCH_MAJOR_VER "." SKETCH_MINOR_VER);
  present(CHILD_ID_TEMP, S_TEMP);
  present(CHILD_ID_HUM, S_HUM);
#ifdef CHILD_ID_BATT
  present(CHILD_ID_BATT, S_POWER);
#endif
}

/*******************************
 * Loop
 *******************************/
void loop() {
#ifdef DEBUG
  Serial.println("loop");
#endif
  measureCount ++;
  sendBattery ++;
  bool forceTransmit = false;
  transmission_occured = false;
  
  if (measureCount >= FORCE_TRANSMIT_INTERVAL) { // force a transmission
    forceTransmit = true; 
    measureCount = 0;
  }
  sendMeasurements(forceTransmit);
  
#ifdef MY_OTA_FIRMWARE_FEATURE
  if (transmission_occured) {
      wait(OTA_WAIT_PERIOD);
  }
#endif

  sleep(MEASURE_INTERVAL);  
}


/*******************************
 * Sends temperature and humidity from Si7021 sensor
 * Parameters
 * - force : Forces transmission of a value (even if it's the same as previous measurement)
 *******************************/
void sendMeasurements(bool force) {
#ifdef DEBUG
  Serial.println("Sending Measurements");
#endif

  bool tx = force;
  si7021_thc data = humiditySensor.getTempAndRH();
  
  raHum.addValue(data.humidityPercent);
  
  float diffTemp = abs(lastTemperature - data.celsiusHundredths/100.0);
  float diffHum = abs(lastHumidity - raHum.getAverage());

  Serial.print("TempDiff :");
  Serial.println(diffTemp);
  Serial.print("HumDiff  :");
  Serial.println(diffHum); 

  if (isnan(diffHum)) tx = true; 
  if (diffTemp >= TEMP_TRANSMIT_THRESHOLD) tx = true;
  if (diffHum >= HUMI_TRANSMIT_THRESHOLD) tx = true;

  if (tx) {
    measureCount = 0;
    float temperature = data.celsiusHundredths / 100.0;
     
    int humidity = data.humidityPercent;
    Serial.print("T: ");
    Serial.println(temperature);
    Serial.print("H: ");
    Serial.println(humidity);
    
    send(msgTemp.set(temperature,1));
    send(msgHum.set(humidity));
    lastTemperature = temperature;
    lastHumidity = humidity;
    transmission_occured = true;
    if (sendBattery > 60) {
      sendBattLevel(true); // Not needed to send battery info that often
      sendBattery = 0;
    }
  }
}

/*******************************
 * Sends battery information (battery percentage)
 * Parameters
 * - force : Forces transmission of a value
 *******************************/
void sendBattLevel(bool force) {
#ifdef DEBUG
  Serial.println("sendBattLevel");
#endif
  if (force) lastBattery = -1;
  long vcc = readVcc();
  if (vcc != lastBattery) {
    lastBattery = vcc;

#ifdef CHILD_ID_BATT
    float send_voltage = float(vcc)/1000.0f;
    send(msgBatt.set(send_voltage, 3));
#endif
    // Calculate percentage
    vcc = vcc - 1900; // subtract 1.9V from vcc, as this is the lowest voltage we will operate at
    long percent = vcc / 14.0;
    sendBatteryLevel(percent);
    transmission_occured = true;
  }
}

/*******************************
 * Internal battery ADC measuring 
 *******************************/
long readVcc() {
  // Read 1.1V reference against AVcc
  // set the reference to Vcc and the measurement to the internal 1.1V reference
  #if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
    ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
    ADMUX = _BV(MUX5) | _BV(MUX0);
  #elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
    ADcdMUX = _BV(MUX3) | _BV(MUX2);
  #else
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #endif  
 
  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA,ADSC)); // measuring
 
  uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH  
  uint8_t high = ADCH; // unlocks both
 
  long result = (high<<8) | low;
  result = 1125300L / result; // Calculate Vcc (in mV); 1125300 = 1.1*1023*1000

  return result; // Vcc in millivolts
}

/*******************************
 * Verify all peripherals, and signal via the LED if any problems.
 *******************************/
void testMode() {
  uint8_t rx_buffer[SHA204_RSP_SIZE_MAX];
  uint8_t ret_code;
  byte tests = 0;
  
  digitalWrite(LED_PIN, HIGH); // Turn on LED.
  Serial.println(" - TestMode");
  Serial.println("Testing peripherals!");
  Serial.flush();
  Serial.print("-> SI7021 : "); 
  Serial.flush();
  
  if (humiditySensor.begin()) {
    Serial.println("ok!");
    tests ++;
  } else {
    Serial.println("failed!");
  }
  Serial.flush();

  Serial.print("-> Flash : ");
  Serial.flush();
  if (flash.initialize()) {
    Serial.println("ok!");
    tests ++;
  } else {
    Serial.println("failed!");
  }
  Serial.flush();

  
  Serial.print("-> SHA204 : ");
  ret_code = sha204.sha204c_wakeup(rx_buffer);
  Serial.flush();
  if (ret_code != SHA204_SUCCESS) {
    Serial.print("Failed to wake device. Response: "); 
    Serial.println(ret_code, HEX);
  }
  Serial.flush();
  if (ret_code == SHA204_SUCCESS) {
    ret_code = sha204.getSerialNumber(rx_buffer);
    if (ret_code != SHA204_SUCCESS) {
      Serial.print("Failed to obtain device serial number. Response: "); Serial.println(ret_code, HEX);
    } else {
      Serial.print("Ok (serial : ");
      for (int i=0; i<9; i++) {
        if (rx_buffer[i] < 0x10) {
          Serial.print('0'); // Because Serial.print does not 0-pad HEX
        }
        Serial.print(rx_buffer[i], HEX);
      }
      Serial.println(")");
      tests ++;
    }
  }
  Serial.flush();

  Serial.println("Test finished");
  
  if (tests == 3) {
    Serial.println("Selftest ok!");
    while (1) { // Blink OK pattern!
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
  } else {
    Serial.println("----> Selftest failed!");
    while (1) {} // Blink FAILED pattern! Rappidly blinking..
  }  
}
