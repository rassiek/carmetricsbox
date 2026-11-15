#include "Radio.h"
#include "Board.h"
#include "State.h"
#include <WiFi.h>

using namespace Proto;

namespace {
EspNowLink radio;
CanLink can;
MultiLink multi;
bool useRadio = false, useCan = false, anyLink = false;
}

bool Radio::begin(bool mainBoard) {
  const SystemConfig& s = gConfig.sys;
  bool canWanted = s.canEnabled && s.canTx >= 0 && s.canRx >= 0;
  useRadio = mainBoard || !canWanted;
  if (useRadio && radio.begin(mainBoard)) multi.add(&radio); else useRadio = false;
  uint8_t mac[6]; WiFi.macAddress(mac);
  useCan = canWanted && can.begin(s.canTx, s.canRx, s.canKbps, mac);
  if (useCan) multi.add(&can);
  anyLink = useRadio || useCan;
  return anyLink;
}
bool Radio::ok() { return anyLink; }
MultiLink& Radio::links() { return multi; }
EspNowLink& Radio::espnow() { return radio; }
bool Radio::usingRadio() { return useRadio; }
bool Radio::usingCan() { return useCan; }

void Radio::toJson(JsonObject o) {
  JsonObject l = o["link"].to<JsonObject>();
  l["type"] = multi.name(); l["ok"] = anyLink; l["channel"] = radio.ok() ? radio.channel() : 0;
  l["rx"] = radio.rxCount; l["tx"] = radio.txCount; l["txFail"] = radio.txFail; l["queueDrops"] = radio.queueDrops;
  if (useCan || gConfig.sys.canEnabled) {
    JsonObject cj = o["can"].to<JsonObject>();
    cj["state"] = can.state(); cj["rx"] = can.rxCount; cj["tx"] = can.txCount; cj["txFail"] = can.txFail; cj["busErrors"] = can.busErrors;
  }
}

// ESP32 image header: magic 0xE9 at 0, chip id (uint16) at 12.
uint8_t Radio::chipOfImage(const uint8_t* hdr) {
  uint16_t id = hdr[12] | (hdr[13] << 8);
  return id == 0 ? Proto::CHIP_ESP32 : id == 9 ? Proto::CHIP_ESP32S3 : id == 5 ? Proto::CHIP_ESP32C3 : id == 13 ? Proto::CHIP_ESP32C6 : Proto::CHIP_OTHER;
}
uint8_t Radio::myChip() {
  String c = Board::chip();
  return c == "esp32s3" ? Proto::CHIP_ESP32S3 : c == "esp32c3" ? Proto::CHIP_ESP32C3 : c == "esp32c6" ? Proto::CHIP_ESP32C6 : Proto::CHIP_ESP32;
}
const char* Radio::chipName(uint8_t c) {
  return c == Proto::CHIP_ESP32S3 ? "ESP32-S3" : c == Proto::CHIP_ESP32 ? "ESP32" : c == Proto::CHIP_ESP32C3 ? "ESP32-C3" : c == Proto::CHIP_ESP32C6 ? "ESP32-C6" : "other chip";
}

BoardNet::Identity Radio::identity(uint8_t role) {
  BoardNet::Identity me;
  me.role = role;
  me.chip = myChip();
  WiFi.macAddress(me.mac);
  me.firmware = FW_VERSION;
  me.name = gConfig.sys.hostname.c_str();
  return me;
}

String Radio::macStr(const uint8_t* m) {
  char b[18]; snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return b;
}
bool Radio::parseMac(const String& s, uint8_t out[6]) {
  unsigned v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

// JSON actuator command -> radio command (other boards). Only what the radio carries.
String Radio::jsonToCommand(JsonVariantConst b, Command& c) {
  c.minutes = (uint16_t)constrain((long)(b["forMinutes"] | 0.0f), 0L, 1440L);
  if (b["auto"] | false) { c.which = ACT_AUTO; return ""; }
  String dir = b["direction"] | "";
  if (!b["on"].isNull()) { c.which = ACT_ON; c.on = (b["on"] | false) ? 1 : 0; return ""; }
  float v = NAN;
  if (!b["duty"].isNull()) v = b["duty"].as<float>();
  else if (!b["angle"].isNull()) v = b["angle"].as<float>();
  else if (dir.length()) {
    float sp = b["speed"] | 100.0f;
    v = dir == "forward" ? sp : dir == "reverse" ? -sp : 0;
  } else if (!b["speed"].isNull()) v = b["speed"].as<float>();
  if (isnan(v)) return "Nothing to do: send on, duty, direction / speed, angle or auto";
  c.which = ACT_VALUE; c.value = v; c.on = v != 0 ? 1 : 0;   // `on` for boards older than 5.3
  return "";
}

// Radio command -> JSON for Relays::command on this board.
void Radio::commandToJson(const Command& c, const RelayConfig& r, JsonObject o) {
  if (c.which == ACT_AUTO) o["auto"] = true;
  else if (c.which == ACT_VALUE) {
    if (r.type == ActType::SERVO) o["angle"] = c.value;
    else if (r.type == ActType::RELAY) o["on"] = c.value != 0;
    else o["duty"] = c.value;
  } else o["on"] = c.on != 0;
  if (c.minutes) o["forMinutes"] = c.minutes;
}

// ---------------------------------------------------------------- board control (main board web pages)
