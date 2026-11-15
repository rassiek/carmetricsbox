// Worker emulator self-test (QEMU, classic ESP32). Only compiled with -DCM_EMU_SELFTEST:
// tools/cm.sh selftest-worker. Checks what makes a worker a worker: no car defaults,
// runs alone (its own sensors, alerts and rules), settings round trip, no file system.
#ifdef CM_EMU_SELFTEST
#include "State.h"
#include "Config.h"
#include "Board.h"
#include "Outputs.h"
#include "Health.h"
#include "Pins.h"
#include "Node.h"
#include <esp_ota_ops.h>

static int passed = 0, failed = 0;
static void check(bool ok, const char* what, const String& detail = "") {
  (ok ? passed : failed)++;
  Serial.printf("[test] %s %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}

void selftestFakeSensors() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();
  StateLock lk;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    SensorRuntime& r = gSensorRt[i];
    if (!c.enabled) { r.status = ST_DISABLED; continue; }
    if (c.id == "tank_temp") { r.value = 92; r.status = ST_OK; r.lastOkMs = millis(); }
    else if (c.id == "tank_level") { r.value = NAN; r.status = ST_NOT_FOUND; }
  }
}


// The firmware image carries its role tag (checked before a file is installed or sent on).
static String __attribute__((noinline)) ownImageRole() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  RoleScan rs; uint8_t buf[512];
  for (uint32_t off = 0; run && off < ESP.getSketchSize() && !rs.role.length(); off += sizeof(buf)) {
    if (esp_partition_read(run, off, buf, sizeof(buf)) != ESP_OK) break;
    rs.feed(buf, sizeof(buf));
  }
  return rs.role;
}

static String cfgJson(const Config& c) {
  JsonDocument d; configToJson(c, d.to<JsonObject>(), true);
  String s; serializeJson(d, s); return s;
}

static void __attribute__((noinline)) bootChecks() {
  String err;
  StateLock lk;
  check(gFirmwareRole == "worker" && gConfig.sys.role == "worker", "firmware role is worker", gConfig.sys.role);
  check(gConfig.sensors.empty(), "worker starts without the main board's car sensors", String(gConfig.sensors.size()));
  check(String(Board::current().id) == "esp32_devkit", "board auto-detected as ESP32 DevKit", Board::current().id);
  check(validateConfig(gConfig, err), "worker default settings valid", err);
  JsonDocument n; Node::toJson(n.to<JsonObject>());
  String js; serializeJson(n, js);
  check(String(n["role"] | "") == "worker" && !(n["node"]["paired"] | true), "node: worker, not paired yet", js);
  JsonDocument h; Health::toJson(h.to<JsonObject>());
  check((h["memory"]["largest"] | 0) > 10000, "health works without a file system partition", String((int)(h["memory"]["largest"] | 0)));
  {   // built at run time so the test itself puts no tag in the image
    String k = String("CM") + "ROLE:", js = "xx" + k + "|" + k + k + "worker|";
    std::vector<uint8_t> t(js.begin(), js.end()); t[2 + k.length()] = 0;
    RoleScan a; a.feed(t.data(), 5); a.feed(t.data() + 5, t.size() - 5);
    check(a.role == "worker", "role tag found across pieces, decoys skipped", a.role); }
  JsonDocument p; Pins::toJson(p.to<JsonObject>());
  check(p["pins"].size() > 20, "pin map available", String(p["pins"].size()));
}

// Its own sensors and a pump on a rule: the worker alerts and switches without a main board.
static void __attribute__((noinline)) standaloneSetup() {
  Config* next; { StateLock lk; next = new Config(gConfig); }
  SensorConfig t; t.id = "tank_temp"; t.name = "Tank"; t.unit = "°C"; t.type = SensorType::ADC; t.pin = 34;
  t.warnHigh = 90; t.critHigh = 98;
  SensorConfig l = t; l.id = "tank_level"; l.name = "Level"; l.unit = "%"; l.pin = 35; l.warnHigh = l.critHigh = NAN;
  next->sensors.push_back(t); next->sensors.push_back(l);
  RelayConfig pump; pump.id = "pump"; pump.name = "Pump"; pump.pin = 26;
  pump.rule = RuleType::THRESHOLD; pump.ruleSensor = "tank_temp"; pump.onAt = 88; pump.offAt = 82;
  RelayConfig light; light.id = "light"; light.name = "Light"; light.pin = 27; light.safeState = 1;
  next->relays.push_back(pump); next->relays.push_back(light);
  String err;
  check(validateConfig(*next, err), "worker settings with sensors + actuators valid", err);
  { Config c = *next; c.relays.back().pin = 34; check(!validateConfig(c, err), "actuator on input-only GPIO34 rejected", err); }
  {   // the same settings as JSON (what the web app or the main board sends)
    Config back; JsonDocument d; err = ""; configToJson(*next, d.to<JsonObject>(), true);
    bool ok = configFromJson(d.as<JsonVariantConst>(), back, err);
    check(ok && cfgJson(back) == cfgJson(*next), "settings JSON round trip", err);
  }
  requestConfigApply(next);
}

