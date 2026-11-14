// CarMetrics - multi-board glue (phase 3): runs the board-to-board exchange
// (BoardNet) over ESP-NOW according to this board's role.
//
//  main board   lists boards that say hello, pairs them when you press Accept,
//               keeps each extra board's sensors + relays ("its share" of the
//               settings, edited on the Sensors / Relays pages with the board
//               selector) and sends them to it; shows their live values.
//  display      (separate firmware, firmware/CarMetricsDisplay) gets the catalog
//               of all sensors + relays and every value each second; can switch
//               relays, mute and change alert limits.
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
String setShareLocked(int id, JsonVariantConst sensors, JsonVariantConst relays);   // state lock already held
String boardCommand(int id, const String& cmd, int minutes);    // identify | reboot | mute | resend
String rename(int id, const String& name);

// Touch displays over Wi-Fi (the main board's access point): the same catalog and
// values a paired display gets over ESP-NOW, and its commands.
String displayCatalog(uint32_t& rev);          // JSON
void displayValues(JsonObject o);              // {rev, level, muted, v:[[value,status,level],...]}
String displayCommand(JsonVariantConst j);     // {kind:"relay",board,index,on} | {kind:"mute",minutes} | {kind:"limit",index,which,value,rev}; "" = ok
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
