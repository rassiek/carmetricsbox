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


// --- Global Definitions & Variables ---

// DS18B20 Setup
const int ONEWIRE_PIN = 4;
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature sensors(&oneWire);

// Analog Pressure Sensor Constants
const float PRESSURE_ZERO_ADC = 410.0;
const float PRESSURE_MAX_ADC = 3686.0;
const float PRESSURE_TRANSDUCER_MAX_PSI = 100.0;

// Analog Voltage Sensor Constants
const float VOLTAGE_DIVIDER_R1 = 30000.0; // Example R1 for voltage divider
const float VOLTAGE_DIVIDER_R2 = 7500.0;  // Example R2 for voltage divider
const float REFERENCE_VOLTAGE_ESP32 = 3.3; // ESP32 ADC reference voltage

// BMP280
Adafruit_BMP280 bmp; // I2C
bool bmpAvailable = false;


// Sensor Configuration
#define MAX_SENSORS 15

enum SensorType {
  UNDEFINED,         // 0
  PRESSURE_ANALOG,   // 1
  TEMP_DS18B20,      // 2
  TEMP_MAX6675,      // 3
  TEMP_BMP085,       // 4 (Legacy, use TEMP_BMP280)
  VOLTAGE_ANALOG,    // 5
  TEMP_BMP280        // 6
};

struct SensorConfig {
  String id; String name; SensorType sensorType; int pin1; int pin2; int pin3;
  uint8_t oneWireAddress[8]; bool enabled; float value;
  float warningThreshold; float criticalThreshold;
  float lowerWarningThreshold; float lowerCriticalThreshold; bool displayOnOLED;
};

SensorConfig configuredSensors[MAX_SENSORS];
int numConfiguredSensors = 0;

// Hardware Pins
int oled_sda_pin = 21; int oled_scl_pin = 22;
// ONEWIRE_PIN is used directly

// OLED Display
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, oled_scl_pin, oled_sda_pin);
bool oledAvailable = false;

// Web Server
AsyncWebServer server(80);
AsyncEventSource events("/events");
const char* apSSID = "EngineMonitorAP";
const char* apPassword = "password123";

// Alert Pins
const int BUZZER_PIN = 18;      // Example GPIO for Buzzer
const int FLASHER_LED_PIN = 19; // Example GPIO for Flasher LED

// --- OLED Pixel Calculation Variables (ported from ArduinoAllProbesV2.ino) ---
// Common
int numberOfPixels = 75; // Typically for the main bar of the gauge (was for 100px bar, might need scaling for 70px bar)

// Oil Pressure Gauge (Example: typically 0-100 psi, but Arduino example used 0-75 for pixel mapping)
float maxOP = 75.0;
float minOP = 0.0;
// Original Arduino thresholds for display markers (not alert thresholds)
// int upperThreshOP_disp = 70;
// int lowerThreshOP_disp = 15;
// float multiplierOP = (maxOP - minOP) != 0 ? pow((maxOP - minOP), -1) * numberOfPixels : 0;
// float baselineOP = minOP * multiplierOP - 25; // Original baseline for 25px offset start

// Coolant Temperature Gauge (Example: 0-120 C)
float maxCT = 127.5;
float minCT = 15.0;
// int upperThreshCT_disp = 95;
// int lowerThreshCT_disp = 40;
// float multiplierCT = (maxCT - minCT) != 0 ? pow((maxCT - minCT), -1) * numberOfPixels : 0;
// float baselineCT = minCT * multiplierCT - 25;

// EGT Gauge (Example: 0-800 C)
float maxEGT = 800.0;
float minEGT = 30.0;
// int upperThreshEGT_disp = 650;
// int lowerThreshEGT_disp = 173;
// float multiplierEGT = (maxEGT - minEGT) != 0 ? pow((maxEGT - minEGT), -1) * numberOfPixels : 0;
// float baselineEGT = minEGT * multiplierEGT - 25;

// Boost Gauge (Example: 0-20 psi)
float maxBST = 20.0;
float minBST = 0.0;
// int upperThreshBST_disp = 18;
// int lowerThreshBST_disp = 5;
// float multiplierBST = (maxBST - minBST) != 0 ? pow((maxBST - minBST), -1) * numberOfPixels : 0;
// float baselineBST = minBST * multiplierBST - 25;


