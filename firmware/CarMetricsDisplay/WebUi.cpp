#include "WebUi.h"
#include "Net.h"
#include "Hw.h"
#include "WebPage.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Update.h>

namespace {
AsyncWebServer server(80);
DispSettings* cfg = nullptr;
Disp::Model* model = nullptr;
WebUi::Hooks hooks;
volatile bool reboot = false;
bool uploadOk = false;
String uploadErr;

bool auth(AsyncWebServerRequest* r) {
  if (r->authenticate("admin", cfg->adminPass.c_str())) return true;
  r->requestAuthentication("CarMetrics Display", false);
  return false;
}
void sendJson(AsyncWebServerRequest* r, int code, JsonDocument& d) {
  AsyncResponseStream* res = r->beginResponseStream("application/json");
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  serializeJson(d, *res);
  r->send(res);
}
void sendMsg(AsyncWebServerRequest* r, int code, const String& m) {
  JsonDocument d; d["ok"] = code == 200; d["message"] = m; sendJson(r, code, d);
}
void jsonPost(const char* path, std::function<void(AsyncWebServerRequest*, JsonVariant&)> fn) {
  AsyncCallbackJsonWebHandler* h = new AsyncCallbackJsonWebHandler(path, [fn](AsyncWebServerRequest* r, JsonVariant& j) { fn(r, j); });
  h->setMaxContentLength(16384);
  server.addHandler(h);
}
}  // namespace

