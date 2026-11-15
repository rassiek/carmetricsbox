// CarMetrics - optional GPS module (u-blox NEO-M8N / M10, ATGM336H...) on a UART.
// Reads NMEA RMC + GGA. If no module is fitted nothing else is affected: GPS
// sensors show "not fitted" and the clock comes from NTP or the browser.
#pragma once
#include <Arduino.h>

namespace Gps {
void begin();     // (re)open the UART from gConfig (loop)
void update();    // parse what arrived (loop, non-blocking)
#ifdef CM_EMU_SELFTEST
bool selftestParse(const char* line);   // feed one NMEA sentence
#endif
}
