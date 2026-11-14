#include "Boards.h"
#include "BoardNet.h"
#include "EspNowLink.h"
#include "CanLink.h"
#include "State.h"
#include "Outputs.h"
#include "Board.h"
#include "Health.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_wifi_ap_get_sta_list.h>
#endif
#include <algorithm>
#include <math.h>

using namespace Proto;
using namespace BoardNet;

namespace {
const char* NS = "boards";       // main board: paired boards + their shares (in the "cfg" NVS partition)
const char* NODE_NS = "node";    // extra board: own id + accepted settings revision

EspNowLink radio;      // ESP-NOW (default)
CanLink can;           // CAN (option)
MultiLink links;       // what the sessions use: ESP-NOW and/or CAN
bool useRadio = false, useCan = false, anyLink = false;
MainSession* mainS = nullptr;
NodeSession* nodeS = nullptr;
bool started = false;
uint8_t hopChannel = 1;
uint32_t lastHop = 0;

void syncMirrors();

// ---------------------------------------------------------------- display catalog (main board)
// What a touch display can show and control: every enabled sensor here (local +
// remote mirrors) and every relay (here and on paired boards). Sent to paired
// displays as their "settings" (revision = CRC-32), and over Wi-Fi at
// /api/v2/display/catalog. Values follow in the same order (sensors, then relays).
struct CatRelay { uint8_t board, index; };
struct Catalog {
  String json;
  uint32_t rev = 0;
  std::vector<int> sensors;          // catalog index -> gConfig.sensors index
  std::vector<CatRelay> relays;
  uint32_t builtFor = 0xFFFFFFFF, builtAt = 0;
} cat;
uint32_t boardsVersion = 1;          // bumped when boards / shares change (catalog lists their relays)
uint32_t catBoardsVersion = 0;
uint32_t lastDisplayPush = 0;

// ---------------------------------------------------------------- firmware images
// ESP32 image header: magic 0xE9 at 0, chip id (uint16) at 12; app description
// (version string at +16) right after the first segment header, at 32.
uint8_t chipOfImage(const uint8_t* hdr) {
  uint16_t id = hdr[12] | (hdr[13] << 8);
  return id == 0 ? Proto::CHIP_ESP32 : id == 9 ? Proto::CHIP_ESP32S3 : Proto::CHIP_OTHER;
}
uint8_t myChip() { return String(Board::chip()) == "esp32s3" ? Proto::CHIP_ESP32S3 : Proto::CHIP_ESP32; }
const char* chipName(uint8_t c) { return c == Proto::CHIP_ESP32S3 ? "ESP32-S3" : c == Proto::CHIP_ESP32 ? "ESP32" : "other chip"; }

// Main board: an image for another board, uploaded into this board's spare
// firmware slot (raw: no boot change; that slot's old "previous firmware" is lost).
struct Uploaded { const esp_partition_t* part = nullptr; uint32_t size = 0, crc = 0; uint8_t chip = 0; String version; bool ok = false; String error; } upload;

// Extra board: OTA writing of an image coming over the link.
esp_ota_handle_t otaHandle = 0;
const esp_partition_t* otaPart = nullptr;

struct PointName { String id, name, unit; uint8_t decimals; bool relay; };
struct Share { String json; std::vector<PointName> names; };
Share shares[MAX_BOARD_ID + 1];

String macStr(const uint8_t* m) {
  char b[18]; snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return b;
}
bool parseMac(const String& s, uint8_t out[6]) {
  unsigned v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}
String hexKey(const uint8_t* k) { char b[33]; for (int i = 0; i < 16; i++) snprintf(b + 2 * i, 3, "%02x", k[i]); return b; }
bool parseKey(const String& s, uint8_t out[16]) {
  if (s.length() != 32) return false;
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)strtoul(s.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);
  return true;
}

uint8_t roleCode(const String& r) {
  return r == "sensor" ? ROLE_SENSOR : r == "relay" ? ROLE_RELAY : r == "sensor_relay" ? ROLE_SENSOR_RELAY : ROLE_MAIN;
}

Identity identity() {
  Identity me;
  me.role = roleCode(gConfig.sys.role);
  me.chip = String(Board::chip()) == "esp32s3" ? Proto::CHIP_ESP32S3 : Proto::CHIP_ESP32;
  WiFi.macAddress(me.mac);
  me.firmware = FW_VERSION;
  me.name = gConfig.sys.hostname.c_str();
  return me;
}

// ---------------------------------------------------------------- main board storage
void indexShare(uint8_t id) {
  Share& s = shares[id];
  s.names.clear();
  JsonDocument d;
  if (!s.json.length() || deserializeJson(d, s.json)) return;
  for (JsonObjectConst o : d["sensors"].as<JsonArrayConst>())
    s.names.push_back({o["id"] | "", o["name"] | "", o["unit"] | "", (uint8_t)(o["decimals"] | 1), false});
  for (JsonObjectConst o : d["relays"].as<JsonArrayConst>())
    s.names.push_back({o["id"] | "", o["name"] | "", "", 0, true});
}

void pushShare(uint8_t id) {
  if (!mainS || !shares[id].json.length()) return;
  const String& j = shares[id].json;
  std::vector<uint8_t> bytes(j.begin(), j.end());
  mainS->setConfig(id, crc32(bytes.data(), bytes.size()), bytes);   // revision = CRC: same settings are never re-sent
}

void saveTable() {
  JsonDocument d;
  for (auto& b : mainS->boards()) {
    JsonObject o = d.add<JsonObject>();
    o["id"] = b.id; o["mac"] = macStr(b.mac); o["name"] = b.name; o["role"] = b.role; o["chip"] = b.chip;
    if (b.hasKey) o["key"] = hexKey(b.key);
  }
  String s; serializeJson(d, s);
  Preferences p;
  if (p.begin(NS, false, "cfg")) { p.putString("list", s); p.end(); }
}