// --- OLED Display Helper Functions ---
void printShortDateTimeOLED(U8G2 &u8g2_display) {
    // RTC not ported yet, display placeholder
    u8g2_display.print("Time N/A");
}

void displayTemperatureOLED(U8G2 &u8g2_display, float tempC, bool error = false) {
  if (error || tempC <= -998.0) { // Check for error codes
    u8g2_display.print("N/A");
  } else {
    u8g2_display.print(tempC, 0); // Display temp with 0 decimal places
  }
}

// Function to find a sensor by its ID
SensorConfig* findSensorById(const String& id) {
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].id.equals(id)) {
      return &configuredSensors[i];
    }
  }
  return nullptr; // Not found
}

// --- Alert Functions ---
void flash() {
  digitalWrite(FLASHER_LED_PIN, HIGH);
  delay(20); // Short flash
  digitalWrite(FLASHER_LED_PIN, LOW);
  delay(30); // Off period to make it a noticeable flash rather than just dimming
}

void buzz() {
  // Simple buzz - for a more distinct tone/duration, would need PWM or more complex logic
  digitalWrite(BUZZER_PIN, HIGH);
  delay(50); // Buzz duration
  digitalWrite(BUZZER_PIN, LOW);
}

void checkAllSensorAlerts() {
  static unsigned long lastAlertCheckTime = 0;
  if (millis() - lastAlertCheckTime < 500) { // Check alerts roughly every 500ms
    return;
  }
  lastAlertCheckTime = millis();

  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].enabled && configuredSensors[i].value > -998.0) { // Check if sensor is valid
      SensorConfig s = configuredSensors[i]; // Local copy for easier access

      // Check Upper Thresholds
      if (s.criticalThreshold > 0 && s.value >= s.criticalThreshold) {
        Serial.printf("CRITICAL ALERT: %s value %.2f >= critical threshold %.2f\n", s.name.c_str(), s.value, s.criticalThreshold);
        buzz();
      } else if (s.warningThreshold > 0 && s.value >= s.warningThreshold) {
        Serial.printf("Warning Alert: %s value %.2f >= warning threshold %.2f\n", s.name.c_str(), s.value, s.warningThreshold);
        flash();
      }

      // Check Lower Thresholds (only if they are set to something other than 0, or a more specific "is_set" flag)
      // For some sensors (like oil pressure), lower thresholds are more critical.
      if (s.lowerCriticalThreshold > 0 || (s.sensorType == PRESSURE_ANALOG && s.lowerCriticalThreshold != 0)) { // Allow 0 as valid for some pressure sensors if needed
          if (s.value <= s.lowerCriticalThreshold) {
            Serial.printf("CRITICAL ALERT: %s value %.2f <= lower critical threshold %.2f\n", s.name.c_str(), s.value, s.lowerCriticalThreshold);
            buzz();
          }
      } else if (s.lowerWarningThreshold > 0 || (s.sensorType == PRESSURE_ANALOG && s.lowerWarningThreshold != 0) ) {
           if (s.value <= s.lowerWarningThreshold) {
            Serial.printf("Warning Alert: %s value %.2f <= lower warning threshold %.2f\n", s.name.c_str(), s.value, s.lowerWarningThreshold);
            flash();
           }
      }
    }
  }
}

// --- Sensor Reading Functions ---
void readDS18B20Sensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_DS18B20) {
    float tempC = sensors.getTempC(sensor.oneWireAddress);
    if (tempC == DEVICE_DISCONNECTED_C || tempC == 85.0 || tempC == -127.0) {
      sensor.value = -999.0;
    } else {
      sensor.value = tempC;
    }
  }
}

void readAnalogPressureSensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == PRESSURE_ANALOG) {
    int rawValue = analogRead(sensor.pin1);
    sensor.value = ((float)rawValue - PRESSURE_ZERO_ADC) * PRESSURE_TRANSDUCER_MAX_PSI / (PRESSURE_MAX_ADC - PRESSURE_ZERO_ADC);
  }
}

void readMAX6675Sensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_MAX6675) {
    MAX6675 thermocouple(sensor.pin1, sensor.pin2, sensor.pin3); // pin1=SCLK, pin2=CS, pin3=SO/DO
    float temp = thermocouple.readCelsius();
    if (isnan(temp)) {
      sensor.value = -999.0;
    } else {
      sensor.value = temp - 8.0; // Apply -8C offset from original Arduino code
    }
  }
}

