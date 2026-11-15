// CarMetrics - the board's name on its own access point: phones and laptops joined
// to the CarMetrics Wi-Fi get the board as their DNS server, which answers
// "<hostname>", "<hostname>.local" (and "<hostname>.<anything>") with the board's
// address. Every other name gets "no such name" (the access point has no internet).
// mDNS (http://<hostname>.local on any network) is started with ArduinoOTA.
#pragma once
#include <Arduino.h>

namespace LocalDns {
void begin();     // after the access point is up
void update();    // from loop(): answers waiting queries
}
