// CarMetrics - pin map of this board (abilities from the board profile + what
// each GPIO is used for) and a bench tester (blink an output, read an input).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Pins {
void toJson(JsonObject o);                       // StateLock held
// mode "blink": toggles a free output for 5 s; "read": level (+ volts on an ADC1 pin).
String test(int gpio, const String& mode, JsonObject result);   // StateLock held; "" = ok
void update();                                   // from loop(): runs a blink test
}