void readBMP280Sensor(SensorConfig &sensor) {
  if (sensor.enabled && (sensor.sensorType == TEMP_BMP280 || sensor.sensorType == TEMP_BMP085)) { // Handle both for transition
    if (bmpAvailable) {
      float temp = bmp.readTemperature();
      if (isnan(temp)) {
        sensor.value = -999.0;
      } else {
        sensor.value = temp;
      }
    } else {
      sensor.value = -998.0; // BMP sensor not available
    }
  }
}

void readAnalogVoltageSensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == VOLTAGE_ANALOG) {
    int rawValue = analogRead(sensor.pin1);
    float adc_voltage = (rawValue * REFERENCE_VOLTAGE_ESP32) / 4095.0;
    sensor.value = adc_voltage / (VOLTAGE_DIVIDER_R2 / (VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2));
  }
}

// --- Stub Functions (some are now real) ---
void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An error has occurred while mounting SPIFFS");
    return;
  }
  Serial.println("SPIFFS mounted successfully");
}

void loadDefaultConfiguration() {
  Serial.println("Loading default sensor configurations...");
  numConfiguredSensors = 0;

  // Sensor 1: Coolant Temperature (DS18B20)
  if (numConfiguredSensors < MAX_SENSORS) {
    configuredSensors[numConfiguredSensors] = {
      "coolant_temp", "Coolant", TEMP_DS18B20, ONEWIRE_PIN, -1, -1,
      {0x28, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01},
      true, 0.0, 95.0, 105.0, 60.0, 50.0, true
    };
    numConfiguredSensors++;
  }
  // Sensor 2: Oil Pressure (Analog)
  if (numConfiguredSensors < MAX_SENSORS) {
    configuredSensors[numConfiguredSensors] = {
      "oil_pressure", "Oil PSI", PRESSURE_ANALOG, 34, -1, -1, {0},
      true, 0.0, 20.0, 80.0, 10.0, 5.0, true
    };
    numConfiguredSensors++;
  }
  // Sensor 3: Battery Voltage (Analog)
  if (numConfiguredSensors < MAX_SENSORS) {
    configuredSensors[numConfiguredSensors] = {
      "bat_voltage", "Battery V", VOLTAGE_ANALOG, 35, -1, -1, {0},
      true, 0.0, 14.8, 15.2, 11.8, 11.5, true
    };
    numConfiguredSensors++;
  }
  // Sensor 4: EGT (MAX6675) - SCLK=GPIO5, CS=GPIO17, SO=GPIO16 (example pins)
  if (numConfiguredSensors < MAX_SENSORS) {
    configuredSensors[numConfiguredSensors] = {
      "egt", "EGT", TEMP_MAX6675, 5, 17, 16, {0},
      true, 0.0, 400.0, 650.0, 0.0, 0.0, true
    };
    numConfiguredSensors++;
  }
  // Sensor 5: Cabin Temperature (BMP280)
  if (numConfiguredSensors < MAX_SENSORS) {
    configuredSensors[numConfiguredSensors] = {
      "cabin_temp", "Cabin Temp", TEMP_BMP280, -1, -1, -1, {0},
      true, 0.0, 35.0, 40.0, 5.0, 0.0, true
    };
    numConfiguredSensors++;
  }
  Serial.println(String(numConfiguredSensors) + " default sensors loaded.");
}