void saveShare(uint8_t id) {
  Preferences p;
  if (!p.begin(NS, false, "cfg")) return;
  char key[6]; snprintf(key, sizeof(key), "s%u", id);
  if (shares[id].json.length()) p.putString(key, shares[id].json); else p.remove(key);
  p.end();
}

void loadMain() {
  Preferences p;
  if (!p.begin(NS, true, "cfg")) return;
  String list = p.getString("list", "[]");
  JsonDocument d;
  if (!deserializeJson(d, list))
    for (JsonObjectConst o : d.as<JsonArrayConst>()) {
      BoardInfo b;
      b.id = o["id"] | 0; b.name = (const char*)(o["name"] | ""); b.role = o["role"] | 0; b.chip = o["chip"] | 0;
      if (b.id < 1 || b.id > MAX_BOARD_ID || !parseMac(o["mac"] | "", b.mac)) continue;
      b.hasKey = parseKey(o["key"] | "", b.key);
      mainS->restore(b);
      radio.setPeer(b.id, b.mac);
      radio.setKey(b.id, b.hasKey ? b.key : nullptr);
      char key[6]; snprintf(key, sizeof(key), "s%u", b.id);
      shares[b.id].json = p.getString(key, "");
      indexShare(b.id);
      pushShare(b.id);
    }
  p.end();
  syncMirrors();
}

// ---------------------------------------------------------------- mirrors of remote sensors
// Keep gConfig's REMOTE sensors in step with the paired boards' shares (called
// with the state lock held, after any change to the boards or their shares).
void syncMirrors() {
  if (!mainS) return;
  std::vector<Boards::ShareInfo> infos;
  for (auto& b : mainS->boards()) infos.push_back({b.id, b.name.c_str(), shares[b.id].json});
  std::vector<SensorConfig> want = Boards::buildMirrors(infos, gConfig.sensors);
  // Compare with what the config has now (as JSON, field by field).
  JsonDocument a, b;
  JsonArray aa = a.to<JsonArray>(), ba = b.to<JsonArray>();
  for (auto& s : gConfig.sensors) if (s.type == SensorType::REMOTE) sensorToJson(s, aa.add<JsonObject>());
  for (auto& s : want) sensorToJson(s, ba.add<JsonObject>());
  String sa, sb; serializeJson(a, sa); serializeJson(b, sb);
  if (sa == sb) return;
  Config* c = new Config(gConfig);
  c->sensors.erase(std::remove_if(c->sensors.begin(), c->sensors.end(), [](const SensorConfig& s) { return s.type == SensorType::REMOTE; }), c->sensors.end());
  for (auto& s : want) c->sensors.push_back(s);
  String err;
  if (!validateConfig(*c, err)) { Serial.printf("[boards] remote sensors not added: %s\n", err.c_str()); delete c; return; }
  requestConfigApply(c);
  Serial.printf("[boards] %u remote sensor(s) on this board's pages\n", (unsigned)want.size());
}

// Live values of the REMOTE sensors (loop task, lock held).
void updateMirrors(uint32_t now) {
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (c.type != SensorType::REMOTE) continue;
    SensorRuntime& r = gSensorRt[i];
    if (!c.enabled) { r.status = ST_DISABLED; r.value = NAN; continue; }
    const BoardInfo* b = mainS->board(c.remoteBoard);
    if (!b) { r.status = ST_NOT_FOUND; r.value = NAN; continue; }
    if (!b->online) { r.status = b->lastHeard ? ST_LINK_LOST : ST_WAITING; r.value = NAN; continue; }
    bool current = b->ackedRev && b->ackedRev == b->configRev;   // values are indexed by the settings the board runs
    if (!current || c.remoteIndex >= b->points.size()) { r.status = ST_WAITING; r.value = NAN; continue; }
    const Point& p = b->points[c.remoteIndex];
    r.value = p.value; r.status = p.status;
    if (p.status == ST_OK) r.lastOkMs = now;
  }
}

// ---------------------------------------------------------------- display catalog
void addLimit(JsonObject o, const char* k, float v) { if (!isnan(v)) o[k] = v; }

void buildCatalog() {   // lock held
  cat.sensors.clear(); cat.relays.clear();
  JsonDocument d;
  d["v"] = 1; d["main"] = gConfig.sys.hostname; d["ap"] = gConfig.sys.apSsid; d["fw"] = FW_VERSION;
  JsonArray sa = d["s"].to<JsonArray>();
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled) continue;
    JsonObject o = sa.add<JsonObject>();
    o["i"] = c.id; o["n"] = c.name; o["u"] = c.unit; o["d"] = c.decimals;
    if (c.oledLabel.length()) o["l"] = c.oledLabel;
    o["lo"] = c.gaugeMin; o["hi"] = c.gaugeMax;
    addLimit(o, "wl", c.warnLow); addLimit(o, "wh", c.warnHigh); addLimit(o, "cl", c.critLow); addLimit(o, "ch", c.critHigh);
    if (c.type == SensorType::REMOTE) o["b"] = c.remoteBoard;
    cat.sensors.push_back((int)i);
  }
  JsonArray ra = d["r"].to<JsonArray>();
  for (size_t i = 0; i < gConfig.relays.size(); i++) {
    if (!gConfig.relays[i].enabled) continue;
    JsonObject o = ra.add<JsonObject>(); o["b"] = 0; o["x"] = i; o["n"] = gConfig.relays[i].name;
    cat.relays.push_back({0, (uint8_t)i});
  }
  if (mainS)
    for (auto& b : mainS->boards()) {
      if (b.role == ROLE_DISPLAY || !shares[b.id].json.length()) continue;
      JsonDocument sd;
      if (deserializeJson(sd, shares[b.id].json)) continue;
      uint8_t x = 0;
      for (JsonObjectConst r : sd["relays"].as<JsonArrayConst>()) {
        JsonObject o = ra.add<JsonObject>(); o["b"] = b.id; o["x"] = x;
        o["n"] = String(b.name.c_str()) + " " + (const char*)(r["name"] | "relay");
        cat.relays.push_back({b.id, x});
        x++;
      }
    }
  cat.json = ""; serializeJson(d, cat.json);
  cat.rev = crc32((const uint8_t*)cat.json.c_str(), cat.json.length());
  cat.builtFor = gSys.configRevision; catBoardsVersion = boardsVersion; cat.builtAt = millis();
}

