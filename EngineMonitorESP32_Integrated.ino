// Libraries
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Arduino_JSON.h>
#include <U8g2lib.h> // For OLED
#include <OneWire.h>
#include <DallasTemperature.h>
#include "max6675.h"      // For MAX6675 EGT sensor
#include <Wire.h>         // For I2C communication (BMP280, OLED)
#include <Adafruit_BMP280.h> // For BMP280 sensor
#include <vector> // For std::vector

// --- Global Definitions & Variables ---

// SPIFFS Configuration Files
const char* CONFIG_FILE = "/sensor_config.json";
const char* GPIO_CONFIG_FILE = "/gpio_panel_config.json";

// DS18B20 Setup
const int ONEWIRE_MAIN_BUS_PIN = 4;
const int ONEWIRE_DISCOVERY_PIN = 25; // New pin for discovery
OneWire oneWireMain(ONEWIRE_MAIN_BUS_PIN);
DallasTemperature sensorsMain(&oneWireMain);
OneWire oneWireDiscovery(ONEWIRE_DISCOVERY_PIN);
DallasTemperature sensorsDiscovery(&oneWireDiscovery);

// OneWire device discovery
std::vector<String> discoveredOneWireAddressesHex;
unsigned long lastOneWireDiscoveryTime = 0;
const unsigned long ONE_WIRE_DISCOVERY_INTERVAL = 60000;
const uint8_t MAX_CONSECUTIVE_FAILURES = 5;

// Analog Pressure Sensor Constants
const float PRESSURE_ZERO_ADC = 410.0;
const float PRESSURE_MAX_ADC = 3686.0;
const float PRESSURE_TRANSDUCER_MAX_PSI = 100.0;

// Analog Voltage Sensor Constants
const float VOLTAGE_DIVIDER_R1 = 30000.0;
const float VOLTAGE_DIVIDER_R2 = 7500.0;
const float REFERENCE_VOLTAGE_ESP32 = 3.3;

// BMP280
Adafruit_BMP280 bmp;
bool bmpAvailable = false;

// Sensor Configuration
#define MAX_SENSORS 15
enum SensorType {
  UNDEFINED, PRESSURE_ANALOG, TEMP_DS18B20, TEMP_MAX6675,
  TEMP_BMP085, VOLTAGE_ANALOG, TEMP_BMP280
};
struct SensorConfig {
  String id; String name; SensorType sensorType; int pin1; int pin2; int pin3;
  uint8_t oneWireAddress[8]; bool enabled; float value;
  uint8_t consecutiveFailures;
  float warningThreshold; float criticalThreshold;
  float lowerWarningThreshold; float lowerCriticalThreshold; bool displayOnOLED;
};
SensorConfig configuredSensors[MAX_SENSORS];
int numConfiguredSensors = 0;

// GPIO Configuration
#define MAX_GPIO_PINS 10
enum PinModeType { GPIO_MODE_ON_OFF, GPIO_MODE_BLINK };
struct GPIOPinConfig {
  String id;
  String name;
  uint8_t pinNumber;
  PinModeType mode;
  bool defaultState;
  unsigned long defaultBlinkDelayMs;
  bool currentState;
  unsigned long currentBlinkDelayMs;
  unsigned long lastBlinkToggleTime;
};
GPIOPinConfig configuredGPIOPins[MAX_GPIO_PINS];
int numConfiguredGPIOPins = 0;

// Hardware Pins
int oled_sda_pin = 21; int oled_scl_pin = 22;

// OLED Display
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, oled_scl_pin, oled_sda_pin);
bool oledAvailable = false;

// Web Server
AsyncWebServer server(80);
AsyncEventSource events("/events");
const char* apSSID = "EngineMonitorAP";
const char* apPassword = "password123";

// Alert Pins
const int BUZZER_PIN = 18;
const int FLASHER_LED_PIN = 19;

// --- OLED Pixel Calculation Variables ---
int numberOfPixels = 75;
float maxOP = 75.0; float minOP = 0.0;
float maxCT = 127.5; float minCT = 15.0;
float maxEGT = 800.0; float minEGT = 30.0;
float maxBST = 20.0; float minBST = 0.0;
float multiplierOP = (maxOP - minOP) != 0 ? pow((maxOP - minOP), -1) * numberOfPixels : 0;
float baselineOP = minOP * multiplierOP - 25;
float multiplierCT = (maxCT - minCT) != 0 ? pow((maxCT - minCT), -1) * numberOfPixels : 0;
float baselineCT = minCT * multiplierCT - 25;
float multiplierEGT = (maxEGT - minEGT) != 0 ? pow((maxEGT - minEGT), -1) * numberOfPixels : 0;
float baselineEGT = minEGT * multiplierEGT - 25;
float multiplierBST = (maxBST - minBST) != 0 ? pow((maxBST - minBST), -1) * numberOfPixels : 0;
float baselineBST = minBST * multiplierBST - 25;

// --- Helper Functions for Type Conversion & Config ---
byte hexToByte(char hex) { /* ... same ... */
  if (hex >= '0' && hex <= '9') return hex - '0';
  if (hex >= 'a' && hex <= 'f') return hex - 'a' + 10;
  if (hex >= 'A' && hex <= 'F') return hex - 'A' + 10;
  return 0;
}
void parseHexStringToByteArray(const String& hexStr, uint8_t* byteArray, int arraySize) { /* ... same ... */
  for (int i = 0; i < arraySize; ++i) {
    if (2 * i + 1 < hexStr.length()) {
      byteArray[i] = (hexToByte(hexStr.charAt(2 * i)) << 4) + hexToByte(hexStr.charAt(2 * i + 1));
    } else {
      byteArray[i] = 0;
    }
  }
}
String sensorTypeToString(SensorType type) { /* ... same ... */
  switch (type) {
    case UNDEFINED: return "UNDEF";
    case PRESSURE_ANALOG: return "PRES_A";
    case TEMP_DS18B20: return "DS18B20";
    case TEMP_MAX6675: return "MAX6675";
    case TEMP_BMP085: return "BMP085";
    case VOLTAGE_ANALOG: return "VOLT_A";
    case TEMP_BMP280: return "BMP280";
    default: return "UNK";
  }
}
SensorType stringToSensorType(String strType) { /* ... same ... */
  strType.toUpperCase();
  if (strType.equals("PRES_A")) return PRESSURE_ANALOG;
  if (strType.equals("DS18B20")) return TEMP_DS18B20;
  if (strType.equals("MAX6675")) return TEMP_MAX6675;
  if (strType.equals("BMP085")) return TEMP_BMP085;
  if (strType.equals("VOLT_A")) return VOLTAGE_ANALOG;
  if (strType.equals("BMP280")) return TEMP_BMP280;
  return UNDEFINED;
}
String oneWireAddressToString(const uint8_t* address) { /* ... same ... */
  String result = "";
  for (int i = 0; i < 8; ++i) {
    if (address[i] < 16) result += "0";
    result += String(address[i], HEX);
  }
  result.toUpperCase();
  return result;
}
String pinModeTypeToString(PinModeType mode) { /* ... same ... */
  switch (mode) {
    case GPIO_MODE_ON_OFF: return "ON_OFF";
    case GPIO_MODE_BLINK: return "BLINK";
    default: return "UNKNOWN";
  }
}
PinModeType stringToPinModeType(String modeStr) { /* ... same ... */
  modeStr.toUpperCase();
  if (modeStr.equals("ON_OFF")) return GPIO_MODE_ON_OFF;
  if (modeStr.equals("BLINK")) return GPIO_MODE_BLINK;
  return GPIO_MODE_ON_OFF;
}

