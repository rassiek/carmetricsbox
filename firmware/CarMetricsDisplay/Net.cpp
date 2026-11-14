#include "Net.h"
#include "BoardNet.h"
#include "EspNowLink.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>

using namespace Proto;
using namespace BoardNet;

namespace {
DispSettings* cfg = nullptr;
Disp::Model* model = nullptr;
Net::Hooks hooks;
EspNowLink radio;
NodeSession* node = nullptr;
SemaphoreHandle_t mtx = nullptr;
uint8_t hopChannel = 1;
uint32_t lastHop = 0, espnowPointsAt = 0, httpValuesAt = 0, lastJoinTry = 0, joinStarted = 0, staleSince = 0;
bool joining = false;
uint8_t joinFails = 0;
String ownAp;
// OTA from the main board
esp_ota_handle_t otaHandle = 0;
const esp_partition_t* otaPart = nullptr;
// commands for the HTTP task
struct HttpCmd { String body; };
QueueHandle_t httpQ = nullptr;
volatile uint32_t pendingCatRev = 0;   // HTTP task: catalog it needs to fetch

uint8_t myChip() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  return CHIP_ESP32S3;
#else
  return CHIP_ESP32;
#endif
}
uint8_t chipOfImage(const uint8_t* hdr) { uint16_t id = hdr[12] | (hdr[13] << 8); return id == 0 ? CHIP_ESP32 : id == 9 ? CHIP_ESP32S3 : CHIP_OTHER; }

bool espnowFresh(uint32_t now) { return node && node->linkUp() && espnowPointsAt && (int32_t)(now - espnowPointsAt) < 3500; }
bool httpFresh(uint32_t now) { return httpValuesAt && (int32_t)(now - httpValuesAt) < 3500; }

String mainSsid() {
  if (cfg->mainSsid.length()) return cfg->mainSsid;
  Net::Lock lk;
  return model->mainAp().empty() ? String("CarMetrics") : String(model->mainAp().c_str());
}

void takeCatalog(const char* json, size_t len, uint32_t rev, const char* from) {
  std::string err;
  if (!model->setCatalog(json, len, rev, err)) { Serial.printf("[net] catalog from %s refused: %s\n", from, err.c_str()); return; }
  DispConfig::saveCatalog(String(json, len), rev);
  Serial.printf("[net] catalog from %s: %u values, %u relays (rev %08x)\n", from, (unsigned)model->sensors().size(), (unsigned)model->relays().size(), (unsigned)rev);
  if (hooks.catalogChanged) hooks.catalogChanged();
}

// ---------------------------------------------------------------- Wi-Fi fallback (own task: HTTP blocks)
void httpTask(void*) {
  HTTPClient http;
  uint32_t lastPoll = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (WiFi.status() != WL_CONNECTED) continue;
    String base = "http://" + WiFi.gatewayIP().toString();
    HttpCmd* c = nullptr;
    if (xQueueReceive(httpQ, &c, 0) == pdTRUE && c) {
      http.begin(base + "/api/v2/display/command");
      http.setTimeout(2000);
      http.setAuthorization("admin", cfg->mainAdmin.c_str());
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(c->body);
      String resp = http.getString();
      http.end();
      delete c;
      JsonDocument d; deserializeJson(d, resp);
      String msg = code == 200 ? "Done" : code == 401 ? "Main board refused: wrong admin password (display settings)" : String(d["message"] | "Not sent");
      if (hooks.notice) hooks.notice(msg);
      continue;
    }
    uint32_t now = millis();
    if (espnowFresh(now) || now - lastPoll < 1000) continue;
    lastPoll = now;
    http.begin(base + "/api/v2/display/values");
    http.setTimeout(1500);
    int code = http.GET();
    if (code != 200) { http.end(); continue; }
    JsonDocument d;
    DeserializationError e = deserializeJson(d, http.getStream());
    http.end();
    if (e) continue;
    uint32_t rev = d["rev"] | 0UL;
    bool needCatalog;
    { Net::Lock lk; needCatalog = rev != model->rev(); }
    if (needCatalog) {
      http.begin(base + "/api/v2/display/catalog");
      http.setTimeout(2000);
      if (http.GET() == 200) {
        String j = http.getString();
        uint32_t crc = Proto::crc32((const uint8_t*)j.c_str(), j.length());
        Net::Lock lk;
        takeCatalog(j.c_str(), j.length(), crc, "Wi-Fi");
      }
      http.end();
      continue;   // values next round, indexed by the new catalog
    }
    Net::Lock lk;
    if (espnowFresh(millis())) continue;
    JsonArrayConst a = d["v"].as<JsonArrayConst>();
    size_t i = 0;
    for (JsonArrayConst e2 : a) {
      float v = e2[0].isNull() ? NAN : e2[0].as<float>();
      model->setValue(i++, v, e2[1] | 1, e2[2] | 0, millis());
    }
    model->mainLevel = d["level"] | 0; model->mainMuted = d["muted"] | false;
    httpValuesAt = millis();
  }
}

