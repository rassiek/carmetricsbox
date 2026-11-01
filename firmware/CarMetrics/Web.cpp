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
#include <set>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>

namespace {
AsyncWebServer server(80);
AsyncEventSource events("/events");
AsyncWebSocket ws("/ws");                 // two-way live channel (v2): state out, commands in
std::set<uint32_t> wsAuthed;               // client ids that sent the admin password
portMUX_TYPE wsMux = portMUX_INITIALIZER_UNLOCKED;
#define WS_MAX_CLIENTS 6
#define SSE_MAX_CLIENTS 3
uint32_t lastPush = 0, eventId = 0;
volatile bool configChanged = false;

// ------------------------------------------------------------------ helpers
// Settings, relays, OTA and reboot need the admin password (HTTP Basic, user "admin").
bool requireAuth(AsyncWebServerRequest* req) {
  String pass;
  { StateLock lk; pass = gConfig.sys.adminPass; }
  if (req->authenticate("admin", pass.c_str())) return true;
  req->requestAuthentication("CarMetrics", false);
  return false;
}

// Is there one free block big enough for a reply of n bytes (plus headroom for
// Wi-Fi / TCP)? Out of memory, the response buffer's `new` throws and the
// board aborts (crash 1 Nov 2026, async_tcp: cbuf::resize -> abort).
static bool roomFor(size_t n) {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > n + 8192;
}

void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
  size_t n = measureJson(doc);
  if (!roomFor(n)) { req->send(503, "text/plain", "Low memory, try again in a moment"); return; }
  // Buffer sized once: growing it byte by byte re-allocated and copied it for
  // every few bytes, needing twice the memory and fragmenting the heap.
  AsyncResponseStream* res = req->beginResponseStream("application/json", n + 32);
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *res);
  req->send(res);
}

void sendStatus(AsyncWebServerRequest* req, int code, const String& msg, bool reboot = false) {
  JsonDocument doc;
  doc["ok"] = code < 400;
  doc["message"] = msg;
  if (reboot) doc["reboot"] = true;
  sendJson(req, code, doc);
}

void stateToJson(JsonObject o) {  // lock held
  o["t"] = millis();
  o["rev"] = gSys.configRevision;
  o["engine"] = gSys.engineRunning;
  o["level"] = alertLevelStr(gSys.overallLevel);
  o["muted"] = gSys.muted;
  if (!gSys.timeSynced) o["clock"] = false;   // the web app then sends the browser's time
  else { o["epoch"] = (uint32_t)time(nullptr); o["clockSrc"] = gSys.timeSource; }
  if (Health::safeMode()) o["safe"] = true;
  if (gSys.ignition >= 0) o["ign"] = gSys.ignition == 1;
  if (gConfig.turbo.enabled) {
    JsonObject t = o["turbo"].to<JsonObject>();
    t["phase"] = gSys.turbo.phase == TURBO_COOLING ? "cooling" : gSys.turbo.phase == TURBO_OK ? "ok" : "off";
    t["leftS"] = gSys.turbo.leftS; t["safe"] = gConfig.turbo.safeEgt;
    if (isnan(gSys.turbo.egt)) t["egt"] = nullptr; else t["egt"] = serialized(String(gSys.turbo.egt, 0));
  }
  if (gConfig.sys.gpsRxPin >= 0) {
    JsonObject g = o["gps"].to<JsonObject>();
    g["fitted"] = gSys.gps.fitted; g["fix"] = gSys.gps.fix; g["sats"] = gSys.gps.sats;
    if (gSys.gps.fix) { g["lat"] = serialized(String(gSys.gps.lat, 6)); g["lon"] = serialized(String(gSys.gps.lon, 6)); }
  }
  if (gSys.muted) o["muteLeftS"] = (int32_t)(gSys.muteUntilMs - millis()) / 1000;
  JsonObject s = o["sensors"].to<JsonObject>();
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorRuntime& r = gSensorRt[i];
    JsonObject x = s[gConfig.sensors[i].id].to<JsonObject>();
    if (isnan(r.value)) x["v"] = nullptr; else x["v"] = serialized(String(r.value, gConfig.sensors[i].decimals + 1));
    x["st"] = sensorStatusStr(r.status);
    x["lvl"] = alertLevelStr(r.level);
    if (!isnan(r.volts)) x["volts"] = serialized(String(r.volts, 3));
  }
  JsonObject rl = o["relays"].to<JsonObject>();
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    JsonObject x = rl[gConfig.relays[i].id].to<JsonObject>();
    x["on"] = gRelayRt[i].on;
    x["out"] = gRelayRt[i].outputHigh;
    x["blinkMs"] = gRelayRt[i].blinkMs;
  }
}

