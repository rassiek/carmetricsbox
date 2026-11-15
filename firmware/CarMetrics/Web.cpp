// CarMetrics - HTTP API v1 (docs/API.md), live updates (WebSocket + SSE) and the
// web app. One route table; each route names the scope it needs.
#include "Web.h"
#include "State.h"
#include "Sensors.h"
#include "Outputs.h"
#include "History.h"
#include "Cloud.h"
#include "Health.h"
#include "Board.h"
#include "BlackBox.h"
#include "WifiMgr.h"
#include "Boards.h"
#include "Keys.h"
#include "Pins.h"
#include "Discover.h"
#include "Route.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

#define API "/api/v1"
#define MAX_BODY (32 * 1024)
#define WS_MAX_CLIENTS 6
#define SSE_MAX_CLIENTS 3

namespace {
AsyncWebServer server(80);
AsyncEventSource events(API "/events-stream");
AsyncWebSocket ws(API "/ws");
struct WsInfo { uint32_t id; uint8_t scope; };
std::vector<WsInfo> wsInfo;            // StateLock
uint32_t lastPush = 0, eventId = 0;
volatile bool configChanged = false;
uint8_t lastLevel = 255; bool lastMuted = false;

// ================================================================== replies
bool roomFor(size_t n) {   // one free block big enough (+ headroom for Wi-Fi / TCP)?
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > n + 8192;
}

void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc, const char* filename = nullptr) {
  size_t n = filename ? measureJsonPretty(doc) : measureJson(doc);
  if (!roomFor(n)) {
    AsyncWebServerResponse* r = req->beginResponse(503, "application/json", "{\"error\":{\"code\":\"busy\",\"message\":\"Low memory, try again in a moment\"}}");
    r->addHeader("Retry-After", "2");
    req->send(r);
    return;
  }
  AsyncResponseStream* res = req->beginResponseStream("application/json", n + 32);
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  if (filename) { res->addHeader("Content-Disposition", String("attachment; filename=") + filename); serializeJsonPretty(doc, *res); }
  else serializeJson(doc, *res);
  req->send(res);
}

void sendError(AsyncWebServerRequest* req, int http, const String& msg, const char* field = nullptr) {
  const char* code = http == 401 ? "auth" : http == 403 ? "scope" : http == 404 ? "not_found" : http == 409 ? "conflict" : http == 503 ? "busy" : "invalid";
  JsonDocument d;
  JsonObject e = d["error"].to<JsonObject>();
  e["code"] = code; e["message"] = msg;
  if (field) e["field"] = field;
  sendJson(req, http, d);
}

void sendOk(AsyncWebServerRequest* req, const String& msg = "", int http = 200) {
  JsonDocument d;
  d["ok"] = true;
  if (msg.length()) d["message"] = msg;
  sendJson(req, http, d);
}
// "" = ok, otherwise the message as a 400.
void sendResult(AsyncWebServerRequest* req, const String& err, const String& okMsg = "") {
  if (err.length()) sendError(req, 400, err); else sendOk(req, okMsg);
}

// ================================================================== auth
uint8_t callerScope(AsyncWebServerRequest* req) {
  const String& a = req->header("Authorization");
  if (a.startsWith("Bearer ")) return Keys::check(a.substring(7));
  String pass; bool readAuth;
  { StateLock lk; pass = gConfig.sys.adminPass; readAuth = gConfig.sys.readNeedsAuth; }
  if (a.length() && req->authenticate("admin", pass.c_str())) return SCOPE_ADMIN;
  return readAuth ? SCOPE_NONE : SCOPE_READ;
}

bool allowed(AsyncWebServerRequest* req, uint8_t need) {
  if (need == SCOPE_NONE) return true;
  uint8_t s = callerScope(req);
  if (s >= need) return true;
  const String& a = req->header("Authorization");
  if (!a.length()) {   // a browser: ask for the password
    String realm; { StateLock lk; realm = gConfig.ui.appName; }
    req->requestAuthentication(realm.c_str(), false);
  } else if (s == SCOPE_NONE) sendError(req, 401, "Wrong password or key");
  else sendError(req, 403, String("This key's scope is '") + scopeStr(s) + "', this needs '" + scopeStr(need) + "'");
  return false;
}

// ================================================================== settings helpers
// Copy the settings, let `edit` change the copy, validate, hand it to loop().
template <typename F>
void changeConfig(AsyncWebServerRequest* req, F edit, const String& okMsg) {
  Config* next;
  { StateLock lk; next = new Config(gConfig); }
  String err;
  if (!edit(*next, err) || !validateConfig(*next, err)) { delete next; sendError(req, 400, err); return; }
  requestConfigApply(next);
  sendOk(req, okMsg);
}

void deepMerge(JsonVariant dst, JsonVariantConst src) {
  if (src.is<JsonObjectConst>() && dst.is<JsonObject>()) {
    for (JsonPairConst kv : src.as<JsonObjectConst>()) {
      JsonVariant d = dst[kv.key()];
      if (kv.value().is<JsonObjectConst>() && d.is<JsonObject>()) deepMerge(d, kv.value());
      else dst[kv.key()] = kv.value();
    }
  } else dst.set(src);
}

// Patch a part of the settings: current settings as JSON, `patch` merged in, read back.
void patchSettings(AsyncWebServerRequest* req, JsonVariantConst patch, const String& okMsg) {
  changeConfig(req, [&](Config& c, String& err) {
    JsonDocument cur;
    configToJson(c, cur.to<JsonObject>(), false);   // passwords blank = keep
    deepMerge(cur.as<JsonVariant>(), patch);
    return configFromJson(cur.as<JsonVariantConst>(), c, err);
  }, okMsg);
}

const char* const kSections[] = {"ui", "system", "weather", "cloud", "turbo", "layout", "blackbox"};
bool isSection(const String& s) { for (auto k : kSections) if (s == k) return true; return false; }

// ================================================================== points
// Address: "local.<id>" for this board's sensors / actuators / system figures,
// "b<N>.<id>" for a sensor of paired board N.
String sensorAddr(const SensorConfig& c) {
  return c.type == SensorType::REMOTE ? "b" + String(c.remoteBoard) + "." + c.remoteId : "local." + c.id;
}
void numOrNull(JsonObject o, const char* k, float v, int dec) {
  if (isnan(v)) o[k] = nullptr; else o[k] = serialized(String(v, dec));
}
bool wanted(const std::vector<String>& only, const String& a) {
  if (only.empty()) return true;
  for (auto& x : only) if (x == a) return true;
  return false;
}