void ensureCatalog() { if (cat.builtFor != gSys.configRevision || catBoardsVersion != boardsVersion || !cat.json.length()) buildCatalog(); }

std::vector<Point> catalogValues() {   // lock held, catalog current
  std::vector<Point> v;
  for (int i : cat.sensors) {
    Point p;
    if (i < (int)gSensorRt.size()) { p.value = gSensorRt[i].value; p.status = gSensorRt[i].status; p.level = gSensorRt[i].level; }
    else { p.value = NAN; p.status = ST_WAITING; }
    v.push_back(p);
  }
  for (auto& r : cat.relays) {
    Point p; p.value = NAN; p.status = ST_WAITING;
    if (r.board == 0) { if (r.index < gRelayRt.size()) { p.value = gRelayRt[r.index].on ? 1 : 0; p.status = ST_OK; } }
    else if (mainS) {
      const BoardInfo* b = mainS->board(r.board);
      size_t nSensors = 0;
      for (auto& n : shares[r.board].names) if (!n.relay) nSensors++;
      size_t k = nSensors + r.index;
      if (b && b->online && b->ackedRev == b->configRev && k < b->points.size()) { p.value = b->points[k].value; p.status = ST_OK; }
      else if (b && !b->online) p.status = ST_LINK_LOST;
    }
    v.push_back(p);
  }
  return v;
}

// Set one alert limit of a catalog sensor (from a display). Local sensor: in this
// board's settings; remote sensor: in that board's share (then sent to it).
uint8_t setLimit(uint8_t index, uint8_t which, float value) {
  if (index >= cat.sensors.size() || which > LIMIT_CRIT_HIGH) return ACK_BAD_INDEX;
  if (!isnan(value) && (value < -1e6f || value > 1e6f)) return ACK_FAILED;
  const SensorConfig& sc = gConfig.sensors[cat.sensors[index]];
  static const char* const keys[] = {"warnLow", "warnHigh", "critLow", "critHigh"};
  if (sc.type == SensorType::REMOTE) {
    uint8_t bid = sc.remoteBoard;
    JsonDocument d;
    if (bid > MAX_BOARD_ID || deserializeJson(d, shares[bid].json)) return ACK_FAILED;
    JsonArray a = d["sensors"].as<JsonArray>();
    if (sc.remoteIndex >= a.size()) return ACK_BAD_INDEX;
    if (isnan(value)) a[sc.remoteIndex].remove(keys[which]); else a[sc.remoteIndex][keys[which]] = value;
    String err = Boards::setShareLocked(bid, d["sensors"], d["relays"]);
    return err.length() ? ACK_FAILED : ACK_OK;
  }
  Config* c = new Config(gConfig);
  SensorConfig& t = c->sensors[cat.sensors[index]];
  float* f[] = {&t.warnLow, &t.warnHigh, &t.critLow, &t.critHigh};
  *f[which] = value;
  String err;
  if (!validateConfig(*c, err)) { delete c; return ACK_FAILED; }
  requestConfigApply(c);
  return ACK_OK;
}

// A command from a display (ESP-NOW) or the Wi-Fi display API. Lock held.
uint8_t runDisplayCommand(const Command& c) {
  switch (c.kind) {
    case CMD_RELAY: {
      if (c.board == 0) {
        if (c.index >= gConfig.relays.size()) return ACK_BAD_INDEX;
        return Relays::control(gConfig.relays[c.index].id, c.on ? 1 : 0, -1).length() ? ACK_FAILED : ACK_OK;
      }
      if (!mainS) return ACK_FAILED;
      const BoardInfo* b = mainS->board(c.board);
      if (!b || b->role == ROLE_DISPLAY) return ACK_BAD_INDEX;
      if (!b->online) return ACK_FAILED;
      Command r; r.kind = CMD_RELAY; r.index = c.index; r.on = c.on ? 1 : 0;
      return mainS->command(c.board, r) < 0 ? ACK_FAILED : ACK_OK;
    }
    case CMD_MUTE: if (c.minutes) Alerts::mute(std::min<uint16_t>(c.minutes, 120) * 60000UL); else Alerts::unmute(); return ACK_OK;
    case CMD_IDENTIFY: Alerts::chirp(150); return ACK_OK;
    case CMD_SET_LIMIT:
      ensureCatalog();
      if (c.rev != (uint16_t)cat.rev) return ACK_STALE;   // the display's list is out of date
      return setLimit(c.index, c.which, c.value);
    default: return ACK_UNKNOWN;
  }
}

// Main board, loop: keep displays' catalogs current and send them all values every second.
void serveDisplays(uint32_t now) {
  if (!mainS) return;
  bool any = false;
  for (auto& b : mainS->boards()) if (b.role == ROLE_DISPLAY) any = true;
  if (!any) return;
  ensureCatalog();
  for (auto& b : mainS->boards())
    if (b.role == ROLE_DISPLAY && b.configRev != cat.rev)
      mainS->setConfig(b.id, cat.rev, std::vector<uint8_t>(cat.json.begin(), cat.json.end()));
  if (now - lastDisplayPush < 1000) return;
  lastDisplayPush = now;
  std::vector<Point> v = catalogValues();
  for (auto& b : mainS->boards())
    if (b.role == ROLE_DISPLAY && b.online && b.ackedRev == cat.rev) mainS->sendPoints(b.id, (uint16_t)cat.rev, v);
}