String stateString() {
  JsonDocument doc;
  { StateLock lk; stateToJson(doc.to<JsonObject>()); }
  String s;
  serializeJson(doc, s);
  return s;
}

// Copy current config, let `edit` modify it, validate, hand to loop().
template <typename F>
void changeConfig(AsyncWebServerRequest* req, F edit, const char* okMsg) {
  Config* next;
  { StateLock lk; next = new Config(gConfig); }
  String err;
  if (!edit(*next, err) || !validateConfig(*next, err)) {
    delete next;
    sendStatus(req, 400, err);
    return;
  }
  requestConfigApply(next);
  sendStatus(req, 200, okMsg);
}

// ------------------------------------------------------------------ OTA upload
String uploadError;
bool uploadAuthorised = false;
size_t uploadTotal = 0;

void handleUpload(AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    String pass;
    { StateLock lk; pass = gConfig.sys.adminPass; }
    uploadAuthorised = req->authenticate("admin", pass.c_str());
    uploadError = "";
    if (!uploadAuthorised) return;
    String target = req->hasParam("target") ? req->getParam("target")->value() : "firmware";
    int cmd = target == "filesystem" ? U_SPIFFS : U_FLASH;
    uploadTotal = req->contentLength();
    Serial.printf("[ota] %s upload: %s (%u bytes)\n", target.c_str(), filename.c_str(), (unsigned)uploadTotal);
    if (cmd == U_SPIFFS) LittleFS.end();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) { uploadError = Update.errorString(); return; }
    StateLock lk; gSys.otaProgress = 0;
  }
  if (!uploadAuthorised || uploadError.length()) return;
  if (Update.write(data, len) != len) { uploadError = Update.errorString(); Update.abort(); return; }
  if (uploadTotal) { StateLock lk; gSys.otaProgress = min(99, (int)((index + len) * 100 / uploadTotal)); }
  if (final) {
    if (!Update.end(true)) uploadError = Update.errorString();
    else Serial.printf("[ota] done, %u bytes\n", (unsigned)(index + len));
  }
}

void handleUploadDone(AsyncWebServerRequest* req) {
  if (!uploadAuthorised) { requireAuth(req); return; }
  if (uploadError.length() || Update.hasError()) {
    { StateLock lk; gSys.otaProgress = -1; }
    if (!LittleFS.begin()) LittleFS.begin(true);
    sendStatus(req, 500, "Update failed: " + (uploadError.length() ? uploadError : String(Update.errorString())));
    return;
  }
  { StateLock lk; gSys.otaProgress = 100; }
  sendStatus(req, 200, "Update installed. Rebooting...", true);
  requestReboot(1500);
}

// ------------------------------------------------------------------ routes
void addJsonPost(const char* uri, ArJsonRequestHandlerFunction fn) {
  auto* h = new AsyncCallbackJsonWebHandler(uri, fn);
  h->setMethod(HTTP_POST);
  h->setMaxContentLength(32 * 1024);
  server.addHandler(h);
}

// ------------------------------------------------------------------ v2: points, commands, WebSocket
// Every value is a "point" addressed board.point (one board today: "local").
#define BOARD_NAME "local"

