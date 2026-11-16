// CarMetrics - configuration model
// All user-editable settings live here. Config is persisted as JSON in NVS
// (power-fail safe, and not wiped by a filesystem OTA update).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#ifdef CM_V52
#define CM_MAX_BUFFER_KB 1536    // offline upload buffer on the 1.75 MB file system
#define CM_MAX_BLACKBOX 60
#else
#define CM_MAX_BUFFER_KB 400
#define CM_MAX_BLACKBOX 30
#endif
// Release label: shown in the web app header, System page, OLED start screen and
// the release file names. Bump for every build handed out; the web app gets the
// same label baked in (tools/build_web.py) and warns when the two don't match.
// One version for every board type since 5.3 (docs/DESIGN.md section 1). The build
// variant (which board family the file is for) is separate: CM_VARIANT.
#define FW_VERSION "5.4.0-beta.3"
// Build variant (tools/cm.sh): which firmware and board this binary is for.
#if !defined(CM_VARIANT) && defined(CM_RELAY_X8)
#define CM_VARIANT "relay-x8"
#elif !defined(CM_VARIANT) && defined(CM_KC868_A4)
#define CM_VARIANT "kc868-a4"
#elif !defined(CM_VARIANT) && defined(CM_T_RELAY)
#define CM_VARIANT "t-relay"
#elif !defined(CM_VARIANT) && defined(CM_WORKER_PLUS)
#define CM_VARIANT "worker-s3"
#elif !defined(CM_VARIANT) && defined(CM_WORKER) && defined(CONFIG_IDF_TARGET_ESP32C3)
#define CM_VARIANT "worker-c3"
#elif !defined(CM_VARIANT) && defined(CM_WORKER) && defined(CONFIG_IDF_TARGET_ESP32C6)
#define CM_VARIANT "worker-c6"
#elif !defined(CM_VARIANT) && defined(CM_WORKER)
#define CM_VARIANT "worker-esp32"
#elif !defined(CM_VARIANT) && defined(CM_V52)
#define CM_VARIANT "main-s3"
#endif
#ifndef CM_VARIANT
#define CM_VARIANT "test"         // emulator / self-test builds
#endif
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
  String chartGroup;       // charts: "" = with the same kind of sensor, "own" = a chart of its own, or a group name

  // --- alerts (NAN = disabled) ---
  float warnHigh = NAN, critHigh = NAN, warnLow = NAN, critLow = NAN;
  float hysteresis = 1.0f;
  bool engineRunningOnly = false;  // only alarm on value while the engine runs (oil pressure, battery)
  bool faultAlarm = true;          // alarm when the sensor is missing/faulty (off = just show NOT FOUND)
};

enum class RelayMode : uint8_t { ON_OFF, BLINK };
// Actuators (5.3): the stored list keeps its old name "relays" so 5.1 / 5.2 boards
// still understand the relays among them; new types and fields are ignored by them.
enum class ActType : uint8_t { RELAY, PWM, HBRIDGE, SERVO };
enum class RuleType : uint8_t { MANUAL, THRESHOLD, CURVE, CONDITION };

struct RelayConfig {
  String id, name;
  ActType type = ActType::RELAY;
  int pin = -1;
  int pin2 = -1;            // H-bridge: the reverse PWM pin (BTS7960 LPWM); forward is pin (RPWM)
  RelayMode mode = RelayMode::ON_OFF;
  bool activeLow = false;   // most cheap relay boards switch ON when the input is LOW
  bool defaultOn = false;   // state after boot (manual)
  uint32_t blinkMs = 500;
  bool enabled = true;
  uint8_t safeState = 0;    // link to the main board lost: 0 off, 1 on, 2 keep as it is
  // PWM / H-bridge / servo
  uint32_t freqHz = 1000;   // PWM: 25000 for 4-pin fans, 100-1000 for MOSFET modules; servo fixed 50
  uint8_t minDuty = 0, maxDuty = 100;      // % limits while running (min = lowest speed that still turns)
  uint16_t kickMs = 0;      // start at 100 % for this long, then settle (fans / pumps that stall)
  float rampPerS = 0;       // % per second towards the target (0 = at once)
  uint8_t safeDuty = 0;     // link lost / sensor fault: this duty (%), or angle for a servo
  float servoMinDeg = 0, servoMaxDeg = 180;
  // control rule
  RuleType rule = RuleType::MANUAL;
  String ruleSensor;        // THRESHOLD / CURVE: sensor id on this board
  float onAt = NAN, offAt = NAN;   // THRESHOLD: on at/above onAt, off at/below offAt (onAt < offAt = on when LOW)
  String curve;             // CURVE: "sensor:duty,..." e.g. "85:0,92:40,100:100"
  String condition;         // CONDITION: running | not_running | ignition | warn | crit | fault
};

struct BoardProfile;
struct WifiNetwork { String ssid, pass; uint8_t priority = 5; };   // priority 1 (low) .. 9 (high)
#define MAX_WIFI_NETWORKS 8

