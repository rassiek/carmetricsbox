// CarMetrics - HTTP API v1 (docs/API.md), live updates (WebSocket + SSE) and the
// web app. One route table; each route names the scope it needs.
#include "Web.h"
#include "State.h"
#include "Sensors.h"
#include "Outputs.h"
#include "Health.h"
#include "Board.h"
#include "WifiMgr.h"
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
const Web::Remote* remote = nullptr;               // other boards (main firmware only)
void (*infoHook)(JsonObject) = nullptr;            // role, capabilities of this firmware
void (*afterPatch)(const String& section) = nullptr;
std::vector<std::pair<const Web::Route*, size_t>> extraRoutes;

}  // namespace

// ================================================================== helpers (namespace Web, used by firmware routes too)
namespace Web {
bool roomFor(size_t n) {   // one free block big enough (+ headroom for Wi-Fi / TCP)?
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > n + 8192;
}

void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc, const char* filename) {
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

void sendError(AsyncWebServerRequest* req, int http, const String& msg, const char* field) {
  const char* code = http == 401 ? "auth" : http == 403 ? "scope" : http == 404 ? "not_found" : http == 409 ? "conflict" : http == 503 ? "busy" : "invalid";
  JsonDocument d;
  JsonObject e = d["error"].to<JsonObject>();
  e["code"] = code; e["message"] = msg;
  if (field) e["field"] = field;
  sendJson(req, http, d);
}

void sendOk(AsyncWebServerRequest* req, const String& msg, int http) {
  JsonDocument d;
  d["ok"] = true;
  if (msg.length()) d["message"] = msg;
  sendJson(req, http, d);
}
// "" = ok, otherwise the message as a 400.
void sendResult(AsyncWebServerRequest* req, const String& err, const String& okMsg) {
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
// ?board=local (default) | b1..bN -> 0 | N, -1 bad
int boardParam(Ctx& c) {
  String b = c.q("board", "local");
  if (b == "local" || b == "0") return 0;
  if (b.startsWith("b") && b.length() > 1) { int n = b.substring(1).toInt(); if (n > 0) return n; }
  return -1;
}

}  // namespace Web

using namespace Web;
namespace {
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
  if (!remote) { err = "This board has no other boards"; return false; }
  if (!remote->load(board, kind, j, err)) return false;
  if (deserializeJson(out, j) || !out.is<JsonArray>()) out.to<JsonArray>();
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
  if (!remote) { sendError(c.req, 400, "This board has no other boards"); return; }
  String err = remote->save(board, kind, v);
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
    d["protocol"] = 1;
    const BoardProfile& b = Board::current();
    JsonObject bo = d["board"].to<JsonObject>(); bo["id"] = b.id; bo["name"] = b.name;
    d["chip"] = Board::chip();
    JsonArray cap = d["capabilities"].to<JsonArray>();
    for (auto k : {"sensors", "actuators", "pwm", "discover", "pins", "keys"}) cap.add(k);
    if (infoHook) infoHook(d.as<JsonObject>());   // role, extra capabilities
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
  sendResult(c.req, remote ? remote->command(b, i, c.body.as<JsonVariantConst>()) : String("This board has no other boards"), "Sent");
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
  if (afterPatch) afterPatch(c.p[0]);
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

// ---- the table. "*" = one path part, passed to the handler in p[0], p[1].
const Route kRoutes[] = {
  {HTTP_GET, "", SCOPE_READ, hIndex, nullptr},
  {HTTP_GET, "info", SCOPE_NONE, hInfo, nullptr},
  {HTTP_GET, "system", SCOPE_READ, hSystem, nullptr},
  {HTTP_GET, "system/crash", SCOPE_READ, hCrash, nullptr},
  {HTTP_DELETE, "system/crash", SCOPE_ADMIN, hCrashClear, nullptr},
  {HTTP_GET, "system/crash/dump", SCOPE_ADMIN, hCrashDump, nullptr},
  {HTTP_POST, "system/reboot", SCOPE_ADMIN, hReboot, nullptr},
  {HTTP_POST, "system/safe-mode/leave", SCOPE_ADMIN, hLeaveSafe, nullptr},
  {HTTP_POST, "system/time", SCOPE_READ, hTime, nullptr},
  {HTTP_GET, "points", SCOPE_READ, hPoints, nullptr},
  {HTTP_GET, "points/*", SCOPE_READ, hPoint, nullptr},
  {HTTP_GET, "alerts", SCOPE_READ, hAlerts, nullptr},
  {HTTP_POST, "alerts/mute", SCOPE_READ, hMute, nullptr},   // one tap away, like the button on the box
  {HTTP_GET, "sensors", SCOPE_READ, hSensors, nullptr},
  {HTTP_POST, "sensors", SCOPE_ADMIN, hSensorAdd, nullptr},
  {HTTP_GET, "sensors/discover", SCOPE_READ, hDiscover, nullptr},
  {HTTP_POST, "sensors/discover", SCOPE_ADMIN, hDiscoverStart, nullptr},
  {HTTP_GET, "sensors/*", SCOPE_READ, hSensors, nullptr},
  {HTTP_PATCH, "sensors/*", SCOPE_CONTROL, hSensorPatch, nullptr},
  {HTTP_DELETE, "sensors/*", SCOPE_ADMIN, hSensorDelete, nullptr},
  {HTTP_POST, "sensors/*/calibrate", SCOPE_ADMIN, hCalibrate, nullptr},
  {HTTP_GET, "actuators", SCOPE_READ, hActuators, nullptr},
  {HTTP_POST, "actuators", SCOPE_ADMIN, hActAdd, nullptr},
  {HTTP_GET, "actuators/*", SCOPE_READ, hActuators, nullptr},
  {HTTP_PATCH, "actuators/*", SCOPE_ADMIN, hActPatch, nullptr},
  {HTTP_DELETE, "actuators/*", SCOPE_ADMIN, hActDelete, nullptr},
  {HTTP_POST, "actuators/*/command", SCOPE_CONTROL, hActCommand, nullptr},
  {HTTP_GET, "settings", SCOPE_READ, hSettings, nullptr},
  {HTTP_GET, "settings/*", SCOPE_READ, hSettings, nullptr},
  {HTTP_PATCH, "settings/*", SCOPE_ADMIN, hSettingsPatch, nullptr},
  {HTTP_POST, "settings/reset", SCOPE_ADMIN, hReset, nullptr},
  {HTTP_POST, "settings/restore", SCOPE_ADMIN, hRestoreSlot, nullptr},
  {HTTP_GET, "backup", SCOPE_ADMIN, hBackup, nullptr},
  {HTTP_POST, "backup", SCOPE_ADMIN, hRestore, nullptr},
  {HTTP_GET, "pins", SCOPE_READ, hPins, nullptr},
  {HTTP_POST, "pins/*/test", SCOPE_ADMIN, hPinTest, nullptr},
  {HTTP_GET, "hardware/profiles", SCOPE_READ, hProfiles, nullptr},
  {HTTP_POST, "hardware/profile", SCOPE_ADMIN, hProfileSet, nullptr},
  {HTTP_POST, "hardware/status-light/test", SCOPE_ADMIN, hLightTest, nullptr},
  {HTTP_GET, "network", SCOPE_READ, hNetwork, nullptr},
  {HTTP_POST, "network/scan", SCOPE_ADMIN, hScan, nullptr},
  {HTTP_POST, "network/join", SCOPE_ADMIN, hJoin, nullptr},
  {HTTP_DELETE, "network/saved/*", SCOPE_ADMIN, hForgetNet, nullptr},
  {HTTP_GET, "keys", SCOPE_ADMIN, hKeys, nullptr},
  {HTTP_POST, "keys", SCOPE_ADMIN, hKeyAdd, nullptr},
  {HTTP_DELETE, "keys/*", SCOPE_ADMIN, hKeyDelete, nullptr},
  {HTTP_PUT, "firmware", SCOPE_ADMIN, hFirmwareDone, nullptr},
};

// Path after /api/v1/, without query or trailing slash. Exact segment matching.
const Route* matchIn(const Route* table, size_t n, uint32_t method, const String& path, String* parts, bool& pathKnown) {
  for (size_t i = 0; i < n; i++) {
    const Route& r = table[i];
    std::string pp[2];
    if (!routeMatch(r.path, path.c_str(), pp, 2)) continue;
    pathKnown = true;
    if (r.method != method) continue;
    parts[0] = pp[0].c_str(); parts[1] = pp[1].c_str();
    return &r;
  }
  return nullptr;
}
const Route* match(uint32_t method, const String& path, String* parts, bool& pathKnown) {
  pathKnown = false;
  for (auto& t : extraRoutes) if (const Route* r = matchIn(t.first, t.second, method, path, parts, pathKnown)) return r;
  return matchIn(kRoutes, sizeof(kRoutes) / sizeof(kRoutes[0]), method, path, parts, pathKnown);
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
  String parts[2]; bool known;
  const Route* route = req->method() == HTTP_PUT ? match(HTTP_PUT, path, parts, known) : nullptr;
  bool fw = route && path == "firmware";
  if (route && (fw || route->upload)) {
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
        if (!webapp && len > 0 && data[0] != 0xE9) { up.ok = false; up.err = "Not a firmware file: choose the …-firmware.bin file (…-full-flash.bin is for USB only)"; return; }
        if (!webapp && total > room) {
          up.ok = false;
          up.err = "File too big for this board's firmware slot (" + fit + "): choose the …-firmware.bin made for this board (…-full-flash.bin is for USB only)";
          return;
        }
        if (webapp && total > room) {
          up.ok = false;
          up.err = "Web app file too big for this board (" + fit + "): use the …-webapp.bin made for this board";
          return;
        }
        if (webapp && len > 0 && data[0] == 0xE9) { up.ok = false; up.err = "That is a firmware file: choose the …-webapp.bin file here"; return; }
        if (webapp) LittleFS.end();
        if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, webapp ? U_SPIFFS : U_FLASH)) { up.ok = false; up.err = Update.errorString(); return; }
        StateLock lk; gSys.otaProgress = 0;
      }
    }
    if (!up.active || !up.ok) return;
    bool final = index + len >= total;
    // A USB image has the partition table at 0x8000: refuse it before it is installed as firmware.
    if (fw && index <= 0x8000 && index + len >= 0x8002 && data[0x8000 - index] == 0xAA && data[0x8001 - index] == 0x50) {
      up.ok = false; up.err = "That is the full-flash image (USB only): over Wi-Fi choose the …-firmware.bin file"; Update.abort(); return;
    }
    if (fw) {
      if (Update.write(data, len) != len) { up.ok = false; up.err = Update.errorString(); Update.abort(); return; }
      if (total) { StateLock lk; gSys.otaProgress = std::min(99, (int)((index + len) * 100 / total)); }
      if (final && !Update.end(true)) { up.ok = false; up.err = Update.errorString(); }
    } else {
      String err;
      if (!route->upload(index, data, len, final, err)) { up.ok = false; up.err = err; }
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
                "<h3>" FW_VERSION "</h3><p>The web app is not installed. Choose the <b>…-webapp.bin</b> file:</p>"
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
void Web::addRoutes(const Route* r, size_t n) { extraRoutes.push_back({r, n}); }
void Web::setRemote(const Remote* r) { remote = r; }
void Web::setInfo(void (*fn)(JsonObject)) { infoHook = fn; }
void Web::setAfterPatch(void (*fn)(const String&)) { afterPatch = fn; }
bool Web::uploadResult(String& err) { err = up.err; return up.ok; }

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
