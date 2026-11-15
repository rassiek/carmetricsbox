// CarMetrics worker - the link to the main board: says hello, pairs, takes its
// sensors and actuators from the main board, sends values every second, runs the
// main board's commands, receives new firmware, puts outputs in their safe state
// when the main board is out of reach. Without a main board it simply runs alone.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Node {
void begin();                    // after WifiMgr::begin()
void update();                   // from loop()
void toJson(JsonObject o);       // link, pairing, main board in range (takes the lock)
bool paired();
String unpair();                 // forget the main board (it can pair again)
}
