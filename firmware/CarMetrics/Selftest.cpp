// Emulator self-test (QEMU). Only compiled with -DCM_EMU_SELFTEST; not part of normal builds.
#ifdef CM_EMU_SELFTEST
#include "State.h"
#include "Display.h"
#include "History.h"
#include "Outputs.h"
#include "Cloud.h"
#include <LittleFS.h>

static int passed = 0, failed = 0;
static void check(bool ok, const char* what, const String& detail = "") {
  (ok ? passed : failed)++;
  Serial.printf("[test] %s %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}

void selftestFakeSensors() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();
  float t = millis() / 1000.0f;
  StateLock lk;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    SensorRuntime& r = gSensorRt[i];
    if (!c.enabled) { r.status = ST_DISABLED; continue; }
    float v = NAN;
    if (c.id == "coolant_temp") v = 101.5f;            // warning
    else if (c.id == "intake_temp") v = 38;
    else if (c.id == "egt") v = 450 + 50 * sinf(t);
    else if (c.id == "boost_pressure") v = 12.4f;
    else if (c.id == "oil_pressure") v = 48;
    else if (c.id == "bat_voltage") v = 14.1f;
    else if (c.id == "cabin_temp") v = 24.6f;
    else if (c.id == "altitude") v = 1661;
    else if (c.id == "gearbox_temp") { r.status = ST_NOT_FOUND; r.value = NAN; continue; }
    else if (c.id == "transfer_case_temp") { r.status = ST_UNASSIGNED; r.value = NAN; continue; }
    else if (c.id == "fuel_pressure") { r.status = ST_FAULT; r.value = NAN; r.volts = 0.02f; continue; }
    r.value = v; r.status = ST_OK; r.lastOkMs = millis();
  }
}

static String cfgJson(const Config& c) {
  JsonDocument d; configToJson(c, d.to<JsonObject>(), true);
  String s; serializeJson(d, s); return s;
}

