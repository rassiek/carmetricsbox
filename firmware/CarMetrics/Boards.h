// CarMetrics - multi-board glue (phase 3): runs the board-to-board exchange
// (BoardNet) over ESP-NOW according to this board's role.
//
//  main board   lists boards that say hello, pairs them when you press Accept,
//               keeps each extra board's sensors + relays ("its share" of the
//               settings, edited on the Sensors / Relays pages with the board
//               selector) and sends them to it; shows their live values.
//  extra board  (role sensor / relay / sensor_relay) finds the main board's
//               radio channel, runs the sensors and relays it is given with its
//               own alarms (buzzer, LED, OLED), sends its values, and puts its
//               relays in their safe state when the link is lost. It never
//               joins a Wi-Fi network (that would fix its channel), but keeps
//               its own access point for status and setup.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "Config.h"

namespace Boards {
void begin();                    // after WifiMgr::begin()
void update();                   // from loop()
bool isMain();
const char* role();

// main board, from web handlers (they take the state lock themselves)
void toJson(JsonObject o);       // role, link, found / paired boards with live values
String accept(const String& mac, const String& name, int& id);   // "" = ok
String forget(int id);
String shareJson(int id, String& out);                           // that board's sensors + relays
String setShare(int id, JsonVariantConst sensors, JsonVariantConst relays);   // either may be null = keep
String relayCommand(int id, int relayIndex, bool on);

// Firmware for another board, sent over the link: this board's own firmware
// (same chip), or a file uploaded into this board's spare firmware slot.
String firmwareFromSelf(int id);
bool uploadChunk(size_t index, const uint8_t* data, size_t len, bool final, String& err);
String firmwareFromUpload(int id);
String firmwareCancel(int id);

// Main board: every sensor of every paired board also exists here as a REMOTE
// sensor ("b1_coolant_temp", name "<board> <sensor>"), so gauges, charts,
// alarms, black box, OLED and cloud upload treat it like a local one.
// buildMirrors() is pure (also used by the self-test); the OLED style, label
// and on/off of an existing mirror are kept when it is rebuilt.
struct ShareInfo { uint8_t id; String boardName, json; };
std::vector<SensorConfig> buildMirrors(const std::vector<ShareInfo>& shares, const std::vector<SensorConfig>& current);
}