static void __attribute__((noinline)) standaloneChecks() {
  {
    StateLock lk;
    int t = findSensorIndex("tank_temp"), l = findSensorIndex("tank_level"), p = findRelayIndex("pump");
    check(t >= 0 && gSensorRt[t].level == LVL_WARN, "own sensor 92 above 90 -> warn", t >= 0 ? alertLevelStr(gSensorRt[t].level) : "missing");
    check(l >= 0 && gSensorRt[l].level == LVL_FAULT, "missing sensor -> fault", l >= 0 ? alertLevelStr(gSensorRt[l].level) : "missing");
    check(p >= 0 && gRelayRt[p].on && !strcmp(gRelayRt[p].by, "rule"), "pump switched on by its own rule", p >= 0 ? String(gRelayRt[p].on) + " " + gRelayRt[p].by : "missing");
    check(gSys.configRevision >= 1, "settings applied", String(gSys.configRevision));
  }
  String err = Relays::control("light", 1, -1);
  { StateLock lk; int i = findRelayIndex("light"); check(err.isEmpty() && i >= 0 && gRelayRt[i].on, "manual switch from the worker's own page", err); }
  err = Relays::control("light", 0, -1);
  check(err.isEmpty(), "switched off again", err);
  Config saved; bool loaded = loadConfig(saved);
  String a = cfgJson(saved), b; { StateLock lk; b = cfgJson(gConfig); }
  check(loaded && a == b, "saved settings reload identically from NVS");
}

// Custom board definition: pins as text, checked against the chip, saved, usable as the board type.
static void __attribute__((noinline)) customBoardChecks() {
  String err;
  auto tryDef = [&](const char* js) { JsonDocument d; deserializeJson(d, js); err = ""; return Board::setCustom(d.as<JsonVariantConst>(), err); };
  check(!Board::custom(), "no custom board at first");
  check(!tryDef(R"({"name":"Bad","valid":"2,6,7"})") && err.indexOf("flash") >= 0, "flash pins refused", err);
  check(!tryDef(R"({"name":"Bad","valid":"2,4,34","output":"34"})") && err.indexOf("output") >= 0, "output on an input-only pin refused", err);
  check(!tryDef(R"({"name":"Bad","chip":"esp32s3","valid":"1-10"})") && err.indexOf("chip") >= 0, "other chip refused", err);
  check(!tryDef(R"({"name":"Bad","valid":"2,4","defaults":{"ledPin":5}})") && err.indexOf("ledPin") >= 0, "default pin not on the board refused", err);
  const char* good = R"({"name":"Pump board v2","flashMB":4,"valid":"2, 4, 5, 13-19, 21-23, 25-27, 32-36","output":[2,4,5,13,14,16,17,18,19,21,22,23,25,26,27,32,33],)"
                     R"("adc1":"32-36","rtc":"32-36","defaults":{"i2cSda":21,"i2cScl":22,"oneWirePin":4,"ledPin":2},"fixed":{"relays":[16,17],"relaysActiveLow":true},"notes":"Home-made"})";
  check(tryDef(good), "custom board accepted (pins as text or lists)", err);
  const BoardProfile* c = Board::custom();
  check(c && String(c->name) == "Pump board v2" && Board::has(c->adc1, 35) && !Board::has(c->output, 34) && c->fixed.nRelays == 2 && Board::has(c->caution, 2),
        "custom board read back (boot pin 2 still marked)", c ? Board::pinList(c->valid) : "");
  JsonDocument l; Board::listToJson(l.to<JsonArray>());
  bool listed = false; for (JsonObject o : l.as<JsonArray>()) listed |= String(o["id"] | "") == "custom";
  check(listed && Board::find("custom") == c, "custom board listed with the built-in ones");
  { Config k; configDefaults(k, *c); k.sys.board = "custom";
    check(validateConfig(k, err) && k.relays.size() == 2 && k.relays[0].pin == 16 && k.relays[0].activeLow && k.sys.i2cSda == 21, "defaults from the custom board valid", err); }
  { Config k; { StateLock lk; k = gConfig; } k.sys.board = "custom"; k.relays.push_back(RelayConfig()); k.relays.back().id = "x"; k.relays.back().pin = 12;
    check(!validateConfig(k, err), "custom board: pin it doesn't bring out refused", err); }
  String js; if (c) { JsonDocument e; Board::profileToJson(*c, e.to<JsonObject>()); serializeJson(e, js); }
  check(Board::clearCustom(err) && !Board::custom(), "custom board removed", err);
  JsonDocument back; deserializeJson(back, js);
  check(Board::setCustom(back.as<JsonVariantConst>(), err), "exported definition imports again", err);
  String saved; check(loadCustomBoard(saved) && saved.indexOf("Pump board v2") > 0, "custom board kept in NVS");
}

static void imageRoleCheck() { String r = ownImageRole(); check(r == "worker", "this firmware's image is tagged worker", r); }

void workerSelftestLoop() {
  static int step = 0;
  uint32_t now = millis();
  if (step == 0 && now > 8000) { step++; bootChecks(); customBoardChecks(); standaloneSetup(); }
  if (step == 1 && now > 16000) { step++; standaloneChecks(); imageRoleCheck(); Serial.printf("[test] DONE: %d passed, %d failed\n", passed, failed); }
}
#endif
