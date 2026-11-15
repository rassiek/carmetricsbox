// Emulator self-test (QEMU). Only compiled with -DCM_EMU_SELFTEST; not part of normal builds.
#ifdef CM_EMU_SELFTEST
#include "State.h"
#include "WifiMgr.h"
#include "Proto.h"
#include "Boards.h"
#include "Display.h"
#include "History.h"
#include "Outputs.h"
#include "Cloud.h"
#include "Board.h"
#include "Health.h"
#include "Pins.h"
#include "Gps.h"
#include "BlackBox.h"
#include <LittleFS.h>
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
  float t = millis() / 1000.0f;
  StateLock lk;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    SensorRuntime& r = gSensorRt[i];
    if (!c.enabled) { r.status = ST_DISABLED; continue; }
    if (c.type == SensorType::COMPUTED || c.type == SensorType::GPS) continue;   // real code paths
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

// 5.3 actuators, kept out of selftestLoop() so its stack frame stays small.
static void __attribute__((noinline)) actuatorSetup() {
  String err;
  // 5.3 actuators: a PWM fan on a coolant threshold, an H-bridge, a servo
  Config* next; { StateLock lk; next = new Config(gConfig); }
  RelayConfig f; f.id = "fan"; f.name = "Fan"; f.type = ActType::PWM; f.pin = 2; f.freqHz = 25000;
  f.minDuty = 20; f.maxDuty = 80; f.rule = RuleType::THRESHOLD; f.ruleSensor = "coolant_temp"; f.onAt = 95; f.offAt = 90;
  RelayConfig h; h.id = "winch"; h.name = "Winch"; h.type = ActType::HBRIDGE; h.pin = 13; h.pin2 = 14; h.kickMs = 300;
  RelayConfig v; v.id = "flap"; v.name = "Flap"; v.type = ActType::SERVO; v.pin = 23; v.servoMinDeg = 10; v.servoMaxDeg = 170;
  next->relays.push_back(f); next->relays.push_back(h); next->relays.push_back(v);
  { JsonDocument j; JsonArray a = j.to<JsonArray>(); relayToJson(f, a.add<JsonObject>()); a[0]["offAt"] = 95;
    std::vector<RelayConfig> rv; check(!relaysFromJson(j.as<JsonVariantConst>(), rv, err), "threshold with on = off rejected", err); }
  { Config c = *next; c.relays[c.relays.size() - 2].pin2 = -1; check(!validateConfig(c, err), "H-bridge without its second pin rejected", err); }
  { Config c = *next; RelayConfig x = f; x.id = "p15"; x.pin = 15; x.rule = RuleType::MANUAL; c.relays.push_back(x);
    c.relays[0].enabled = true; c.relays[0].type = ActType::PWM;   // relay1 as PWM
    c.sys.rgbExternal = true;   // 4 + 1 + 1 + 3 = 9
    check(!validateConfig(c, err) && err.indexOf("8 PWM") >= 0, "more than 8 PWM channels rejected", err); }
  check(validateConfig(*next, err), "fan / winch / flap settings valid", err);
  // Ready-made relay boards
  {
    const BoardProfile* x8 = Board::find("relay_x8");
    setFirmwareRole("worker");   // what the worker firmware does
    Config c; configDefaults(c, *x8); c.sys.board = "relay_x8";
    check(x8 && c.relays.size() == 8 && c.relays[6].pin == 12 && c.relays[6].enabled && c.sensors.empty() && c.sys.role == "worker",
          "Relay X8 defaults (worker): 8 relays (relay 7 on GPIO12), no sensors", String(c.relays.size()) + " " + c.sys.role);
    check(validateConfig(c, err), "Relay X8 defaults valid (GPIO12 allowed as an output there)", err);
    const BoardProfile* kc = Board::find("kc868_a4");
    Config k; configDefaults(k, *kc); k.sys.board = "kc868_a4";
    check(kc && k.relays.size() == 4 && k.relays[0].pin == 2 && validateConfig(k, err), "KC868-A4 defaults valid", err);
    setFirmwareRole("main");
  }
  { JsonDocument j; StateLock lk; Pins::toJson(j.to<JsonObject>());
    bool ok = j["pins"].size() > 20; for (JsonObject p : j["pins"].as<JsonArray>()) if (p["gpio"] == 18) ok = ok && String((const char*)(p["usedBy"] | "")) == "buzzer";
    check(ok, "pin map lists the board's GPIOs and what uses them", String(j["pins"].size()));
    JsonDocument r; check(Pins::test(18, "blink", r.to<JsonObject>()).length() > 0, "pin test refuses a pin in use"); }
  requestConfigApply(next);
}

