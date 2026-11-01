// CarMetrics - configuration model
// All user-editable settings live here. Config is persisted as JSON in NVS
// (power-fail safe, and not wiped by a filesystem OTA update).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// Release label: shown in the web app header, System page, OLED start screen and
// the release file names. Bump for every build handed out; the web app gets the
// same label baked in (tools/build_web.py) and warns when the two don't match.
#define FW_VERSION "5.1.0-beta.8"
#define CONFIG_SCHEMA 2   // bumped when the stored layout changes; older configs are upgraded on load
#define MAX_SENSORS 24
#define MAX_RELAYS 12

enum class SensorType : uint8_t { DS18B20, MAX6675, MCP9600, BMP, ADC, ADS1115,
                                  RPM,       // pulses on a GPIO (alternator W terminal)
                                  SWITCH,    // digital input (warning switch, handbrake...)
                                  GPS,       // field of the optional GPS module
                                  INA226,    // I2C current/voltage monitor
                                  IMU,       // I2C accelerometer (MPU-6050 family): roll / pitch
                                  COMPUTED,  // derived from other sensors
                                  REMOTE,    // main board: a sensor on another board (kept in step with that board's settings)
                                  UNKNOWN };
enum class BmpField : uint8_t { TEMPERATURE, PRESSURE, ALTITUDE, HUMIDITY };   // humidity: BME280 only
enum class OledStyle : uint8_t { OFF, TEXT, BAR, MARKER };   // MARKER: needle between the threshold ticks (original v1 look)

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
  uint8_t i2cAddr = 0;     // INA226 (0x40-0x4F) / IMU (0x68-0x69); 0 = type default
  String field;            // GPS: speed|altitude|sats|heading|hdop  INA226: current|voltage|power  IMU: roll|pitch
  float pulsesPerRev = 10; // RPM: pulses per engine revolution (calibrate at idle)
  bool activeLow = true;   // SWITCH: input pulled to ground when active (value 1)
  float shuntOhms = 0.001f;// INA226 shunt resistor
  // COMPUTED: diff = a - b, sum = a + b, rate = change of a per minute,
  // time_in_warn = minutes a has spent in warning or worse, curve_margin = a - curve(b)
  String formula, srcA, srcB;
  uint8_t remoteBoard = 0, remoteIndex = 0;   // REMOTE: board id, position in that board's value list
  String remoteId;                            // REMOTE: the sensor's id on that board
  // Lookup curve "x:y,x:y,..." (sorted by x). Analog: sensor volts -> value (non-linear senders).
  // curve_margin: b -> minimum allowed a (e.g. RPM -> minimum oil pressure).
  String curve;

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
  uint8_t safeState = 0;    // relay board, link to the main board lost: 0 off, 1 on, 2 keep as it is
};

struct BoardProfile;
struct WifiNetwork { String ssid, pass; uint8_t priority = 5; };   // priority 1 (low) .. 9 (high)
#define MAX_WIFI_NETWORKS 8

struct SystemConfig {
  String board = "auto";             // board profile id (see Board.cpp); "auto" = detect
  String role = "main";              // main | sensor | relay | sensor_relay (extra boards, phase 3); change = reboot
  bool boardEncrypt = false;         // main board: give boards paired from now on an encryption key (off in the 5.1 betas until bench-tested)
  std::vector<WifiNetwork> networks; // known networks to join (phone hotspot, home...)
  bool scanWhileApInUse = true;      // false: don't search for networks while a phone uses the CarMetrics AP

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

  // Ignition sense + sleep (optional, -1 = not fitted)
  int ignitionPin = -1;
  bool ignitionActiveLow = true;     // opto-coupler pulls the input low while the key is on
  uint16_t sleepAfterMinutes = 0;    // deep sleep this long after key-off (0 = never); wakes on ignition

  // GPS module (optional, UART; -1 = not fitted)
  int gpsRxPin = -1, gpsTxPin = -1;  // ESP32 RX <- GPS TX
  uint32_t gpsBaud = 9600;

  // Misc
  float seaLevelHpa = 1013.25f;
  uint16_t historyIntervalSeconds = 5;     // chart history: 360 samples
  int16_t tzOffsetMinutes = 180;           // Nairobi UTC+3, updated by browser
};

// Public weather (Open-Meteo) for chosen towns - only fetched when the ESP32 has internet.
struct WeatherTown { String name; float lat = 0, lon = 0; };
struct WeatherConfig {
  bool enabled = true;
  std::vector<WeatherTown> towns;    // default: Nairobi
  String refTown = "Nairobi";        // its sea-level pressure calibrates the altimeter
  bool autoSeaLevel = true;
  uint16_t refreshMinutes = 20;
};