// IP address a board got on this board's access point (displays join it as a Wi-Fi fallback).
String apIpOf(const uint8_t mac[6]) {
  wifi_sta_list_t sta;
  if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return "";
#if ESP_IDF_VERSION_MAJOR >= 5
  wifi_sta_mac_ip_list_t ips;
  if (esp_wifi_ap_get_sta_list_with_ip(&sta, &ips) != ESP_OK) return "";
#else
  esp_netif_sta_list_t ips;
  if (esp_netif_get_sta_list(&sta, &ips) != ESP_OK) return "";
#endif
  for (int i = 0; i < ips.num; i++)
    if (!memcmp(ips.sta[i].mac, mac, 6) && ips.sta[i].ip.addr) {
      char b[16]; uint32_t a = ips.sta[i].ip.addr;
      snprintf(b, sizeof(b), "%u.%u.%u.%u", (unsigned)(a & 255), (unsigned)((a >> 8) & 255), (unsigned)((a >> 16) & 255), (unsigned)(a >> 24));
      return b;
    }
  return "";
}

// ---------------------------------------------------------------- extra board
uint8_t nodePoints(Point* out, uint8_t max) {
  size_t n = gConfig.sensors.size() + gConfig.relays.size();
  if (!out) return n > 255 ? 255 : (uint8_t)n;
  uint8_t k = 0;
  for (size_t i = 0; i < gSensorRt.size() && k < max; i++, k++) {
    out[k].value = gSensorRt[i].value; out[k].status = gSensorRt[i].status; out[k].level = gSensorRt[i].level;
  }
  for (size_t i = 0; i < gRelayRt.size() && k < max; i++, k++) {
    out[k].value = gRelayRt[i].on ? 1 : 0; out[k].status = ST_OK; out[k].level = LVL_OK;
  }
  return k;
}

uint8_t nodeConfig(const std::vector<uint8_t>& json, uint32_t rev, std::string& msg) {
  JsonDocument d;
  if (deserializeJson(d, (const char*)json.data(), json.size())) { msg = "not valid JSON"; return CFG_INVALID; }
  Config* c = new Config(gConfig);
  String err;
  bool ok = (d["sensors"].isNull() || sensorsFromJson(d["sensors"], c->sensors, err)) &&
            (d["relays"].isNull() || relaysFromJson(d["relays"], c->relays, err));
  // The engine-running sensor (default bat_voltage) lives on the main board: when it
  // isn't among this board's sensors, "engine running only" alarms here simply always
  // apply instead of refusing the whole settings (5.2.0-beta.2: "Engine-running sensor
  // 'bat_voltage' does not exist").
  if (ok && c->sys.engineSensorId.length()) {
    bool have = false;
    for (auto& x : c->sensors) if (x.id == c->sys.engineSensorId) have = true;
    if (!have) c->sys.engineSensorId = "";
  }
  ok = ok && validateConfig(*c, err);
  if (!ok) { delete c; msg = err.c_str(); return CFG_INVALID; }
  requestConfigApply(c);   // applied and saved by loop() on its next pass
  Preferences p;
  if (p.begin(NODE_NS, false, "cfg")) { p.putUInt("rev", rev); p.end(); }
  Serial.printf("[boards] new settings from the main board: %u sensors, %u relays\n", (unsigned)c->sensors.size(), (unsigned)c->relays.size());
  return CFG_OK;
}

uint8_t nodeCommand(const Command& c) {
  switch (c.kind) {
    case CMD_RELAY: {
      if (c.index >= gConfig.relays.size()) return 2;
      String err = Relays::control(gConfig.relays[c.index].id, c.on, -1);
      return err.length() ? 1 : 0;
    }
    case CMD_MUTE: if (c.minutes) Alerts::mute(c.minutes * 60000UL); else Alerts::unmute(); return 0;
    case CMD_IDENTIFY: Alerts::chirp(300); return 0;
    case CMD_REBOOT: requestReboot(500); return 0;
    default: return 3;
  }
}

void nodeLink(bool up) {
  Serial.printf("[boards] link to the main board %s\n", up ? "up" : "LOST: relays to their safe state");
  if (up) return;
  for (auto& r : gConfig.relays) {
    if (!r.enabled || r.safeState == 2) continue;   // 2 = keep as it is
    Relays::control(r.id, r.safeState == 1 ? 1 : 0, -1);
  }
}
}  // namespace

std::vector<SensorConfig> Boards::buildMirrors(const std::vector<ShareInfo>& infos, const std::vector<SensorConfig>& current) {
  std::vector<SensorConfig> out;
  for (auto& info : infos) {
    if (!info.json.length()) continue;
    JsonDocument d;
    if (deserializeJson(d, info.json)) continue;
    std::vector<SensorConfig> sv; String err;
    if (!sensorsFromJson(d["sensors"], sv, err)) continue;
    for (size_t k = 0; k < sv.size() && k < 255; k++) {
      SensorConfig m = sv[k];   // thresholds, units, gauge range, decimals: as on the board
      m.type = SensorType::REMOTE;
      m.remoteBoard = info.id; m.remoteIndex = (uint8_t)k; m.remoteId = sv[k].id;
      m.id = "b" + String(info.id) + "_" + sv[k].id;
      if (m.id.length() > 31) m.id = m.id.substring(0, 31);
      m.name = info.boardName + " " + sv[k].name;
      // Hardware and conversion happen on the board; none of it applies here.
      m.address = ""; m.pin = m.sckPin = m.soPin = -1; m.curve = ""; m.formula = m.srcA = m.srcB = ""; m.field = "";
      m.offset = 0; m.smoothing = 1;
      m.oledStyle = OledStyle::OFF; m.oledLabel = "";
      for (auto& old : current)   // keep the choices made here
        if (old.type == SensorType::REMOTE && old.id == m.id) { m.oledStyle = old.oledStyle; m.oledLabel = old.oledLabel; m.enabled = old.enabled; }
      out.push_back(m);
    }
  }
  return out;
}

