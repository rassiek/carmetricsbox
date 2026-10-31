// CarMetrics - vehicle state: ignition sense, deep sleep after key-off, and the
// turbo cool-down indicator. All optional; nothing here affects alarms.
#pragma once
#include <Arduino.h>

namespace Vehicle {
void begin();     // (re)configure from gConfig (loop)
void update();    // loop
}