void loadConfiguration() {
  Serial.println("Attempting to load configuration from SPIFFS (stubbed)...");
  loadDefaultConfiguration();
}
void saveConfiguration() {
  Serial.println("Attempting to save configuration to SPIFFS (stubbed)...");
  Serial.println("Configuration save requested (stub).");
}
void initWifi() {
  Serial.println("Setting up WiFi Access Point...");
  WiFi.softAP(apSSID, apPassword);
  IPAddress AP_IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(AP_IP);
}
String getSensorReadingsJSON() {
  JSONVar currentReadings;
  for(int i=0; i < numConfiguredSensors; i++){
    if(configuredSensors[i].enabled){
        String key = configuredSensors[i].id;
        // Short keys for some common sensors for existing web UI compatibility
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
void sendSensorEvents() {
  static unsigned long lastEventTime = 0;
  if (millis() - lastEventTime > 3000) {
    events.send(getSensorReadingsJSON().c_str(), "new_readings", millis());
    lastEventTime = millis();
  }
}
void initWebServer() {
  Serial.println("Initializing Web Server...");
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/style.css", "text/css"); });
  server.on("/all.css", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/all.css", "text/css"); });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/script.js", "application/javascript"); });
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/settings.html", "text/html"); });
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/settings.js", "application/javascript"); });
  server.on("/gauge.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/gauge.js", "application/javascript"); });
  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(200, "application/json", getSensorReadingsJSON()); });
  server.addHandler(&events);
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request){
    JSONVar sensorsArray;
    for(int i=0; i < numConfiguredSensors; i++){
        JSONVar sensorConf;
        sensorConf["id"] = configuredSensors[i].id;
        sensorConf["name"] = configuredSensors[i].name;
        sensorConf["sensorType"] = (int)configuredSensors[i].sensorType;
        sensorConf["pin1"] = configuredSensors[i].pin1;
        sensorConf["pin2"] = configuredSensors[i].pin2;
        sensorConf["pin3"] = configuredSensors[i].pin3;
        String addrStr = "";
        for(int j=0; j<8; j++) { if(configuredSensors[i].oneWireAddress[j] < 0x10) addrStr += "0"; addrStr += String(configuredSensors[i].oneWireAddress[j], HEX); }
        sensorConf["oneWireAddress"] = addrStr.toUpperCase();
        sensorConf["enabled"] = configuredSensors[i].enabled;
        sensorConf["warningThreshold"] = configuredSensors[i].warningThreshold;
        sensorConf["criticalThreshold"] = configuredSensors[i].criticalThreshold;
        sensorConf["lowerWarningThreshold"] = configuredSensors[i].lowerWarningThreshold;
        sensorConf["lowerCriticalThreshold"] = configuredSensors[i].lowerCriticalThreshold;
        sensorConf["displayOnOLED"] = configuredSensors[i].displayOnOLED;
        sensorsArray[i] = sensorConf;
    }
    request->send(200, "application/json", JSON.stringify(sensorsArray));
  });
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (request->url().equals("/api/setconfig")) {
        if (index == 0) { Serial.println("/api/setconfig POST received"); }
        if (index + len == total) {
            JSONVar response; response["status"] = "success_stub"; response["message"] = "SET_SENSOR_CONF received (stub)";
            request->send(200, "application/json", JSON.stringify(response));
        }
    }
  });
  server.on("/api/saveconfig", HTTP_POST, [](AsyncWebServerRequest *request){
    saveConfiguration();
    JSONVar response; response["status"] = "success_stub"; response["message"] = "SAVE_CONF command received (stub)";
    request->send(200, "application/json", JSON.stringify(response));
  });
  server.on("/api/loaddefaults", HTTP_POST, [](AsyncWebServerRequest *request){
    loadDefaultConfiguration();
    JSONVar response; response["status"] = "success"; response["message"] = "Defaults loaded into active config. Save to persist.";
    response["newConfig"] = JSON.parse(getSensorReadingsJSON());
    request->send(200, "application/json", JSON.stringify(response));
  });
  server.begin();
  Serial.println("Web Server started.");
}
void initOLED() {
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

void readAllSensors() {
  if (numConfiguredSensors > 0) {
    bool ds18b20Present = false;
    for(int i=0; i < numConfiguredSensors; i++){
      if(configuredSensors[i].enabled && configuredSensors[i].sensorType == TEMP_DS18B20){
        ds18b20Present = true; break;
      }
    }
    if (ds18b20Present) { sensors.requestTemperatures(); }
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
          case TEMP_BMP085: // Falls through to BMP280 if BMP085 specific logic not needed
          case TEMP_BMP280: readBMP280Sensor(configuredSensors[i]); break;
          default: break;
        }
      }
    }
    lastReadTime = millis();
  }
}