// --- OLED Display Helper Functions ---
void printShortDateTimeOLED(U8G2 &u8g2_display) { u8g2_display.print("Time N/A"); }
void displayTemperatureOLED(U8G2 &u8g2_display, float tempC, bool error = false) { /* ... same ... */
  if (error || tempC <= -998.0) { u8g2_display.print("N/A"); }
  else { u8g2_display.print(tempC, 0); }
}
SensorConfig* findSensorById(const String& id) { /* ... same ... */
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].id.equals(id)) return &configuredSensors[i];
  }
  return nullptr;
}
GPIOPinConfig* findGPIOPinById(const String& id) { /* ... same ... */
  for (int i = 0; i < numConfiguredGPIOPins; i++) {
    if (configuredGPIOPins[i].id.equals(id)) {
      return &configuredGPIOPins[i];
    }
  }
  return nullptr;
}

// --- Alert Functions ---
void flash() { /* ... same ... */
  digitalWrite(FLASHER_LED_PIN, HIGH); delay(20);
  digitalWrite(FLASHER_LED_PIN, LOW); delay(30);
}
void buzz() { /* ... same ... */
  digitalWrite(BUZZER_PIN, HIGH); delay(50);
  digitalWrite(BUZZER_PIN, LOW);
}
void checkAllSensorAlerts() { /* ... same ... */
  static unsigned long lastAlertCheckTime = 0;
  if (millis() - lastAlertCheckTime < 500) return;
  lastAlertCheckTime = millis();
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].enabled && configuredSensors[i].value > -998.0) {
      SensorConfig s = configuredSensors[i];
      if (s.criticalThreshold > 0 && s.value >= s.criticalThreshold) { Serial.printf("CRITICAL ALERT: %s value %.2f >= critical threshold %.2f\n", s.name.c_str(), s.value, s.criticalThreshold); buzz(); }
      else if (s.warningThreshold > 0 && s.value >= s.warningThreshold) { Serial.printf("Warning Alert: %s value %.2f >= warning threshold %.2f\n", s.name.c_str(), s.value, s.warningThreshold); flash(); }
      if (s.lowerCriticalThreshold > 0 || (s.sensorType == PRESSURE_ANALOG && s.lowerCriticalThreshold != 0)) {
          if (s.value <= s.lowerCriticalThreshold) { Serial.printf("CRITICAL ALERT: %s value %.2f <= lower critical threshold %.2f\n", s.name.c_str(), s.value, s.lowerCriticalThreshold); buzz(); }
      } else if (s.lowerWarningThreshold > 0 || (s.sensorType == PRESSURE_ANALOG && s.lowerWarningThreshold != 0) ) {
           if (s.value <= s.lowerWarningThreshold) { Serial.printf("Warning Alert: %s value %.2f <= lower warning threshold %.2f\n", s.name.c_str(), s.value, s.lowerWarningThreshold); flash(); }
      }
    }
  }
}

// --- Sensor Reading Functions ---
void readDS18B20Sensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_DS18B20) {
    float tempC = sensorsMain.getTempC(sensor.oneWireAddress); // Use sensorsMain
    if (tempC == DEVICE_DISCONNECTED_C || tempC == 85.0 || tempC == -127.0) {
      sensor.value = -999.0;
      if(sensor.consecutiveFailures < 255) sensor.consecutiveFailures++;
      if(sensor.consecutiveFailures >= MAX_CONSECUTIVE_FAILURES && sensor.consecutiveFailures % MAX_CONSECUTIVE_FAILURES == 0) {
         Serial.printf("Sensor %s (DS18B20) has failed %d consecutive times.\n", sensor.name.c_str(), sensor.consecutiveFailures);
      }
    } else {
      sensor.value = tempC;
      if(sensor.consecutiveFailures > 0) {
          Serial.printf("Sensor %s (DS18B20) recovered after %d failures.\n", sensor.name.c_str(), sensor.consecutiveFailures);
      }
      sensor.consecutiveFailures = 0;
    }
  }
}
void readAnalogPressureSensor(SensorConfig &sensor) { /* ... same ... */
  if (sensor.enabled && sensor.sensorType == PRESSURE_ANALOG) {
    int rawValue = analogRead(sensor.pin1);
    sensor.value = ((float)rawValue - PRESSURE_ZERO_ADC) * PRESSURE_TRANSDUCER_MAX_PSI / (PRESSURE_MAX_ADC - PRESSURE_ZERO_ADC);
  }
}
void readMAX6675Sensor(SensorConfig &sensor) { /* ... same ... */
  if (sensor.enabled && sensor.sensorType == TEMP_MAX6675) {
    MAX6675 thermocouple(sensor.pin1, sensor.pin2, sensor.pin3);
    float temp = thermocouple.readCelsius();
    if (isnan(temp)) { sensor.value = -999.0; }
    else { sensor.value = temp - 8.0; }
  }
}
void readBMP280Sensor(SensorConfig &sensor) { /* ... same ... */
  if (sensor.enabled && (sensor.sensorType == TEMP_BMP280 || sensor.sensorType == TEMP_BMP085)) {
    if (bmpAvailable) {
      float temp = bmp.readTemperature();
      if (isnan(temp)) { sensor.value = -999.0; }
      else { sensor.value = temp; }
    } else { sensor.value = -998.0; }
  }
}
void readAnalogVoltageSensor(SensorConfig &sensor) { /* ... same ... */
  if (sensor.enabled && sensor.sensorType == VOLTAGE_ANALOG) {
    int rawValue = analogRead(sensor.pin1);
    float adc_voltage = (rawValue * REFERENCE_VOLTAGE_ESP32) / 4095.0;
    sensor.value = adc_voltage / (VOLTAGE_DIVIDER_R2 / (VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2));
  }
}

// --- GPIO Control Functions ---
void initializeGPIOPin(GPIOPinConfig &config) {
    if (config.pinNumber >= GPIO_NUM_MAX) {
        Serial.printf("Error: Invalid pin number %d for GPIO %s\n", config.pinNumber, config.id.c_str());
        return;
    }
    pinMode(config.pinNumber, OUTPUT);
    config.currentState = config.defaultState;
    config.currentBlinkDelayMs = config.defaultBlinkDelayMs;
    // Initialize lastBlinkToggleTime to ensure the first state is set correctly for blinking pins
    config.lastBlinkToggleTime = millis();

    if (config.mode == GPIO_MODE_ON_OFF) {
        digitalWrite(config.pinNumber, config.currentState ? HIGH : LOW);
    } else { // GPIO_MODE_BLINK
        if (config.currentState) { // If blinking is active by default
            if (config.currentBlinkDelayMs == 0) { // Steady ON if delay is 0
                digitalWrite(config.pinNumber, HIGH);
            } else {
                 // For active blinking, start with the pin LOW, handleGPIOOutputs will toggle it HIGH after first delay
                 digitalWrite(config.pinNumber, LOW);
            }
        } else { // Blinking not active by default
            digitalWrite(config.pinNumber, LOW);
        }
    }
}

