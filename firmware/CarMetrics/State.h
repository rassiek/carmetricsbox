// CarMetrics - shared runtime state
//
// Threading model:
//  * loop() (Arduino task) owns the hardware and is the ONLY writer of config
//    and runtime state. It may read config without locking.
//  * Web handlers (AsyncTCP task) and the display task only READ shared state,
//    always inside a StateLock. Config changes from the web are validated in
//    the handler and handed to loop() via requestConfigApply().
#pragma once
#include <Arduino.h>
#include "Config.h"

enum AlertLevel : uint8_t { LVL_OK = 0, LVL_WARN = 1, LVL_FAULT = 2, LVL_CRIT = 3 };

enum SensorStatus : uint8_t {
  ST_DISABLED = 0,
  ST_WAITING,      // no reading yet
  ST_OK,
  ST_UNASSIGNED,   // DS18B20 with no address yet
  ST_NOT_FOUND,    // device missing (BMP / ADS1115 not detected, DS18B20 not on bus)
  ST_FAULT,        // reading failed / out of electrical range
  ST_LINK_LOST,    // main board: the board this sensor is on is out of reach
};
const char* sensorStatusStr(uint8_t st);
const char* alertLevelStr(uint8_t lvl);

struct SensorRuntime {
  float value = NAN;      // calibrated, smoothed
  float volts = NAN;      // sensor-side volts (analog types) for calibration help
  uint8_t status = ST_WAITING;
  uint8_t level = LVL_OK;
  uint8_t fails = 0;
  uint32_t lastOkMs = 0;
};

struct RelayRuntime {
  bool on = false;          // logical state requested
  uint32_t blinkMs = 0;
  bool outputHigh = false;  // logical output level (before active-low inversion)
  uint32_t lastToggleMs = 0;
};

struct GpsRuntime {
  bool fitted = false;          // NMEA sentences arriving
  bool fix = false;
  uint8_t sats = 0;
  double lat = NAN, lon = NAN;
  float speedKmh = NAN, altM = NAN, heading = NAN, hdop = NAN;
  uint32_t lastSentenceMs = 0, lastFixMs = 0;
};

enum TurboPhase : uint8_t { TURBO_OFF = 0, TURBO_COOLING, TURBO_OK };
struct TurboRuntime {
  uint8_t phase = TURBO_OFF;
  int32_t leftS = 0;            // minimum idle time left
  float egt = NAN;
  uint32_t hotShutdowns = 0;    // since boot
};

struct SystemRuntime {
  int8_t ignition = -1;         // -1 not fitted, 0 off, 1 on
  String wifiSsid;              // joined network ("" = none)
  String wifiState = "off";     // off | scanning | connecting | connected | no known network
  GpsRuntime gps;
  TurboRuntime turbo;
  bool engineRunning = true;
  uint8_t overallLevel = LVL_OK;
  bool muted = false;
  uint32_t muteUntilMs = 0;
  bool timeSynced = false;
  const char* timeSource = "";  // "internet", "GPS" or "browser"
  String bmpModel = "none";     // "BMP280", "BMP180", "none"
  String oledStatus = "off";    // "ok", "not found", "off"
  String ip, apIp;
  int otaProgress = -1;         // 0..100 while updating
  float stationHpa = NAN;       // latest raw BMP pressure (for altitude calibration)
  uint32_t configRevision = 0;  // bumped on every applied change
};

struct WeatherReading {
  String name, time;
  float temp = NAN, feels = NAN, rh = NAN, precip = NAN, cloud = NAN;
  float msl = NAN, sfc = NAN, wind = NAN, windDir = NAN, gust = NAN, elevation = NAN;
  int code = -1;
};

struct NetStatus {
  bool sta = false;            // joined a Wi-Fi network
  bool internet = false;       // last internet request succeeded
  bool ntp = false;            // clock set from NTP
  // weather
  uint32_t weatherAtMs = 0;
  String weatherError;
  float seaLevelAuto = NAN;    // hPa from the reference town (runtime only, never written to flash)
  String seaLevelTown;
  // cloud upload
  bool recording = false;
  uint32_t ramChunks = 0, backlogFiles = 0, backlogBytes = 0;
  uint32_t sentRecords = 0, droppedRecords = 0, pendingRecords = 0;
  uint32_t lastOkMs = 0, lastTryMs = 0;
  String lastError;
};

extern std::vector<WeatherReading> gWeather;
extern NetStatus gNet;
float effectiveSeaLevelHpa();   // call with lock held

extern Config gConfig;
extern std::vector<SensorRuntime> gSensorRt;
extern std::vector<RelayRuntime> gRelayRt;
extern SystemRuntime gSys;

void stateInit();
void stateLock();
void stateUnlock();
struct StateLock {
  StateLock() { stateLock(); }
  ~StateLock() { stateUnlock(); }
};

// Web -> loop hand-over
void requestConfigApply(Config* newConfig);   // takes ownership
Config* takePendingConfig();                  // loop: returns nullptr if none
void requestReboot(uint32_t delayMs = 1000);
bool rebootDue();

int findSensorIndex(const String& id);   // call with lock held (or from loop)
int findRelayIndex(const String& id);
