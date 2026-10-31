// CarMetrics - alarm "black box": every new warning/critical alarm (and events
// such as a hot shutdown) saves all sensors for the 30 s before and the 30 s
// after it to flash (/bb on LittleFS), newest kept, oldest dropped.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define BB_SECONDS 30          // before and after the event, 1 sample per second

class Print;
namespace BlackBox {
void begin();                  // (re)size the ring for gConfig (loop)
void update();                 // sample once a second; finish recordings (loop)
void trigger(const String& reason);   // start a recording (ignored while one is running or < 60 s after the last)
void listToJson(JsonArray a);  // saved events, newest first (any task)
bool read(uint32_t seq, String& out);
void clear();
}