void handleGPIOOutputs() {
    unsigned long currentTime = millis();
    for (int i = 0; i < numConfiguredGPIOPins; i++) {
        if (configuredGPIOPins[i].pinNumber >= GPIO_NUM_MAX) continue;

        if (configuredGPIOPins[i].mode == GPIO_MODE_BLINK) {
            if (configuredGPIOPins[i].currentState && configuredGPIOPins[i].currentBlinkDelayMs > 0) {
                if (currentTime - configuredGPIOPins[i].lastBlinkToggleTime >= configuredGPIOPins[i].currentBlinkDelayMs) {
                    digitalWrite(configuredGPIOPins[i].pinNumber, !digitalRead(configuredGPIOPins[i].pinNumber));
                    configuredGPIOPins[i].lastBlinkToggleTime = currentTime;
                }
            } else { // Not actively blinking (either currentState is false or delay is 0)
                 bool steadyState = (configuredGPIOPins[i].currentState && configuredGPIOPins[i].currentBlinkDelayMs == 0) ? HIGH : LOW;
                 digitalWrite(configuredGPIOPins[i].pinNumber, steadyState);
            }
        } else { // GPIO_MODE_ON_OFF
            digitalWrite(configuredGPIOPins[i].pinNumber, configuredGPIOPins[i].currentState ? HIGH : LOW);
        }
    }
}

// --- SPIFFS & Configuration Functions ---
void initSPIFFS() { /* ... same ... */
  if (!SPIFFS.begin(true)) {
    Serial.println("An error has occurred while mounting SPIFFS");
    return;
  }
  Serial.println("SPIFFS mounted successfully");
}

void loadDefaultGPIOConfiguration() { /* ... same ... */
  Serial.println("Loading default GPIO configurations...");
  numConfiguredGPIOPins = 0;
  if (numConfiguredGPIOPins < MAX_GPIO_PINS) {
    configuredGPIOPins[numConfiguredGPIOPins++] = { "pump1", "Fuel Pump", 26, GPIO_MODE_ON_OFF, false, 0, false, 0, 0 };
  }
  if (numConfiguredGPIOPins < MAX_GPIO_PINS) {
    configuredGPIOPins[numConfiguredGPIOPins++] = { "warn_led", "Warning LED", 27, GPIO_MODE_BLINK, false, 500, false, 500, 0 };
  }
  for(int i=0; i < numConfiguredGPIOPins; i++){
    initializeGPIOPin(configuredGPIOPins[i]);
  }
  Serial.println(String(numConfiguredGPIOPins) + " default GPIO pins loaded and initialized.");
}

void saveGPIOConfiguration() { /* ... same ... */
  Serial.println("Saving GPIO configuration to SPIFFS...");
  JSONVar gpioArray;
  for (int i = 0; i < numConfiguredGPIOPins; i++) {
    JSONVar gpioConf;
    gpioConf["id"] = configuredGPIOPins[i].id;
    gpioConf["name"] = configuredGPIOPins[i].name;
    gpioConf["pinNumber"] = configuredGPIOPins[i].pinNumber;
    gpioConf["mode"] = pinModeTypeToString(configuredGPIOPins[i].mode);
    gpioConf["defaultState"] = configuredGPIOPins[i].defaultState;
    gpioConf["defaultBlinkDelayMs"] = configuredGPIOPins[i].defaultBlinkDelayMs;
    gpioArray[i] = gpioConf;
  }
  File configFile = SPIFFS.open(GPIO_CONFIG_FILE, "w");
  if (!configFile) { Serial.println("Failed to open GPIO config file for writing"); return; }
  String jsonString = JSON.stringify(gpioArray);
  if (configFile.print(jsonString)) { Serial.println("GPIO Configuration saved successfully to SPIFFS."); }
  else { Serial.println("Failed to write GPIO config to file"); }
  configFile.close();
}

void loadGPIOConfiguration() { /* ... same ... */
  Serial.println("Loading GPIO configuration from SPIFFS...");
  if (SPIFFS.exists(GPIO_CONFIG_FILE)) {
    File configFile = SPIFFS.open(GPIO_CONFIG_FILE, "r");
    if (configFile) {
      String configData = configFile.readString();
      configFile.close();
      JSONVar parsedConfig = JSON.parse(configData);
      if (JSON.typeof(parsedConfig) == "array") {
        numConfiguredGPIOPins = 0;
        for (int i = 0; i < parsedConfig.length(); i++) {
          if (numConfiguredGPIOPins >= MAX_GPIO_PINS) { Serial.println("MAX_GPIO_PINS reached while loading GPIO config."); break; }
          JSONVar gpioJson = parsedConfig[i]; GPIOPinConfig tempConf;
          tempConf.id = String((const char*) gpioJson["id"]);
          tempConf.name = String((const char*) gpioJson["name"]);
          tempConf.pinNumber = (uint8_t) (int) gpioJson["pinNumber"];
          tempConf.mode = stringToPinModeType(String((const char*)gpioJson["mode"]));
          tempConf.defaultState = (bool) gpioJson["defaultState"];
          tempConf.defaultBlinkDelayMs = (unsigned long) (int) gpioJson["defaultBlinkDelayMs"];
          initializeGPIOPin(tempConf);
          configuredGPIOPins[numConfiguredGPIOPins++] = tempConf;
        }
        Serial.println("GPIO Configuration loaded successfully from SPIFFS."); return;
      } else { Serial.println("Error parsing GPIO config file. Loading defaults."); }
    } else { Serial.println("Error opening GPIO config file. Loading defaults."); }
  } else { Serial.println("GPIO Config file not found. Loading and saving defaults."); }
  loadDefaultGPIOConfiguration();
  saveGPIOConfiguration();
}