void selftestLoop() {
  static int step = 0;
  uint32_t now = millis();
  if (step == 0 && now > 9000) {
    step++;
    Serial.println("[test] --- frame: default config, SSD1327 128x128 ---");
    Display::dumpFrame();
    StateLock lk;
    for (size_t i = 0; i < gConfig.sensors.size(); i++)
      Serial.printf("[test] %-20s st=%-10s lvl=%-5s v=%.2f\n", gConfig.sensors[i].id.c_str(), sensorStatusStr(gSensorRt[i].status),
                    alertLevelStr(gSensorRt[i].level), gSensorRt[i].value);
    Serial.printf("[test] engine=%d overall=%s\n", gSys.engineRunning, alertLevelStr(gSys.overallLevel));
    check(gSys.engineRunning, "engine detected running from battery voltage");
    check(gSensorRt[findSensorIndex("coolant_temp")].level == LVL_WARN, "coolant 101.5 -> warn");
    check(gSensorRt[findSensorIndex("gearbox_temp")].level == LVL_FAULT, "missing sensor -> fault");
    check(gSensorRt[findSensorIndex("transfer_case_temp")].level == LVL_OK, "unassigned sensor -> no alert");
    check(gSys.overallLevel == LVL_FAULT, "overall = fault");
    {  // EGT unplugged: NOT FOUND shown, but no alarm (faultAlarm = false by default)
      int e = findSensorIndex("egt");
      gSensorRt[e].status = ST_NOT_FOUND;
      bool silent = !gConfig.sensors[e].faultAlarm;
      check(silent && gConfig.sensors[e].offset == 0, "EGT defaults: no fault alarm, no offset");
    }
  }
  if (step == 1 && now > 25000) {
    step++;
    Config base; { StateLock lk; base = gConfig; }
    String err;
    // --- validation must reject bad configs
    { Config c = base; c.sensors[findSensorIndex("boost_pressure")].pin = 25; check(!validateConfig(c, err), "ADC on GPIO25 rejected", err); }
    { Config c = base; c.relays[0].enabled = true; c.relays[0].pin = 34; check(!validateConfig(c, err), "relay on input-only GPIO34 rejected", err); }
    { Config c = base; c.relays[0].enabled = true; c.relays[0].pin = 18; check(!validateConfig(c, err), "relay on buzzer pin rejected", err); }
    { Config c = base; c.sensors.push_back(c.sensors[0]); check(!validateConfig(c, err), "duplicate id rejected", err); }
    { Config c = base; c.sys.engineSensorId = "nope"; check(!validateConfig(c, err), "unknown engine sensor rejected", err); }
    {
      JsonDocument d; deserializeJson(d, "[{\"id\":\"x\",\"type\":\"warp_drive\"}]");
      std::vector<SensorConfig> v; check(!sensorsFromJson(d.as<JsonVariantConst>(), v, err), "unknown sensor type rejected", err);
    }
    {
      JsonDocument d; deserializeJson(d, "[{\"id\":\"Bad Id!\",\"type\":\"adc\"}]");
      std::vector<SensorConfig> v; check(!sensorsFromJson(d.as<JsonVariantConst>(), v, err), "bad id rejected", err);
    }
    {  // two MAX6675 may share SCK/SO but not CS
      Config c = base; SensorConfig t2 = c.sensors[findSensorIndex("egt")]; t2.id = "egt2"; t2.pin = 13;
      c.sensors.push_back(t2);
      check(validateConfig(c, err), "second MAX6675 sharing SCK/SO accepted", err);
    }
    {  // MCP9600 address range
      JsonDocument d; deserializeJson(d, "[{\"id\":\"egt\",\"type\":\"mcp9600\",\"mcpAddress\":72}]");
      std::vector<SensorConfig> v; check(!sensorsFromJson(d.as<JsonVariantConst>(), v, err), "MCP9600 bad address rejected", err);
      JsonDocument d2; deserializeJson(d2, "[{\"id\":\"egt\",\"type\":\"mcp9600\",\"mcpAddress\":103,\"unit\":\"C\"}]");
      check(sensorsFromJson(d2.as<JsonVariantConst>(), v, err) && v[0].type == SensorType::MCP9600 && v[0].mcpAddress == 0x67, "MCP9600 sensor parsed", err);
    }
    check(validateConfig(base, err), "default config valid", err);
    // --- JSON round trip
    {
      JsonDocument d; configToJson(base, d.to<JsonObject>(), true);
      Config back; configDefaults(back); back.sensors.clear(); back.relays.clear();
      bool ok = configFromJson(d.as<JsonVariantConst>(), back, err);
      check(ok && cfgJson(back) == cfgJson(base), "config JSON round trip", err);
    }
    // --- apply a change through the same path the web API uses
    Config* next = new Config(base);
    SensorConfig fp;
    fp.id = "fuel_pressure"; fp.name = "Fuel pressure"; fp.unit = "psi"; fp.type = SensorType::ADS1115;
    fp.adsAddress = 0x48; fp.adsChannel = 1; fp.divider = 1.5f; fp.inMin = 0.5f; fp.inMax = 4.5f; fp.outMin = 0; fp.outMax = 100;
    fp.faultLowV = 0.2f; fp.oledStyle = OledStyle::TEXT; fp.oledLabel = "F";
    next->sensors.push_back(fp);
    next->relays[0].enabled = true; next->relays[0].pin = 26;
    next->sys.oledType = "ssd1306_128x64";
    check(validateConfig(*next, err), "modified config valid", err);
    requestConfigApply(next);
    Serial.println("[test] config change requested");
  }
  if (step == 2 && now > 28000) {
    step++;
    Config fromNvs;
    bool loaded = loadConfig(fromNvs);
    String a, b; { StateLock lk; a = cfgJson(gConfig); } b = cfgJson(fromNvs);
    check(loaded && a == b, "saved config reloads identically from NVS");
    { StateLock lk; check(findSensorIndex("fuel_pressure") >= 0 && gSensorRt.size() == gConfig.sensors.size(), "new sensor applied, runtime resized"); }
    { StateLock lk; check(gSys.configRevision == 1, "config revision bumped", String(gSys.configRevision)); }
    Serial.println("[test] --- frame: SSD1306 128x64 + fuel pressure fault ---");
    Display::dumpFrame();
  }
  if (step == 3 && now > 32000) {
    step++;
    Display::dumpFrame();   // next OLED page (pages rotate every 4 s)
  }
  if (step == 4 && now > 40000) {
    step++;
    Alerts::mute(60000);
    { StateLock lk; check(gSys.muted, "mute works"); }
    String h; { StateLock lk; struct S : Print { String* s; size_t write(uint8_t c) override { *s += (char)c; return 1; } } p; p.s = &h; History::writeJson(p); }
    check(h.startsWith("{\"interval\":5") && h.indexOf("\"coolant_temp\":[") > 0, "history JSON", h.substring(0, 90));
  }
  if (step == 5 && now > 41000) {
    step++;
    String err = Relays::control("relay1", 1, -1);
    { StateLock lk; check(err.length() == 0 && gRelayRt[0].on && gRelayRt[0].outputHigh, "relay switched on", err); }
    err = Relays::control("relay2", 1, -1);
    check(err.length() > 0, "disabled relay refused", err);
  }
  if (step == 6 && now > 43000) {
    step++;
    String influx, zabbix; uint32_t recs;
    Cloud::selftestEncode(influx, zabbix, recs);
    Serial.println("[test] influx payload:\n" + influx);
    Serial.println("[test] zabbix payload: " + zabbix.substring(0, 400));
    check(recs == 2, "chunk round trip: 2 records decoded", String(recs));
    check(influx.startsWith("carmetrics,device=carmetrics coolant_temp=90,coolant_temp_min=89,coolant_temp_max=91.5"), "influx line protocol");
    check(influx.indexOf("\n") > 0 && influx.indexOf("coolant_temp=91,") > 0, "influx second record");
    check(zabbix.startsWith("{\"request\":\"sender data\",\"data\":[{\"host\":\"my car\",\"key\":\"carmetrics.json\",\"value\":\"{\\\"coolant_temp\\\":[90,89,91.5]"), "zabbix packed payload");
    JsonDocument zd;
    check(!deserializeJson(zd, zabbix) && zd["data"].size() == 2, "zabbix payload is valid JSON with 2 items");
    // offline buffering: enable cloud, no Wi-Fi in the emulator -> chunks must land on flash
    Config* next; { StateLock lk; next = new Config(gConfig); }
    next->cloud.enabled = true; next->cloud.protocol = "zabbix"; next->cloud.zbxServer = "zabbix.invalid";
    next->cloud.intervalMs = 250; next->cloud.uploadEverySec = 10; next->cloud.onlyEngineRunning = false;
    requestConfigApply(next);
  }
  if (step == 7 && now > 100000) {
    step++;
    StateLock lk;
    Serial.printf("[test] cloud: recording=%d ram=%u backlogFiles=%u backlogBytes=%u pending=%u dropped=%u err=%s fsFree=%u\n", gNet.recording, gNet.ramChunks, gNet.backlogFiles, gNet.backlogBytes, gNet.pendingRecords, gNet.droppedRecords, gNet.lastError.c_str(), (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
    check(gNet.recording, "recorder active");
    check(gNet.backlogFiles >= 1 && gNet.backlogBytes > 0, "offline data written to flash ring buffer");
    Serial.printf("[test] DONE: %d passed, %d failed\n", passed, failed);
  }
}
#endif