void pointsToJson(JsonObject o) {  // lock held
  o["board"] = BOARD_NAME; o["rev"] = gSys.configRevision; o["t"] = millis();
  JsonArray a = o["points"].to<JsonArray>();
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i]; const SensorRuntime& r = gSensorRt[i];
    JsonObject x = a.add<JsonObject>();
    x["a"] = String(BOARD_NAME ".") + c.id; x["k"] = "sensor"; x["n"] = c.name; x["u"] = c.unit; x["d"] = c.decimals;
    x["min"] = c.gaugeMin; x["max"] = c.gaugeMax;
    if (isnan(r.value)) x["v"] = nullptr; else x["v"] = serialized(String(r.value, c.decimals + 1));
    x["st"] = sensorStatusStr(r.status); x["lvl"] = alertLevelStr(r.level);
  }
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    JsonObject x = a.add<JsonObject>();
    x["a"] = String(BOARD_NAME ".") + gConfig.relays[i].id; x["k"] = "relay"; x["n"] = gConfig.relays[i].name;
    x["on"] = gRelayRt[i].on; x["out"] = gRelayRt[i].outputHigh; x["en"] = gConfig.relays[i].enabled;
  }
  for (uint8_t i = 0; i < Health::kSysCount; i++) {
    JsonObject x = a.add<JsonObject>();
    x["a"] = String(BOARD_NAME ".") + Health::kSysIds[i]; x["k"] = "system"; x["n"] = Health::kSysIds[i] + 4;
    float v = Health::sysMetric(i);
    if (isnan(v)) x["v"] = nullptr; else x["v"] = serialized(String(v, 1));
  }
}

// "local.relay1" -> "relay1"; other boards don't exist yet
bool localId(const String& addr, String& id) {
  int dot = addr.indexOf('.');
  if (dot < 0) { id = addr; return true; }
  if (addr.substring(0, dot) != BOARD_NAME) return false;
  id = addr.substring(dot + 1);
  return true;
}

// Shared by POST /api/v2/command and WebSocket "cmd" messages. Returns "" on success.
String runCommand(JsonVariantConst j, bool authed) {
  String cmd = j["cmd"] | "";
  if (cmd == "mute") {
    int minutes = j["minutes"] | 5;
    if (minutes <= 0) Alerts::unmute(); else Alerts::mute(min(minutes, 120) * 60000UL);
    return "";
  }
  if (cmd == "relay") {
    if (!authed) return "Password required";
    String id;
    if (!localId(j["addr"] | "", id)) return "Unknown board";
    int on = j["on"].isNull() ? -1 : (j["on"].as<bool>() ? 1 : 0);
    long blink = j["blinkMs"].isNull() ? -1 : j["blinkMs"].as<long>();
    return Relays::control(id, on, blink);
  }
  if (cmd == "ping") return "";
  return "Unknown command '" + cmd + "'";
}

void wsSend(AsyncWebSocketClient* c, JsonDocument& d) { String s; serializeJson(d, s); c->text(s); }

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    if (server->count() > WS_MAX_CLIENTS) { client->close(1013, "too many clients"); return; }
    JsonDocument d;
    d["type"] = "hello"; d["board"] = BOARD_NAME; d["fw"] = FW_VERSION;
    { StateLock lk; d["rev"] = gSys.configRevision; }
    wsSend(client, d);
    client->text(stateString());
  } else if (type == WS_EVT_DISCONNECT) {
    portENTER_CRITICAL(&wsMux); wsAuthed.erase(client->id()); portEXIT_CRITICAL(&wsMux);
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > 1024) return;
    JsonDocument in;
    if (deserializeJson(in, data, len)) return;
    JsonDocument out;
    out["type"] = "ack"; out["id"] = in["id"] | 0;
    String t = in["type"] | "";
    bool authed;
    portENTER_CRITICAL(&wsMux); authed = wsAuthed.count(client->id()); portEXIT_CRITICAL(&wsMux);
    String err;
    if (t == "auth") {
      String pass; { StateLock lk; pass = gConfig.sys.adminPass; }
      if (pass == (in["pass"] | "")) { portENTER_CRITICAL(&wsMux); wsAuthed.insert(client->id()); portEXIT_CRITICAL(&wsMux); }
      else err = "Wrong password";
    } else if (t == "cmd") err = runCommand(in.as<JsonVariantConst>(), authed);
    else err = "Unknown message type";
    out["ok"] = err.length() == 0;
    if (err.length()) out["message"] = err;
    wsSend(client, out);
  }
}