void loadDefaultConfiguration() {
  Serial.println("Loading default sensor configurations...");
  numConfiguredSensors = 0;
  if (numConfiguredSensors < MAX_SENSORS) { configuredSensors[numConfiguredSensors] = { "coolant_temp", "Coolant", TEMP_DS18B20, ONEWIRE_MAIN_BUS_PIN, -1, -1, {0x28, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}, true, 0.0, 0, 95.0, 105.0, 60.0, 50.0, true }; numConfiguredSensors++; }
  if (numConfiguredSensors < MAX_SENSORS) { configuredSensors[numConfiguredSensors] = { "oil_pressure", "Oil PSI", PRESSURE_ANALOG, 34, -1, -1, {0}, true, 0.0, 0, 20.0, 80.0, 10.0, 5.0, true }; numConfiguredSensors++; }
  if (numConfiguredSensors < MAX_SENSORS) { configuredSensors[numConfiguredSensors] = { "bat_voltage", "Battery V", VOLTAGE_ANALOG, 35, -1, -1, {0}, true, 0.0, 0, 14.8, 15.2, 11.8, 11.5, true }; numConfiguredSensors++; }
  if (numConfiguredSensors < MAX_SENSORS) { configuredSensors[numConfiguredSensors] = { "egt", "EGT", TEMP_MAX6675, 5, 17, 16, {0}, true, 0.0, 0, 400.0, 650.0, 0.0, 0.0, true }; numConfiguredSensors++; }
  if (numConfiguredSensors < MAX_SENSORS) { configuredSensors[numConfiguredSensors] = { "cabin_temp", "Cabin Temp", TEMP_BMP280, -1, -1, -1, {0}, true, 0.0, 0, 35.0, 40.0, 5.0, 0.0, true }; numConfiguredSensors++; }
  Serial.println(String(numConfiguredSensors) + " default sensors loaded.");
}

void saveConfiguration() { /* ... Sensor config ... */
  Serial.printf("SAVE_CONF_INFO: SPIFFS Total: %d, Used: %d, Free: %d\n", SPIFFS.totalBytes(), SPIFFS.usedBytes(), SPIFFS.totalBytes() - SPIFFS.usedBytes());
  Serial.println("Saving sensor configuration to SPIFFS...");
  JSONVar sensorsArray;
  for (int i = 0; i < numConfiguredSensors; i++) {
    JSONVar sensorConf;
    sensorConf["id"] = configuredSensors[i].id;
    sensorConf["name"] = configuredSensors[i].name;
    sensorConf["sensorType"] = sensorTypeToString(configuredSensors[i].sensorType);
    sensorConf["pin1"] = configuredSensors[i].pin1;
    sensorConf["pin2"] = configuredSensors[i].pin2;
    sensorConf["pin3"] = configuredSensors[i].pin3;
    sensorConf["oneWireAddress"] = oneWireAddressToString(configuredSensors[i].oneWireAddress);
    sensorConf["enabled"] = configuredSensors[i].enabled;
    sensorConf["displayOnOLED"] = configuredSensors[i].displayOnOLED;
    sensorConf["warningThreshold"] = configuredSensors[i].warningThreshold;
    sensorConf["criticalThreshold"] = configuredSensors[i].criticalThreshold;
    sensorConf["lowerWarningThreshold"] = configuredSensors[i].lowerWarningThreshold;
    sensorConf["lowerCriticalThreshold"] = configuredSensors[i].lowerCriticalThreshold;
    sensorsArray[i] = sensorConf;
  }
  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("SAVE_CONF_ERROR: Failed to open config file for writing. Check SPIFFS status.");
    Serial.printf("SAVE_CONF_DEBUG: SPIFFS.exists(CONFIG_FILE) before open: %s\n", SPIFFS.exists(CONFIG_FILE) ? "true" : "false");
    return;
  }
  Serial.println("SAVE_CONF_INFO: Config file opened for writing successfully.");
  String jsonString;
  jsonString = JSON.stringify(sensorsArray);
  Serial.println("SAVE_CONF_INFO: Attempting to write JSON to config file...");
  Serial.print("SAVE_CONF_INFO: JSON String to write (length ");
  Serial.print(jsonString.length());
  Serial.print("): ");
  Serial.println(jsonString);
  size_t bytesWritten = configFile.print(jsonString);
  if (bytesWritten == jsonString.length()) {
    Serial.printf("SAVE_CONF_SUCCESS: Successfully wrote %d bytes to config file.\n", bytesWritten);
  } else {
    Serial.printf("SAVE_CONF_ERROR: Bytes written (%d) does not match JSON string length (%d). File write operation may have failed or been partial.\n", bytesWritten, jsonString.length());
  }
  configFile.close();
  Serial.println("SAVE_CONF_INFO: Config file closed.");
}

void loadConfiguration() { /* ... Sensor config loading, then GPIO config loading ... */
  Serial.println("Loading sensor configuration from SPIFFS...");
  if (SPIFFS.exists(CONFIG_FILE)) {
    File configFile = SPIFFS.open(CONFIG_FILE, "r");
    if (configFile) {
      String configData = configFile.readString();
      configFile.close();
      JSONVar parsedConfig = JSON.parse(configData);
      if (JSON.typeof(parsedConfig) == "array") {
        numConfiguredSensors = 0;
        for (int i = 0; i < parsedConfig.length(); i++) {
          if (numConfiguredSensors >= MAX_SENSORS) { Serial.println("Max sensors reached while loading config."); break; }
          JSONVar sensorJson = parsedConfig[i];
          configuredSensors[numConfiguredSensors].id = String((const char*) sensorJson["id"]);
          configuredSensors[numConfiguredSensors].name = String((const char*) sensorJson["name"]);
          configuredSensors[numConfiguredSensors].sensorType = stringToSensorType(String((const char*)sensorJson["sensorType"]));
          configuredSensors[numConfiguredSensors].pin1 = (int) sensorJson["pin1"];
          configuredSensors[numConfiguredSensors].pin2 = (int) sensorJson["pin2"];
          configuredSensors[numConfiguredSensors].pin3 = (int) sensorJson["pin3"];
          parseHexStringToByteArray(String((const char*)sensorJson["oneWireAddress"]), configuredSensors[numConfiguredSensors].oneWireAddress, 8);
          configuredSensors[numConfiguredSensors].enabled = (bool) sensorJson["enabled"];
          configuredSensors[numConfiguredSensors].displayOnOLED = (bool) sensorJson["displayOnOLED"];
          configuredSensors[numConfiguredSensors].warningThreshold = (double) sensorJson["warningThreshold"];
          configuredSensors[numConfiguredSensors].criticalThreshold = (double) sensorJson["criticalThreshold"];
          configuredSensors[numConfiguredSensors].lowerWarningThreshold = (double) sensorJson["lowerWarningThreshold"];
          configuredSensors[numConfiguredSensors].lowerCriticalThreshold = (double) sensorJson["lowerCriticalThreshold"];
          configuredSensors[numConfiguredSensors].value = 0.0;
          configuredSensors[numConfiguredSensors].consecutiveFailures = 0;
          numConfiguredSensors++;
        }
        Serial.println("Sensor configuration loaded successfully from SPIFFS.");
      } else { Serial.println("Error parsing sensor config file or not an array. Loading defaults."); loadDefaultConfiguration(); saveConfiguration(); }
    } else { Serial.println("Error opening sensor config file. Loading defaults."); loadDefaultConfiguration(); saveConfiguration(); }
  } else { Serial.println("Sensor config file not found. Loading and saving defaults."); loadDefaultConfiguration(); saveConfiguration(); }

  loadGPIOConfiguration(); // Load GPIO Configuration
}

