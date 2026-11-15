// CarMetrics - sensor reading (non-blocking state machines, called from loop())
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Sensors {
// (Re)initialise hardware for gConfig. Call from loop() after config changes.
void begin();
// Poll all sensors. Never blocks for more than a few ms.
void update();
// 1-Wire bus scan (runs inside update(); results readable under StateLock)
void requestScan();
void scanToJson(JsonObject o);  // call with StateLock held
float rpmHz(const String& id);  // current pulse frequency of an RPM sensor (for calibration)
}  // namespace Sensors