static void __attribute__((noinline)) actuatorCommands() {
  int fi = findRelayIndex("fan");
  { StateLock lk; check(fi >= 0 && fabsf(gRelayRt[fi].output - 80) < 0.1f && !strcmp(gRelayRt[fi].by, "rule"), "fan: coolant 101.5 above 95 -> rule runs it at its 80 % limit", fi >= 0 ? String(gRelayRt[fi].output) + " " + gRelayRt[fi].by : "missing"); }
  JsonDocument d; d["duty"] = 40; d["forMinutes"] = 5;
  String e1 = Relays::command("fan", d.as<JsonVariantConst>());
  JsonDocument d2; d2["direction"] = "reverse"; d2["speed"] = 50;
  String e2 = Relays::command("winch", d2.as<JsonVariantConst>());
  JsonDocument d3; d3["angle"] = 175;
  String e3 = Relays::command("flap", d3.as<JsonVariantConst>());
  check(e1.isEmpty() && e2.isEmpty(), "fan duty / winch reverse commands accepted", e1 + e2);
  check(e3.length() > 0, "servo angle outside its range refused", e3);
  d3["angle"] = 90; Relays::command("flap", d3.as<JsonVariantConst>());
  { StateLock lk; int w = findRelayIndex("winch"); check(w >= 0 && gRelayRt[w].output == 0, "winch not moved before update()"); }
  delay(50); Relays::update();
  { StateLock lk; int w = findRelayIndex("winch"); check(fabsf(gRelayRt[w].output + 100) < 0.1f, "winch kick-start: full power in reverse first", String(gRelayRt[w].output)); }
}