void pointsToJson(JsonObject o, bool meta, const std::vector<String>& only) {   // lock held
  o["t"] = millis();
  if (gSys.timeSynced) o["epoch"] = (uint32_t)time(nullptr);
  o["level"] = alertLevelStr(gSys.overallLevel); o["muted"] = gSys.muted;
  o["running"] = gSys.engineRunning;
  if (gSys.ignition >= 0) o["ignition"] = gSys.ignition == 1;
  if (Health::safeMode()) o["safeMode"] = true;
  o["rev"] = gSys.configRevision;
  if (gConfig.turbo.enabled && gSys.turbo.phase != TURBO_OFF) {
    JsonObject t = o["turbo"].to<JsonObject>();
    t["phase"] = gSys.turbo.phase == TURBO_COOLING ? "cooling" : "ok"; t["leftS"] = gSys.turbo.leftS;
  }
  JsonArray a = o["items"].to<JsonArray>();
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i]; const SensorRuntime& r = gSensorRt[i];
    String addr = sensorAddr(c);
    if (!wanted(only, addr)) continue;
    JsonObject x = a.add<JsonObject>();
    x["a"] = addr; numOrNull(x, "v", r.value, c.decimals + 1);
    x["st"] = sensorStatusStr(c.enabled ? r.status : ST_DISABLED); x["lvl"] = alertLevelStr(r.level);
    if (!meta) continue;
    x["kind"] = "sensor"; x["name"] = c.name; x["label"] = c.oledLabel; x["unit"] = c.unit; x["decimals"] = c.decimals;
    x["min"] = c.gaugeMin; x["max"] = c.gaugeMax;
    x["board"] = c.type == SensorType::REMOTE ? "b" + String(c.remoteBoard) : String("local");
    JsonObject l = x["limits"].to<JsonObject>();
    numOrNull(l, "wl", c.warnLow, 2); numOrNull(l, "wh", c.warnHigh, 2); numOrNull(l, "cl", c.critLow, 2); numOrNull(l, "ch", c.critHigh, 2);
    if (!isnan(r.volts)) numOrNull(x, "volts", r.volts, 3);
  }
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    String addr = "local." + c.id;
    if (!wanted(only, addr)) continue;
    JsonObject x = a.add<JsonObject>();
    x["a"] = addr; x["v"] = Relays::value(i); x["st"] = c.enabled ? "ok" : "disabled"; x["lvl"] = "ok";
    if (!meta) continue;
    static const char* const types[] = {"relay", "pwm", "hbridge", "servo"};
    x["kind"] = "actuator"; x["type"] = types[(int)c.type]; x["name"] = c.name; x["board"] = "local";
    x["unit"] = c.type == ActType::SERVO ? "°" : c.type == ActType::RELAY ? "" : "%";
    x["by"] = gRelayRt[i].by;
  }
  for (uint8_t i = 0; i < Health::kSysCount; i++) {
    String addr = String("local.") + Health::kSysIds[i];
    if (!wanted(only, addr)) continue;
    JsonObject x = a.add<JsonObject>();
    x["a"] = addr; numOrNull(x, "v", Health::sysMetric(i), 1); x["st"] = "ok"; x["lvl"] = "ok";
    if (meta) { x["kind"] = "system"; x["name"] = Health::kSysIds[i] + 4; x["board"] = "local"; }
  }
}

void alertsToJson(JsonObject o) {   // lock held
  o["level"] = alertLevelStr(gSys.overallLevel); o["muted"] = gSys.muted;
  if (gSys.muted) o["mutedForS"] = (int32_t)(gSys.muteUntilMs - millis()) / 1000;
  JsonArray a = o["items"].to<JsonArray>();
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorRuntime& r = gSensorRt[i];
    if (r.level == LVL_OK) continue;
    const SensorConfig& c = gConfig.sensors[i];
    JsonObject x = a.add<JsonObject>();
    x["a"] = sensorAddr(c); x["lvl"] = alertLevelStr(r.level); x["name"] = c.name;
    x["text"] = r.level == LVL_FAULT ? c.name + ": " + sensorStatusStr(r.status) : c.name + " " + (isnan(r.value) ? String("-") : String(r.value, (unsigned int)c.decimals)) + " " + c.unit;
  }
}

// ================================================================== requests
struct Ctx {
  AsyncWebServerRequest* req;
  String p[2];                  // the "*" parts of the route
  JsonDocument body;            // parsed JSON body (null when none)
  String q(const char* k, const char* def = "") const { return req->hasParam(k) ? req->getParam(k)->value() : String(def); }
};
using Fn = void (*)(Ctx&);
struct Route { uint32_t method; const char* path; uint8_t need; Fn fn; };

// ?board=local (default) | b1..bN -> 0 | N, -1 bad
int boardParam(Ctx& c) {
  String b = c.q("board", "local");
  if (b == "local" || b == "0") return 0;
  if (b.startsWith("b") && b.length() > 1) { int n = b.substring(1).toInt(); if (n > 0) return n; }
  return -1;
}

// ---- sensors / actuators lists (this board or a paired board), edited as JSON
// kind "sensors" | "relays"
bool loadList(int board, const char* kind, JsonDocument& out, String& err) {
  if (board == 0) {
    StateLock lk;
    JsonArray a = out.to<JsonArray>();
    if (!strcmp(kind, "sensors")) { for (auto& s : gConfig.sensors) if (s.type != SensorType::REMOTE) sensorToJson(s, a.add<JsonObject>()); }
    else for (auto& r : gConfig.relays) relayToJson(r, a.add<JsonObject>());
    return true;
  }
  String j;
  err = Boards::shareJson(board, j);
  if (err.length()) return false;
  JsonDocument d;
  if (deserializeJson(d, j)) { err = "Stored settings of that board are damaged"; return false; }
  out.set(d[kind]);
  if (!out.is<JsonArray>()) out.to<JsonArray>();
  return true;
}

void saveList(Ctx& c, int board, const char* kind, JsonDocument& list, const String& okMsg) {
  if (board == 0) {
    bool sensors = !strcmp(kind, "sensors");
    changeConfig(c.req, [&](Config& cfg, String& err) {
      if (!sensors) return relaysFromJson(list.as<JsonVariantConst>(), cfg.relays, err);
      std::vector<SensorConfig> mine;
      if (!sensorsFromJson(list.as<JsonVariantConst>(), mine, err)) return false;
      for (auto& s : cfg.sensors) if (s.type == SensorType::REMOTE) mine.push_back(s);   // mirrors stay
      cfg.sensors = mine;
      return true;
    }, okMsg);
    return;
  }
  JsonVariantConst v = list.as<JsonVariantConst>();
  String err = !strcmp(kind, "sensors") ? Boards::setShare(board, v, JsonVariantConst()) : Boards::setShare(board, JsonVariantConst(), v);
  sendResult(c.req, err, okMsg + ". Sending it to the board.");
}

int findIn(JsonDocument& list, const String& id) {
  JsonArray a = list.as<JsonArray>();
  for (size_t i = 0; i < a.size(); i++) if (id == (const char*)(a[i]["id"] | "")) return i;
  return -1;
}

String makeId(JsonDocument& list, String name) {
  String id;
  name.toLowerCase();
  for (char ch : name) id += isalnum((unsigned char)ch) ? ch : '_';
  while (id.indexOf("__") >= 0) id.replace("__", "_");
  if (!id.length() || !isalpha((unsigned char)id[0])) id = "x" + id;
  id = id.substring(0, 28);
  String base = id;
  for (int n = 2; findIn(list, id) >= 0; n++) id = base + String(n);
  return id;
}

// {"limits":{"wl":..,"wh":..,"cl":..,"ch":..}} -> the sensor's own field names
void mapLimits(JsonVariant item, JsonVariantConst body) {
  for (JsonPairConst kv : body.as<JsonObjectConst>()) {
    if (strcmp(kv.key().c_str(), "limits") != 0) { item[kv.key()] = kv.value(); continue; }
    static const char* const from[] = {"wl", "wh", "cl", "ch"};
    static const char* const to[] = {"warnLow", "warnHigh", "critLow", "critHigh"};
    for (int k = 0; k < 4; k++) if (!kv.value()[from[k]].isUnbound()) item[to[k]] = kv.value()[from[k]];
  }
}

