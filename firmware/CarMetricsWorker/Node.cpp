#include "Node.h"
#include "Radio.h"
#include "State.h"
#include "Outputs.h"
#include "Health.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

using namespace Proto;
using namespace BoardNet;

namespace {
const char* NODE_NS = "node";    // own id + accepted settings revision + link key (in the "cfg" NVS partition)
NodeSession* nodeS = nullptr;
bool started = false;
uint8_t hopChannel = 1;
uint32_t lastHop = 0;
esp_ota_handle_t otaHandle = 0;
const esp_partition_t* otaPart = nullptr;

// Values sent each second: sensors, then actuators (the main board knows the order).
uint8_t onPoints(Point* out, uint8_t max) {
  size_t n = gConfig.sensors.size() + gConfig.relays.size();
  if (!out) return n > 255 ? 255 : (uint8_t)n;
  uint8_t k = 0;
  for (size_t i = 0; i < gSensorRt.size() && k < max; i++, k++) {
    out[k].value = gSensorRt[i].value; out[k].status = gSensorRt[i].status; out[k].level = gSensorRt[i].level;
  }
  for (size_t i = 0; i < gRelayRt.size() && k < max; i++, k++) {
    out[k].value = Relays::value(i); out[k].status = ST_OK; out[k].level = LVL_OK;
  }
  return k;
}

uint8_t onConfig(const std::vector<uint8_t>& json, uint32_t rev, std::string& msg) {
  JsonDocument d;
  if (deserializeJson(d, (const char*)json.data(), json.size())) { msg = "not valid JSON"; return CFG_INVALID; }
  Config* c = new Config(gConfig);
  String err;
  bool ok = (d["sensors"].isNull() || sensorsFromJson(d["sensors"], c->sensors, err)) &&
            (d["relays"].isNull() || relaysFromJson(d["relays"], c->relays, err));
  // The running sensor lives on the main board: "while running" alarms here then always apply.
  if (ok && c->sys.engineSensorId.length()) {
    bool have = false;
    for (auto& x : c->sensors) if (x.id == c->sys.engineSensorId) have = true;
    if (!have) c->sys.engineSensorId = "";
  }
  ok = ok && validateConfig(*c, err);
  if (!ok) {   // the reply carries 47 characters: keep the part that says what to fix
    delete c;
    err.replace("Actuator ", ""); err.replace("Sensor ", ""); err.replace(" can't be used as an output", " is input-only/missing");
    int on = err.indexOf(" on "); if (on > 0 && err.indexOf("(use") > on) err = err.substring(0, on);
    msg = err.c_str(); return CFG_INVALID;
  }
  requestConfigApply(c);   // applied and saved by loop() on its next pass
  Preferences p;
  if (p.begin(NODE_NS, false, "cfg")) { p.putUInt("rev", rev); p.end(); }
  Serial.printf("[node] new settings from the main board: %u sensors, %u actuators\n", (unsigned)c->sensors.size(), (unsigned)c->relays.size());
  return CFG_OK;
}

uint8_t onCommand(const Command& c) {
  switch (c.kind) {
    case CMD_RELAY: {
      if (c.index >= gConfig.relays.size()) return 2;
      JsonDocument d;
      Radio::commandToJson(c, gConfig.relays[c.index], d.to<JsonObject>());
      String err = Relays::command(gConfig.relays[c.index].id, d.as<JsonVariantConst>());
      return err.length() ? 1 : 0;
    }
    case CMD_MUTE: if (c.minutes) Alerts::mute(c.minutes * 60000UL); else Alerts::unmute(); return 0;
    case CMD_IDENTIFY: Alerts::chirp(300); return 0;
    case CMD_REBOOT: requestReboot(500); return 0;
    default: return 3;
  }
}

void onLink(bool up) {
  Serial.printf("[node] link to the main board %s\n", up ? "up" : "LOST: actuators to their safe state");
  Relays::linkLost(!up);
}
}  // namespace