void queueHttp(const String& body) {
  if (WiFi.status() != WL_CONNECTED) { if (hooks.notice) hooks.notice("No link to the main board"); return; }
  HttpCmd* c = new HttpCmd{body};
  if (xQueueSend(httpQ, &c, 0) != pdTRUE) { delete c; if (hooks.notice) hooks.notice("Busy, try again"); }
}

void sendCmd(const Command& c, const String& httpBody) {
  uint32_t now = millis();
  if (node && node->linkUp() && node->sendCommand(c) >= 0) return;
  if (WiFi.status() == WL_CONNECTED && (httpFresh(now) || !node || !node->linkUp())) { queueHttp(httpBody); return; }
  if (hooks.notice) hooks.notice("No link to the main board");
}
}  // namespace

namespace Net {
void lock() { if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
void unlock() { if (mtx) xSemaphoreGiveRecursive(mtx); }

void begin(DispSettings& s, Disp::Model& m, const Hooks& h) {
  cfg = &s; model = &m; hooks = h;
  mtx = xSemaphoreCreateRecursiveMutex();
  httpQ = xQueueCreate(8, sizeof(HttpCmd*));
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ap[32]; snprintf(ap, sizeof ap, "CarMetrics-Display-%02X%02X", mac[4], mac[5]);
  ownAp = ap;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.softAPConfig(IPAddress(192, 168, 5, 1), IPAddress(192, 168, 5, 1), IPAddress(255, 255, 255, 0));   // not 192.168.4.x: that's the main board's network
  WiFi.softAP(ownAp.c_str(), s.apPass.c_str());
  WiFi.setHostname("carmetrics-display");
  MDNS.begin("carmetrics-display");
  MDNS.addService("http", "tcp", 80);

  radio.begin(false);
  // stored catalog: screen works at once; its revision is what the node session reports
  String cj; uint32_t crev = 0;
  if (DispConfig::loadCatalog(cj, crev)) { Lock lk; std::string err; if (!m.setCatalog(cj.c_str(), cj.length(), crev, err)) crev = 0; }
  uint8_t id = DispConfig::loadNodeId();
  uint8_t key[16];
  if (id != UNPAIRED && DispConfig::loadNodeKey(key)) radio.setKey(MAIN_BOARD, key);
  Identity me; me.role = ROLE_DISPLAY; me.chip = myChip(); memcpy(me.mac, mac, 6); me.firmware = DISP_VERSION; me.name = s.name.c_str();
  NodeSession::Callbacks cb;
  cb.paired = [](uint8_t newId, const std::string&, const uint8_t* k) {
    DispConfig::saveNodeId(newId, k);
    radio.setKey(MAIN_BOARD, k);
    Serial.printf("[net] %s\n", newId == UNPAIRED ? "unpaired by the main board" : ("paired as board " + String(newId)).c_str());
  };
  cb.config = [](const std::vector<uint8_t>& json, uint32_t rev, std::string& msg) -> uint8_t {
    Lock lk;
    std::string err;
    Disp::Model test;
    if (!test.setCatalog((const char*)json.data(), json.size(), rev, err)) { msg = err; return CFG_INVALID; }
    takeCatalog((const char*)json.data(), json.size(), rev, "ESP-NOW");
    return CFG_OK;
  };
  cb.mainPoints = [](const Points& p) {
    Lock lk;
    if (p.configRev != (uint16_t)model->rev()) return;   // values for another catalog
    uint32_t now = millis();
    for (uint8_t i = 0; i < p.count; i++) model->setValue(p.first + i, p.points[i].value, p.points[i].status, p.points[i].level, now);
    espnowPointsAt = now;
  };
  cb.mainStatus = [](uint8_t level, uint8_t flags) { Lock lk; model->mainLevel = level; model->mainMuted = flags & HB_MUTED; };
  cb.command = [](const Command& c) -> uint8_t {   // from the main board (Boards page buttons)
    if (c.kind == CMD_IDENTIFY) { if (hooks.identify) hooks.identify(); return ACK_OK; }
    if (c.kind == CMD_REBOOT) { delay(200); ESP.restart(); return ACK_OK; }
    return ACK_UNKNOWN;
  };
  cb.commandResult = [](uint16_t, uint8_t st) {
    static const char* const msg[] = {"Done", "Main board: failed", "Main board: unknown sensor or relay", "Main board: unknown command", "List out of date: try again", "Main board: not allowed"};
    if (hooks.notice) hooks.notice(st == ACK_NO_ANSWER ? "No answer from the main board" : st < 6 ? msg[st] : "Failed");
  };
  cb.link = [](bool up) { Serial.printf("[net] ESP-NOW link to the main board %s\n", up ? "up" : "lost"); };
  cb.fwBegin = [](uint32_t size, uint8_t chip, const char* version, std::string& msg) -> uint8_t {
    if (chip != myChip()) { msg = "image is for another chip"; return FW_WRONG_CHIP; }
    otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!otaPart) { msg = "no spare firmware slot"; return FW_REFUSED; }
    if (size > otaPart->size) { msg = "image bigger than the firmware slot"; return FW_TOO_BIG; }
    if (esp_ota_begin(otaPart, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK) { msg = "could not start the update"; return FW_REFUSED; }
    Serial.printf("[net] receiving firmware %s (%u bytes)\n", version, (unsigned)size);
    if (hooks.notice) hooks.notice("Receiving new firmware…");
    return FW_RECEIVING;
  };
  cb.fwWrite = [](uint32_t offset, const uint8_t* data, size_t len) {
    if (offset == 0 && (len < 16 || data[0] != 0xE9 || chipOfImage(data) != myChip())) return false;
    return esp_ota_write(otaHandle, data, len) == ESP_OK;
  };
  cb.fwEnd = [](std::string& msg) -> uint8_t {
    if (esp_ota_end(otaHandle) != ESP_OK) { otaHandle = 0; msg = "image check failed"; return FW_BAD_CRC; }
    otaHandle = 0;
    if (esp_ota_set_boot_partition(otaPart) != ESP_OK) { msg = "could not switch firmware"; return FW_WRITE_ERROR; }
    return FW_DONE;
  };
  cb.fwAbort = [] { if (otaHandle) { esp_ota_abort(otaHandle); otaHandle = 0; } };
  cb.fwDone = [] { if (hooks.notice) hooks.notice("New firmware: restarting"); delay(1500); ESP.restart(); };
  node = new NodeSession(radio, me, id, crev, cb);
  radio.onReceive([](const uint8_t* f, size_t n, int8_t rssi) { if (node) node->receive(f, n, rssi, millis()); });
  xTaskCreatePinnedToCore(httpTask, "http", 8192, nullptr, 1, nullptr, 0);
}

void applyWifiSettings() {
  WiFi.softAP(ownAp.c_str(), cfg->apPass.c_str());
  WiFi.disconnect(false);
  joining = false; joinFails = 0; lastJoinTry = 0;
}

void update(uint32_t now) {
  radio.poll();
  node->update(now);
  bool sta = WiFi.status() == WL_CONNECTED;
  // Wi-Fi: join the main board's access point (fallback + reachable page)
  if (joining && (sta || now - joinStarted > 10000)) {
    if (sta) { joinFails = 0; Serial.printf("[net] joined %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str()); }
    else { joinFails++; WiFi.disconnect(false); }
    joining = false;
  }
  uint32_t every = joinFails < 3 ? 20000 : 90000;
  if (cfg->wifi && !sta && !joining && (!lastJoinTry || now - lastJoinTry > every)) {
    lastJoinTry = now ? now : 1;
    joining = true; joinStarted = now;
    WiFi.begin(mainSsid().c_str(), cfg->mainPass.c_str());
  }
  // ESP-NOW channel search: only while not joined (then the channel is the main board's anyway),
  // not while joining, and not while someone uses this display's own access point.
  if (!sta && !joining && !node->mainInRange(now) && WiFi.softAPgetStationNum() == 0 && now - lastHop >= 300) {
    lastHop = now;
    hopChannel = hopChannel % 13 + 1;
    radio.setChannel(hopChannel);
    node->sayHello();
  }
  // nothing fresh for a while: show it
  bool fresh = espnowFresh(now) || httpFresh(now);
  if (fresh) staleSince = 0;
  else if (!staleSince) staleSince = now ? now : 1;
  else if (now - staleSince > 6000) {
    Lock lk;
    for (size_t i = 0; i < model->valueCount(); i++) model->setValue(i, NAN, Disp::ST_LINK_LOST, Disp::LVL_OK, 0);
    staleSince = now - 3000;   // again in 3 s
  }
}

uint8_t linkKind() { uint32_t now = millis(); return espnowFresh(now) ? 1 : httpFresh(now) ? 2 : 0; }
bool canCommand() { return (node && node->linkUp()) || WiFi.status() == WL_CONNECTED; }
bool paired() { return node && node->paired(); }
int boardId() { return node && node->paired() ? node->boardId() : 0; }
int channel() { return radio.channel(); }
String wifiSsid() { return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(); }
String wifiIp() { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(); }
String apName() { return ownAp; }
String apIp() { return WiFi.softAPIP().toString(); }

void relay(uint8_t board, uint8_t index, bool on) {
  Command c; c.kind = CMD_RELAY; c.board = board; c.index = index; c.on = on;
  JsonDocument d; d["kind"] = "relay"; d["board"] = board; d["index"] = index; d["on"] = on;
  String b; serializeJson(d, b);
  sendCmd(c, b);
}
void mute(uint16_t minutes) {
  Command c; c.kind = CMD_MUTE; c.minutes = minutes;
  JsonDocument d; d["kind"] = "mute"; d["minutes"] = minutes;
  String b; serializeJson(d, b);
  sendCmd(c, b);
}
void setLimit(int sensor, uint8_t which, float value) {
  uint32_t rev;
  { Lock lk; rev = model->rev(); }
  Command c; c.kind = CMD_SET_LIMIT; c.index = (uint8_t)sensor; c.which = which; c.value = value; c.rev = (uint16_t)rev;
  JsonDocument d; d["kind"] = "limit"; d["index"] = sensor; d["which"] = which; d["rev"] = rev & 0xFFFF;
  if (isnan(value)) d["value"] = nullptr; else d["value"] = value;
  String b; serializeJson(d, b);
  sendCmd(c, b);
}

void statusJson(JsonObject o) {
  uint32_t now = millis();
  o["link"] = linkKind() == 1 ? "espnow" : linkKind() == 2 ? "wifi" : "none";
  JsonObject e = o["espnow"].to<JsonObject>();
  e["paired"] = paired(); e["id"] = boardId(); e["up"] = node && node->linkUp(); e["inRange"] = node && node->mainInRange(now);
  e["channel"] = channel(); e["encrypted"] = radio.encrypted(MAIN_BOARD); e["rx"] = radio.rxCount; e["tx"] = radio.txCount;
  e["valuesAgeMs"] = espnowPointsAt ? (int32_t)(now - espnowPointsAt) : -1;
  JsonObject w = o["wifi"].to<JsonObject>();
  w["enabled"] = cfg->wifi; w["ssid"] = mainSsid(); w["connected"] = WiFi.status() == WL_CONNECTED; w["ip"] = wifiIp();
  w["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; w["valuesAgeMs"] = httpValuesAt ? (int32_t)(now - httpValuesAt) : -1;
  JsonObject a = o["ap"].to<JsonObject>();
  a["name"] = ownAp; a["ip"] = apIp(); a["clients"] = WiFi.softAPgetStationNum();
}
}  // namespace Net