void listGet(Ctx& c, const char* kind) {
  int b = boardParam(c);
  if (b < 0) { sendError(c.req, 400, "board: local or b1, b2...", "board"); return; }
  JsonDocument list; String err;
  if (!loadList(b, kind, list, err)) { sendError(c.req, 404, err); return; }
  if (b == 0 && !strcmp(kind, "relays")) {   // live state of this board's actuators
    StateLock lk;
    JsonArray a = list.as<JsonArray>();
    for (size_t i = 0; i < a.size() && i < gRelayRt.size(); i++) {
      JsonObject s = a[i]["state"].to<JsonObject>();
      s["value"] = Relays::value(i); s["on"] = gRelayRt[i].on; s["by"] = gRelayRt[i].by; s["manual"] = gRelayRt[i].manual;
      if (gRelayRt[i].manualUntil) s["manualForS"] = (int32_t)(gRelayRt[i].manualUntil - millis()) / 1000;
    }
  }
  if (c.p[0].length()) {
    int i = findIn(list, c.p[0]);
    if (i < 0) { sendError(c.req, 404, "No '" + c.p[0] + "' on that board"); return; }
    JsonDocument one; one.set(list[i]);
    sendJson(c.req, 200, one);
    return;
  }
  JsonDocument out; out["board"] = c.q("board", "local"); out["items"] = list;
  sendJson(c.req, 200, out);
}

void listChange(Ctx& c, const char* kind, char op) {   // op: a(dd) p(atch) d(elete)
  int b = boardParam(c);
  if (b < 0) { sendError(c.req, 400, "board: local or b1, b2...", "board"); return; }
  JsonDocument list; String err;
  if (!loadList(b, kind, list, err)) { sendError(c.req, 404, err); return; }
  const char* what = !strcmp(kind, "sensors") ? "Sensor" : "Actuator";
  if (op == 'a') {
    if (!c.body.is<JsonObject>()) { sendError(c.req, 400, "Send the settings as a JSON object"); return; }
    String id = c.body["id"] | "";
    if (!id.length()) id = makeId(list, c.body["name"] | what);
    else if (findIn(list, id) >= 0) { sendError(c.req, 409, String(what) + " '" + id + "' already exists", "id"); return; }
    JsonObject item = list.as<JsonArray>().add<JsonObject>();
    mapLimits(item, c.body.as<JsonVariantConst>());
    item["id"] = id;
    saveList(c, b, kind, list, String(what) + " '" + id + "' added");
    return;
  }
  int i = findIn(list, c.p[0]);
  if (i < 0) { sendError(c.req, 404, String("No ") + what + " '" + c.p[0] + "' on that board"); return; }
  if (op == 'd') { list.as<JsonArray>().remove(i); saveList(c, b, kind, list, String(what) + " removed"); return; }
  if (!c.body.is<JsonObject>()) { sendError(c.req, 400, "Send the fields to change as a JSON object"); return; }
  if (!c.body["id"].isNull() && c.p[0] != (const char*)(c.body["id"] | "")) { sendError(c.req, 400, "The id can't be changed: add a new one instead", "id"); return; }
  mapLimits(list[i], c.body.as<JsonVariantConst>());
  saveList(c, b, kind, list, "Saved");
}

// ================================================================== handlers
void hIndex(Ctx& c) {
  JsonDocument d; d["api"] = 1;
  JsonObject l = d["links"].to<JsonObject>();
  for (auto k : {"info", "system", "points", "alerts", "sensors", "actuators", "boards", "settings", "pins", "network", "events", "history", "keys"})
    l[k] = String(API "/") + k;
  l["ws"] = API "/ws";
  sendJson(c.req, 200, d);
}

void hInfo(Ctx& c) {
  JsonDocument d;
  {
    StateLock lk;
    d["appName"] = gConfig.ui.appName; d["machineName"] = gConfig.ui.machineName;
    d["firmware"] = FW_VERSION; d["build"] = Health::buildStamp(); d["variant"] = CM_VARIANT;
    JsonArray api = d["api"].to<JsonArray>(); api.add(1);
    d["protocol"] = 1; d["role"] = Boards::role();
    const BoardProfile& b = Board::current();
    JsonObject bo = d["board"].to<JsonObject>(); bo["id"] = b.id; bo["name"] = b.name;
    d["chip"] = Board::chip();
    JsonArray cap = d["capabilities"].to<JsonArray>();
    for (auto k : {"actuators", "pwm", "history", "events", "discover", "pins", "keys"}) cap.add(k);
    if (Boards::isMain()) { cap.add("boards"); cap.add("cloud"); cap.add("weather"); }
    d["uptimeS"] = millis() / 1000;
    if (gSys.timeSynced) d["epoch"] = (uint32_t)time(nullptr);
    d["hostname"] = gConfig.sys.hostname; d["ip"] = gSys.ip; d["apIp"] = gSys.apIp;
    if (Health::safeMode()) d["safeMode"] = Health::safeModeReason();
  }
  sendJson(c.req, 200, d);
}

void hSystem(Ctx& c) { JsonDocument d; Health::toJson(d.to<JsonObject>()); sendJson(c.req, 200, d); }
void hCrash(Ctx& c) {
  JsonDocument all; Health::toJson(all.to<JsonObject>());
  if (all["crash"].isNull()) { sendError(c.req, 404, "No crash stored"); return; }
  JsonDocument d; d.set(all["crash"]); sendJson(c.req, 200, d);
}
void hCrashClear(Ctx& c) { Health::clearCrash(); sendOk(c.req, "Crash report cleared"); }
void hCrashDump(Ctx& c) {
  uint32_t off = 0, size = 0;
  if (!Health::crashDumpLocation(off, size)) { sendError(c.req, 404, "No crash dump stored"); return; }
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  AsyncWebServerResponse* res = c.req->beginResponse("application/octet-stream", size,
    [p, off, size](uint8_t* buf, size_t max, size_t index) -> size_t {
      if (index >= size) return 0;
      size_t n = std::min<size_t>(max, size - index);
      return esp_partition_read(p, off + index, buf, n) == ESP_OK ? n : 0;
    });
  res->addHeader("Content-Disposition", "attachment; filename=crash.elf");
  c.req->send(res);
}
void hReboot(Ctx& c) { sendOk(c.req, "Restarting"); requestReboot(1000); }
void hLeaveSafe(Ctx& c) { Health::leaveSafeMode(); sendOk(c.req, "Leaving safe mode: restarting"); requestReboot(1000); }
void hTime(Ctx& c) {
  double epoch = c.body["epoch"] | 0.0;
  if (epoch < 1.6e9) { sendError(c.req, 400, "epoch: Unix seconds", "epoch"); return; }
  struct timeval tv; tv.tv_sec = (time_t)epoch; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  StateLock lk;
  gSys.timeSynced = true;
  if (!gNet.ntp) gSys.timeSource = "browser";
  sendOk(c.req, "Time set");
}