// --- OneWire Discovery ---
void discoverOneWireDevices(bool forceScan = false, bool printToSerial = false) {
  if (!forceScan && millis() - lastOneWireDiscoveryTime < ONE_WIRE_DISCOVERY_INTERVAL) {
    return;
  }
  lastOneWireDiscoveryTime = millis();
  if(printToSerial) Serial.println("Scanning for OneWire devices on discovery pin...");

  discoveredOneWireAddressesHex.clear();
  uint8_t newAddr[8];
  oneWireDiscovery.reset_search(); // Use oneWireDiscovery
  delay(100);

  while (oneWireDiscovery.search(newAddr)) { // Use oneWireDiscovery
    if (OneWire::crc8(newAddr, 7) == newAddr[7]) { // CRC check is a static method of OneWire class
      String addrHex = oneWireAddressToString(newAddr);
      discoveredOneWireAddressesHex.push_back(addrHex);
      if (printToSerial) {
        Serial.print("  Found: " + addrHex);
        // To read temp for verification, use sensorsDiscovery
        sensorsDiscovery.requestTemperaturesByAddress(newAddr); // Request for specific address
        float tempC = sensorsDiscovery.getTempC(newAddr);
        if (tempC == DEVICE_DISCONNECTED_C) {
            Serial.print(" (Could not get temp)");
        } else {
            Serial.print(" Temp: "); Serial.print(tempC); Serial.print("C");
        }
        // Note: isParasitePowerMode is on the OneWire object
        if (oneWireDiscovery.isParasitePowerMode()) Serial.print(" (Parasite Power)");
        Serial.println();
      }
    } else {
      if (printToSerial) Serial.println("  Found device with CRC error on discovery pin.");
    }
  }
  if (printToSerial) Serial.println("OneWire discovery scan complete.");
}
std::vector<String> getUnconfiguredOneWireDevices() { /* ... same ... */
  std::vector<String> unconfiguredList;
  for (const String& discoveredAddr : discoveredOneWireAddressesHex) {
    bool isConfigured = false;
    for (int i = 0; i < numConfiguredSensors; ++i) {
      if (configuredSensors[i].sensorType == TEMP_DS18B20) {
        String configuredAddrHex = oneWireAddressToString(configuredSensors[i].oneWireAddress);
        if (discoveredAddr.equalsIgnoreCase(configuredAddrHex)) { isConfigured = true; break; }
      }
    }
    if (!isConfigured) { unconfiguredList.push_back(discoveredAddr); }
  }
  return unconfiguredList;
}


