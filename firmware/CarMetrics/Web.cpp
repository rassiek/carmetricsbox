#include "Web.h"
#include "State.h"
#include "Sensors.h"
#include "Outputs.h"
#include "History.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <sys/time.h>
#include <esp_system.h>

namespace {
AsyncWebServer server(80);
AsyncEventSource events("/events");
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

void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
  AsyncResponseStream* res = req->beginResponseStream("application/json");
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
      doc["fsUsed"] = LittleFS.usedBytes();
      doc["fsTotal"] = LittleFS.totalBytes();
      doc["sketchSize"] = ESP.getSketchSize();
      doc["sketchFree"] = ESP.getFreeSketchSpace();
      doc["apSsid"] = gConfig.sys.apSsid;
      doc["apIp"] = gSys.apIp;
      doc["staSsid"] = gConfig.sys.staSsid;
      doc["ip"] = gSys.ip;
      doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      doc["hostname"] = gConfig.sys.hostname;
      doc["bmp"] = gSys.bmpModel;
      doc["oled"] = gSys.oledStatus;
      doc["timeSynced"] = gSys.timeSynced;
      doc["resetReason"] = (int)esp_reset_reason();
      doc["sseClients"] = events.count();
    }
    sendJson(req, 200, doc);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    { StateLock lk; configToJson(gConfig, doc.to<JsonObject>(), false); doc["revision"] = gSys.configRevision; }
    sendJson(req, 200, doc);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncResponseStream* res = req->beginResponseStream("application/json", 4096);
    res->addHeader("Cache-Control", "no-store");
    { StateLock lk; History::writeJson(*res); }
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
    AsyncResponseStream* res = req->beginResponseStream("application/json");
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
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    sendStatus(req, 200, "Rebooting", true);
    requestReboot(1000);
  });
  server.on("/api/update", HTTP_POST, handleUploadDone, handleUpload);

  // ---- live events
  events.onConnect([](AsyncEventSourceClient* client) {
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
  }
  if (now - lastPush >= 1000 && events.count() > 0) {
    lastPush = now;
    events.send(stateString().c_str(), "state", ++eventId);
  }
}