void hPoints(Ctx& c) {
  std::vector<String> only;
  String a = c.q("a");
  while (a.length()) { int k = a.indexOf(','); only.push_back(k < 0 ? a : a.substring(0, k)); a = k < 0 ? "" : a.substring(k + 1); }
  bool meta = c.q("meta") == "1";
  JsonDocument d;
  { StateLock lk; pointsToJson(d.to<JsonObject>(), meta, only); }
  sendJson(c.req, 200, d);
}
void hPoint(Ctx& c) {
  JsonDocument d;
  { StateLock lk; pointsToJson(d.to<JsonObject>(), true, {c.p[0]}); }
  if (d["items"].size() == 0) { sendError(c.req, 404, "No point '" + c.p[0] + "'"); return; }
  JsonDocument one; one.set(d["items"][0]); sendJson(c.req, 200, one);
}
void hHistory(Ctx& c) {
  std::shared_ptr<History::Reader> rd;
  { StateLock lk; rd = std::make_shared<History::Reader>(); }
  AsyncWebServerResponse* res = c.req->beginChunkedResponse("application/json",
    [rd](uint8_t* buf, size_t max, size_t) -> size_t { return rd->read(buf, max); });
  res->addHeader("Cache-Control", "no-store");
  c.req->send(res);
}

