#include "State.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

Config gConfig;
std::vector<SensorRuntime> gSensorRt;
std::vector<RelayRuntime> gRelayRt;
SystemRuntime gSys;
std::vector<WeatherReading> gWeather;
NetStatus gNet;

float effectiveSeaLevelHpa() {
  if (gConfig.weather.enabled && gConfig.weather.autoSeaLevel && !isnan(gNet.seaLevelAuto)) return gNet.seaLevelAuto;
  return gConfig.sys.seaLevelHpa;
}

static SemaphoreHandle_t sMutex = nullptr;
static portMUX_TYPE sPendingMux = portMUX_INITIALIZER_UNLOCKED;
static Config* sPending = nullptr;
static uint32_t sRebootAt = 0;

void stateInit() {
  if (!sMutex) sMutex = xSemaphoreCreateRecursiveMutex();
}
void stateLock() { xSemaphoreTakeRecursive(sMutex, portMAX_DELAY); }
void stateUnlock() { xSemaphoreGiveRecursive(sMutex); }

void requestConfigApply(Config* c) {
  Config* old;
  portENTER_CRITICAL(&sPendingMux);
  old = sPending;
  sPending = c;
  portEXIT_CRITICAL(&sPendingMux);
  delete old;  // a newer request supersedes one not yet applied
}

Config* takePendingConfig() {
  Config* c;
  portENTER_CRITICAL(&sPendingMux);
  c = sPending;
  sPending = nullptr;
  portEXIT_CRITICAL(&sPendingMux);
  return c;
}

void requestReboot(uint32_t delayMs) { sRebootAt = millis() + delayMs; if (!sRebootAt) sRebootAt = 1; }
bool rebootDue() { return sRebootAt && (int32_t)(millis() - sRebootAt) >= 0; }

int findSensorIndex(const String& id) {
  for (size_t i = 0; i < gConfig.sensors.size(); i++) if (gConfig.sensors[i].id == id) return (int)i;
  return -1;
}
int findRelayIndex(const String& id) {
  for (size_t i = 0; i < gConfig.relays.size(); i++) if (gConfig.relays[i].id == id) return (int)i;
  return -1;
}

const char* sensorStatusStr(uint8_t st) {
  switch (st) {
    case ST_DISABLED:   return "disabled";
    case ST_WAITING:    return "waiting";
    case ST_OK:         return "ok";
    case ST_UNASSIGNED: return "unassigned";
    case ST_NOT_FOUND:  return "not_found";
    default:            return "fault";
  }
}
const char* alertLevelStr(uint8_t lvl) {
  switch (lvl) {
    case LVL_WARN:  return "warn";
    case LVL_FAULT: return "fault";
    case LVL_CRIT:  return "crit";
    default:        return "ok";
  }
}