void Node::begin() {
#ifdef CM_EMULATOR_TEST
  return;   // QEMU has no radio
#endif
  if (started) return;
  started = true;
  if (!Radio::begin(false)) return;
  Preferences p;
  uint8_t id = UNPAIRED; uint32_t rev = 0; uint8_t key[16]; bool hasKey = false;
  if (p.begin(NODE_NS, true, "cfg")) {
    id = p.getUChar("id", UNPAIRED); rev = p.getUInt("rev", 0);
    hasKey = id != UNPAIRED && p.getBytes("key", key, 16) == 16;
    p.end();
  }
  if (hasKey) Radio::espnow().setKey(MAIN_BOARD, key);   // the main board's address is learned from its first frame
  NodeSession::Callbacks cb;
  cb.paired = [](uint8_t newId, const std::string&, const uint8_t* key) {
    Preferences q;
    if (q.begin(NODE_NS, false, "cfg")) {
      q.putUChar("id", newId);
      if (newId == UNPAIRED) q.putUInt("rev", 0);
      if (key) q.putBytes("key", key, 16); else q.remove("key");
      q.end();
    }
    Radio::espnow().setKey(MAIN_BOARD, key);
    Serial.printf("[node] %s\n", newId == UNPAIRED ? "unpaired by the main board" : ("paired as board " + String(newId)).c_str());
  };
  cb.config = onConfig;
  cb.command = onCommand;
  cb.link = onLink;
  cb.points = onPoints;
  cb.level = [] { return (uint8_t)gSys.overallLevel; };
  cb.fwBegin = [](uint32_t size, uint8_t chip, const char* version, std::string& msg) -> uint8_t {
    if (chip != Radio::myChip()) { msg = std::string("image is for ") + Radio::chipName(chip) + ", this is " + Radio::chipName(Radio::myChip()); return FW_WRONG_CHIP; }
    otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!otaPart) { msg = "no spare firmware slot"; return FW_REFUSED; }
    if (size > otaPart->size) { msg = "image bigger than the firmware slot"; return FW_TOO_BIG; }
    // Erase as we go (not all at once), so sensors and alarms keep running during the transfer.
    if (esp_ota_begin(otaPart, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK) { msg = "could not start the update"; return FW_REFUSED; }
    Serial.printf("[node] receiving firmware %s (%u bytes) from the main board\n", version, (unsigned)size);
    return FW_RECEIVING;
  };
  cb.fwWrite = [](uint32_t offset, const uint8_t* data, size_t len) {
    if (offset == 0 && (len < 16 || data[0] != 0xE9 || Radio::chipOfImage(data) != Radio::myChip())) return false;   // not for this chip
    return esp_ota_write(otaHandle, data, len) == ESP_OK;
  };
  cb.fwEnd = [](std::string& msg) -> uint8_t {
    if (esp_ota_end(otaHandle) != ESP_OK) { otaHandle = 0; msg = "image check failed"; return FW_BAD_CRC; }
    otaHandle = 0;
    if (esp_ota_set_boot_partition(otaPart) != ESP_OK) { msg = "could not switch firmware"; return FW_WRITE_ERROR; }
    return FW_DONE;   // the new firmware is confirmed after 60 s of running (rollback otherwise)
  };
  cb.fwAbort = [] { if (otaHandle) { esp_ota_abort(otaHandle); otaHandle = 0; } };
  cb.fwDone = [] { Serial.println("[node] new firmware received: restarting"); requestReboot(1500); };
  nodeS = new NodeSession(Radio::links(), Radio::identity(ROLE_SENSOR_RELAY), id, rev, cb);
  Radio::links().onReceive([](const uint8_t* f, size_t n, int8_t rssi) { if (nodeS) nodeS->receive(f, n, rssi, millis()); });
}

void Node::update() {
  if (!started || !Radio::ok() || !nodeS) return;
  uint32_t now = millis();
  StateLock lk;
  Radio::links().poll();
  nodeS->flags = (gSys.muted ? HB_MUTED : 0) | (Health::safeMode() ? HB_SAFE_MODE : 0);
  nodeS->update(now);
  // Channel search: hop until the main board answers (its beacon / reply to our hello).
  if (Radio::usingRadio() && !nodeS->mainInRange(now) && now - lastHop >= 300) {
    lastHop = now;
    hopChannel = hopChannel % 13 + 1;
    Radio::espnow().setChannel(hopChannel);
    nodeS->sayHello();
  }
}

bool Node::paired() { StateLock lk; return nodeS && nodeS->paired(); }

String Node::unpair() {
  Preferences q;
  if (q.begin(NODE_NS, false, "cfg")) { q.putUChar("id", UNPAIRED); q.putUInt("rev", 0); q.remove("key"); q.end(); }
  requestReboot(1000);
  return "";
}

void Node::toJson(JsonObject o) {
  StateLock lk;
  uint32_t now = millis();
  o["role"] = "worker";
  Radio::toJson(o);
  o["firmware"] = FW_VERSION; o["chip"] = Radio::chipName(Radio::myChip());
  JsonObject n = o["node"].to<JsonObject>();
  n["paired"] = nodeS && nodeS->paired(); n["id"] = nodeS ? nodeS->boardId() : 0;
  n["linkUp"] = nodeS && nodeS->linkUp(); n["mainInRange"] = nodeS && nodeS->mainInRange(now);
  n["configRev"] = nodeS ? nodeS->configRev() : 0; n["encrypted"] = Radio::espnow().encrypted(MAIN_BOARD);
}