void initWifi() { /* ... same ... */
  Serial.println("Setting up WiFi Access Point...");
  WiFi.softAP(apSSID, apPassword);
  IPAddress AP_IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(AP_IP);
}
String getSensorReadingsJSON() { /* ... same ... */
  JSONVar currentReadings;
  for(int i=0; i < numConfiguredSensors; i++){
    if(configuredSensors[i].enabled){
        String key = configuredSensors[i].id;
        if(configuredSensors[i].id == "coolant_temp") key = "CT";
        else if(configuredSensors[i].id == "oil_pressure") key = "Oil";
        else if(configuredSensors[i].id == "bat_voltage") key = "BAT1";
        else if(configuredSensors[i].id == "egt") key = "EGT";
        else if(configuredSensors[i].id == "cabin_temp") key = "CTMP";
        currentReadings[key] = configuredSensors[i].value;
    }
  }
  return JSON.stringify(currentReadings);
}
void sendSensorEvents() { /* ... same ... */
  static unsigned long lastEventTime = 0;
  if (millis() - lastEventTime > 3000) {
    events.send(getSensorReadingsJSON().c_str(), "new_readings", millis());
    lastEventTime = millis();
  }
}
void initWebServer() { /* ... same ... */
  Serial.println("Initializing Web Server...");
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.on("/charts", HTTP_GET, [](AsyncWebServerRequest *request){ if(SPIFFS.exists("/charts.html")){ request->send(SPIFFS, "/charts.html", "text/html"); } else { request->send(404, "text/plain", "Chart page not found."); } });
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){ if(SPIFFS.exists("/settings.html")){ request->send(SPIFFS, "/settings.html", "text/html"); } else { request->send(404, "text/plain", "Settings page not found."); } });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/style.css", "text/css"); });
  server.on("/all.css", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/all.css", "text/css"); });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/script.js", "application/javascript"); });
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/settings.html", "text/html"); });
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/settings.js", "application/javascript"); });
  server.on("/gauge.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/gauge.js", "application/javascript"); });
  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(200, "application/json", getSensorReadingsJSON()); });
  server.addHandler(&events);
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request){ /* ... same ... */
    JSONVar sensorsArray;
    for(int i=0; i < numConfiguredSensors; i++){
        JSONVar sensorConf;
        sensorConf["id"] = configuredSensors[i].id;
        sensorConf["name"] = configuredSensors[i].name;
        sensorConf["sensorType"] = sensorTypeToString(configuredSensors[i].sensorType);
        sensorConf["pin1"] = configuredSensors[i].pin1;
        sensorConf["pin2"] = configuredSensors[i].pin2;
        sensorConf["pin3"] = configuredSensors[i].pin3;
        sensorConf["oneWireAddress"] = oneWireAddressToString(configuredSensors[i].oneWireAddress);
        sensorConf["enabled"] = configuredSensors[i].enabled;
        sensorConf["displayOnOLED"] = configuredSensors[i].displayOnOLED;
        sensorConf["warningThreshold"] = configuredSensors[i].warningThreshold;
        sensorConf["criticalThreshold"] = configuredSensors[i].criticalThreshold;
        sensorConf["lowerWarningThreshold"] = configuredSensors[i].lowerWarningThreshold;
        sensorConf["lowerCriticalThreshold"] = configuredSensors[i].lowerCriticalThreshold;
        sensorsArray[i] = sensorConf;
    }
    request->send(200, "application/json", JSON.stringify(sensorsArray));
  });
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (request->url().equals("/api/setconfig")) { /* ... sensor config ... */
        static String bodyContent;
        if (index == 0) { bodyContent = ""; Serial.println("/api/setconfig POST received"); }
        bodyContent.concat((char*)data, len);
        if (index + len == total) {
            Serial.println("Full body: " + bodyContent);
            JSONVar jsonData = JSON.parse(bodyContent); bodyContent = "";
            if (JSON.typeof(jsonData) == "undefined") { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid JSON\"}"); return; }
            String idToUpdate = String((const char*) jsonData["id"]);
            if (idToUpdate.length() == 0) { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Missing 'id' in JSON\"}"); return; }
            bool found = false;
            for (int i = 0; i < numConfiguredSensors; i++) {
                if (configuredSensors[i].id.equals(idToUpdate)) {
                    if(jsonData.hasOwnProperty("name")) configuredSensors[i].name = String((const char*)jsonData["name"]);
                    if(jsonData.hasOwnProperty("sensorType")) configuredSensors[i].sensorType = stringToSensorType(String((const char*)jsonData["sensorType"]));
                    if(jsonData.hasOwnProperty("pin1")) configuredSensors[i].pin1 = (int)jsonData["pin1"];
                    if(jsonData.hasOwnProperty("pin2")) configuredSensors[i].pin2 = (int)jsonData["pin2"];
                    if(jsonData.hasOwnProperty("pin3")) configuredSensors[i].pin3 = (int)jsonData["pin3"];
                    if(jsonData.hasOwnProperty("oneWireAddress")) parseHexStringToByteArray(String((const char*)jsonData["oneWireAddress"]), configuredSensors[i].oneWireAddress, 8);
                    if(jsonData.hasOwnProperty("enabled")) configuredSensors[i].enabled = (bool)jsonData["enabled"];
                    if(jsonData.hasOwnProperty("displayOnOLED")) configuredSensors[i].displayOnOLED = (bool)jsonData["displayOnOLED"];
                    if(jsonData.hasOwnProperty("warningThreshold")) configuredSensors[i].warningThreshold = (double)jsonData["warningThreshold"];
                    if(jsonData.hasOwnProperty("criticalThreshold")) configuredSensors[i].criticalThreshold = (double)jsonData["criticalThreshold"];
                    if(jsonData.hasOwnProperty("lowerWarningThreshold")) configuredSensors[i].lowerWarningThreshold = (double)jsonData["lowerWarningThreshold"];
                    if(jsonData.hasOwnProperty("lowerCriticalThreshold")) configuredSensors[i].lowerCriticalThreshold = (double)jsonData["lowerCriticalThreshold"];
                    found = true; break;
                }
            }
            if (!found && numConfiguredSensors < MAX_SENSORS) {
                configuredSensors[numConfiguredSensors].id = idToUpdate;
                configuredSensors[numConfiguredSensors].name = jsonData.hasOwnProperty("name") ? String((const char*)jsonData["name"]) : "New Sensor";
                configuredSensors[numConfiguredSensors].sensorType = jsonData.hasOwnProperty("sensorType") ? stringToSensorType(String((const char*)jsonData["sensorType"])) : UNDEFINED;
                configuredSensors[numConfiguredSensors].pin1 = jsonData.hasOwnProperty("pin1") ? (int)jsonData["pin1"] : -1;
                configuredSensors[numConfiguredSensors].pin2 = jsonData.hasOwnProperty("pin2") ? (int)jsonData["pin2"] : -1;
                configuredSensors[numConfiguredSensors].pin3 = jsonData.hasOwnProperty("pin3") ? (int)jsonData["pin3"] : -1;
                if(jsonData.hasOwnProperty("oneWireAddress")) parseHexStringToByteArray(String((const char*)jsonData["oneWireAddress"]), configuredSensors[numConfiguredSensors].oneWireAddress, 8);
                else memset(configuredSensors[numConfiguredSensors].oneWireAddress, 0, 8);
                configuredSensors[numConfiguredSensors].enabled = jsonData.hasOwnProperty("enabled") ? (bool)jsonData["enabled"] : false;
                configuredSensors[numConfiguredSensors].displayOnOLED = jsonData.hasOwnProperty("displayOnOLED") ? (bool)jsonData["displayOnOLED"] : true;
                configuredSensors[numConfiguredSensors].warningThreshold = jsonData.hasOwnProperty("warningThreshold") ? (double)jsonData["warningThreshold"] : 0.0;
                configuredSensors[numConfiguredSensors].criticalThreshold = jsonData.hasOwnProperty("criticalThreshold") ? (double)jsonData["criticalThreshold"] : 0.0;
                configuredSensors[numConfiguredSensors].lowerWarningThreshold = jsonData.hasOwnProperty("lowerWarningThreshold") ? (double)jsonData["lowerWarningThreshold"] : 0.0;
                configuredSensors[numConfiguredSensors].lowerCriticalThreshold = jsonData.hasOwnProperty("lowerCriticalThreshold") ? (double)jsonData["lowerCriticalThreshold"] : 0.0;
                configuredSensors[numConfiguredSensors].value = 0.0; configuredSensors[numConfiguredSensors].consecutiveFailures = 0;
                numConfiguredSensors++; found = true;
            }
            if (found) { saveConfiguration(); request->send(200, "application/json", "{\"status\":\"success\", \"message\":\"Configuration updated and saved\"}"); }
            else if (numConfiguredSensors >= MAX_SENSORS) { request->send(500, "application/json", "{\"status\":\"error\", \"message\":\"Max sensors reached, cannot add new one.\"}"); }
            else { request->send(404, "application/json", "{\"status\":\"error\", \"message\":\"Sensor ID not found and could not add.\"}"); }
        }
    }
    else if (request->url().equals("/api/gpioconfig") && request->method() == HTTP_POST) { /* ... GPIO config POST ... */
        static String bodyContent_gpio;
        if (index == 0) { bodyContent_gpio = ""; Serial.println("/api/gpioconfig POST received (onRequestBody)"); }
        bodyContent_gpio.concat((char*)data, len);
        if (index + len == total) {
            Serial.println("Full GPIO body: " + bodyContent_gpio);
            JSONVar gpioJsonArray = JSON.parse(bodyContent_gpio);
            bodyContent_gpio = "";
            if (JSON.typeof(gpioJsonArray) == "array") {
                numConfiguredGPIOPins = 0;
                for (int i = 0; i < gpioJsonArray.length(); i++) {
                    if (numConfiguredGPIOPins >= MAX_GPIO_PINS) { Serial.println("MAX_GPIO_PINS reached."); break; }
                    JSONVar pinJson = gpioJsonArray[i];
                    GPIOPinConfig tempConf;
                    tempConf.id = String((const char*) pinJson["id"]);
                    tempConf.name = String((const char*) pinJson["name"]);
                    tempConf.pinNumber = (uint8_t)(int)pinJson["pinNumber"];
                    tempConf.mode = stringToPinModeType(String((const char*)pinJson["mode"]));
                    tempConf.defaultState = (bool)pinJson["defaultState"];
                    tempConf.defaultBlinkDelayMs = (unsigned long)(int)pinJson["defaultBlinkDelayMs"];
                    initializeGPIOPin(tempConf);
                    configuredGPIOPins[numConfiguredGPIOPins++] = tempConf;
                }
                saveGPIOConfiguration();
                request->send(200, "application/json", "{\"status\":\"success\", \"message\":\"GPIO configurations updated and saved via onRequestBody\"}");
            } else {
                request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid JSON array for GPIO config\"}");
            }
        }
    }
    else if (request->url().equals("/api/gpio/control")) { /* ... GPIO control POST ... */
        static String bodyContent_gpio_control;
        if (index == 0) { bodyContent_gpio_control = ""; Serial.println("/api/gpio/control POST received"); }
        bodyContent_gpio_control.concat((char*)data, len);
        if (index + len == total) {
            Serial.println("Full body for /api/gpio/control: " + bodyContent_gpio_control);
            JSONVar jsonData = JSON.parse(bodyContent_gpio_control); bodyContent_gpio_control = "";
            if (JSON.typeof(jsonData) == "undefined") { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid JSON\"}"); return; }
            String id = String((const char*)jsonData["id"]); String command = String((const char*)jsonData["command"]);
            if (id.length() == 0 || command.length() == 0) { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Missing 'id' or 'command' in JSON\"}"); return; }
            GPIOPinConfig* pinToControl = findGPIOPinById(id);
            if (!pinToControl) { request->send(404, "application/json", "{\"status\":\"error\", \"message\":\"GPIO ID not found\"}"); return; }
            if (command.equalsIgnoreCase("SET_STATE")) {
                if (!jsonData.hasOwnProperty("state")) { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Missing 'state' for SET_STATE command\"}"); return; }
                pinToControl->currentState = (bool)jsonData["state"];
                Serial.printf("GPIO %s: SET_STATE to %s\n", id.c_str(), pinToControl->currentState ? "true" : "false");
                request->send(200, "application/json", "{\"status\":\"success\", \"id\":\"" + id + "\", \"new_control_state\": " + (pinToControl->currentState ? "true":"false") + "}");
            } else if (command.equalsIgnoreCase("SET_BLINK_DELAY")) {
                if (pinToControl->mode != GPIO_MODE_BLINK) { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Pin is not in BLINK mode\"}"); return; }
                if (!jsonData.hasOwnProperty("delay_ms")) { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Missing 'delay_ms' for SET_BLINK_DELAY command\"}"); return; }
                pinToControl->currentBlinkDelayMs = (unsigned long)(int)jsonData["delay_ms"];
                Serial.printf("GPIO %s: SET_BLINK_DELAY to %lu ms\n", id.c_str(), pinToControl->currentBlinkDelayMs);
                request->send(200, "application/json", "{\"status\":\"success\", \"id\":\"" + id + "\", \"new_blink_delay_ms\": " + String(pinToControl->currentBlinkDelayMs) + "}");
            } else { request->send(400, "application/json", "{\"status\":\"error\", \"message\":\"Unknown GPIO command\"}"); }
        }
    }
  });
  server.on("/api/saveconfig", HTTP_POST, [](AsyncWebServerRequest *request){ /* ... same ... */
    saveConfiguration();
    JSONVar response; response["status"] = "success"; response["message"] = "Configuration saved to ESP32 Flash.";
    request->send(200, "application/json", JSON.stringify(response));
  });
  server.on("/api/loaddefaults", HTTP_POST, [](AsyncWebServerRequest *request){ /* ... same ... */
    loadDefaultConfiguration();
    saveConfiguration();
    loadDefaultGPIOConfiguration();
    saveGPIOConfiguration();
    JSONVar response; response["status"] = "success"; response["message"] = "Defaults loaded and saved to ESP32 Flash.";
    request->send(200, "application/json", JSON.stringify(response));
  });
  server.on("/api/onewire/unconfigured", HTTP_GET, [](AsyncWebServerRequest *request){ /* ... same ... */
    discoverOneWireDevices(true, false);
    std::vector<String> unconfiguredList = getUnconfiguredOneWireDevices();
    JSONVar jsonArray;
    for(size_t i=0; i < unconfiguredList.size(); i++){ jsonArray[i] = unconfiguredList[i]; }
    request->send(200, "application/json", JSON.stringify(jsonArray));
  });
  server.on("/api/gpioconfig", HTTP_GET, [](AsyncWebServerRequest *request){ /* ... same ... */
    JSONVar gpioArray;
    for(int i=0; i < numConfiguredGPIOPins; i++){
        JSONVar gpioConf;
        gpioConf["id"] = configuredGPIOPins[i].id;
        gpioConf["name"] = configuredGPIOPins[i].name;
        gpioConf["pinNumber"] = configuredGPIOPins[i].pinNumber;
        gpioConf["mode"] = pinModeTypeToString(configuredGPIOPins[i].mode);
        gpioConf["defaultState"] = configuredGPIOPins[i].defaultState;
        gpioConf["defaultBlinkDelayMs"] = configuredGPIOPins[i].defaultBlinkDelayMs;
        gpioArray[i] = gpioConf;
    }
    request->send(200, "application/json", JSON.stringify(gpioArray));
  });
  server.on("/api/gpio/status", HTTP_GET, [](AsyncWebServerRequest *request){ /* ... same ... */
    JSONVar gpioStatusArray;
    for(int i=0; i < numConfiguredGPIOPins; i++){
        JSONVar gpioState;
        gpioState["id"] = configuredGPIOPins[i].id;
        gpioState["pin_number"] = configuredGPIOPins[i].pinNumber;
        gpioState["current_control_state"] = configuredGPIOPins[i].currentState;
        if (configuredGPIOPins[i].pinNumber < GPIO_NUM_MAX) {
             pinMode(configuredGPIOPins[i].pinNumber, OUTPUT);
        }
        gpioState["actual_pin_level"] = digitalRead(configuredGPIOPins[i].pinNumber);
        gpioState["mode"] = pinModeTypeToString(configuredGPIOPins[i].mode);
        gpioState["blink_delay_ms"] = configuredGPIOPins[i].currentBlinkDelayMs;
        gpioStatusArray[gpioStatusArray.size()] = gpioState;
    }
    request->send(200, "application/json", JSON.stringify(gpioStatusArray));
  });

  server.begin();
  Serial.println("Web Server started.");
}
void initOLED() { /* ... same ... */
  Serial.println("Initializing OLED...");
  if (u8g2.begin()) {
    oledAvailable = true;
    Serial.println("OLED Initialized Successfully.");
    u8g2.clearBuffer(); u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 10, "Hello!"); u8g2.drawStr(0, 25, "Engine Monitor");
    u8g2.sendBuffer();
  } else {
    oledAvailable = false;
    Serial.println("OLED Initialization Failed.");
  }
}

