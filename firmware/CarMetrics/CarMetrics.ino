// CarMetrics v4 - single-ESP32 engine monitor
// https://github.com/rassiek/carmetricsbox
//
// Board: ESP32 Dev Module (4 MB). Partition scheme: uses partitions.csv in this
// folder (two 1.6 MB OTA slots + 576 KB LittleFS for the web app).
//
// Module map:
//   Config.*   settings model, JSON, validation, NVS persistence
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

static uint32_t lastNetCheck = 0;

static void startWifi() {
#ifdef CM_EMULATOR_TEST  // QEMU has no Wi-Fi radio
  esp_netif_init();  // the web server still needs the TCP/IP stack
  Serial.println("[wifi] skipped (emulator build)");
  return;
#endif
  const SystemConfig& s = gConfig.sys;
  bool sta = s.staSsid.length() > 0;
  WiFi.persistent(false);
  WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
  WiFi.setHostname(s.hostname.c_str());
  WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str());
  if (sta) WiFi.begin(s.staSsid.c_str(), s.staPass.c_str());
  StateLock lk;
  gSys.apIp = WiFi.softAPIP().toString();
  Serial.printf("[wifi] AP '%s' at %s%s\n", s.apSsid.c_str(), gSys.apIp.c_str(), sta ? ", joining network too" : "");
}

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
  bool reboot = a.apSsid != b.apSsid || a.apPass != b.apPass || a.staSsid != b.staSsid || a.staPass != b.staPass ||
                a.hostname != b.hostname || a.i2cSda != b.i2cSda || a.i2cScl != b.i2cScl;
  {
    StateLock lk;
    Config old = gConfig;
    gConfig = *next;
    resizeRuntime(old);
    gSys.configRevision++;
  }
  delete next;
  saveConfig(gConfig);
  ArduinoOTA.setPassword(gConfig.sys.adminPass.c_str());
  Sensors::begin();
  Alerts::begin();
  Relays::begin();
  History::begin();
  Display::reconfigure();
  Web::notifyConfigChanged();
  if (reboot) { Serial.println("[cfg] network/I2C settings changed - rebooting"); requestReboot(2000); }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n\nCarMetrics %s booting (reset reason %d)\n", FW_VERSION, (int)esp_reset_reason());
  stateInit();

  loadConfig(gConfig);
  gSensorRt.assign(gConfig.sensors.size(), SensorRuntime());
  gRelayRt.assign(gConfig.relays.size(), RelayRuntime());

  // Outputs first so relays/buzzer sit in a known state as early as possible.
  Relays::begin();
  Alerts::begin();

  if (!LittleFS.begin(false)) {
    Serial.println("[fs] mount failed - formatting");
    LittleFS.begin(true);
  }
  Wire.begin(gConfig.sys.i2cSda, gConfig.sys.i2cScl, 400000);

  Sensors::begin();
  History::begin();
  startWifi();
  startOta();
  Web::begin();
  Display::start();
  Serial.println("[boot] ready");
}

void loop() {
  if (Config* next = takePendingConfig()) applyConfig(next);

  Sensors::update();
  Alerts::update();
  Relays::update();
  History::update();
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
#ifdef CM_EMULATOR_TEST
  static uint32_t hb = 0;
  if (millis() - hb > 2000) { hb = millis(); Serial.printf("[hb] %lus heap %u\n", millis() / 1000, ESP.getFreeHeap()); }
#endif
  delay(2);
}