void setupV2Routes() {
  server.on("/api/v2/points", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; pointsToJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  server.on("/api/v2/system", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; Health::toJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  // ---- other boards (phase 3)
  server.on("/api/v2/boards", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    Boards::toJson(doc.to<JsonObject>());
    sendJson(req, 200, doc);
  });
  addJsonPost("/api/v2/boards/accept", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    int id = -1;
    String err = Boards::accept(j["mac"] | "", j["name"] | "", id);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "Paired as board " + String(id) + ". Set up its sensors and relays next.");
  });
  addJsonPost("/api/v2/boards/forget", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String err = Boards::forget(j["id"] | 0);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "Board forgotten");
  });
  server.on("/api/v2/boards/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    String out;
    String err = Boards::shareJson(req->hasParam("id") ? req->getParam("id")->value().toInt() : 0, out);
    if (err.length()) { sendStatus(req, 404, err); return; }
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  addJsonPost("/api/v2/boards/config", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String err = Boards::setShare(j["id"] | 0, j["sensors"], j["relays"]);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "Saved. Sending it to the board; the Boards page shows when it is accepted.");
  });
  addJsonPost("/api/v2/boards/relay", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String err = Boards::relayCommand(j["id"] | 0, j["index"] | -1, j["on"] | false);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "Sent");
  });

  // ---- last crash (core dump in flash)
  server.on("/api/v2/crash/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Health::clearCrash();
    sendStatus(req, 200, "Crash report cleared");
  });
  server.on("/api/v2/crash/dump", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t off = 0, size = 0;
    if (!Health::crashDumpLocation(off, size)) { sendStatus(req, 404, "No crash dump stored"); return; }
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    AsyncWebServerResponse* res = req->beginResponse("application/octet-stream", size,
      [p, off, size](uint8_t* buf, size_t max, size_t index) -> size_t {
        if (index >= size) return 0;
        size_t n = std::min<size_t>(max, size - index);
        return esp_partition_read(p, off + index, buf, n) == ESP_OK ? n : 0;
      });
    res->addHeader("Content-Disposition", "attachment; filename=carmetrics-crash.elf");
    req->send(res);
  });
  server.on("/api/v2/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; WifiMgr::toJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  server.on("/api/v2/wifi/search", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    WifiMgr::searchNow();
    sendStatus(req, 200, "Searching for networks");
  });
  server.on("/api/v2/board", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    {
      StateLock lk;
      doc["chip"] = Board::chip(); doc["setting"] = gConfig.sys.board;
      doc["detected"] = Board::detected().id;
      Board::profileToJson(Board::current(), doc["current"].to<JsonObject>());
    }
    Board::listToJson(doc["profiles"].to<JsonArray>());
    sendJson(req, 200, doc);
  });
  addJsonPost("/api/v2/board", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String id = j["board"] | "auto";
    bool applyPins = j["applyPins"] | false;
    changeConfig(req, [&](Config& c, String& err) {
      if (id != "auto" && !Board::find(id)) { err = "Unknown board for this firmware"; return false; }
      c.sys.board = id;
      if (applyPins) applyBoardPins(c, Board::resolve(id));
      return true;
    }, applyPins ? "Board saved and pins moved to its defaults. Rebooting..." : "Board saved");
  });
  addJsonPost("/api/v2/command", [](AsyncWebServerRequest* req, JsonVariant& j) {
    String cmd = j["cmd"] | "";
    bool needAuth = cmd != "mute" && cmd != "ping";
    if (needAuth && !requireAuth(req)) return;
    String err = runCommand(j.as<JsonVariantConst>(), true);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "ok");
  });
  addJsonPost("/api/v2/config/restore", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String which = j["slot"] | "previous";
    CfgSlot slot = which == "good" ? CfgSlot::GOOD : CfgSlot::PREVIOUS;
    if (!hasConfigSlot(slot)) { sendStatus(req, 404, "No " + which + " configuration saved yet"); return; }
    Config* c = new Config();
    if (!loadConfig(*c, slot)) { delete c; sendStatus(req, 500, "The saved " + which + " configuration could not be loaded"); return; }
    requestConfigApply(c);
    sendStatus(req, 200, "Restored the " + which + " configuration");
  });
  server.on("/api/v2/safemode/leave", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Health::leaveSafeMode();
    sendStatus(req, 200, "Leaving safe mode. Rebooting...", true);
    requestReboot(1000);
  });
  // ---- turbo timer, layout
  addJsonPost("/api/config/turbo", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return turboFromJson(j, c.turbo, err); }, "Turbo timer saved");
  });
  addJsonPost("/api/config/layout", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return layoutFromJson(j, c.layout, err); }, "Layout saved");
  });
  // ---- RPM calibration: enter the engine speed you see now (e.g. idle on a tachometer)
  addJsonPost("/api/rpm/calibrate", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String id = j["id"] | ""; float rpm = j["rpm"] | 0.0f;
    float hz = Sensors::rpmHz(id);
    if (isnan(hz)) { sendStatus(req, 400, "Not an active RPM sensor"); return; }
    if (rpm < 200 || rpm > 8000) { sendStatus(req, 400, "Enter the current engine speed (200-8000 RPM)"); return; }
    if (hz < 5) { sendStatus(req, 400, "No pulses: is the engine running and the signal wired?"); return; }
    float ppr = hz * 60.0f / rpm;
    changeConfig(req, [&](Config& c, String& err) {
      for (auto& s : c.sensors) if (s.id == id) s.pulsesPerRev = ppr;
      return true;
    }, ("Calibrated: " + String(ppr, 2) + " pulses per revolution (" + String(hz, 1) + " Hz at " + String(rpm, 0) + " RPM)").c_str());
  });
  // ---- alarm black box
  server.on("/api/blackbox", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["enabled"] = gConfig.blackbox.enabled; doc["keep"] = gConfig.blackbox.keepEvents;
    BlackBox::listToJson(doc["events"].to<JsonArray>());
    sendJson(req, 200, doc);
  });
  server.on("/api/blackbox/event", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t seq = req->hasParam("seq") ? req->getParam("seq")->value().toInt() : 0;
    String body;
    if (!BlackBox::read(seq, body)) { sendStatus(req, 404, "No such event"); return; }
    req->send(200, "application/json", body);
  });
  server.on("/api/blackbox/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    BlackBox::clear();
    sendStatus(req, 200, "Recordings deleted");
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
}

