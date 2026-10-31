// CarMetrics v5 - ESP32 / ESP32-S3 engine monitor
// https://github.com/rassiek/carmetricsbox
//
// Boards: "ESP32 Dev Module" (classic ESP32) or "ESP32S3 Dev Module" (set PSRAM
// to "OPI PSRAM" on N8R8/N16R8). Builds on ESP32 Arduino core 2.0.17 and 3.x.
// Partitions: partitions.csv in this folder (two 1.6 MB OTA slots + 576 KB
// LittleFS for the web app + settings), fits every board with 4 MB flash or more.
//
// Module map:
//   Config.*   settings model, JSON, validation, NVS persistence
//   Board.*    board profiles: valid / output / analog pins, default pins
//   Health.*   board metrics, firmware rollback, safe-mode boot
//   WifiMgr.*  access point + best saved network
//   Gps.*      optional GPS module (NMEA)
//   Vehicle.*  ignition sense, deep sleep, turbo cool-down timer
//   BlackBox.* 30 s before/after every alarm, saved to flash
//   State.*    shared runtime state + lock (see threading notes in State.h)
//   Sensors.*  DS18B20, MAX6675, BMP180/280, ESP32 ADC, ADS1115
//   Outputs.*  alerts (buzzer/LED) and relays
//   Display.*  OLED (own task)
//   History.*  30-minute chart history in RAM
//   Web.*      HTTP API, live events (SSE), OTA upload

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <esp_netif.h>
#include "Config.h"
#include "State.h"
#include "Sensors.h"
#include "Outputs.h"
#include "Display.h"
#include "History.h"
#include "Web.h"
#include "Cloud.h"
#include "Health.h"
#include "Board.h"
#include "WifiMgr.h"
#include "Gps.h"
#include "Vehicle.h"
#include "BlackBox.h"

static uint32_t lastNetCheck = 0;

static void startOta() {
  ArduinoOTA.setHostname(gConfig.sys.hostname.c_str());
  ArduinoOTA.setPassword(gConfig.sys.adminPass.c_str());
  ArduinoOTA.onStart([]() {
    if (ArduinoOTA.getCommand() == U_SPIFFS) LittleFS.end();
    StateLock lk; gSys.otaProgress = 0;
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    StateLock lk; gSys.otaProgress = total ? done * 100 / total : 0;
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[ota] error %u\n", e);
    StateLock lk; gSys.otaProgress = -1;
  });
#ifdef CM_EMULATOR_TEST
  return;
#endif
  ArduinoOTA.begin();  // also starts mDNS: http://<hostname>.local
  MDNS.addService("http", "tcp", 80);
}

// Size runtime arrays to match config, carrying over readings for sensors that still exist.
static void resizeRuntime(const Config& oldCfg) {
  std::vector<SensorRuntime> rt(gConfig.sensors.size());
  for (size_t i = 0; i < gConfig.sensors.size(); i++)
    for (size_t j = 0; j < oldCfg.sensors.size() && j < gSensorRt.size(); j++)
      if (oldCfg.sensors[j].id == gConfig.sensors[i].id && oldCfg.sensors[j].type == gConfig.sensors[i].type) rt[i] = gSensorRt[j];
  gSensorRt = rt;
  gRelayRt.resize(gConfig.relays.size());
}

static void applyConfig(Config* next) {
  const SystemConfig &a = gConfig.sys, &b = next->sys;
  bool wifiChanged = a.networks.size() != b.networks.size();
  for (size_t i = 0; !wifiChanged && i < a.networks.size(); i++)
    wifiChanged = a.networks[i].ssid != b.networks[i].ssid || a.networks[i].pass != b.networks[i].pass || a.networks[i].priority != b.networks[i].priority;
  bool reboot = a.apSsid != b.apSsid || a.apPass != b.apPass ||
                a.hostname != b.hostname || a.board != b.board || a.i2cSda != b.i2cSda || a.i2cScl != b.i2cScl || a.i2cKhz != b.i2cKhz;
  {
    StateLock lk;
    Config old = gConfig;
    gConfig = *next;
    resizeRuntime(old);
    gSys.configRevision++;
  }
  delete next;
  saveConfig(gConfig);
  Health::configApplied();
  ArduinoOTA.setPassword(gConfig.sys.adminPass.c_str());
  Sensors::begin();
  Alerts::begin();
  Relays::begin();
  History::begin();
  Cloud::begin();
  Gps::begin();
  Vehicle::begin();
  BlackBox::begin();
  if (wifiChanged && !reboot) WifiMgr::reconfigure();
  Display::reconfigure();
  Web::notifyConfigChanged();
  if (reboot) { Serial.println("[cfg] network/I2C settings changed - rebooting"); requestReboot(2000); }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n\nCarMetrics %s booting on %s\n", FW_VERSION, ESP.getChipModel());
  Health::earlyBoot();
  Health::registerTask("loop");
  stateInit();

  // Safe mode starts from the last configuration that ran well.
  if (!(Health::safeMode() && hasConfigSlot(CfgSlot::GOOD) && loadConfig(gConfig, CfgSlot::GOOD))) loadConfig(gConfig);
  Serial.printf("[board] %s\n", Board::current().name);
  gSensorRt.assign(gConfig.sensors.size(), SensorRuntime());
  gRelayRt.assign(gConfig.relays.size(), RelayRuntime());

  // Outputs first so relays/buzzer sit in a known state as early as possible.
  Relays::begin();
  Alerts::begin();

  if (!LittleFS.begin(false)) {
    Serial.println("[fs] mount failed - formatting");
    LittleFS.begin(true);
  }
  Wire.begin(gConfig.sys.i2cSda, gConfig.sys.i2cScl, gConfig.sys.i2cKhz * 1000UL);

  Sensors::begin();
  History::begin();
  Cloud::begin();
  Gps::begin();
  Vehicle::begin();
  BlackBox::begin();
  WifiMgr::begin();
  startOta();
  Web::begin();
  Display::start();
  if (!Health::safeMode()) Cloud::startNetTask();   // no internet features in safe mode
  Serial.println("[boot] ready");
}

void loop() {
  Health::loopStart();
  if (Config* next = takePendingConfig()) applyConfig(next);

  Sensors::update();
  Alerts::update();
  Relays::update();
  History::update();
  Cloud::update();
  Gps::update();
  Vehicle::update();
  BlackBox::update();
  WifiMgr::update();
  Web::update();
  ArduinoOTA.handle();

  uint32_t now = millis();
  if (now - lastNetCheck > 2000) {
    lastNetCheck = now;
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    StateLock lk;
    gSys.ip = ip;
  }
  if (rebootDue()) {
    Serial.println("[sys] rebooting");
    delay(100);
    ESP.restart();
  }
#ifdef CM_EMU_SELFTEST
  extern void selftestLoop(); selftestLoop();
#endif
  Health::loopEnd();
#ifdef CM_EMULATOR_TEST
  static uint32_t hb = 0;
  if (millis() - hb > 2000) { hb = millis(); Serial.printf("[hb] %lus heap %u\n", millis() / 1000, ESP.getFreeHeap()); }
#endif
  delay(2);
}