void readAllSensors() { /* ... same ... */
  if (numConfiguredSensors > 0) {
    bool ds18b20Present = false;
    for(int i=0; i < numConfiguredSensors; i++){
      if(configuredSensors[i].enabled && configuredSensors[i].sensorType == TEMP_DS18B20){
        ds18b20Present = true; break;
      }
    }
    if (ds18b20Present) { sensorsMain.requestTemperatures(); } // Changed to sensorsMain
  }
  static unsigned long lastReadTime = 0;
  if (millis() - lastReadTime > 250) {
    for(int i=0; i < numConfiguredSensors; i++){
      if(configuredSensors[i].enabled){
        switch(configuredSensors[i].sensorType){
          case TEMP_DS18B20: readDS18B20Sensor(configuredSensors[i]); break;
          case PRESSURE_ANALOG: readAnalogPressureSensor(configuredSensors[i]); break;
          case VOLTAGE_ANALOG: readAnalogVoltageSensor(configuredSensors[i]); break;
          case TEMP_MAX6675: readMAX6675Sensor(configuredSensors[i]); break;
          case TEMP_BMP085: case TEMP_BMP280: readBMP280Sensor(configuredSensors[i]); break;
          default: break;
        }
      }
    }
    lastReadTime = millis();
  }
}

void updateOLED() { /* ... same ... */
  if (!oledAvailable) return;
  static unsigned long lastOLEDRefresh = 0;
  if (millis() - lastOLEDRefresh < 500 && lastOLEDRefresh != 0) { return; }
  lastOLEDRefresh = millis();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.firstPage();
  do {
    SensorConfig* cabinTempSensor = findSensorById("cabin_temp");
    SensorConfig* bat1Sensor = findSensorById("bat_voltage");
    u8g2.setCursor(0, 8);
    if (cabinTempSensor && cabinTempSensor->enabled && cabinTempSensor->displayOnOLED) { displayTemperatureOLED(u8g2, cabinTempSensor->value, cabinTempSensor->value <= -998.0); } else { u8g2.print("N/A"); }
    u8g2.print("c");
    u8g2.setCursor(30, 8);
    if (bmpAvailable) { float altitude = bmp.readAltitude(1013.25); u8g2.print(altitude, 0); u8g2.print("m"); }
    else { u8g2.print("Alt:N/A"); }
    u8g2.setCursor(75, 8);
    if (bat1Sensor && bat1Sensor->enabled && bat1Sensor->displayOnOLED) { u8g2.print(bat1Sensor->value, 1); } else { u8g2.print("N/A"); }
    u8g2.print("v");
    SensorConfig* imtSensor = findSensorById("intake_temp");
    SensorConfig* ctSensorRow2 = findSensorById("coolant_temp");
    SensorConfig* oilSensorRow2 = findSensorById("oil_pressure");
    u8g2.setCursor(0, 18); u8g2.print("I:");
    if (imtSensor && imtSensor->enabled && imtSensor->displayOnOLED) displayTemperatureOLED(u8g2, imtSensor->value, imtSensor->value <= -998.0); else u8g2.print("NA");
    u8g2.setCursor(33, 18); u8g2.print("C:");
    if (ctSensorRow2 && ctSensorRow2->enabled && ctSensorRow2->displayOnOLED) displayTemperatureOLED(u8g2, ctSensorRow2->value, ctSensorRow2->value <= -998.0); else u8g2.print("NA");
    u8g2.setCursor(66, 18); u8g2.print("Oil:");
    if (oilSensorRow2 && oilSensorRow2->enabled && oilSensorRow2->displayOnOLED && oilSensorRow2->value > -998.0) { u8g2.print(oilSensorRow2->value, 0); }
    else { u8g2.print("NA"); }
    int gaugeBarX = 25; int gaugeBarWidth = 75; int valuePrintX = 102; int labelX = 0; float indicatorXPos;
    SensorConfig* egtSensor = findSensorById("egt");
    SensorConfig* boostSensor = findSensorById("boost_pressure");
    SensorConfig* ctGaugeSensor = findSensorById("coolant_temp");
    int egt_y_label = 30; int egt_y_bar = egt_y_label - 8;
    u8g2.setCursor(labelX, egt_y_label); u8g2.print(F("EGT"));
    u8g2.drawFrame(gaugeBarX, egt_y_bar, gaugeBarWidth, 8);
    if (egtSensor && egtSensor->enabled && egtSensor->displayOnOLED && egtSensor->value > -998.0) {
      indicatorXPos = ((egtSensor->value - minEGT) / (maxEGT - minEGT)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth -2) indicatorXPos = gaugeBarWidth - 2;
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, egt_y_bar + 1, 2, 6);
      u8g2.setCursor(valuePrintX, egt_y_label); u8g2.print((int)egtSensor->value);
    } else { u8g2.setCursor(valuePrintX, egt_y_label); u8g2.print("N/A");}
    int bst_y_label = 41; int bst_y_bar = bst_y_label - 8;
    u8g2.setCursor(labelX, bst_y_label); u8g2.print(F("BST"));
    u8g2.drawFrame(gaugeBarX, bst_y_bar, gaugeBarWidth, 8);
    if (boostSensor && boostSensor->enabled && boostSensor->displayOnOLED && boostSensor->value > -998.0) {
      indicatorXPos = ((boostSensor->value - minBST) / (maxBST - minBST)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth -2) indicatorXPos = gaugeBarWidth -2;
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, bst_y_bar + 1, 2, 6);
      u8g2.setCursor(valuePrintX, bst_y_label); u8g2.print(boostSensor->value,0);
    } else { u8g2.setCursor(valuePrintX, bst_y_label); u8g2.print("N/A");}
    int ct_y_label = 52; int ct_y_bar = ct_y_label - 8;
    u8g2.setCursor(labelX, ct_y_label); u8g2.print(F("CT "));
    u8g2.drawFrame(gaugeBarX, ct_y_bar, gaugeBarWidth, 8);
    if (ctGaugeSensor && ctGaugeSensor->enabled && ctGaugeSensor->displayOnOLED && ctGaugeSensor->value > -998.0) {
      indicatorXPos = ((ctGaugeSensor->value - minCT) / (maxCT - minCT)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth-2) indicatorXPos = gaugeBarWidth-2;
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, ct_y_bar + 1, 2, 6);
      u8g2.setCursor(valuePrintX, ct_y_label); displayTemperatureOLED(u8g2, ctGaugeSensor->value);
    } else { u8g2.setCursor(valuePrintX, ct_y_label); u8g2.print("N/A");}
    SensorConfig* tctSensor = findSensorById("transfer_case_temp");
    SensorConfig* gbtSensor = findSensorById("gearbox_temp");
    u8g2.setCursor(labelX, 63); u8g2.print("T:");
    if (tctSensor && tctSensor->enabled && tctSensor->displayOnOLED) displayTemperatureOLED(u8g2, tctSensor->value, tctSensor->value <= -998.0); else u8g2.print("NA");
    u8g2.print("/");
    if (gbtSensor && gbtSensor->enabled && gbtSensor->displayOnOLED) displayTemperatureOLED(u8g2, gbtSensor->value, gbtSensor->value <= -998.0); else u8g2.print("NA");
  } while (u8g2.nextPage());
}

