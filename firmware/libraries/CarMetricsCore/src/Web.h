// CarMetrics - HTTP API v1 (docs/API.md), live updates (WebSocket + SSE) and the
// web app, shared by the main and the worker firmware. Each firmware adds its own
// routes (Web::addRoutes) and hooks before Web::begin().
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "Config.h"
#include "State.h"
#include "Keys.h"

namespace Web {
struct Ctx {
  AsyncWebServerRequest* req;
  String p[2];                  // the "*" parts of the route
  JsonDocument body;            // parsed JSON body (null when none)
  String q(const char* k, const char* def = "") const { return req->hasParam(k) ? req->getParam(k)->value() : String(def); }
};
using Fn = void (*)(Ctx&);
// PUT with a raw body streamed in pieces (firmware files): called for every piece.
using UploadFn = bool (*)(size_t index, const uint8_t* data, size_t len, bool final, String& err);
struct Route { uint32_t method; const char* path; uint8_t need; Fn fn; UploadFn upload; };

// Sensors / actuators of other boards (?board=b1): only the main firmware has them.
struct Remote {
  bool (*load)(int board, const char* kind, String& json, String& err);   // kind "sensors" | "relays"; json = the list
  String (*save)(int board, const char* kind, JsonVariantConst list);     // "" = ok
  String (*command)(int board, int index, JsonVariantConst body);          // actuator command
};

void addRoutes(const Route* routes, size_t count);   // before begin()
void setRemote(const Remote* r);
void setInfo(void (*fn)(JsonObject info));           // adds "role" and capabilities to GET /info
void setAfterPatch(void (*fn)(const String& section));
void begin();
void update();                                       // from loop(): pushes live values
void notifyConfigChanged();

// ---- for route handlers
void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc, const char* filename = nullptr);
void sendError(AsyncWebServerRequest* req, int http, const String& msg, const char* field = nullptr);
void sendOk(AsyncWebServerRequest* req, const String& msg = "", int http = 200);
void sendResult(AsyncWebServerRequest* req, const String& err, const String& okMsg = "");   // "" = ok
uint8_t callerScope(AsyncWebServerRequest* req);
bool allowed(AsyncWebServerRequest* req, uint8_t need);   // answers 401 / 403 itself
bool uploadResult(String& err);                     // the last streamed upload: ok?
int boardParam(Ctx& c);                             // ?board=local -> 0, b<N> -> N, bad -> -1
void patchSettings(AsyncWebServerRequest* req, JsonVariantConst patch, const String& okMsg);

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
}