void setupRoutes() {
  // ---- read-only
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", stateString());
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    {
      StateLock lk;
      doc["version"] = FW_VERSION;
      doc["build"] = __DATE__ " " __TIME__;
      doc["uptimeS"] = millis() / 1000;
      doc["heapFree"] = ESP.getFreeHeap();
      doc["heapMin"] = ESP.getMinFreeHeap();
      doc["fsUsed"] = Health::fsUsed();   // cached: walking the file system here held the lock for up to ~1 s
      doc["fsTotal"] = LittleFS.totalBytes();
      doc["sketchSize"] = ESP.getSketchSize();
      doc["sketchFree"] = ESP.getFreeSketchSpace();
      doc["apSsid"] = gConfig.sys.apSsid;
      doc["apIp"] = gSys.apIp;
      doc["staSsid"] = gSys.wifiSsid; doc["wifiState"] = gSys.wifiState;
      doc["networks"] = gConfig.sys.networks.size();
      doc["ip"] = gSys.ip;
      doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      doc["hostname"] = gConfig.sys.hostname;
      doc["bmp"] = gSys.bmpModel;
      doc["oled"] = gSys.oledStatus;
      doc["timeSynced"] = gSys.timeSynced; doc["timeSource"] = gSys.timeSource;
      doc["resetReason"] = (int)esp_reset_reason();
      doc["sseClients"] = events.count();
      doc["internet"] = gNet.internet; doc["ntp"] = gNet.ntp;
      if (!isnan(gSys.stationHpa)) doc["stationHpa"] = gSys.stationHpa;
      doc["seaLevelHpa"] = effectiveSeaLevelHpa();
      doc["seaLevelSource"] = !isnan(gNet.seaLevelAuto) && gConfig.weather.autoSeaLevel && gConfig.weather.enabled ? "weather: " + gNet.seaLevelTown : String("manual");
    }
    sendJson(req, 200, doc);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; configToJson(gConfig, doc.to<JsonObject>(), false); doc["revision"] = gSys.configRevision; }
    sendJson(req, 200, doc);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    std::shared_ptr<History::Reader> rd;
    { StateLock lk; rd = std::make_shared<History::Reader>(); }
    AsyncWebServerResponse* res = req->beginChunkedResponse("application/json",
      [rd](uint8_t* buf, size_t max, size_t) -> size_t { return rd->read(buf, max); });
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  server.on("/api/onewire", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; Sensors::scanToJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  server.on("/api/onewire/scan", HTTP_POST, [](AsyncWebServerRequest* req) {
    Sensors::requestScan();
    sendStatus(req, 202, "Scanning");
  });

  server.on("/api/auth/check", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (requireAuth(req)) sendStatus(req, 200, "ok");
  });

  // ---- alerts / time (no password: you want mute to be one tap away)
  addJsonPost("/api/alerts/mute", [](AsyncWebServerRequest* req, JsonVariant& j) {
    int minutes = j["minutes"] | 5;
    if (minutes <= 0) Alerts::unmute(); else Alerts::mute(min(minutes, 120) * 60000UL);
    sendStatus(req, 200, minutes <= 0 ? "Unmuted" : "Muted");
  });

  addJsonPost("/api/time", [](AsyncWebServerRequest* req, JsonVariant& j) {
    double epochMs = j["epochMs"] | 0.0;
    if (epochMs < 1.6e12) { sendStatus(req, 400, "Bad time"); return; }
    struct timeval tv;
    tv.tv_sec = (time_t)(epochMs / 1000.0);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    StateLock lk;
    gSys.timeSynced = true;
    if (!gNet.ntp) gSys.timeSource = "browser";
    sendStatus(req, 200, "Time set");
  });

  // ---- configuration (password)
  addJsonPost("/api/config/sensors", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return sensorsFromJson(j, c.sensors, err); }, "Sensors saved");
  });
  addJsonPost("/api/config/relays", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return relaysFromJson(j, c.relays, err); }, "Relays saved");
  });
  addJsonPost("/api/config/system", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return systemFromJson(j, c.sys, err); }, "System settings saved");
  });
  addJsonPost("/api/config/import", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return configFromJson(j, c, err); }, "Configuration imported");
  });
  server.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Config* c = new Config();
    configDefaults(*c);
    { StateLock lk; c->sys = gConfig.sys; }  // keep Wi-Fi / passwords / pins
    requestConfigApply(c);
    sendStatus(req, 200, "Sensors and relays reset to defaults");
  });
  server.on("/api/config/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    JsonDocument doc;
    { StateLock lk; configToJson(gConfig, doc.to<JsonObject>(), false); }
    size_t n = measureJsonPretty(doc);
    if (!roomFor(n)) { req->send(503, "text/plain", "Low memory, try again in a moment"); return; }
    AsyncResponseStream* res = req->beginResponseStream("application/json", n + 32);
    res->addHeader("Content-Disposition", "attachment; filename=carmetrics-config.json");
    serializeJsonPretty(doc, *res);
    req->send(res);
  });

  // ---- relays
  addJsonPost("/api/relays/control", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    String id = j["id"] | "";
    int on = j["on"].isNull() ? -1 : (j["on"].as<bool>() ? 1 : 0);
    long blink = j["blinkMs"].isNull() ? -1 : j["blinkMs"].as<long>();
    String err = Relays::control(id, on, blink);
    if (err.length()) sendStatus(req, 400, err); else sendStatus(req, 200, "ok");
  });

  // ---- maintenance
  // ---- weather & cloud
  server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; Cloud::weatherToJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  server.on("/api/weather/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
    Cloud::requestWeather();
    sendStatus(req, 202, "Refreshing");
  });
  server.on("/api/cloud", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; Cloud::statusToJson(doc.to<JsonObject>()); }
    sendJson(req, 200, doc);
  });
  server.on("/api/cloud/flush", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Cloud::requestFlush();
    sendStatus(req, 202, "Upload requested");
  });
  server.on("/api/cloud/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Cloud::requestClearBacklog();
    sendStatus(req, 200, "Backlog cleared");
  });
  addJsonPost("/api/config/weather", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return weatherFromJson(j, c.weather, err); }, "Weather settings saved");
    Cloud::requestWeather();
  });
  addJsonPost("/api/config/cloud", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    changeConfig(req, [&](Config& c, String& err) { return cloudFromJson(j, c.cloud, err); }, "Cloud settings saved");
  });
  // Known-altitude calibration: derive the sea-level reference from the BMP reading.
  addJsonPost("/api/altitude/calibrate", [](AsyncWebServerRequest* req, JsonVariant& j) {
    if (!requireAuth(req)) return;
    if (j["altitudeM"].isNull()) { sendStatus(req, 400, "Enter your current altitude in metres"); return; }
    float alt = j["altitudeM"].as<float>();
    float p; { StateLock lk; p = gSys.stationHpa; }
    if (isnan(p)) { sendStatus(req, 400, "No BMP pressure reading available"); return; }
    if (alt < -500 || alt > 6000) { sendStatus(req, 400, "Altitude out of range"); return; }
    float sl = p / powf(1.0f - alt / 44330.0f, 5.255f);
    changeConfig(req, [&](Config& c, String& err) {
      c.sys.seaLevelHpa = sl;
      c.weather.autoSeaLevel = false;   // a manual calibration wins over the weather value
      return true;
    }, ("Sea-level reference set to " + String(sl, 1) + " hPa (automatic weather calibration turned off)").c_str());
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    sendStatus(req, 200, "Rebooting", true);
    requestReboot(1000);
  });
  server.on("/api/update", HTTP_POST, handleUploadDone, handleUpload);

  setupV2Routes();

  // ---- live events
  events.onConnect([](AsyncEventSourceClient* client) {
    // Live data normally goes over the WebSocket; this is only the fallback.
    // Cap it: every client holds send buffers, and stale ones (a phone that
    // left the access point) linger until TCP gives up.
    if (events.count() > SSE_MAX_CLIENTS) { client->close(); return; }
    client->send(stateString().c_str(), "state", ++eventId, 3000);
  });
  server.addHandler(&events);

  // ---- static web app (only /www is served: config never lives on the filesystem)
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("no-cache");

  server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->url().startsWith("/api/")) { sendStatus(req, 404, "Not found"); return; }
    if (!LittleFS.exists("/www/index.html") && !LittleFS.exists("/www/index.html.gz")) {
      req->send(200, "text/html",
                "<h3>CarMetrics " FW_VERSION "</h3><p>The web app is not installed. Upload "
                "<b>littlefs.bin</b> (filesystem image) here:</p>"
                "<form method=POST action='/api/update?target=filesystem' enctype=multipart/form-data>"
                "<input type=file name=f><button>Upload</button></form>");
      return;
    }
    req->redirect("/");
  });
}
}  // namespace