// --- Main Setup & Loop ---
void setup() {
  Serial.begin(115200);
  Serial.println("Booting Engine Monitor ESP32 (Integrated)...");

  initSPIFFS();
  loadConfiguration();
  loadGPIOConfiguration();

  sensorsMain.begin(); // Changed from sensors.begin()
  sensorsDiscovery.begin(); // Initialize discovery bus
  Serial.println("DS18B20 sensors driver initialized (Main & Discovery).");

  Serial.println("Initializing BMP280 sensor...");
  if (bmp.begin(BMP280_ADDRESS_ALT)) {
    bmpAvailable = true;
    Serial.println("BMP280 sensor found at 0x76.");
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
  } else if (bmp.begin()) {
    bmpAvailable = true;
    Serial.println("BMP280 sensor found at 0x77.");
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
  } else {
    bmpAvailable = false;
    Serial.println("Could not find a valid BMP280 sensor, check wiring or I2C address!");
  }

  initWifi();
  initWebServer();
  initOLED();

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLASHER_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(FLASHER_LED_PIN, LOW);

  discoverOneWireDevices(true, true);

  Serial.println("Setup Complete. Entering loop.");
}

void loop() {
  readAllSensors();
  checkAllSensorAlerts();
  handleGPIOOutputs();
  if (oledAvailable) { updateOLED(); }
  sendSensorEvents();
  discoverOneWireDevices();
}

[end of EngineMonitorESP32_Integrated.ino]
