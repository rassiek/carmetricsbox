// CarMetrics main board - the API routes only the main board has: other boards,
// touch displays, chart history, alarm recordings, weather, metrics upload, presets.
// The shared API (sensors, actuators, settings, pins, network...) is in CarMetricsCore/Web.
#include "WebMain.h"
#include "Web.h"
#include "Boards.h"
#include "BlackBox.h"
#include "Cloud.h"
#include "Health.h"
#include <LittleFS.h>

using namespace Web;

namespace {


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
  String src = c.body["source"] | "file";   // 5.4: always a file (the main firmware is not a worker's)
  String err = src == "cancel" ? Boards::firmwareCancel(id) : src == "file" ? Boards::firmwareFromUpload(id) : String("source: file or cancel");
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


// Presets: plain JSON files of the web app (web/presets), merged into the settings.
void hPresets(Ctx& c) {
  JsonDocument d; JsonArray a = d["items"].to<JsonArray>();
  for (size_t i = 0; i < Web::fileCount(); i++) {
    String n = Web::fileAt(i).path;
    if (n.startsWith("/presets/") && n.endsWith(".json")) a.add(n.substring(9, n.length() - 5));
  }
  { StateLock lk; d["current"] = gConfig.ui.preset; }
  sendJson(c.req, 200, d);
}
void hPresetApply(Ctx& c) {
  String p = c.body["preset"] | "";
  const Web::File* f = Web::file("/presets/" + p + ".json");
  if (!f || f->gz) { sendError(c.req, 404, "Unknown preset '" + p + "'", "preset"); return; }
  JsonDocument preset;
  if (deserializeJson(preset, (const char*)f->data, f->len)) { sendError(c.req, 409, "Preset file is damaged"); return; }
  preset["ui"]["preset"] = p;
  patchSettings(c.req, preset.as<JsonVariantConst>(), "Preset '" + p + "' applied");
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


void hBoardFileDone(Ctx& c) { String err; if (uploadResult(err)) sendOk(c.req, "Firmware file stored on this board"); else sendError(c.req, 400, err); }


const Route kMainRoutes[] = {
  {HTTP_GET, "boards", SCOPE_READ, hBoards, nullptr},
  {HTTP_POST, "boards", SCOPE_ADMIN, hBoardPair, nullptr},
  {HTTP_PATCH, "boards/*", SCOPE_ADMIN, hBoardRename, nullptr},
  {HTTP_DELETE, "boards/*", SCOPE_ADMIN, hBoardForget, nullptr},
  {HTTP_POST, "boards/*/command", SCOPE_ADMIN, hBoardCommand, nullptr},
  {HTTP_POST, "boards/*/firmware", SCOPE_ADMIN, hBoardFirmware, nullptr},
  {HTTP_PUT, "boards/firmware-file", SCOPE_ADMIN, hBoardFileDone, Boards::uploadChunk},
  {HTTP_GET, "displays/catalog", SCOPE_READ, hDispCatalog, nullptr},
  {HTTP_GET, "displays/values", SCOPE_READ, hDispValues, nullptr},
  {HTTP_POST, "displays/command", SCOPE_READ, hDispCommand, nullptr},   // mute open; others checked inside
  {HTTP_GET, "presets", SCOPE_READ, hPresets, nullptr},
  {HTTP_POST, "presets/apply", SCOPE_ADMIN, hPresetApply, nullptr},
  {HTTP_POST, "hardware/altitude", SCOPE_ADMIN, hAltitude, nullptr},
  {HTTP_GET, "events", SCOPE_READ, hEvents, nullptr},
  {HTTP_GET, "events/*", SCOPE_READ, hEvent, nullptr},
  {HTTP_DELETE, "events", SCOPE_ADMIN, hEventsClear, nullptr},
  {HTTP_GET, "weather", SCOPE_READ, hWeather, nullptr},
  {HTTP_POST, "weather/refresh", SCOPE_READ, hWeatherRefresh, nullptr},
  {HTTP_GET, "cloud", SCOPE_READ, hCloud, nullptr},
  {HTTP_POST, "cloud/flush", SCOPE_ADMIN, hCloudFlush, nullptr},
  {HTTP_DELETE, "cloud/backlog", SCOPE_ADMIN, hCloudClear, nullptr},
};

// Other boards' sensors / actuators for the shared list handlers (?board=bN)
bool remoteLoad(int board, const char* kind, String& json, String& err) {
  String all;
  err = Boards::shareJson(board, all);
  if (err.length()) return false;
  JsonDocument d;
  if (deserializeJson(d, all)) { err = "Stored settings of that board are damaged"; return false; }
  json = "";
  serializeJson(d[kind].isNull() ? JsonVariantConst() : d[kind].as<JsonVariantConst>(), json);
  if (json == "null") json = "[]";
  return true;
}
String remoteSave(int board, const char* kind, JsonVariantConst list) {
  return !strcmp(kind, "sensors") ? Boards::setShare(board, list, JsonVariantConst()) : Boards::setShare(board, JsonVariantConst(), list);
}
String remoteCommand(int board, int index, JsonVariantConst body) { return Boards::actuatorCommand(board, index, body); }
const Remote kRemote = {remoteLoad, remoteSave, remoteCommand};

void info(JsonObject d) {
  d["role"] = "main";
  JsonArray cap = d["capabilities"];
  for (auto k : {"boards", "displays", "history", "events", "cloud", "weather", "presets"}) cap.add(k);
}
void afterPatch(const String& section) { if (section == "weather") Cloud::requestWeather(); }
}  // namespace

void WebMain::begin() {
  Web::addRoutes(kMainRoutes, sizeof(kMainRoutes) / sizeof(kMainRoutes[0]));
  Web::setRemote(&kRemote);
  Web::setInfo(info);
  Web::setAfterPatch(afterPatch);
  Web::begin();
}