void hAlerts(Ctx& c) { JsonDocument d; { StateLock lk; alertsToJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }
void hMute(Ctx& c) {
  int m = c.body["minutes"] | 15;
  if (m <= 0) Alerts::unmute(); else Alerts::mute(std::min(m, 120) * 60000UL);
  sendOk(c.req, m <= 0 ? "Unmuted" : "Muted for " + String(std::min(m, 120)) + " min");
}

void hSensors(Ctx& c) { listGet(c, "sensors"); }
void hSensorAdd(Ctx& c) { listChange(c, "sensors", 'a'); }
void hSensorPatch(Ctx& c) {
  // Changing only limits needs scope "control"; anything else "admin".
  bool limitsOnly = c.body.is<JsonObject>() && c.body.size() == 1 && c.body["limits"].is<JsonObject>();
  if (!limitsOnly && callerScope(c.req) < SCOPE_ADMIN) { allowed(c.req, SCOPE_ADMIN); return; }
  listChange(c, "sensors", 'p');
}
void hSensorDelete(Ctx& c) { listChange(c, "sensors", 'd'); }
void hCalibrate(Ctx& c) {   // RPM: the engine speed you see now (e.g. idle on a tachometer)
  String id = c.p[0]; float rpm = c.body["rpm"] | 0.0f;
  float hz = Sensors::rpmHz(id);
  if (isnan(hz)) { sendError(c.req, 400, "Not an active RPM sensor"); return; }
  if (rpm < 200 || rpm > 8000) { sendError(c.req, 400, "rpm: the current engine speed (200-8000)", "rpm"); return; }
  if (hz < 5) { sendError(c.req, 400, "No pulses: is it running and the signal wired?"); return; }
  float ppr = hz * 60.0f / rpm;
  changeConfig(c.req, [&](Config& cfg, String&) { for (auto& s : cfg.sensors) if (s.id == id) s.pulsesPerRev = ppr; return true; },
               "Calibrated: " + String(ppr, 2) + " pulses per revolution (" + String(hz, 1) + " Hz at " + String(rpm, 0) + " RPM)");
}
void hDiscoverStart(Ctx& c) {
  if (boardParam(c) != 0) { sendError(c.req, 400, "Run discovery on that board's own page (its access point)"); return; }
  Discover::start(); sendOk(c.req, "Searching", 202);
}
void hDiscover(Ctx& c) { JsonDocument d; { StateLock lk; Discover::toJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }

void hActuators(Ctx& c) { listGet(c, "relays"); }
void hActAdd(Ctx& c) { listChange(c, "relays", 'a'); }
void hActPatch(Ctx& c) { listChange(c, "relays", 'p'); }
void hActDelete(Ctx& c) { listChange(c, "relays", 'd'); }
void hActCommand(Ctx& c) {
  int b = boardParam(c);
  if (b < 0) { sendError(c.req, 400, "board: local or b1, b2...", "board"); return; }
  if (!c.body.is<JsonObject>()) { sendError(c.req, 400, "Send a command such as {\"on\":true}"); return; }
  if (b == 0) { sendResult(c.req, Relays::command(c.p[0], c.body.as<JsonVariantConst>()), "Done"); return; }
  JsonDocument list; String err;
  if (!loadList(b, "relays", list, err)) { sendError(c.req, 404, err); return; }
  int i = findIn(list, c.p[0]);
  if (i < 0) { sendError(c.req, 404, "No actuator '" + c.p[0] + "' on that board"); return; }
  sendResult(c.req, Boards::actuatorCommand(b, i, c.body.as<JsonVariantConst>()), "Sent");
}

int boardId(Ctx& c) { return c.p[0].toInt(); }
void hBoards(Ctx& c) {
  JsonDocument d; Boards::toJson(d.to<JsonObject>());
  if (!d["boards"].isNull()) { d["items"] = d["boards"]; d.remove("boards"); }
  sendJson(c.req, 200, d);
}
void hBoardPair(Ctx& c) {
  int id = -1;
  String err = Boards::accept(c.body["mac"] | "", c.body["name"] | "", id);
  if (err.length()) sendError(c.req, 400, err);
  else { JsonDocument d; d["ok"] = true; d["id"] = id; d["message"] = "Paired as b" + String(id) + ": set up its sensors and actuators next"; sendJson(c.req, 200, d); }
}
void hBoardRename(Ctx& c) { sendResult(c.req, Boards::rename(boardId(c), c.body["name"] | ""), "Renamed"); }
void hBoardForget(Ctx& c) { sendResult(c.req, Boards::forget(boardId(c)), "Board forgotten"); }
void hBoardCommand(Ctx& c) {
  String cmd = c.body["cmd"] | "";
  sendResult(c.req, Boards::boardCommand(boardId(c), cmd, c.body["minutes"] | 15), cmd == "resend" ? "Sending the settings again" : "Sent");
}
void hBoardFirmware(Ctx& c) {
  int id = boardId(c);
  String src = c.body["source"] | "self";
  String err = src == "cancel" ? Boards::firmwareCancel(id) : src == "file" ? Boards::firmwareFromUpload(id) : Boards::firmwareFromSelf(id);
  sendResult(c.req, err, src == "cancel" ? "Cancelled" : "Sending the firmware; the board restarts when it has all of it");
}
void hDispCatalog(Ctx& c) {
  uint32_t rev = 0;
  String j = Boards::displayCatalog(rev);
  AsyncWebServerResponse* res = c.req->beginResponse(200, "application/json", j);
  res->addHeader("Cache-Control", "no-store"); res->addHeader("X-Catalog-Rev", String(rev));
  c.req->send(res);
}
void hDispValues(Ctx& c) { JsonDocument d; Boards::displayValues(d.to<JsonObject>()); sendJson(c.req, 200, d); }
void hDispCommand(Ctx& c) {
  String kind = c.body["kind"] | "";
  if (kind != "mute" && !allowed(c.req, SCOPE_CONTROL)) return;
  sendResult(c.req, Boards::displayCommand(c.body.as<JsonVariantConst>()), "Done");
}

void hSettings(Ctx& c) {
  JsonDocument d;
  { StateLock lk; configToJson(gConfig, d.to<JsonObject>(), false); d["rev"] = gSys.configRevision; }
  if (!c.p[0].length()) { sendJson(c.req, 200, d); return; }
  if (!isSection(c.p[0])) { sendError(c.req, 404, "Sections: ui, system, weather, cloud, turbo, layout, blackbox"); return; }
  JsonDocument one; one.set(d[c.p[0]]); sendJson(c.req, 200, one);
}
void hSettingsPatch(Ctx& c) {
  if (c.p[0] == "sensors") {   // the whole list at once, mirrors of other boards' sensors included (layout editor)
    if (!c.body.is<JsonArray>()) { sendError(c.req, 400, "Send the sensor list as a JSON array"); return; }
    changeConfig(c.req, [&](Config& cfg, String& err) { return sensorsFromJson(c.body.as<JsonVariantConst>(), cfg.sensors, err); }, "Saved");
    return;
  }
  if (!isSection(c.p[0])) { sendError(c.req, 404, "Sections: ui, system, weather, cloud, turbo, layout, blackbox, sensors"); return; }
  if (!c.body.is<JsonObject>()) { sendError(c.req, 400, "Send the fields to change as a JSON object"); return; }
  JsonDocument patch; patch[c.p[0]] = c.body;
  patchSettings(c.req, patch.as<JsonVariantConst>(), "Saved");
  if (c.p[0] == "weather") Cloud::requestWeather();
}
void hBackup(Ctx& c) {
  JsonDocument d;
  { StateLock lk; configToJson(gConfig, d.to<JsonObject>(), false); }
  String name; { StateLock lk; name = gConfig.sys.hostname; }
  sendJson(c.req, 200, d, (name + "-settings.json").c_str());
}
void hRestore(Ctx& c) {
  changeConfig(c.req, [&](Config& cfg, String& err) { return configFromJson(c.body.as<JsonVariantConst>(), cfg, err); }, "Settings restored");
}
void hRestoreSlot(Ctx& c) {   // {"slot":"previous"|"good"}: one-step undo / last settings that ran well
  String which = c.body["slot"] | "previous";
  CfgSlot slot = which == "good" ? CfgSlot::GOOD : CfgSlot::PREVIOUS;
  if (!hasConfigSlot(slot)) { sendError(c.req, 404, "No " + which + " settings saved yet"); return; }
  Config* cfg = new Config();
  if (!loadConfig(*cfg, slot)) { delete cfg; sendError(c.req, 409, "The saved " + which + " settings could not be read"); return; }
  requestConfigApply(cfg);
  sendOk(c.req, "Restored the " + which + " settings");
}
void hReset(Ctx& c) {
  bool keepSystem = true;
  if (c.body["keep"].is<JsonArray>()) { keepSystem = false; for (JsonVariant k : c.body["keep"].as<JsonArray>()) if (k == "system") keepSystem = true; }
  Config* cfg = new Config();
  configDefaults(*cfg);
  if (keepSystem) { StateLock lk; cfg->sys = gConfig.sys; }
  requestConfigApply(cfg);
  sendOk(c.req, keepSystem ? "Back to defaults (network, passwords and pins kept)" : "Back to defaults");
}
void hPresets(Ctx& c) {
  JsonDocument d; JsonArray a = d["items"].to<JsonArray>();
  File dir = LittleFS.open("/www/presets");
  if (dir && dir.isDirectory())
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      if (n.endsWith(".gz")) n.remove(n.length() - 3);
      if (n.endsWith(".json")) a.add(n.substring(0, n.length() - 5));
    }
  { StateLock lk; d["current"] = gConfig.ui.preset; }
  sendJson(c.req, 200, d);
}
void hPresetApply(Ctx& c) {
  String p = c.body["preset"] | "";
  for (char ch : p) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') { sendError(c.req, 400, "Unknown preset", "preset"); return; }
  File f = LittleFS.open("/www/presets/" + p + ".json");
  if (!f) { sendError(c.req, 404, "Unknown preset '" + p + "'", "preset"); return; }
  JsonDocument preset;
  DeserializationError e = deserializeJson(preset, f);
  f.close();
  if (e) { sendError(c.req, 409, "Preset file is damaged"); return; }
  preset["ui"]["preset"] = p;
  patchSettings(c.req, preset.as<JsonVariantConst>(), "Preset '" + p + "' applied");
}

void hPins(Ctx& c) { JsonDocument d; { StateLock lk; Pins::toJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }
void hPinTest(Ctx& c) {
  JsonDocument d; String err;
  { StateLock lk; err = Pins::test(c.p[0].toInt(), c.body["mode"] | "read", d.to<JsonObject>()); }
  if (err.length()) sendError(c.req, 400, err); else sendJson(c.req, 200, d);
}
void hLightTest(Ctx& c) { StatusLight::test(String(c.body["which"] | "builtin") == "external"); sendOk(c.req, "Red, green, blue"); }
void hProfiles(Ctx& c) {
  JsonDocument d;
  {
    StateLock lk;
    d["chip"] = Board::chip(); d["setting"] = gConfig.sys.board; d["detected"] = Board::detected().id;
    Board::profileToJson(Board::current(), d["current"].to<JsonObject>());
  }
  Board::listToJson(d["items"].to<JsonArray>());
  sendJson(c.req, 200, d);
}
void hProfileSet(Ctx& c) {   // {"board":"relay_x8","applyPins":true}
  String id = c.body["board"] | "auto";
  bool applyPins = c.body["applyPins"] | false;
  changeConfig(c.req, [&](Config& cfg, String& err) {
    if (id != "auto" && !Board::find(id)) { err = "Unknown board for this firmware"; return false; }
    cfg.sys.board = id;
    if (applyPins) applyBoardPins(cfg, Board::resolve(id));
    return true;
  }, applyPins ? "Board saved, pins moved to its defaults" : "Board saved");
}
void hAltitude(Ctx& c) {   // known altitude -> sea-level pressure reference
  if (c.body["altitudeM"].isNull()) { sendError(c.req, 400, "altitudeM: where you are now, in metres", "altitudeM"); return; }
  float alt = c.body["altitudeM"].as<float>(), p;
  { StateLock lk; p = gSys.stationHpa; }
  if (isnan(p)) { sendError(c.req, 400, "No air pressure reading (BMP/BME280)"); return; }
  if (alt < -500 || alt > 6000) { sendError(c.req, 400, "Altitude out of range", "altitudeM"); return; }
  float sl = p / powf(1.0f - alt / 44330.0f, 5.255f);
  changeConfig(c.req, [&](Config& cfg, String&) { cfg.sys.seaLevelHpa = sl; cfg.weather.autoSeaLevel = false; return true; },
               "Sea-level reference " + String(sl, 1) + " hPa (automatic weather correction off)");
}

void hNetwork(Ctx& c) { JsonDocument d; { StateLock lk; WifiMgr::toJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }
void hScan(Ctx& c) { WifiMgr::searchNow(); sendOk(c.req, "Searching for networks", 202); }
void hJoin(Ctx& c) {
  String ssid = c.body["ssid"] | "", pass = c.body["pass"] | "";
  if (!ssid.length() || ssid.length() > 32) { sendError(c.req, 400, "ssid: the network name", "ssid"); return; }
  changeConfig(c.req, [&](Config& cfg, String& err) {
    auto& n = cfg.sys.networks;
    for (size_t i = 0; i < n.size(); i++) if (n[i].ssid == ssid) { if (!pass.length()) pass = n[i].pass; n.erase(n.begin() + i); break; }
    if (n.size() >= MAX_WIFI_NETWORKS) { err = "8 networks saved: forget one first"; return false; }
    WifiNetwork w; w.ssid = ssid; w.pass = pass; w.priority = 9;
    n.insert(n.begin(), w);
    return true;
  }, "Saved: joining '" + ssid + "'");
}
void hForgetNet(Ctx& c) {
  String ssid = c.p[0];
  ssid.replace("%20", " ");
  changeConfig(c.req, [&](Config& cfg, String& err) {
    auto& n = cfg.sys.networks;
    for (size_t i = 0; i < n.size(); i++) if (n[i].ssid == ssid) { n.erase(n.begin() + i); return true; }
    err = "No saved network '" + ssid + "'"; return false;
  }, "Forgotten");
}

void hEvents(Ctx& c) {
  JsonDocument d;
  { StateLock lk; d["enabled"] = gConfig.blackbox.enabled; d["keep"] = gConfig.blackbox.keepEvents; }
  BlackBox::listToJson(d["items"].to<JsonArray>());
  sendJson(c.req, 200, d);
}
void hEvent(Ctx& c) {
  String body;
  if (!BlackBox::read(c.p[0].toInt(), body)) { sendError(c.req, 404, "No such event"); return; }
  AsyncWebServerResponse* r = c.req->beginResponse(200, "application/json", body);
  r->addHeader("Cache-Control", "no-store");
  c.req->send(r);
}
void hEventsClear(Ctx& c) { BlackBox::clear(); sendOk(c.req, "Recordings deleted"); }

void hWeather(Ctx& c) { JsonDocument d; { StateLock lk; Cloud::weatherToJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }
void hWeatherRefresh(Ctx& c) { Cloud::requestWeather(); sendOk(c.req, "Refreshing", 202); }
void hCloud(Ctx& c) { JsonDocument d; { StateLock lk; Cloud::statusToJson(d.to<JsonObject>()); } sendJson(c.req, 200, d); }
void hCloudFlush(Ctx& c) { Cloud::requestFlush(); sendOk(c.req, "Upload requested", 202); }
void hCloudClear(Ctx& c) { Cloud::requestClearBacklog(); sendOk(c.req, "Waiting data deleted"); }

void hKeys(Ctx& c) { JsonDocument d; Keys::toJson(d["items"].to<JsonArray>()); sendJson(c.req, 200, d); }
void hKeyAdd(Ctx& c) {
  String key, err = Keys::create(c.body["name"] | "", scopeFromStr(c.body["scope"] | ""), key);
  if (err.length()) { sendError(c.req, 400, err); return; }
  JsonDocument d; d["ok"] = true; d["key"] = key; d["message"] = "Copy the key now: it is not shown again";
  sendJson(c.req, 200, d);
}
void hKeyDelete(Ctx& c) { if (Keys::remove(c.p[0])) sendOk(c.req, "Key deleted"); else sendError(c.req, 404, "No such key"); }

// ---- raw uploads (body streamed straight to flash, never held in RAM)
struct Upload { bool active = false, ok = false; String err; };
Upload up;
void hFirmwareDone(Ctx& c) {
  if (!up.ok) { { StateLock lk; gSys.otaProgress = -1; } if (!LittleFS.begin()) LittleFS.begin(true); sendError(c.req, 400, "Update failed: " + up.err); return; }
  { StateLock lk; gSys.otaProgress = 100; }
  sendOk(c.req, "Installed: restarting");
  requestReboot(1500);
}
void hBoardFileDone(Ctx& c) { if (up.ok) sendOk(c.req, "Firmware file stored on this board"); else sendError(c.req, 400, up.err); }

// ---- the table. "*" = one path part, passed to the handler in p[0], p[1].
const Route kRoutes[] = {
  {HTTP_GET, "", SCOPE_READ, hIndex},
  {HTTP_GET, "info", SCOPE_NONE, hInfo},
  {HTTP_GET, "system", SCOPE_READ, hSystem},
  {HTTP_GET, "system/crash", SCOPE_READ, hCrash},
  {HTTP_DELETE, "system/crash", SCOPE_ADMIN, hCrashClear},
  {HTTP_GET, "system/crash/dump", SCOPE_ADMIN, hCrashDump},
  {HTTP_POST, "system/reboot", SCOPE_ADMIN, hReboot},
  {HTTP_POST, "system/safe-mode/leave", SCOPE_ADMIN, hLeaveSafe},
  {HTTP_POST, "system/time", SCOPE_READ, hTime},
  {HTTP_GET, "points", SCOPE_READ, hPoints},
  {HTTP_GET, "points/*", SCOPE_READ, hPoint},
  {HTTP_GET, "history", SCOPE_READ, hHistory},
  {HTTP_GET, "alerts", SCOPE_READ, hAlerts},
  {HTTP_POST, "alerts/mute", SCOPE_READ, hMute},   // one tap away, like the button on the box
  {HTTP_GET, "sensors", SCOPE_READ, hSensors},
  {HTTP_POST, "sensors", SCOPE_ADMIN, hSensorAdd},
  {HTTP_GET, "sensors/discover", SCOPE_READ, hDiscover},
  {HTTP_POST, "sensors/discover", SCOPE_ADMIN, hDiscoverStart},
  {HTTP_GET, "sensors/*", SCOPE_READ, hSensors},
  {HTTP_PATCH, "sensors/*", SCOPE_CONTROL, hSensorPatch},
  {HTTP_DELETE, "sensors/*", SCOPE_ADMIN, hSensorDelete},
  {HTTP_POST, "sensors/*/calibrate", SCOPE_ADMIN, hCalibrate},
  {HTTP_GET, "actuators", SCOPE_READ, hActuators},
  {HTTP_POST, "actuators", SCOPE_ADMIN, hActAdd},
  {HTTP_GET, "actuators/*", SCOPE_READ, hActuators},
  {HTTP_PATCH, "actuators/*", SCOPE_ADMIN, hActPatch},
  {HTTP_DELETE, "actuators/*", SCOPE_ADMIN, hActDelete},
  {HTTP_POST, "actuators/*/command", SCOPE_CONTROL, hActCommand},
  {HTTP_GET, "boards", SCOPE_READ, hBoards},
  {HTTP_POST, "boards", SCOPE_ADMIN, hBoardPair},
  {HTTP_PATCH, "boards/*", SCOPE_ADMIN, hBoardRename},
  {HTTP_DELETE, "boards/*", SCOPE_ADMIN, hBoardForget},
  {HTTP_POST, "boards/*/command", SCOPE_ADMIN, hBoardCommand},
  {HTTP_POST, "boards/*/firmware", SCOPE_ADMIN, hBoardFirmware},
  {HTTP_PUT, "boards/firmware-file", SCOPE_ADMIN, hBoardFileDone},
  {HTTP_GET, "displays/catalog", SCOPE_READ, hDispCatalog},
  {HTTP_GET, "displays/values", SCOPE_READ, hDispValues},
  {HTTP_POST, "displays/command", SCOPE_READ, hDispCommand},   // mute open; others checked inside
  {HTTP_GET, "settings", SCOPE_READ, hSettings},
  {HTTP_GET, "settings/*", SCOPE_READ, hSettings},
  {HTTP_PATCH, "settings/*", SCOPE_ADMIN, hSettingsPatch},
  {HTTP_POST, "settings/reset", SCOPE_ADMIN, hReset},
  {HTTP_POST, "settings/restore", SCOPE_ADMIN, hRestoreSlot},
  {HTTP_GET, "backup", SCOPE_ADMIN, hBackup},
  {HTTP_POST, "backup", SCOPE_ADMIN, hRestore},
  {HTTP_GET, "presets", SCOPE_READ, hPresets},
  {HTTP_POST, "presets/apply", SCOPE_ADMIN, hPresetApply},
  {HTTP_GET, "pins", SCOPE_READ, hPins},
  {HTTP_POST, "pins/*/test", SCOPE_ADMIN, hPinTest},
  {HTTP_GET, "hardware/profiles", SCOPE_READ, hProfiles},
  {HTTP_POST, "hardware/profile", SCOPE_ADMIN, hProfileSet},
  {HTTP_POST, "hardware/status-light/test", SCOPE_ADMIN, hLightTest},
  {HTTP_POST, "hardware/altitude", SCOPE_ADMIN, hAltitude},
  {HTTP_GET, "network", SCOPE_READ, hNetwork},
  {HTTP_POST, "network/scan", SCOPE_ADMIN, hScan},
  {HTTP_POST, "network/join", SCOPE_ADMIN, hJoin},
  {HTTP_DELETE, "network/saved/*", SCOPE_ADMIN, hForgetNet},
  {HTTP_GET, "events", SCOPE_READ, hEvents},
  {HTTP_GET, "events/*", SCOPE_READ, hEvent},
  {HTTP_DELETE, "events", SCOPE_ADMIN, hEventsClear},
  {HTTP_GET, "weather", SCOPE_READ, hWeather},
  {HTTP_POST, "weather/refresh", SCOPE_READ, hWeatherRefresh},
  {HTTP_GET, "cloud", SCOPE_READ, hCloud},
  {HTTP_POST, "cloud/flush", SCOPE_ADMIN, hCloudFlush},
  {HTTP_DELETE, "cloud/backlog", SCOPE_ADMIN, hCloudClear},
  {HTTP_GET, "keys", SCOPE_ADMIN, hKeys},
  {HTTP_POST, "keys", SCOPE_ADMIN, hKeyAdd},
  {HTTP_DELETE, "keys/*", SCOPE_ADMIN, hKeyDelete},
  {HTTP_PUT, "firmware", SCOPE_ADMIN, hFirmwareDone},
};

// Path after /api/v1/, without query or trailing slash. Exact segment matching.
const Route* match(uint32_t method, const String& path, String* parts, bool& pathKnown) {
  pathKnown = false;
  for (const Route& r : kRoutes) {
    std::string pp[2];
    if (!routeMatch(r.path, path.c_str(), pp, 2)) continue;
    pathKnown = true;
    if (r.method != method) continue;
    parts[0] = pp[0].c_str(); parts[1] = pp[1].c_str();
    return &r;
  }
  return nullptr;
}

String apiPath(AsyncWebServerRequest* req) {
  String p = req->url().substring(strlen(API));
  if (p.startsWith("/")) p.remove(0, 1);
  if (p.endsWith("/")) p.remove(p.length() - 1);
  return p;
}

// Body: JSON gathered in a malloc'd buffer (the library frees _tempObject);
// PUT firmware / firmware-file streamed to flash.
void onBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  String path = apiPath(req);
  bool fw = path == "firmware", bfile = path == "boards/firmware-file";
  if (req->method() == HTTP_PUT && (fw || bfile)) {
    if (index == 0) {
      up = Upload();
      up.active = callerScope(req) >= SCOPE_ADMIN;
      if (!up.active) { up.err = "admin password or key needed"; return; }
      up.ok = true;
      if (fw) {
        bool webapp = req->hasParam("target") && req->getParam("target")->value() == "webapp";
        Serial.printf("[ota] %s upload, %u bytes\n", webapp ? "web app" : "firmware", (unsigned)total);
        // Say which file was wrong instead of the library's "Bad Size Given".
        const esp_partition_t* slot = webapp ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr)
                                             : esp_ota_get_next_update_partition(nullptr);
        size_t room = slot ? slot->size : 0;
        String fit = String(room / 1024) + " KB";
        if (!webapp && len > 0 && data[0] != 0xE9) { up.ok = false; up.err = "Not a firmware file: choose the …-firmware.bin file"; return; }
        if (!webapp && total > room) {
          up.ok = false;
          up.err = total >= 0x3E0000 ? "That is a full-flash image (for USB only): choose the …-firmware.bin file"
                                     : "File too big for this board's firmware slot (" + fit + "): use the firmware made for this board";
          return;
        }
        if (webapp && total > room) {
          up.ok = false;
          up.err = "Web app file too big for this board (" + fit + "): use the web app file made for this board"
                   " (the 4 MB one for 4 MB boards, the 8 MB one for the main S3)";
          return;
        }
        if (webapp && len > 0 && data[0] == 0xE9) { up.ok = false; up.err = "That is a firmware file: choose the web app (…littlefs.bin) file here"; return; }
        if (webapp) LittleFS.end();
        if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, webapp ? U_SPIFFS : U_FLASH)) { up.ok = false; up.err = Update.errorString(); return; }
        StateLock lk; gSys.otaProgress = 0;
      }
    }
    if (!up.active || !up.ok) return;
    bool final = index + len >= total;
    if (fw) {
      if (Update.write(data, len) != len) { up.ok = false; up.err = Update.errorString(); Update.abort(); return; }
      if (total) { StateLock lk; gSys.otaProgress = std::min(99, (int)((index + len) * 100 / total)); }
      if (final && !Update.end(true)) { up.ok = false; up.err = Update.errorString(); }
    } else {
      String err;
      if (!Boards::uploadChunk(index, data, len, final, err)) { up.ok = false; up.err = err; }
    }
    return;
  }
  if (total > MAX_BODY) return;   // refused in onRequest
  if (index == 0) {
    if (req->_tempObject) free(req->_tempObject);
    req->_tempObject = malloc(total + 1);
    if (!req->_tempObject) return;
  }
  if (!req->_tempObject) return;
  memcpy((uint8_t*)req->_tempObject + index, data, len);
  if (index + len == total) ((char*)req->_tempObject)[total] = 0;
}

