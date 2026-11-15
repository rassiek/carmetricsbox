// CarMetrics worker firmware (5.4): a board with its own sensors and actuators that
// works with a main board (paired over ESP-NOW or CAN) or on its own. Lean: no
// cloud, weather, OLED, touch displays or other boards. Builds: tools/cm.sh worker.
//   worker-s3   ESP32-S3, 4 MB+: also keeps chart history (CM_WORKER_PLUS)
//   worker-esp32 / -c3 / -c6: generic boards; relay-x8, kc868-a4, t-relay: ready-made boards
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "Config.h"
#include "State.h"
#include "Sensors.h"
#include "Outputs.h"
#include "Health.h"
#include "Board.h"
#include "WifiMgr.h"
#include "LocalDns.h"
#include "Gps.h"
#include "Discover.h"
#include "Pins.h"
#include "Web.h"
#include "History.h"
#include "Node.h"
#include "WebWorker.h"

SET_LOOP_TASK_STACK_SIZE(10 * 1024);

static void applyConfig(Config* next) {
  const SystemConfig &a = gConfig.sys, &b = next->sys;
  bool wifiChanged = a.networks.size() != b.networks.size();
  bool reboot = a.apSsid != b.apSsid || a.apPass != b.apPass || a.hostname != b.hostname || a.board != b.board ||
                a.canEnabled != b.canEnabled || a.canTx != b.canTx || a.canRx != b.canRx || a.canKbps != b.canKbps ||
                a.i2cSda != b.i2cSda || a.i2cScl != b.i2cScl || a.i2cKhz != b.i2cKhz;
  {
    StateLock lk;
    Config old = gConfig;
    gConfig = *next;
    std::vector<SensorRuntime> rt(gConfig.sensors.size());   // keep readings of sensors that stay
    for (size_t i = 0; i < gConfig.sensors.size(); i++)
      for (size_t j = 0; j < old.sensors.size() && j < gSensorRt.size(); j++)
        if (old.sensors[j].id == gConfig.sensors[i].id && old.sensors[j].type == gConfig.sensors[i].type) rt[i] = gSensorRt[j];
    gSensorRt = rt;
    gRelayRt.resize(gConfig.relays.size());
    gSys.configRevision++;
  }
  delete next;
  saveConfig(gConfig);
  Health::configApplied();
  ArduinoOTA.setPassword(gConfig.sys.adminPass.c_str());
  Sensors::begin();
  Alerts::begin();
  Relays::begin();
  StatusLight::begin();
  Gps::begin();
#ifdef CM_WORKER_PLUS
  History::begin();
#endif
  if (wifiChanged && !reboot) WifiMgr::reconfigure();
  Web::notifyConfigChanged();
  if (reboot) { Serial.println("[cfg] network/board settings changed - rebooting"); requestReboot(2000); }
}

void setup() {
  setFirmwareRole("worker");
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n\nCarMetrics worker %s (%s) booting on %s\n", FW_VERSION, CM_VARIANT, ESP.getChipModel());
  Health::earlyBoot();
  Health::registerTask("loop");
#ifdef CM_WORKER_PLUS
  Health::setHistorySize(History::bytes);
#endif
  stateInit();
  if (!(Health::safeMode() && hasConfigSlot(CfgSlot::GOOD) && loadConfig(gConfig, CfgSlot::GOOD))) loadConfig(gConfig);
  Serial.printf("[board] %s\n", Board::current().name);
  gSensorRt.assign(gConfig.sensors.size(), SensorRuntime());
  gRelayRt.assign(gConfig.relays.size(), RelayRuntime());

  // Outputs first so relays sit in a known state as early as possible.
  Relays::begin();
  Alerts::begin();
  StatusLight::begin();
  Wire.begin(gConfig.sys.i2cSda, gConfig.sys.i2cScl, gConfig.sys.i2cKhz * 1000UL);

  Sensors::begin();
  Gps::begin();
#ifdef CM_WORKER_PLUS
  History::begin();
#endif
  WifiMgr::begin();
  LocalDns::begin();
  Node::begin();
#ifndef CM_EMULATOR_TEST
  ArduinoOTA.setHostname(gConfig.sys.hostname.c_str());
  ArduinoOTA.setPassword(gConfig.sys.adminPass.c_str());
  ArduinoOTA.begin();   // also mDNS: http://<hostname>.local
  MDNS.addService("http", "tcp", 80);
#endif
  WebWorker::begin();
  Serial.println("[boot] ready");
}

void loop() {
  Health::loopStart();
  if (Config* next = takePendingConfig()) applyConfig(next);
  Sensors::update();
  Discover::update();
  Pins::update();
  Alerts::update();
  Relays::update();
  StatusLight::update();
  Gps::update();
#ifdef CM_WORKER_PLUS
  History::update();
#endif
  WifiMgr::update();
  LocalDns::update();
  Node::update();
  Web::update();
#ifndef CM_EMULATOR_TEST
  ArduinoOTA.handle();
#endif
  if (rebootDue()) { Serial.println("[sys] rebooting"); delay(100); ESP.restart(); }
#ifdef CM_EMU_SELFTEST
  extern void workerSelftestLoop(); workerSelftestLoop();
#endif
  Health::loopEnd();
  delay(2);
}
