// CarMetrics - Wi-Fi: the CarMetrics access point is always on; the board also
// joins the best saved network in range (phone hotspot, home...).
//
// The ESP32 has one radio, so the access point follows the channel of the joined
// network, and a scan interrupts it for ~2 s. To keep that to a minimum:
//  - the last network joined (name, router address, channel) is remembered, the
//    access point starts on that channel and the board rejoins it directly,
//    without scanning first;
//  - a join uses the channel and router address (BSSID) from our own scan, so the
//    Wi-Fi driver doesn't scan again;
//  - short glitches are retried straight away (3 tries per router), then the next
//    best router is tried, then the board waits 10 s .. 60 s before scanning again;
//  - optionally no scans at all while someone uses the access point.
// Every attempt is kept in a join log (System page, /api/v2/wifi, serial) with the
// networks seen and the driver's disconnect reason.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace WifiMgr {
void begin();        // from setup(): start AP (+ station if networks are saved)
void update();       // from loop(): non-blocking scan / join state machine
void reconfigure();  // networks changed: drop the current connection and rescan
void searchNow();    // any task: scan on the next update(), reset the back-off
void toJson(JsonObject o);       // state, last scan and join log (call with StateLock held)
uint8_t channel();               // current radio channel (the AP follows a joined network)
const char* reasonText(uint8_t reason);   // ESP-IDF disconnect reason in plain words
}
