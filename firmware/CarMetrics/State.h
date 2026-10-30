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

struct SystemRuntime {
  bool engineRunning = true;
  uint8_t overallLevel = LVL_OK;
  bool muted = false;
  uint32_t muteUntilMs = 0;
  bool timeSynced = false;
  String bmpModel = "none";     // "BMP280", "BMP180", "none"
  String oledStatus = "off";    // "ok", "not found", "off"
  String ip, apIp;
  int otaProgress = -1;         // 0..100 while updating
  uint32_t configRevision = 0;  // bumped on every applied change
};

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