void onRequest(AsyncWebServerRequest* req) {
  String path = apiPath(req);
  Ctx c; c.req = req;
  bool known;
  const Route* r = match(req->method(), path, c.p, known);
  if (!r) { if (known) sendError(req, 405, "Method not allowed here"); else sendError(req, 404, "No such address: see " API); return; }
  if (!allowed(req, r->need)) return;
  if (req->contentLength() > MAX_BODY && req->method() != HTTP_PUT) { sendError(req, 413, "Request too big"); return; }
  if (req->_tempObject && req->method() != HTTP_PUT) {
    DeserializationError e = deserializeJson(c.body, (const char*)req->_tempObject);
    if (e) { sendError(req, 400, String("Body is not valid JSON: ") + e.c_str()); return; }
  }
  r->fn(c);
}

// ================================================================== live
String livePoints() {
  JsonDocument d;
  d["type"] = "points";
  {
    StateLock lk;
    JsonDocument p; pointsToJson(p.to<JsonObject>(), false, {});
    d["t"] = p["t"]; d["level"] = p["level"]; d["muted"] = p["muted"]; d["running"] = p["running"]; d["rev"] = p["rev"];
    if (!p["turbo"].isNull()) d["turbo"] = p["turbo"];
    if (!p["epoch"].isNull()) d["epoch"] = p["epoch"];
    JsonObject v = d["v"].to<JsonObject>();
    for (JsonObject x : p["items"].as<JsonArray>()) {
      JsonArray a = v[(const char*)x["a"]].to<JsonArray>();
      a.add(x["v"]); a.add(x["st"]); a.add(x["lvl"]);
    }
  }
  String s; serializeJson(d, s);
  return s;
}
String liveAlerts() {
  JsonDocument d; d["type"] = "alerts";
  { StateLock lk; alertsToJson(d.as<JsonObject>()); }
  String s; serializeJson(d, s);
  return s;
}