struct SystemConfig {
  String board = "auto";             // board profile id (see Board.cpp); "auto" = detect
  String role = "main";              // main | worker: from the firmware (setFirmwareRole)
  bool boardEncrypt = false;
  bool canEnabled = false;           // CAN link (built-in controller + 3.3 V transceiver, e.g. SN65HVD230)
  int canTx = -1, canRx = -1;        // to the transceiver's D (TX) and R (RX) pins
  uint16_t canKbps = 500;            // 125 / 250 / 500 / 1000, the same on every board         // main board: give boards paired from now on an encryption key (off in the 5.1 betas until bench-tested)
  std::vector<WifiNetwork> networks; // known networks to join (phone hotspot, home...)
  bool scanWhileApInUse = true;      // false: don't search for networks while a phone uses the CarMetrics AP

  // Wi-Fi
  String apSsid = "CarMetrics";
  String apPass = "carmetrics";      // >= 8 chars
  String staSsid;                    // optional: join an existing network too
  String staPass;
  String hostname = "carmetrics";    // http://carmetrics.local
  String adminPass = "carmetrics";   // settings, relays, OTA (user "admin")
  bool readNeedsAuth = false;         // API: reading needs a key or the password too

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

  // Status light (5.3): built-in addressable RGB and/or an external 3-pin RGB LED, both at once
  bool rgbBuiltin = true;  int rgbBuiltinPin = -1;  uint8_t rgbBuiltinBright = 15;  // -1 = the board profile's (S3 DevKitC: 48)
  bool rgbExternal = false; int rgbR = -1, rgbG = -1, rgbB = -1; bool rgbCommonAnode = false; uint8_t rgbExtBright = 80;   // %

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
struct LayoutTile { String id; uint8_t size = 1; String widget = "bar"; String with; };   // size 1|2, widget bar (dial)|number|spark|bars; with: more sensor ids for "bars" ("a,b,c")
struct LayoutConfig {
  std::vector<LayoutTile> tiles;     // empty = every enabled sensor, config order
  std::vector<String> oled;          // sensor ids, "|" = page break; empty = automatic
  uint8_t oledColumns = 0;           // text values per OLED line: 0 = automatic (2 on 128x128, 3 on 128x64), 2..4
};

struct BlackBoxConfig {
  bool enabled = true;
  uint8_t keepEvents = 10;           // oldest dropped
};

// Everything a person reads that names the machine (docs/DESIGN.md section 7).
// Defaults are CarMetrics / car wording; a boat, farm or generator just changes them.
struct UiConfig {
  String appName = "CarMetrics";      // header, browser tab, OLED start screen, displays
  String machineName;                 // "My Defender", "MV Aurora", "Borehole 2" (optional)
  String pageDashboard = "Dashboard", pageSensors = "Sensors", pageActuators = "Actuators",
         pageBoards = "Boards", pageSystem = "System";
  String runningWord = "engine";      // "<word> running" / "<word> off" (engine, pump, generator...)
  String tempUnit = "C";              // display preference: C | F
  String pressureUnit = "";           // "" = as each sensor says | bar | psi | kPa
  String speedUnit = "";              // "" = as each sensor says | km/h | mph | kn
  String preset = "car";              // preset the settings started from (car | boat | farm | blank | custom)
};

struct Config {
  UiConfig ui;
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

// "main" or "worker": set by the firmware first thing in setup(); every loaded or
// imported configuration gets this role (it is not a setting any more).
void setFirmwareRole(const char* role);
extern String gFirmwareRole;
// Every build carries a role tag ("main" or "worker") in its image, so a firmware file
// can be checked before it is installed or sent to another board. Feed it the file in
// pieces; role stays "" for files without a tag (5.3 and older, touch displays).
struct RoleScan {
  String role;
  void feed(const uint8_t* d, size_t n);
 private:
  uint8_t m = 0, g = 0; char got[8] = {0};
};
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
void uiToJson(const UiConfig& u, JsonObject o);
bool uiFromJson(JsonVariantConst v, UiConfig& out, String& err);
void layoutToJson(const LayoutConfig& l, JsonObject o);
bool layoutFromJson(JsonVariantConst v, LayoutConfig& out, String& err);
bool parseCurve(const String& s, std::vector<std::pair<float, float>>& out);   // "x:y,x:y"
float curveLookup(const std::vector<std::pair<float, float>>& c, float x);      // linear, clamped
void cloudToJson(const CloudConfig& c, JsonObject o, bool includeSecrets);

// Cross-checks pins, ids, references. Returns false with a human readable error.
bool validateConfig(const Config& c, String& err);
bool validateItems(const std::vector<SensorConfig>& sensors, const std::vector<RelayConfig>& relays, String& err);   // another board's lists, without its pin map

// Persistence (NVS). Every save keeps the previous config ("prev") for a one-step
// undo; after a healthy run the current config is also kept as "good" (used by safe mode).
enum class CfgSlot : uint8_t { CURRENT, PREVIOUS, GOOD };
bool loadConfig(Config& c, CfgSlot slot = CfgSlot::CURRENT);
bool loadCustomBoard(String& json);          // custom board definition (Board::setCustom), "" = none
bool saveCustomBoard(const String& json);    // "" removes it   // false = defaults were used
bool saveConfig(const Config& c);
bool hasConfigSlot(CfgSlot slot);
bool markConfigGood();          // copy current -> good (only writes if different)
uint32_t configWriteCount();    // NVS writes this boot (board metrics)