bool Boards::isMain() { return gConfig.sys.role == "main"; }
const char* Boards::role() { return gConfig.sys.role.c_str(); }

void Boards::begin() {
#ifdef CM_EMULATOR_TEST
  return;   // QEMU has no radio
#endif
  if (started) return;
  started = true;
  // Main board: ESP-NOW always, CAN too when enabled. Extra board: CAN when
  // enabled (then no radio channel search), otherwise ESP-NOW.
  const SystemConfig& s = gConfig.sys;
  bool canWanted = s.canEnabled && s.canTx >= 0 && s.canRx >= 0;
  useRadio = isMain() || !canWanted;
  if (useRadio && radio.begin(isMain())) links.add(&radio); else useRadio = false;
  uint8_t mac[6]; WiFi.macAddress(mac);
  useCan = canWanted && can.begin(s.canTx, s.canRx, s.canKbps, mac);
  if (useCan) links.add(&can);
  anyLink = useRadio || useCan;
  if (!anyLink) return;
  if (isMain()) {
    MainSession::Callbacks cb;
    cb.online = [](const BoardInfo& b, bool on) { Serial.printf("[boards] board %u '%s' %s\n", b.id, b.name.c_str(), on ? "online" : "OFFLINE"); };
    cb.configResult = [](const BoardInfo& b) {
      Serial.printf("[boards] board %u settings %s %s\n", b.id, b.configStatus == CFG_OK ? "accepted" : "REFUSED:", b.configMessage.c_str());
    };
    cb.boardsChanged = [] { saveTable(); boardsVersion++; };
    cb.boardCommand = [](const BoardInfo& b, const Command& c) -> uint8_t {
      if (b.role != ROLE_DISPLAY) return ACK_NOT_ALLOWED;   // only displays control the main board
      uint8_t st = runDisplayCommand(c);
      Serial.printf("[boards] display %u: command %u -> %u\n", b.id, c.kind, st);
      return st;
    };
    cb.status = [](uint8_t& level, uint8_t& flags) { level = (uint8_t)gSys.overallLevel; flags = gSys.muted ? HB_MUTED : 0; };
    mainS = new MainSession(links, identity(), cb);
    loadMain();
  } else {
    Preferences p;
    uint8_t id = UNPAIRED; uint32_t rev = 0; uint8_t key[16]; bool hasKey = false;
    if (p.begin(NODE_NS, true, "cfg")) {
      id = p.getUChar("id", UNPAIRED); rev = p.getUInt("rev", 0);
      hasKey = id != UNPAIRED && p.getBytes("key", key, 16) == 16;
      p.end();
    }
    if (hasKey) radio.setKey(MAIN_BOARD, key);   // the main board's radio address is learned from its first frame
    NodeSession::Callbacks cb;
    cb.paired = [](uint8_t newId, const std::string&, const uint8_t* key) {
      Preferences q;
      if (q.begin(NODE_NS, false, "cfg")) {
        q.putUChar("id", newId);
        if (newId == UNPAIRED) q.putUInt("rev", 0);
        if (key) q.putBytes("key", key, 16); else q.remove("key");
        q.end();
      }
      radio.setKey(MAIN_BOARD, key);
      Serial.printf("[boards] %s\n", newId == UNPAIRED ? "unpaired by the main board" : ("paired as board " + String(newId)).c_str());
    };
    cb.config = nodeConfig;
    cb.command = nodeCommand;
    cb.link = nodeLink;
    cb.points = nodePoints;
    cb.level = [] { return (uint8_t)gSys.overallLevel; };
    cb.fwBegin = [](uint32_t size, uint8_t chip, const char* version, std::string& msg) -> uint8_t {
      if (chip != myChip()) { msg = std::string("image is for ") + chipName(chip) + ", this is " + chipName(myChip()); return FW_WRONG_CHIP; }
      otaPart = esp_ota_get_next_update_partition(nullptr);
      if (!otaPart) { msg = "no spare firmware slot"; return FW_REFUSED; }
      if (size > otaPart->size) { msg = "image bigger than the firmware slot"; return FW_TOO_BIG; }
      // Erase as we go (not all at once), so sensors and alarms keep running during the transfer.
      if (esp_ota_begin(otaPart, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK) { msg = "could not start the update"; return FW_REFUSED; }
      Serial.printf("[boards] receiving firmware %s (%u bytes) from the main board\n", version, (unsigned)size);
      return FW_RECEIVING;
    };
    cb.fwWrite = [](uint32_t offset, const uint8_t* data, size_t len) {
      if (offset == 0 && (len < 16 || data[0] != 0xE9 || chipOfImage(data) != myChip())) return false;   // not a firmware for this chip
      return esp_ota_write(otaHandle, data, len) == ESP_OK;
    };
    cb.fwEnd = [](std::string& msg) -> uint8_t {
      if (esp_ota_end(otaHandle) != ESP_OK) { otaHandle = 0; msg = "image check failed"; return FW_BAD_CRC; }
      otaHandle = 0;
      if (esp_ota_set_boot_partition(otaPart) != ESP_OK) { msg = "could not switch firmware"; return FW_WRITE_ERROR; }
      return FW_DONE;   // the new firmware is confirmed after 60 s of running (rollback otherwise)
    };
    cb.fwAbort = [] { if (otaHandle) { esp_ota_abort(otaHandle); otaHandle = 0; } };
    cb.fwDone = [] { Serial.println("[boards] new firmware received: restarting"); requestReboot(1500); };
    nodeS = new NodeSession(links, identity(), id, rev, cb);
  }
  links.onReceive([](const uint8_t* f, size_t n, int8_t rssi) {
    if (mainS) mainS->receive(f, n, rssi, millis());
    if (nodeS) nodeS->receive(f, n, rssi, millis());
  });
}

void Boards::update() {
  if (!started || !anyLink) return;
  uint32_t now = millis();
  StateLock lk;   // web handlers use the main session too
  links.poll();
  if (mainS) { mainS->update(now); updateMirrors(now); serveDisplays(now); }
  if (nodeS) {
    nodeS->flags = (gSys.muted ? HB_MUTED : 0) | (Health::safeMode() ? HB_SAFE_MODE : 0);
    nodeS->update(now);
    // Channel search: hop until the main board answers (its beacon / reply to our hello).
    if (useRadio && !nodeS->mainInRange(now) && now - lastHop >= 300) {
      lastHop = now;
      hopChannel = hopChannel % 13 + 1;
      radio.setChannel(hopChannel);
      nodeS->sayHello();
    }
  }
}

// ---------------------------------------------------------------- web
void Boards::toJson(JsonObject o) {
  StateLock lk;
  uint32_t now = millis();
  o["role"] = gConfig.sys.role;
  JsonObject l = o["link"].to<JsonObject>();
  l["type"] = links.name(); l["ok"] = anyLink; l["channel"] = radio.ok() ? radio.channel() : 0;
  if (useCan || gConfig.sys.canEnabled) {
    JsonObject cj = o["can"].to<JsonObject>();
    cj["state"] = can.state(); cj["rx"] = can.rxCount; cj["tx"] = can.txCount; cj["txFail"] = can.txFail; cj["busErrors"] = can.busErrors;
  }
  o["firmware"] = FW_VERSION; o["chip"] = chipName(myChip());
  if (upload.ok) { JsonObject u = o["uploaded"].to<JsonObject>(); u["version"] = upload.version; u["chip"] = chipName(upload.chip); u["size"] = upload.size; }
  l["rx"] = radio.rxCount; l["tx"] = radio.txCount; l["txFail"] = radio.txFail; l["queueDrops"] = radio.queueDrops;
  if (nodeS) {
    JsonObject n = o["node"].to<JsonObject>();
    n["paired"] = nodeS->paired(); n["id"] = nodeS->boardId(); n["linkUp"] = nodeS->linkUp();
    n["mainInRange"] = nodeS->mainInRange(now); n["configRev"] = nodeS->configRev();
    n["encrypted"] = radio.encrypted(MAIN_BOARD);
  }
  if (!mainS) return;
  JsonArray fa = o["found"].to<JsonArray>();
  for (auto& f : mainS->found()) {
    JsonObject x = fa.add<JsonObject>();
    x["mac"] = macStr(f.mac); x["name"] = f.name; x["firmware"] = f.firmware; x["role"] = roleName(f.role);
    x["chip"] = f.chip == Proto::CHIP_ESP32S3 ? "ESP32-S3" : "ESP32"; x["rssi"] = f.rssi; x["ageS"] = std::max<int32_t>(0, (int32_t)(now - f.lastHeard)) / 1000;
  }
  JsonArray ba = o["boards"].to<JsonArray>();
  for (auto& b : mainS->boards()) {
    JsonObject x = ba.add<JsonObject>();
    x["encrypted"] = b.hasKey;
    if (b.fw.state != BoardInfo::Fw::IDLE) {
      JsonObject f = x["fw"].to<JsonObject>();
      static const char* const names[] = {"idle", "offering", "sending", "done", "failed"};
      f["state"] = names[b.fw.state]; f["version"] = b.fw.version; f["error"] = b.fw.error;
      f["percent"] = b.fw.size ? (int)(100.0 * b.fw.acked / b.fw.size) : 0;
      f["seconds"] = (now - b.fw.startedAt) / 1000;
    }
    x["sameChip"] = b.chip == myChip();
    x["id"] = b.id; x["name"] = b.name; x["mac"] = macStr(b.mac); x["role"] = roleName(b.role); x["firmware"] = b.firmware;
    x["chip"] = b.chip == Proto::CHIP_ESP32S3 ? "ESP32-S3" : "ESP32";
    x["online"] = b.online; x["ageS"] = b.lastHeard ? std::max<int32_t>(0, (int32_t)(now - b.lastHeard)) / 1000 : -1;
    x["rssi"] = b.rssi; x["rssiAtBoard"] = b.rssiAtBoard; x["uptimeS"] = b.uptimeS;
    x["level"] = alertLevelStr(b.level);
    x["relaysSafe"] = (b.flags & HB_RELAYS_SAFE) != 0; x["safeMode"] = (b.flags & HB_SAFE_MODE) != 0;
    x["received"] = b.seqs.received(); x["lost"] = b.seqs.lost();
    uint32_t tot = b.seqs.received() + b.seqs.lost();
    x["lossPct"] = tot ? (int)(100.0 * b.seqs.lost() / tot + 0.5) : 0;
    x["commandsIn"] = b.commandsIn; x["commandsFailed"] = b.commandsFailed;
    x["muted"] = (b.flags & HB_MUTED) != 0;
    if (b.role == ROLE_DISPLAY) {
      JsonObject dj = x["display"].to<JsonObject>();
      dj["catalogSent"] = b.ackedRev && b.ackedRev == cat.rev;
      dj["sensors"] = cat.sensors.size(); dj["relays"] = cat.relays.size();
      String ip = apIpOf(b.mac);
      if (ip.length()) dj["ip"] = ip;
    }
    JsonObject c = x["config"].to<JsonObject>();
    c["pending"] = b.configPending; c["status"] = b.configStatus == CFG_OK ? "accepted" : b.configStatus == CFG_INVALID ? "refused" : "error";
    c["message"] = b.configMessage; c["hasShare"] = shares[b.id].json.length() > 0;
    c["upToDate"] = b.ackedRev && b.ackedRev == b.configRev;
    JsonArray pa = x["points"].to<JsonArray>();
    const auto& names = shares[b.id].names;
    for (size_t i = 0; i < names.size(); i++) {
      JsonObject p = pa.add<JsonObject>();
      p["id"] = names[i].id; p["name"] = names[i].name; p["unit"] = names[i].unit; p["relay"] = names[i].relay; p["decimals"] = names[i].decimals;
      if (i < b.points.size() && b.online) {
        const Point& v = b.points[i];
        if (isnan(v.value)) p["value"] = nullptr; else p["value"] = v.value;
        p["status"] = sensorStatusStr(v.status); p["level"] = alertLevelStr(v.level);
      } else p["status"] = "no data";
    }
  }
}

String Boards::accept(const String& mac, const String& name, int& id) {
  if (!mainS) return "Only the main board pairs other boards";
  uint8_t m[6];
  if (!parseMac(mac, m)) return "Bad board address";
  StateLock lk;
  uint8_t key[16];
  bool enc = gConfig.sys.boardEncrypt;
  if (enc) esp_fill_random(key, sizeof(key));
  id = mainS->accept(m, name.c_str(), enc ? key : nullptr);
  if (id > 0) { radio.setPeer((uint8_t)id, m); radio.setKey((uint8_t)id, enc ? key : nullptr); }
  if (id < 0) return mainS->boards().size() >= MAX_BOARDS ? "Already " + String(MAX_BOARDS) + " boards paired" : "That board is no longer in range";
  boardsVersion++;
  if (mainS->board((uint8_t)id)->role == ROLE_DISPLAY) return "";   // a display gets the catalog (serveDisplays), not sensors
  shares[id].json = "{\"sensors\":[],\"relays\":[]}";   // a new board starts empty: set it up on the Sensors / Relays pages
  indexShare(id); saveShare(id); pushShare(id);
  return "";
}

String Boards::forget(int id) {
  if (!mainS) return "Only the main board pairs other boards";
  StateLock lk;
  if (id < 1 || id > MAX_BOARD_ID || !mainS->forget((uint8_t)id)) return "Unknown board";
  radio.forget((uint8_t)id);
  shares[id] = Share(); saveShare(id);
  boardsVersion++;
  syncMirrors();   // its sensors leave the main board's pages
  return "";
}

String Boards::shareJson(int id, String& out) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  if (id < 1 || id > MAX_BOARD_ID || !mainS->board((uint8_t)id)) return "Unknown board";
  JsonDocument d;
  if (shares[id].json.length()) deserializeJson(d, shares[id].json);
  if (!d["sensors"].is<JsonArray>()) d["sensors"].to<JsonArray>();
  if (!d["relays"].is<JsonArray>()) d["relays"].to<JsonArray>();
  d["id"] = id; d["name"] = mainS->board((uint8_t)id)->name;
  serializeJson(d, out);
  return "";
}