void updateOLED() {
  if (!oledAvailable) return;

  static unsigned long lastOLEDRefresh = 0;
  // Refresh OLED approx every 250ms to match sensor reading, or 1s if too flickery.
  // Let's try 500ms for a balance.
  if (millis() - lastOLEDRefresh < 500 && lastOLEDRefresh != 0) {
    return;
  }
  lastOLEDRefresh = millis();

  u8g2.setFont(u8g2_font_ncenB08_tr); // Default font for this display pass

  u8g2.firstPage();
  do {
    // Row 1: Cabin Temp, Altitude, Battery 1
    SensorConfig* cabinTempSensor = findSensorById("cabin_temp");
    SensorConfig* bat1Sensor = findSensorById("bat_voltage");

    u8g2.setCursor(0, 8);
    if (cabinTempSensor && cabinTempSensor->enabled && cabinTempSensor->displayOnOLED) {
      displayTemperatureOLED(u8g2, cabinTempSensor->value, cabinTempSensor->value <= -998.0);
    } else { u8g2.print("N/A"); }
    u8g2.print("c");

    u8g2.setCursor(30, 8);
    // Altitude: Read from BMP280 if available and a sensor config for it exists.
    // For now, assuming no separate "altitude" sensor ID.
    // If bmpAvailable, could read bmp.readAltitude(SEALEVELPRESSURE_HPA) here.
    // SEALEVELPRESSURE_HPA would need to be defined or configurable.
    if (bmpAvailable) {
        float altitude = bmp.readAltitude(1013.25); // Using standard sea level pressure
        u8g2.print(altitude, 0); u8g2.print("m");
    } else {
        u8g2.print("Alt:N/A");
    }

    u8g2.setCursor(75, 8);
    if (bat1Sensor && bat1Sensor->enabled && bat1Sensor->displayOnOLED) {
      u8g2.print(bat1Sensor->value, 1);
    } else { u8g2.print("N/A"); }
    u8g2.print("v");

    // Time placeholder - RTC not ported
    // u8g2.setCursor(100, 8); printShortDateTimeOLED(u8g2);


    // Row 2: IMT, CT (Coolant Temp), TCT / GBT (Transfer Case / Gearbox)
    SensorConfig* imtSensor = findSensorById("intake_temp");
    SensorConfig* ctSensorRow2 = findSensorById("coolant_temp");
    // SensorConfig* tctSensor = findSensorById("transfer_case_temp"); // Moved to last line
    // SensorConfig* gbtSensor = findSensorById("gearbox_temp"); // Moved to last line
    SensorConfig* oilSensorRow2 = findSensorById("oil_pressure"); // For text display

    u8g2.setCursor(0, 18);
    u8g2.print("I:");
    if (imtSensor && imtSensor->enabled && imtSensor->displayOnOLED) displayTemperatureOLED(u8g2, imtSensor->value, imtSensor->value <= -998.0); else u8g2.print("NA");

    u8g2.setCursor(33, 18); // Adjusted X
    u8g2.print("C:");
    if (ctSensorRow2 && ctSensorRow2->enabled && ctSensorRow2->displayOnOLED) displayTemperatureOLED(u8g2, ctSensorRow2->value, ctSensorRow2->value <= -998.0); else u8g2.print("NA");

    u8g2.setCursor(66, 18); // Adjusted X
    u8g2.print("Oil:"); // Oil Pressure text
    if (oilSensorRow2 && oilSensorRow2->enabled && oilSensorRow2->displayOnOLED && oilSensorRow2->value > -998.0) {
        u8g2.print(oilSensorRow2->value, 0); // Oil pressure with 0 decimal
        // u8g2.print("psi"); // Unit might make it too long
    } else {
        u8g2.print("NA");
    }


    // Gauges Area (from Y=28 downwards on 128x64 screen)
    int gaugeBarX = 25; // Start X of gauge bar label
    int gaugeBarWidth = 75; // Width of the bar itself (128 - 25 - ~28 for value)
    int valuePrintX = 102;  // X position for printing numeric value
    int labelX = 0;
    float indicatorXPos; // Calculated position for the indicator bar


    SensorConfig* egtSensor = findSensorById("egt");
    SensorConfig* boostSensor = findSensorById("boost_pressure");
    SensorConfig* ctGaugeSensor = findSensorById("coolant_temp");

    // EGT Gauge
    int egt_y_label = 30; int egt_y_bar = egt_y_label - 8;
    u8g2.setCursor(labelX, egt_y_label); u8g2.print(F("EGT"));
    u8g2.drawFrame(gaugeBarX, egt_y_bar, gaugeBarWidth, 8); // Bar height 8
    if (egtSensor && egtSensor->enabled && egtSensor->displayOnOLED && egtSensor->value > -998.0) {
      indicatorXPos = ((egtSensor->value - minEGT) / (maxEGT - minEGT)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth -2) indicatorXPos = gaugeBarWidth - 2; // Constrain bar
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, egt_y_bar + 1, 2, 6); // Draw small bar 'I'
      u8g2.setCursor(valuePrintX, egt_y_label); u8g2.print((int)egtSensor->value);
    } else { u8g2.setCursor(valuePrintX, egt_y_label); u8g2.print("N/A");}

    // Boost Gauge
    int bst_y_label = 41; int bst_y_bar = bst_y_label - 8;
    u8g2.setCursor(labelX, bst_y_label); u8g2.print(F("BST"));
    u8g2.drawFrame(gaugeBarX, bst_y_bar, gaugeBarWidth, 8);
    if (boostSensor && boostSensor->enabled && boostSensor->displayOnOLED && boostSensor->value > -998.0) {
      indicatorXPos = ((boostSensor->value - minBST) / (maxBST - minBST)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth -2) indicatorXPos = gaugeBarWidth -2;
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, bst_y_bar + 1, 2, 6);
      u8g2.setCursor(valuePrintX, bst_y_label); u8g2.print(boostSensor->value,0);
    } else { u8g2.setCursor(valuePrintX, bst_y_label); u8g2.print("N/A");}

    // Coolant Temp Gauge
    int ct_y_label = 52; int ct_y_bar = ct_y_label - 8;
    u8g2.setCursor(labelX, ct_y_label); u8g2.print(F("CT "));
    u8g2.drawFrame(gaugeBarX, ct_y_bar, gaugeBarWidth, 8);
    if (ctGaugeSensor && ctGaugeSensor->enabled && ctGaugeSensor->displayOnOLED && ctGaugeSensor->value > -998.0) {
      indicatorXPos = ((ctGaugeSensor->value - minCT) / (maxCT - minCT)) * gaugeBarWidth;
      if (indicatorXPos < 0) indicatorXPos = 0; if (indicatorXPos > gaugeBarWidth-2) indicatorXPos = gaugeBarWidth-2;
      u8g2.drawBox(gaugeBarX + (int)indicatorXPos, ct_y_bar + 1, 2, 6);
      u8g2.setCursor(valuePrintX, ct_y_label); displayTemperatureOLED(u8g2, ctGaugeSensor->value);
    } else { u8g2.setCursor(valuePrintX, ct_y_label); u8g2.print("N/A");}

    // TCT/GBT on last line
    SensorConfig* tctSensor = findSensorById("transfer_case_temp");
    SensorConfig* gbtSensor = findSensorById("gearbox_temp");
    u8g2.setCursor(labelX, 63);
    u8g2.print("T:");
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

  sensors.begin();
  Serial.println("DS18B20 sensors driver initialized.");
  Serial.println("Configured & Enabled DS18B20 Sensors:");
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].sensorType == TEMP_DS18B20 && configuredSensors[i].enabled) {
      Serial.print("  Name: " + configuredSensors[i].name + ", Address: ");
      for (int j = 0; j < 8; j++) {
        if (configuredSensors[i].oneWireAddress[j] < 0x10) Serial.print("0");
        Serial.print(configuredSensors[i].oneWireAddress[j], HEX); Serial.print(" ");
      }
      Serial.println();
    }
  }

  Serial.println("Initializing BMP280 sensor...");
  // Wire.begin(oled_sda_pin, oled_scl_pin); // Often called by u8g2.begin() or bmp.begin() if not done before
  if (bmp.begin(BMP280_ADDRESS_ALT)) { // Check for 0x76
    bmpAvailable = true;
    Serial.println("BMP280 sensor found at 0x76.");
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
  } else if (bmp.begin()) { // Default address 0x77
    bmpAvailable = true;
    Serial.println("BMP280 sensor found at 0x77.");
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
  } else {
    bmpAvailable = false;
    Serial.println("Could not find a valid BMP280 sensor, check wiring or I2C address!");
  }

  initWifi();
  initWebServer();
  initOLED(); // OLED init after I2C sensors potentially

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLASHER_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // Ensure off
  digitalWrite(FLASHER_LED_PIN, LOW); // Ensure off

  Serial.println("Setup Complete. Entering loop.");
}

void loop() {
  readAllSensors();
  checkAllSensorAlerts(); // Check for alerts after reading sensors
  if (oledAvailable) { updateOLED(); }
  sendSensorEvents();
}
