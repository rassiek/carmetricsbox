// CarMetrics - Wi-Fi: the CarMetrics access point is always on; the board also
// joins the best saved network in range (phone hotspot, home...).
//
// The ESP32 has one radio, so the access point follows the channel of the joined
// network, and a scan interrupts it for ~2 s. Scans therefore only run while
// disconnected, with a growing back-off (10 s .. 5 min), and optionally only
// when nobody is using the access point.
#pragma once
#include <Arduino.h>

namespace WifiMgr {
void begin();     // from setup(): start AP (+ station if networks are saved)
void update();    // from loop(): non-blocking scan / join state machine
void reconfigure();  // networks changed: drop the current connection and rescan
}