uint8_t wsScope(uint32_t id) { StateLock lk; for (auto& w : wsInfo) if (w.id == id) return w.scope; return SCOPE_READ; }

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    if (server->count() > WS_MAX_CLIENTS) { client->close(1013, "too many clients"); return; }
    bool readAuth; { StateLock lk; readAuth = gConfig.sys.readNeedsAuth; wsInfo.push_back({client->id(), (uint8_t)(readAuth ? SCOPE_NONE : SCOPE_READ)}); }
    JsonDocument d; d["type"] = "hello"; d["api"] = 1; d["firmware"] = FW_VERSION;
    { StateLock lk; d["rev"] = gSys.configRevision; d["appName"] = gConfig.ui.appName; }
    String s; serializeJson(d, s); client->text(s);
    if (!readAuth) { client->text(livePoints()); client->text(liveAlerts()); }
  } else if (type == WS_EVT_DISCONNECT) {
    StateLock lk;
    for (size_t i = 0; i < wsInfo.size(); i++) if (wsInfo[i].id == client->id()) { wsInfo.erase(wsInfo.begin() + i); break; }
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > 1024) return;
    JsonDocument in;
    if (deserializeJson(in, data, len)) return;
    JsonDocument out; out["type"] = "ack"; out["id"] = in["id"] | 0;
    String t = in["type"] | "", err;
    uint8_t scope = wsScope(client->id());
    if (t == "auth") {
      String key = in["key"] | "", pass = in["pass"] | "", admin;
      { StateLock lk; admin = gConfig.sys.adminPass; }
      uint8_t s = key.length() ? Keys::check(key) : (pass.length() && pass == admin ? SCOPE_ADMIN : SCOPE_NONE);
      if (s == SCOPE_NONE) err = "Wrong password or key";
      else { StateLock lk; for (auto& w : wsInfo) if (w.id == client->id()) w.scope = s; out["scope"] = scopeStr(s); }
    } else if (t == "command") {
      // {"path":"/actuators/fan/command","body":{...},"board":"local"} or {"path":"/alerts/mute","body":{"minutes":15}}
      String path = in["path"] | "";
      JsonVariantConst body = in["body"];
      if (path == "/alerts/mute") { int m = body["minutes"] | 15; if (m <= 0) Alerts::unmute(); else Alerts::mute(std::min(m, 120) * 60000UL); }
      else if (path.startsWith("/actuators/") && path.endsWith("/command")) {
        if (scope < SCOPE_CONTROL) err = "Needs scope 'control': send {\"type\":\"auth\",\"key\":...} first";
        else {
          String id = path.substring(11, path.length() - 8);
          String b = in["board"] | "local";
          if (b == "local") err = Relays::command(id, body);
          else err = "Commands for other boards: use HTTP POST /actuators/{id}/command?board=" + b;
        }
      } else err = "Over the socket: /alerts/mute and /actuators/{id}/command";
    } else if (t != "subscribe") err = "Unknown message type";
    out["ok"] = err.length() == 0;
    if (err.length()) out["message"] = err;
    String s; serializeJson(out, s); client->text(s);
  }
}

