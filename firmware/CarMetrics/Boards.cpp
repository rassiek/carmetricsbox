#include "Boards.h"
#include "BoardNet.h"
#include "EspNowLink.h"
#include "State.h"
#include "Outputs.h"
#include "Board.h"
#include "Health.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <algorithm>

using namespace Proto;
using namespace BoardNet;

namespace {
const char* NS = "boards";       // main board: paired boards + their shares (in the "cfg" NVS partition)
const char* NODE_NS = "node";    // extra board: own id + accepted settings revision

EspNowLink radio;
MainSession* mainS = nullptr;
NodeSession* nodeS = nullptr;
bool started = false;
uint8_t hopChannel = 1;
uint32_t lastHop = 0;

void syncMirrors();

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
            (d["relays"].isNull() || relaysFromJson(d["relays"], c->relays, err)) && validateConfig(*c, err);
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
  if (!radio.begin(isMain())) return;
  if (isMain()) {
    MainSession::Callbacks cb;
    cb.online = [](const BoardInfo& b, bool on) { Serial.printf("[boards] board %u '%s' %s\n", b.id, b.name.c_str(), on ? "online" : "OFFLINE"); };
    cb.configResult = [](const BoardInfo& b) {
      Serial.printf("[boards] board %u settings %s %s\n", b.id, b.configStatus == CFG_OK ? "accepted" : "REFUSED:", b.configMessage.c_str());
    };
    cb.boardsChanged = [] { saveTable(); };
    mainS = new MainSession(radio, identity(), cb);
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
    nodeS = new NodeSession(radio, identity(), id, rev, cb);
  }
  radio.onReceive([](const uint8_t* f, size_t n, int8_t rssi) {
    if (mainS) mainS->receive(f, n, rssi, millis());
    if (nodeS) nodeS->receive(f, n, rssi, millis());
  });
}

void Boards::update() {
  if (!started || !radio.ok()) return;
  uint32_t now = millis();
  StateLock lk;   // web handlers use the main session too
  radio.poll();
  if (mainS) { mainS->update(now); updateMirrors(now); }
  if (nodeS) {
    nodeS->flags = (gSys.muted ? HB_MUTED : 0) | (Health::safeMode() ? HB_SAFE_MODE : 0);
    nodeS->update(now);
    // Channel search: hop until the main board answers (its beacon / reply to our hello).
    if (!nodeS->mainInRange(now) && now - lastHop >= 300) {
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
  l["type"] = radio.name(); l["ok"] = radio.ok(); l["channel"] = radio.ok() ? radio.channel() : 0;
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
    x["chip"] = f.chip == Proto::CHIP_ESP32S3 ? "ESP32-S3" : "ESP32"; x["rssi"] = f.rssi; x["ageS"] = (now - f.lastHeard) / 1000;
  }
  JsonArray ba = o["boards"].to<JsonArray>();
  for (auto& b : mainS->boards()) {
    JsonObject x = ba.add<JsonObject>();
    x["encrypted"] = b.hasKey;
    x["id"] = b.id; x["name"] = b.name; x["mac"] = macStr(b.mac); x["role"] = roleName(b.role); x["firmware"] = b.firmware;
    x["chip"] = b.chip == Proto::CHIP_ESP32S3 ? "ESP32-S3" : "ESP32";
    x["online"] = b.online; x["ageS"] = b.lastHeard ? (int32_t)((now - b.lastHeard) / 1000) : -1;
    x["rssi"] = b.rssi; x["rssiAtBoard"] = b.rssiAtBoard; x["uptimeS"] = b.uptimeS;
    x["level"] = alertLevelStr(b.level);
    x["relaysSafe"] = (b.flags & HB_RELAYS_SAFE) != 0; x["safeMode"] = (b.flags & HB_SAFE_MODE) != 0;
    x["received"] = b.seqs.received(); x["lost"] = b.seqs.lost();
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
  if (!mainS) return "Only on the main board";
  StateLock lk;
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
  shares[id].json = j;
  indexShare(id); saveShare(id); pushShare(id);
  syncMirrors();   // its sensors show up on gauges, charts, alarms...
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
