#include "Boards.h"
#include "BoardNet.h"
#include "Radio.h"
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
MainSession* mainS = nullptr;
bool started = false;
EspNowLink& radio = Radio::espnow();
using Radio::myChip; using Radio::chipName; using Radio::chipOfImage; using Radio::macStr; using Radio::parseMac;

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
// Main board: an image for another board, uploaded into this board's spare
// firmware slot (raw: no boot change; that slot's old "previous firmware" is lost).
struct Uploaded { const esp_partition_t* part = nullptr; uint32_t size = 0, crc = 0; uint8_t chip = 0; String version; bool ok = false; String error; RoleScan roles; } upload;

struct PointName { String id, name, unit; uint8_t decimals; bool relay; };
struct Share { String json; std::vector<PointName> names; };
Share shares[MAX_BOARD_ID + 1];

String hexKey(const uint8_t* k) { char b[33]; for (int i = 0; i < 16; i++) snprintf(b + 2 * i, 3, "%02x", k[i]); return b; }
bool parseKey(const String& s, uint8_t out[16]) {
  if (s.length() != 32) return false;
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)strtoul(s.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);
  return true;
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
    if (r.board == 0) { if (r.index < gRelayRt.size()) { p.value = Relays::value(r.index); p.status = ST_OK; } }
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



void Boards::begin() {
#ifdef CM_EMULATOR_TEST
  return;   // QEMU has no radio
#endif
  if (started) return;
  started = true;
  if (!Radio::begin(true)) return;   // the main board always listens on ESP-NOW (and CAN when enabled)
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
  mainS = new MainSession(Radio::links(), Radio::identity(ROLE_MAIN), cb);
  loadMain();
  Radio::links().onReceive([](const uint8_t* f, size_t n, int8_t rssi) { if (mainS) mainS->receive(f, n, rssi, millis()); });
}

void Boards::update() {
  if (!started || !Radio::ok() || !mainS) return;
  uint32_t now = millis();
  StateLock lk;   // web handlers use the main session too
  Radio::links().poll();
  mainS->update(now); updateMirrors(now); serveDisplays(now);
}

// ---------------------------------------------------------------- web
void Boards::toJson(JsonObject o) {
  StateLock lk;
  uint32_t now = millis();
  o["role"] = "main";
  Radio::toJson(o);
  o["firmware"] = FW_VERSION; o["chip"] = chipName(myChip());
  if (upload.ok) { JsonObject u = o["uploaded"].to<JsonObject>(); u["version"] = upload.version; u["chip"] = chipName(upload.chip); u["size"] = upload.size; }
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
  // Same chip as this board: catch impossible pins here instead of having the board refuse them.
  if (mainS->board((uint8_t)id)->chip == myChip()) {
    for (auto& r : rv) {
      if (!r.enabled) continue;
      for (int p : {r.pin, r.type == ActType::HBRIDGE ? r.pin2 : -1})
        if (p >= 0 && !Board::chipCanOutput(p)) return "Actuator '" + r.id + "': GPIO " + String(p) + " can't drive an output on a " + String(Board::chip()) + " board";
    }
    for (auto& s : sv)
      for (int p : {s.pin, s.sckPin, s.soPin})
        if (s.enabled && p >= 0 && !Board::chipHasPin(p)) return "Sensor '" + s.id + "': GPIO " + String(p) + " doesn't exist on a " + String(Board::chip()) + " board";
  }
  if (mainS->board((uint8_t)id)->role == ROLE_DISPLAY) return "A display has no sensors or relays of its own";
  shares[id].json = j;
  indexShare(id); saveShare(id); pushShare(id);
  boardsVersion++;
  syncMirrors();   // its sensors show up on gauges, charts, alarms...
  return "";
}

// ---------------------------------------------------------------- firmware for other boards
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
  upload.roles.feed(data, len);
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
  // 5.4: the main firmware never goes to another board; worker files never to a touch display.
  if (upload.roles.role == "main") return "That is the main board firmware: choose the board's own file (carmetrics-worker-…, relay board or display)";
  if (upload.roles.role == "worker" && b->role == ROLE_DISPLAY) return "That is a worker firmware: a display needs carmetrics-display-…-firmware.bin";
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
  JsonDocument d; d["on"] = on;
  return actuatorCommand(id, index, d.as<JsonVariantConst>());
}

String Boards::actuatorCommand(int id, int index, JsonVariantConst body) {
  if (!mainS) return "Only on the main board";
  Command c; c.kind = CMD_RELAY; c.index = (uint8_t)index;
  String err = Radio::jsonToCommand(body, c);
  if (err.length()) return err;
  StateLock lk;
  const BoardInfo* b = id >= 1 && id <= MAX_BOARD_ID ? mainS->board((uint8_t)id) : nullptr;
  if (!b) return "Unknown board";
  if (!b->online) return "'" + String(b->name.c_str()) + "' is offline";
  return mainS->command((uint8_t)id, c) < 0 ? "Not sent" : "";
}

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