void selftestLoop() {
  static uint32_t hwmAt = 0;
  if (millis() - hwmAt > 30000) { hwmAt = millis(); Serial.printf("[test] loop stack free %u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL)); }
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
    // --- board profiles
    check(String(Board::current().id) == "esp32_devkit", "board auto-detected as ESP32 DevKit", Board::current().id);
    {
      Config c = base; c.sys.board = "esp32_wrover";
      bool rejected = !validateConfig(c, err);
      check(rejected, "WROVER: EGT pins on PSRAM GPIO16/17 rejected", err);
      applyBoardPins(c, Board::resolve("esp32_wrover"));
      check(validateConfig(c, err) && c.sensors[findSensorIndex("egt")].soPin == 25, "WROVER: board default pins valid", err);
    }
    {
      JsonDocument d; deserializeJson(d, "{\"board\":\"esp32s3_n8r8\"}");
      SystemConfig sc = base.sys; check(!systemFromJson(d.as<JsonVariantConst>(), sc, err), "S3 profile refused by classic-ESP32 firmware", err);
    }
    check(Board::pinList(Board::current().adc1) == "32-39", "ADC1 pin list", Board::pinList(Board::current().adc1));
    // --- phase 1 parsing
    {
      std::vector<std::pair<float, float>> c;
      check(parseCurve("0.5:0, 2.5:40,4.5:100", c) && fabsf(curveLookup(c, 1.5f) - 20) < 0.01f && curveLookup(c, 9) == 100, "curve parse + lookup");
      check(!parseCurve("2:1,1:2", c) && !parseCurve("1:2", c) && !parseCurve("a:b,c:d", c), "bad curves rejected");
      JsonDocument d; deserializeJson(d, "{\"staSsid\":\"Phone\",\"staPass\":\"secret123\",\"networks\":[{\"ssid\":\"Home\",\"pass\":\"homepass1\",\"priority\":8}]}");
      SystemConfig sc = base.sys;
      bool ok = systemFromJson(d.as<JsonVariantConst>(), sc, err);
      check(ok && sc.networks.size() == 2 && sc.networks[0].ssid == "Home" && sc.networks[1].ssid == "Phone" && sc.networks[1].pass == "secret123", "Wi-Fi list + v4 network migrated", err);
      JsonDocument d2; deserializeJson(d2, "{\"networks\":[{\"ssid\":\"Home\",\"pass\":\"\",\"priority\":3}]}");
      check(systemFromJson(d2.as<JsonVariantConst>(), sc, err) && sc.networks.size() == 1 && sc.networks[0].pass == "homepass1" && sc.networks[0].priority == 3, "blank Wi-Fi password keeps the saved one", err);
      check(String(WifiMgr::reasonText(15)).indexOf("password") >= 0 && String(WifiMgr::reasonText(201)) == "network not found", "Wi-Fi reason codes in plain words");
      JsonDocument w; { StateLock lk; WifiMgr::toJson(w.to<JsonObject>()); }
      check(w["log"].is<JsonArray>() && w["scan"].is<JsonArray>() && w["remembered"].is<JsonObject>(), "Wi-Fi join log JSON");
      {   // other boards' sensors mirrored on the main board
        std::vector<Boards::ShareInfo> infos = {{1, "Engine bay", "{\"sensors\":[{\"id\":\"coolant\",\"name\":\"Coolant\",\"type\":\"ds18b20\",\"unit\":\"\u00b0C\",\"warnHigh\":100,\"critHigh\":105,\"pin\":4},"
                                                   "{\"id\":\"oil\",\"name\":\"Oil\",\"type\":\"adc\",\"pin\":34}],\"relays\":[]}"}};
        std::vector<SensorConfig> cur = gConfig.sensors;
        SensorConfig old; old.id = "b1_coolant"; old.type = SensorType::REMOTE; old.remoteBoard = 1; old.oledStyle = OledStyle::MARKER; old.oledLabel = "EBC";
        cur.push_back(old);
        auto m = Boards::buildMirrors(infos, cur);
        bool ok = m.size() == 2 && m[0].id == "b1_coolant" && m[0].name == "Engine bay Coolant" && m[0].critHigh == 105 && m[0].pin == -1 &&
                  m[0].oledStyle == OledStyle::MARKER && m[0].oledLabel == "EBC" && m[1].remoteIndex == 1 && m[1].oledStyle == OledStyle::OFF;
        Config c2 = gConfig; for (auto& x : m) c2.sensors.push_back(x);
        String e2; bool valid = validateConfig(c2, e2);
        check(ok && valid, "other boards' sensors mirrored (thresholds, ids, OLED choices kept)", e2);
      }
      {   // board-to-board protocol on the chip itself (the full test runs on a PC: tools/cm.sh prototest)
        uint8_t fr[Proto::MAX_FRAME]; Proto::Header h; h.src = 2; h.seq = 9;
        Proto::Points pm; pm.count = Proto::MAX_POINTS_PER_FRAME;
        for (int i = 0; i < pm.count; i++) { pm.points[i].value = i * 0.5f; pm.points[i].level = i % 3; }
        size_t n = Proto::encodePoints(fr, h, pm);
        Proto::Header h2; const uint8_t* p; size_t pn; Proto::Points d;
        bool ok = n == 248 && !Proto::parse(fr, n, h2, p, pn) && Proto::decodePoints(p, pn, d) && d.count == pm.count && d.points[38].value == 19.0f;
        check(ok && Proto::crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "board-to-board frames encode and decode on the ESP32", String(n) + " bytes");
      }
      Config c2 = base; SensorConfig r; r.id = "rpm"; r.type = SensorType::RPM; r.pin = 18; c2.sensors.push_back(r);
      check(!validateConfig(c2, err), "RPM on the buzzer pin rejected", err);
      Config c3 = base; c3.sys.ignitionPin = 23; c3.sys.sleepAfterMinutes = 10;
      check(!validateConfig(c3, err), "ignition wake needs an RTC GPIO", err);
      c3.sys.ignitionPin = 27; check(validateConfig(c3, err), "ignition on RTC GPIO27 accepted", err);
    }
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
    {  // v5 phase 1: computed + GPS sensors, turbo timer, OLED layout
      SensorConfig cb; cb.id = "boost_minus_oil"; cb.name = "Boost - oil"; cb.type = SensorType::COMPUTED;
      cb.formula = "diff"; cb.srcA = "boost_pressure"; cb.srcB = "oil_pressure"; cb.oledStyle = OledStyle::OFF; cb.smoothing = 1;
      next->sensors.push_back(cb);
      SensorConfig cm = cb; cm.id = "oil_margin"; cm.formula = "curve_margin"; cm.srcA = "oil_pressure"; cm.srcB = "egt"; cm.curve = "400:20,500:40";
      next->sensors.push_back(cm);
      SensorConfig gs; gs.id = "gps_speed"; gs.name = "Speed"; gs.type = SensorType::GPS; gs.field = "speed"; gs.faultAlarm = false; gs.oledStyle = OledStyle::OFF;
      next->sensors.push_back(gs);
      next->sys.gpsRxPin = 25;
      next->turbo.enabled = true; next->turbo.egtId = "egt"; next->turbo.safeEgt = 200;
      LayoutTile lt; lt.id = "egt"; lt.size = 2; lt.widget = "spark"; next->layout.tiles.push_back(lt);
      next->layout.oled = {"egt", "|", "coolant_temp", "boost_pressure"};
    }
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
    {
      bool ok = Gps::selftestParse("$GPRMC,083559.00,A,0117.18240,S,03649.03320,E,24.30,77.5,311026,,,A*74");
      Gps::selftestParse("$GPGGA,083559.00,0117.18240,S,03649.03320,E,1,08,1.01,1661.2,M,-14.2,M,,*51");
      StateLock lk;
      const GpsRuntime& g = gSys.gps;
      Serial.printf("[test] gps fix=%d lat=%.5f lon=%.5f speed=%.1f alt=%.1f sats=%u\n", g.fix, g.lat, g.lon, g.speedKmh, g.altM, g.sats);
      check(ok && g.fix && fabs(g.lat + 1.28637) < 1e-4 && fabs(g.lon - 36.81722) < 1e-4 && fabsf(g.speedKmh - 45.0f) < 0.1f && g.sats == 8 && fabsf(g.altM - 1661.2f) < 0.1f, "GPS RMC + GGA parsed");
      check(!Gps::selftestParse("$GPRMC,083559.00,A,0117.18240,S,03649.03320,E,24.30,77.5,311026,,,A*00"), "GPS bad checksum rejected");
    }
    Serial.println("[test] --- frame: SSD1306 128x64 + fuel pressure fault ---");
    Display::dumpFrame();
  }
  if (step == 3 && now > 32000) {
    step++;
    {
      StateLock lk;
      int a = findSensorIndex("boost_minus_oil"), m = findSensorIndex("oil_margin"), g = findSensorIndex("gps_speed");
      Serial.printf("[test] computed: diff=%.2f (%s) margin=%.2f gps=%.1f (%s) turbo phase=%u egt=%.0f\n", gSensorRt[a].value, sensorStatusStr(gSensorRt[a].status),
                    gSensorRt[m].value, gSensorRt[g].value, sensorStatusStr(gSensorRt[g].status), gSys.turbo.phase, gSys.turbo.egt);
      check(gSensorRt[a].status == ST_OK && fabsf(gSensorRt[a].value - (12.4f - 48)) < 0.01f, "computed diff: boost - oil");
      float egt = gSensorRt[findSensorIndex("egt")].value, expect = 48 - (20 + (egt - 400) * 20 / 100);
      check(gSensorRt[m].status == ST_OK && fabsf(gSensorRt[m].value - expect) < 5, "computed curve margin", String(gSensorRt[m].value) + " vs " + String(expect));
      check(gSensorRt[g].status == ST_OK || gSensorRt[g].status == ST_NOT_FOUND, "GPS sensor follows the module");
      check(gSys.turbo.phase == TURBO_COOLING, "turbo timer: EGT above safe -> cooling");
    }
    Display::dumpFrame();   // next OLED page (pages rotate every 4 s)
  }
  if (step == 4 && now > 40000) {
    step++;
    Alerts::mute(60000);
    { StateLock lk; check(gSys.muted, "mute works"); }
    String h; { StateLock lk; struct S : Print { String* s; size_t write(uint8_t c) override { *s += (char)c; return 1; } } p; p.s = &h; History::writeJson(p); }
    check(h.startsWith("{\"interval\":5") && h.indexOf("\"coolant_temp\":[") > 0, "history JSON", h.substring(0, 90));
    {   // the chunked reader sends exactly the same JSON, whatever the piece size
      String c; uint8_t b[37]; size_t n;
      { StateLock lk; History::Reader rd; while ((n = rd.read(b, sizeof(b))) > 0) c.concat((const char*)b, n); }
      auto noAge = [](String j) { int a = j.indexOf("\"ageMs\":"), e = j.indexOf(',', a); return a < 0 ? j : j.substring(0, a) + j.substring(e + 1); };
      check(noAge(c) == noAge(h) && c.length() > 200, "history streamed in pieces = same JSON", String(c.length()) + " bytes, " + String(History::bytes()) + " B RAM");
    }
  }
  if (step == 5 && now > 41000) {
    step++;
    String err = Relays::control("relay1", 1, -1);
    { StateLock lk; check(err.length() == 0 && gRelayRt[0].on && gRelayRt[0].outputHigh, "relay switched on", err); }
    err = Relays::control("relay2", 1, -1);
    check(err.length() > 0, "disabled relay refused", err);
    actuatorSetup();
  }
  static bool actDone = false;
  if (step == 6 && now > 42000 && !actDone) { actDone = true; actuatorCommands(); }
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
    next->cloud.intervalMs = 1000; next->cloud.uploadEverySec = 10; next->cloud.onlyEngineRunning = false;
    next->cloud.sendSystem = true;
    requestConfigApply(next);
  }
  if (step == 7 && now > 100000) {
    step++;
    {
      StateLock lk;
      int fi = findRelayIndex("fan"), w = findRelayIndex("winch"), v = findRelayIndex("flap");
      check(fi >= 0 && fabsf(gRelayRt[fi].output - 40) < 0.1f && gRelayRt[fi].manual, "fan: timed manual 40 % kept through a settings change", fi >= 0 ? String(gRelayRt[fi].output) : "");
      check(w >= 0 && fabsf(gRelayRt[w].output + 50) < 0.1f, "winch: after the kick, reverse 50 %", w >= 0 ? String(gRelayRt[w].output) : "");
      check(v >= 0 && fabsf(gRelayRt[v].output - 90) < 0.1f && fabsf(Relays::value(v) - 90) < 0.1f, "flap servo at 90 degrees", v >= 0 ? String(gRelayRt[v].output) : "");
    }
    StateLock lk;
    Serial.printf("[test] cloud: recording=%d ram=%u backlogFiles=%u backlogBytes=%u pending=%u dropped=%u err=%s fsFree=%u\n", gNet.recording, gNet.ramChunks, gNet.backlogFiles, gNet.backlogBytes, gNet.pendingRecords, gNet.droppedRecords, gNet.lastError.c_str(), (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
    check(gNet.recording, "recorder active");
    check(gNet.backlogFiles >= 1 && gNet.backlogBytes > 0, "offline data written to flash ring buffer");
    // --- config slots: the cloud change above kept the previous config
    {
      Config prev;
      bool ok = hasConfigSlot(CfgSlot::PREVIOUS) && loadConfig(prev, CfgSlot::PREVIOUS);
      check(ok && !prev.cloud.enabled && gConfig.cloud.enabled, "previous config kept for one-step undo");
    }
    // --- board metrics
    {
      JsonDocument d; Health::toJson(d.to<JsonObject>());
      String js; serializeJson(d, js);
      Serial.println("[test] metrics: " + js.substring(0, 600));
      check(d["cpu"].size() == 2 && !d["cpu"][0].isNull() && !d["cpu"][1].isNull(), "CPU load measured on both cores", js.substring(js.indexOf("\"cpu\""), js.indexOf("\"cpu\"") + 20));
      check(d["tasks"].size() >= 3 && (d["tasks"][0]["stackFree"] | 0) > 0, "task stack headroom reported", String((int)d["tasks"].size()));
      check((d["memory"]["largest"] | 0) > 10000 && (d["loop"]["maxUs"] | 0) > 0, "heap and loop timing reported");
      check(d["uptime"]["history"].size() >= 1 && String(d["firmware"]["core"] | "").startsWith("2."), "reset history and core version");
      check(!d["safeMode"].as<bool>(), "not in safe mode");
    }
    check(!isnan(Health::sysMetric(0)) && Health::sysMetric(0) > 10, "sys_heap_free_kb metric", String(Health::sysMetric(0)));
    {
      JsonDocument d; BlackBox::listToJson(d.to<JsonArray>());
      String js; serializeJson(d, js);
      Serial.println("[test] blackbox: " + js);
      String ev;
      bool ok = d.size() >= 1 && BlackBox::read(d[0]["seq"] | 0, ev);
      JsonDocument e; bool parsed = ok && !deserializeJson(e, ev);
      check(parsed && e["rows"].size() >= 25 && e["rows"].size() <= 61 && String(e["reason"] | "").indexOf("warn") > 0, "alarm black box recorded 30 s before/after", String((int)e["rows"].size()) + " rows");
    }
    { String r = ownImageRole(); check(r == "main", "this firmware's image is tagged main", r); }
    Serial.printf("[test] DONE: %d passed, %d failed\n", passed, failed);
  }
}
#endif