String Boards::setShare(int id, JsonVariantConst sensors, JsonVariantConst relays) {
  StateLock lk;
  return setShareLocked(id, sensors, relays);
}

String Boards::setShareLocked(int id, JsonVariantConst sensors, JsonVariantConst relays) {
  if (!mainS) return "Only on the main board";
  if (id < 1 || id > MAX_BOARD_ID || !mainS->board((uint8_t)id)) return "Unknown board";
  JsonDocument cur;
  if (shares[id].json.length()) deserializeJson(cur, shares[id].json);
  JsonDocument out;
  String err;
  // Normalise through the same parsers the board uses (pins are checked by the board itself).
  std::vector<SensorConfig> sv; std::vector<RelayConfig> rv;
  if (!sensorsFromJson(sensors.isNull() ? cur["sensors"].as<JsonVariantConst>() : sensors, sv, err)) return err;
  if (!relaysFromJson(relays.isNull() ? cur["relays"].as<JsonVariantConst>() : relays, rv, err)) return err;
  JsonArray sa = out["sensors"].to<JsonArray>();
  for (auto& s : sv) sensorToJson(s, sa.add<JsonObject>());
  JsonArray ra = out["relays"].to<JsonArray>();
  for (auto& r : rv) relayToJson(r, ra.add<JsonObject>());
  String j; serializeJson(out, j);
  if (j.length() > MAX_CONFIG) return "Too many settings for one board";
  if (mainS->board((uint8_t)id)->role == ROLE_DISPLAY) return "A display has no sensors or relays of its own";
  shares[id].json = j;
  indexShare(id); saveShare(id); pushShare(id);
  boardsVersion++;
  syncMirrors();   // its sensors show up on gauges, charts, alarms...
  return "";
}