void Web::begin() {
  setupRoutes();
  server.begin();
  Serial.println("[web] server started");
}

void Web::notifyConfigChanged() { configChanged = true; }

void Web::update() {
  uint32_t now = millis();
  if (configChanged) {
    configChanged = false;
    uint32_t rev;
    { StateLock lk; rev = gSys.configRevision; }
    events.send(String(rev).c_str(), "config", ++eventId);
    if (ws.count()) ws.textAll("{\"type\":\"config\",\"rev\":" + String(rev) + "}");
  }
  if (now - lastPush >= 1000) {
    lastPush = now;
    ws.cleanupClients(WS_MAX_CLIENTS);
    Health::setClientCounts(events.count(), ws.count());
    if (events.count() > 0 || ws.count() > 0) {
      String st = stateString();
      // Don't queue more for clients that aren't keeping up (a phone with the
      // page in the background): each queued second holds ~1-2 KB until sent.
      if (events.count() > 0 && events.avgPacketsWaiting() < 3) events.send(st.c_str(), "state", ++eventId);
      if (ws.count() > 0) {
        AsyncWebSocketSharedBuffer buf = std::make_shared<std::vector<uint8_t>>(st.begin(), st.end());   // one copy for all
        for (auto& c : ws.getClients())
          if (c.status() == WS_CONNECTED && c.queueLen() < 3) c.text(buf);
      }
    }
  }
}
