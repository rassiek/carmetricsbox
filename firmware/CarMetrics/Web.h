// CarMetrics - web server, REST API, live events and OTA upload
#pragma once
#include <Arduino.h>

namespace Web {
void begin();
void update();             // from loop(): pushes live state over SSE
void notifyConfigChanged();
}