void setupServer() {
  server.on(AsyncURIMatcher::prefix(API), HTTP_ANY, onRequest, nullptr, onBody);
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  // SSE: capped BEFORE the connection is adopted (closing it in onConnect crashed async_tcp, 5.1.0-beta.8).
  events.authorizeConnect([](AsyncWebServerRequest* req) { return events.count() < SSE_MAX_CLIENTS && callerScope(req) >= SCOPE_READ; });
  events.onConnect([](AsyncEventSourceClient* client) { client->send(livePoints().c_str(), "points", ++eventId, 3000); });
  server.addHandler(&events);
  // Web app (only /www is served: settings never live on the file system)
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("no-cache");
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->url().startsWith("/api/")) { sendError(req, 404, "No such address: the API is at " API); return; }
    if (!LittleFS.exists("/www/index.html") && !LittleFS.exists("/www/index.html.gz")) {
      req->send(200, "text/html",
                "<h3>" FW_VERSION "</h3><p>The web app is not installed. Choose <b>littlefs.bin</b>:</p>"
                "<input type=file id=f><button onclick=\"fetch('" API "/firmware?target=webapp',{method:'PUT',body:f.files[0]})"
                ".then(r=>r.text()).then(t=>document.body.append(t))\">Upload</button>");
      return;
    }
    req->redirect("/");
  });
}
}  // namespace

void Web::begin() {
  Keys::begin();
  setupServer();
  server.begin();
  Serial.println("[web] API v1 at " API);
}

void Web::notifyConfigChanged() { configChanged = true; }

// Live channel subscribers that may read.
void sendLive(const String& msg, const char* event) {
  if (events.count() > 0 && events.avgPacketsWaiting() < 3) events.send(msg.c_str(), event, ++eventId);
  if (!ws.count()) return;
  AsyncWebSocketSharedBuffer buf = std::make_shared<std::vector<uint8_t>>(msg.begin(), msg.end());
  std::vector<uint32_t> ok;
  { StateLock lk; for (auto& w : wsInfo) if (w.scope >= SCOPE_READ) ok.push_back(w.id); }
  for (auto& c : ws.getClients())
    if (c.status() == WS_CONNECTED && c.queueLen() < 3 && std::find(ok.begin(), ok.end(), c.id()) != ok.end()) c.text(buf);
}

void Web::update() {
  uint32_t now = millis();
  if (configChanged) {
    configChanged = false;
    uint32_t rev; { StateLock lk; rev = gSys.configRevision; }
    sendLive("{\"type\":\"config\",\"rev\":\"" + String(rev) + "\"}", "config");
  }
  if (now - lastPush < 1000) return;
  lastPush = now;
  ws.cleanupClients(WS_MAX_CLIENTS);
  Health::setClientCounts(events.count(), ws.count());
  if (!events.count() && !ws.count()) return;
  sendLive(livePoints(), "points");
  uint8_t lvl; bool muted; { StateLock lk; lvl = gSys.overallLevel; muted = gSys.muted; }
  if (lvl != lastLevel || muted != lastMuted) { lastLevel = lvl; lastMuted = muted; sendLive(liveAlerts(), "alerts"); }
}