// ---------------------------------------------------------------- firmware for other boards
String Boards::firmwareFromSelf(int id) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  const BoardInfo* b = id >= 1 && id <= MAX_BOARD_ID ? mainS->board((uint8_t)id) : nullptr;
  if (!b) return "Unknown board";
  if (b->chip != myChip()) return String("That board is an ") + chipName(b->chip) + ": upload a firmware file for it instead";
  const esp_partition_t* run = esp_ota_get_running_partition();
  uint32_t size = ESP.getSketchSize();
  if (!run || !size) return "Can't read this board's firmware";
  uint8_t buf[512]; uint32_t crc = 0;
  for (uint32_t off = 0; off < size; off += sizeof(buf)) {
    size_t n = std::min<uint32_t>(sizeof(buf), size - off);
    if (esp_partition_read(run, off, buf, n) != ESP_OK) return "Can't read this board's firmware";
    crc = crc32(buf, n, crc);
  }
  mainS->startFirmware((uint8_t)id, size, crc, myChip(), FW_VERSION,
    [run](uint32_t off, uint8_t* out, size_t len) { return esp_partition_read(run, off, out, len) == ESP_OK; });
  return "";
}

bool Boards::uploadChunk(size_t index, const uint8_t* data, size_t len, bool final, String& err) {
  if (!mainS) { err = "Only on the main board"; return false; }
  if (index == 0) {
    upload = Uploaded();
    upload.part = esp_ota_get_next_update_partition(nullptr);   // this board's spare slot
    if (!upload.part) { err = "No spare firmware slot"; return false; }
    if (len < 64 || data[0] != 0xE9) { err = "Not an ESP32 firmware file (use carmetrics-...-firmware.bin, not the full image)"; return false; }
    upload.chip = chipOfImage(data);
    char v[33] = {0}; memcpy(v, data + 32 + 16, 32); upload.version = v;
  }
  if (!upload.part) { err = "Upload not started"; return false; }
  if (index + len > upload.part->size) { err = "File bigger than a firmware slot"; upload.part = nullptr; return false; }
  // erase each 4 KB sector as the data reaches it
  for (size_t sec = (index + 4095) / 4096 * 4096; sec < index + len; sec += 4096)
    if (esp_partition_erase_range(upload.part, sec, 4096) != ESP_OK) { err = "Erase failed"; upload.part = nullptr; return false; }
  if (esp_partition_write(upload.part, index, data, len) != ESP_OK) { err = "Write failed"; upload.part = nullptr; return false; }
  upload.crc = crc32(data, len, upload.crc);
  upload.size = index + len;
  if (final) upload.ok = true;
  return true;
}

