// CarMetrics - in-RAM history for charts (survives page reloads, not reboots)
#pragma once
#include <Arduino.h>

#define HISTORY_LEN 360   // 360 samples x 5 s = 30 minutes by default

class Print;
namespace History {
void begin();             // clears and sizes for gConfig (call from loop)
void update();            // samples every historyIntervalSeconds
void writeJson(Print& out);  // call with StateLock held
}