// Metrics upload with an offline ring buffer (RAM first, flash backlog when offline).
struct CloudConfig {
  bool enabled = false;
  String protocol = "zabbix";        // "zabbix" (trapper) | "influx" (line protocol over HTTP)
  uint32_t intervalMs = 60000;       // 250 (every reading) | 1000 | 5000 | 10000 | 30000 | 60000 | 300000
  uint16_t uploadEverySec = 60;      // how often to send while online
  uint16_t bufferKB = 256;           // flash backlog cap; oldest data dropped beyond this
  bool onlyEngineRunning = true;     // don't record while parked
  bool sendSystem = false;           // also upload board metrics (sys_heap_free_kb, sys_cpu0, ...)
  // Zabbix trapper
  String zbxServer;
  uint16_t zbxPort = 10051;
  String zbxHost = "carmetrics";     // host name as configured in Zabbix
  bool zbxPacked = true;             // one JSON item per sample (cheapest) vs one item per metric
  String zbxPskIdentity, zbxPsk;     // optional TLS-PSK (psk = hex)
  // InfluxDB line protocol (InfluxDB v1/v2, VictoriaMetrics, Grafana Cloud...)
  String influxUrl;                  // e.g. http://vm.example:8428/write
  String influxAuth;                 // optional Authorization header value ("Token xyz", "Basic ...")
  String influxMeasurement = "carmetrics";
};

// Turbo cool-down indicator: after a run, show EGT and a countdown until it's safe to switch off.
struct TurboConfig {
  bool enabled = false;
  String egtId = "egt";
  float safeEgt = 200;               // °C: OK to switch off below this
  uint16_t minIdleSeconds = 60;      // and after at least this long stopped/idling
  String rpmId;                      // optional: idle = RPM below idleRpm
  float idleRpm = 1000;
  String speedId;                    // optional: stopped = speed below 3 km/h
  bool beep = true;                  // short beep when it's OK
};

// Display layout: one model for the web dashboard tiles and the OLED pages.
struct LayoutTile { String id; uint8_t size = 1; String widget = "bar"; };   // size 1|2, widget bar|number|spark
struct LayoutConfig {
  std::vector<LayoutTile> tiles;     // empty = every enabled sensor, config order
  std::vector<String> oled;          // sensor ids, "|" = page break; empty = automatic
  uint8_t oledColumns = 0;           // text values per OLED line: 0 = automatic (2 on 128x128, 3 on 128x64), 2..4
};

struct BlackBoxConfig {
  bool enabled = true;
  uint8_t keepEvents = 10;           // oldest dropped
};

struct Config {
  SystemConfig sys;
  TurboConfig turbo;
  LayoutConfig layout;
  BlackBoxConfig blackbox;
  std::vector<SensorConfig> sensors;
  std::vector<RelayConfig> relays;
  WeatherConfig weather;
  CloudConfig cloud;
};

// --- helpers ---
const char* sensorTypeToStr(SensorType t);
SensorType sensorTypeFromStr(const String& s);
const char* bmpFieldToStr(BmpField f);
BmpField bmpFieldFromStr(const String& s);
const char* oledStyleToStr(OledStyle s);
OledStyle oledStyleFromStr(const String& s);

void configDefaults(Config& c);                          // for the detected board
void configDefaults(Config& c, const BoardProfile& b);
void applyBoardPins(Config& c, const BoardProfile& b);   // move bus/output/sensor pins to the board defaults
void defaultSensors(std::vector<SensorConfig>& v, const BoardProfile& b);
void defaultRelays(std::vector<RelayConfig>& v, const BoardProfile& b);

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
bool weatherFromJson(JsonVariantConst v, WeatherConfig& out, String& err);
bool cloudFromJson(JsonVariantConst v, CloudConfig& out, String& err);
void weatherToJson(const WeatherConfig& w, JsonObject o);
void turboToJson(const TurboConfig& t, JsonObject o);
bool turboFromJson(JsonVariantConst v, TurboConfig& out, String& err);
void layoutToJson(const LayoutConfig& l, JsonObject o);
bool layoutFromJson(JsonVariantConst v, LayoutConfig& out, String& err);
bool parseCurve(const String& s, std::vector<std::pair<float, float>>& out);   // "x:y,x:y"
float curveLookup(const std::vector<std::pair<float, float>>& c, float x);      // linear, clamped
void cloudToJson(const CloudConfig& c, JsonObject o, bool includeSecrets);

// Cross-checks pins, ids, references. Returns false with a human readable error.
bool validateConfig(const Config& c, String& err);

// Persistence (NVS). Every save keeps the previous config ("prev") for a one-step
// undo; after a healthy run the current config is also kept as "good" (used by safe mode).
enum class CfgSlot : uint8_t { CURRENT, PREVIOUS, GOOD };
bool loadConfig(Config& c, CfgSlot slot = CfgSlot::CURRENT);   // false = defaults were used
bool saveConfig(const Config& c);
bool hasConfigSlot(CfgSlot slot);
bool markConfigGood();          // copy current -> good (only writes if different)
uint32_t configWriteCount();    // NVS writes this boot (board metrics)