String Boards::firmwareFromUpload(int id) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  const BoardInfo* b = id >= 1 && id <= MAX_BOARD_ID ? mainS->board((uint8_t)id) : nullptr;
  if (!b) return "Unknown board";
  if (!upload.ok || !upload.part) return "No firmware file uploaded";
  if (upload.chip != b->chip) return String("That file is for an ") + chipName(upload.chip) + ", the board is an " + chipName(b->chip);
  const esp_partition_t* part = upload.part;
  mainS->startFirmware((uint8_t)id, upload.size, upload.crc, upload.chip, upload.version.c_str(),
    [part](uint32_t off, uint8_t* out, size_t len) { return esp_partition_read(part, off, out, len) == ESP_OK; });
  return "";
}

String Boards::firmwareCancel(int id) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  mainS->cancelFirmware((uint8_t)id);
  return "";
}

String Boards::relayCommand(int id, int index, bool on) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  const BoardInfo* b = id >= 1 && id <= MAX_BOARD_ID ? mainS->board((uint8_t)id) : nullptr;
  if (!b) return "Unknown board";
  if (!b->online) return "'" + String(b->name.c_str()) + "' is offline";
  Command c; c.kind = CMD_RELAY; c.index = (uint8_t)index; c.on = on;
  return mainS->command((uint8_t)id, c) < 0 ? "Not sent" : "";
}

// ---------------------------------------------------------------- board control (main board web pages)
String Boards::boardCommand(int id, const String& cmd, int minutes) {
  if (!mainS) return "Only on the main board";
  StateLock lk;
  const BoardInfo* b = id >= 1 && id <= MAX_BOARD_ID ? mainS->board((uint8_t)id) : nullptr;
  if (!b) return "Unknown board";
  if (cmd == "resend") { mainS->resendConfig((uint8_t)id); return ""; }
  if (!b->online) return "'" + String(b->name.c_str()) + "' is offline";
  Command c;
  if (cmd == "identify") c.kind = CMD_IDENTIFY;
  else if (cmd == "reboot") c.kind = CMD_REBOOT;
  else if (cmd == "mute") { c.kind = CMD_MUTE; c.minutes = (uint16_t)constrain(minutes, 0, 120); }
  else return "Unknown command '" + cmd + "'";
  return mainS->command((uint8_t)id, c) < 0 ? "Not sent" : "";
}

String Boards::rename(int id, const String& name) {
  if (!mainS) return "Only on the main board";
  String n = name; n.trim();
  if (!n.length() || n.length() > 23) return "Name: 1 to 23 characters";
  StateLock lk;
  if (id < 1 || id > MAX_BOARD_ID || !mainS->board((uint8_t)id)) return "Unknown board";
  mainS->rename((uint8_t)id, n.c_str());
  syncMirrors();   // mirrors are named "<board> <sensor>"
  return "";
}

// ---------------------------------------------------------------- display API over Wi-Fi
String Boards::displayCatalog(uint32_t& rev) {
  StateLock lk;
  ensureCatalog();
  rev = cat.rev;
  return cat.json;
}

void Boards::displayValues(JsonObject o) {
  StateLock lk;
  ensureCatalog();
  o["rev"] = cat.rev; o["level"] = (uint8_t)gSys.overallLevel; o["muted"] = gSys.muted;
  JsonArray a = o["v"].to<JsonArray>();
  for (auto& p : catalogValues()) {
    JsonArray e = a.add<JsonArray>();
    if (isnan(p.value)) e.add(nullptr); else e.add(p.value);
    e.add(p.status); e.add(p.level);
  }
}

String Boards::displayCommand(JsonVariantConst j) {
  Command c;
  String k = j["kind"] | "";
  if (k == "relay") { c.kind = CMD_RELAY; c.board = j["board"] | 0; c.index = j["index"] | 0; c.on = (j["on"] | false) ? 1 : 0; }
  else if (k == "mute") { c.kind = CMD_MUTE; c.minutes = j["minutes"] | 5; }
  else if (k == "limit") {
    c.kind = CMD_SET_LIMIT; c.index = j["index"] | 255; c.which = j["which"] | 255;
    c.value = j["value"].isNull() ? NAN : j["value"].as<float>(); c.rev = (uint16_t)(j["rev"] | 0UL);
  } else return "Unknown command";
  StateLock lk;
  uint8_t st = runDisplayCommand(c);
  static const char* const msg[] = {"", "Failed", "Unknown sensor or relay", "Unknown command", "List out of date: reload", "Not allowed"};
  return st == ACK_OK ? "" : String(st < 6 ? msg[st] : "Failed");
}
