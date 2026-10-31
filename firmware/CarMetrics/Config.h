// CarMetrics - configuration model
// All user-editable settings live here. Config is persisted as JSON in NVS
// (power-fail safe, and not wiped by a filesystem OTA update).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#define FW_VERSION "4.0.0"
#define MAX_SENSORS 24
#define MAX_RELAYS 12

enum class SensorType : uint8_t { DS18B20, MAX6675, MCP9600, BMP, ADC, ADS1115, UNKNOWN };
enum class BmpField : uint8_t { TEMPERATURE, PRESSURE, ALTITUDE };
enum class OledStyle : uint8_t { OFF, TEXT, BAR };

struct SensorConfig {
  String id;    // unique, [a-z0-9_]
  String name;  // shown in the web UI
  String unit;  // e.g. "°C", "psi", "V"
  SensorType type = SensorType::UNKNOWN;
  bool enabled = true;

  // --- hardware (which fields are used depends on type) ---
  String address;          // DS18B20: 16 hex chars, empty = not yet assigned
  int pin = -1;            // ADC: analog pin (32-39). MAX6675: CS pin
  int sckPin = -1;         // MAX6675 SCK (may be shared between MAX6675s)
  int soPin = -1;          // MAX6675 SO  (may be shared between MAX6675s)
  uint8_t adsAddress = 0x48;  // ADS1115 I2C address (0x48-0x4B)
  uint8_t adsChannel = 0;     // ADS1115 channel 0-3
  uint8_t mcpAddress = 0x67;  // MCP9600/MCP9601 thermocouple amp I2C address (0x60-0x67)
  BmpField bmpField = BmpField::TEMPERATURE;

  // --- calibration (analog types: ADC, ADS1115) ---
  // sensorVolts = pinVolts * divider
  // value = outMin + (sensorVolts - inMin) * (outMax - outMin) / (inMax - inMin)
  float divider = 1.0f;
  float inMin = 0.0f, inMax = 1.0f;
  float outMin = 0.0f, outMax = 1.0f;
  float faultLowV = NAN;   // sensorVolts below this = wiring fault (e.g. 0.2 V for 0.5-4.5 V senders)
  float faultHighV = NAN;  // sensorVolts above this = wiring fault
  // --- all types ---
  float offset = 0.0f;     // added after conversion (e.g. thermocouple correction)
  float smoothing = 0.3f;  // 0.05 (very smooth) .. 1.0 (raw)

  // --- display ---
  uint8_t decimals = 0;
  float gaugeMin = 0, gaugeMax = 100;
  OledStyle oledStyle = OledStyle::TEXT;
  String oledLabel;        // up to 3 chars, e.g. "CT"

  // --- alerts (NAN = disabled) ---
  float warnHigh = NAN, critHigh = NAN, warnLow = NAN, critLow = NAN;
  float hysteresis = 1.0f;
  bool engineRunningOnly = false;  // only alarm on value while the engine runs (oil pressure, battery)
  bool faultAlarm = true;          // alarm when the sensor is missing/faulty (off = just show NOT FOUND)
};

enum class RelayMode : uint8_t { ON_OFF, BLINK };

struct RelayConfig {
  String id, name;
  int pin = -1;
  RelayMode mode = RelayMode::ON_OFF;
  bool activeLow = false;   // most cheap relay boards switch ON when the input is LOW
  bool defaultOn = false;   // state after boot
  uint32_t blinkMs = 500;
  bool enabled = true;
};

struct SystemConfig {
  // Wi-Fi
  String apSsid = "CarMetrics";
  String apPass = "carmetrics";      // >= 8 chars
  String staSsid;                    // optional: join an existing network too
  String staPass;
  String hostname = "carmetrics";    // http://carmetrics.local
  String adminPass = "carmetrics";   // settings, relays, OTA (user "admin")

  // Buses
  int i2cSda = 21, i2cScl = 22;
  uint16_t i2cKhz = 400;             // 100 if an MCP9600 misbehaves (it clock-stretches)
  int oneWirePin = 4;
  uint8_t oneWireResolution = 11;    // 9..12 bits (11 = 0.125 °C, 375 ms)

  // OLED
  String oledType = "ssd1327_128x128";  // none | ssd1306_128x64 | sh1106_128x64 | ssd1327_128x128 | ssd1327_ws_128x128
  bool oledFlip = false;
  bool oledClock = true;             // show time once synced from a browser
  uint16_t oledPageSeconds = 4;      // page rotation if not everything fits
  uint8_t oledContrast = 255;

  // Alert outputs (-1 = not fitted)
  int buzzerPin = 18; bool buzzerActiveLow = false;
  int ledPin = 19;    bool ledActiveLow = false;

  // Engine-running detection (used by engineRunningOnly sensors)
  String engineSensorId = "bat_voltage";   // empty = always treat engine as running
  float engineRunningAbove = 13.2f;        // alternator charging
  uint16_t engineDelaySeconds = 5;

  // Misc
  float seaLevelHpa = 1013.25f;
  uint16_t historyIntervalSeconds = 5;     // chart history: 360 samples
  int16_t tzOffsetMinutes = 180;           // Nairobi UTC+3, updated by browser
};

struct Config {
  SystemConfig sys;
  std::vector<SensorConfig> sensors;
  std::vector<RelayConfig> relays;
};

// --- helpers ---
const char* sensorTypeToStr(SensorType t);
SensorType sensorTypeFromStr(const String& s);
const char* bmpFieldToStr(BmpField f);
BmpField bmpFieldFromStr(const String& s);
const char* oledStyleToStr(OledStyle s);
OledStyle oledStyleFromStr(const String& s);

void configDefaults(Config& c);
void defaultSensors(std::vector<SensorConfig>& v);
void defaultRelays(std::vector<RelayConfig>& v);

// JSON (de)serialisation. includeSecrets=false blanks out passwords.
void sensorToJson(const SensorConfig& s, JsonObject o);
void relayToJson(const RelayConfig& r, JsonObject o);
void systemToJson(const SystemConfig& s, JsonObject o, bool includeSecrets);
void configToJson(const Config& c, JsonObject o, bool includeSecrets);

// Parse into `out`; returns false and fills `err` on invalid input.
// For system: empty password fields keep the value already in `out`.
bool sensorsFromJson(JsonVariantConst v, std::vector<SensorConfig>& out, String& err);
bool relaysFromJson(JsonVariantConst v, std::vector<RelayConfig>& out, String& err);
bool systemFromJson(JsonVariantConst v, SystemConfig& out, String& err);
bool configFromJson(JsonVariantConst v, Config& out, String& err);

// Cross-checks pins, ids, references. Returns false with a human readable error.
bool validateConfig(const Config& c, String& err);

// Persistence (NVS)
bool loadConfig(Config& c);   // false = defaults were used
bool saveConfig(const Config& c);