namespace WebUi {
bool rebootRequested() { return reboot; }

void begin(DispSettings& s, Disp::Model& m, const Hooks& h) {
  cfg = &s; model = &m; hooks = h;
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", WEB_PAGE_GZ, sizeof(WEB_PAGE_GZ));
    res->addHeader("Content-Encoding", "gzip");
    r->send(res);
  });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    int w = 320, hh = 480; if (hooks.screenSize) hooks.screenSize(w, hh);
    d["name"] = cfg->name; d["firmware"] = DISP_VERSION; d["profile"] = Hw::profile(); d["chip"] = Hw::chipName();
    d["width"] = w; d["height"] = hh; d["rotation"] = cfg->rotation; d["brightness"] = cfg->brightness;
    d["heap"] = ESP.getFreeHeap(); d["psram"] = ESP.getFreePsram(); d["uptimeS"] = millis() / 1000;
    Net::statusJson(d["net"].to<JsonObject>());
    { Net::Lock lk; d["main"] = model->mainName(); d["mainFirmware"] = model->mainFirmware(); d["catalogRev"] = model->rev();
      d["level"] = model->mainLevel; d["muted"] = model->mainMuted; }
    sendJson(r, 200, d);
  });
  server.on("/api/catalog", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    Net::Lock lk;
    d["rev"] = model->rev();
    JsonArray sa = d["sensors"].to<JsonArray>();
    for (auto& s2 : model->sensors()) {
      JsonObject o = sa.add<JsonObject>();
      o["id"] = s2.id; o["name"] = s2.name; o["label"] = s2.shortLabel(); o["unit"] = s2.unit; o["decimals"] = s2.decimals;
      o["lo"] = s2.lo; o["hi"] = s2.hi; o["board"] = s2.board;
      if (!isnan(s2.wl)) o["wl"] = s2.wl;
      if (!isnan(s2.wh)) o["wh"] = s2.wh;
      if (!isnan(s2.cl)) o["cl"] = s2.cl;
      if (!isnan(s2.ch)) o["ch"] = s2.ch;
    }
    JsonArray ra = d["relays"].to<JsonArray>();
    for (auto& rl : model->relays()) { JsonObject o = ra.add<JsonObject>(); o["ref"] = Disp::relayRef(rl.board, rl.index); o["name"] = rl.name; o["board"] = rl.board; }
    sendJson(r, 200, d);
  });
  server.on("/api/values", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    Net::Lock lk;
    d["rev"] = model->rev(); d["level"] = model->mainLevel; d["muted"] = model->mainMuted; d["link"] = Net::linkKind();
    JsonArray a = d["v"].to<JsonArray>();
    for (size_t i = 0; i < model->valueCount(); i++) {
      const Disp::Val& v = i < model->sensors().size() ? model->sensorVal((int)i) : model->relayVal((int)(i - model->sensors().size()));
      JsonArray e = a.add<JsonArray>();
      if (isnan(v.v)) e.add(nullptr); else e.add(v.v);
      e.add(v.st); e.add(v.lvl);
    }
    sendJson(r, 200, d);
  });
  server.on("/api/layout", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    { Net::Lock lk; model->layoutToJson(d.to<JsonObject>()); }
    sendJson(r, 200, d);
  });
  jsonPost("/api/layout", [](AsyncWebServerRequest* r, JsonVariant& j) {
    if (!auth(r)) return;
    std::string err;
    String out;
    {
      Net::Lock lk;
      if (!model->setLayout(j, err)) { sendMsg(r, 400, err.c_str()); return; }
      JsonDocument d; model->layoutToJson(d.to<JsonObject>()); d.remove("default"); serializeJson(d, out);
    }
    DispConfig::saveLayout(out);
    if (hooks.layoutChanged) hooks.layoutChanged();
    sendMsg(r, 200, "Layout saved");
  });
  server.on("/api/layout-reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    DispConfig::saveLayout("");
    int w = 320, hh = 480; if (hooks.screenSize) hooks.screenSize(w, hh);
    { Net::Lock lk; model->useDefaultLayout(hh >= w); }
    if (hooks.layoutChanged) hooks.layoutChanged();
    sendMsg(r, 200, "Back to the automatic layout");
  });
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d; DispConfig::toJson(*cfg, d.to<JsonObject>()); sendJson(r, 200, d);
  });
  jsonPost("/api/settings", [](AsyncWebServerRequest* r, JsonVariant& j) {
    if (!auth(r)) return;
    String err;
    if (!DispConfig::fromJson(j, *cfg, err)) { sendMsg(r, 400, err); return; }
    DispConfig::save(*cfg);
    if (hooks.settingsChanged) hooks.settingsChanged();
    sendMsg(r, 200, "Saved");
  });
  server.on("/api/touch", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d; int x, y;
    bool t = Hw::readTouchRaw(x, y);
    d["touched"] = t; if (t) { d["x"] = x; d["y"] = y; }
    sendJson(r, 200, d);
  });
  server.on("/api/identify", HTTP_POST, [](AsyncWebServerRequest* r) { if (hooks.identify) hooks.identify(); sendMsg(r, 200, "Flashing the screen"); });
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) { if (!auth(r)) return; reboot = true; sendMsg(r, 200, "Restarting"); });
  // firmware: the raw carmetrics-display-...-firmware.bin as the request body (no multipart)
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!auth(r)) return;
      if (!uploadOk) { sendMsg(r, 400, uploadErr.length() ? uploadErr : String("No firmware received")); return; }
      reboot = true;
      sendMsg(r, 200, "Firmware installed, restarting");
    },
    nullptr,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        uploadOk = false; uploadErr = "";
        if (!r->authenticate("admin", cfg->adminPass.c_str())) { uploadErr = "Password required"; return; }
        if (len < 16 || data[0] != 0xE9) { uploadErr = "Not a firmware file (use carmetrics-display-...-firmware.bin)"; return; }
        if (!Update.begin(total, U_FLASH)) { uploadErr = String("Can't start: ") + Update.errorString(); return; }
      }
      if (uploadErr.length() || !Update.isRunning()) return;
      if (Update.write(data, len) != len) { uploadErr = String("Write failed: ") + Update.errorString(); Update.abort(); return; }
      if (index + len == total) {
        if (Update.end(true)) uploadOk = true; else uploadErr = String("Check failed: ") + Update.errorString();
      }
    });
  server.onNotFound([](AsyncWebServerRequest* r) { sendMsg(r, 404, "Not found"); });
  server.begin();
}
}  // namespace WebUi
