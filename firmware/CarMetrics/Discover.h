// CarMetrics - find what is wired to this board: I2C devices (with a guess at
// the driver), ADS1115 channel voltages, free analog pins, 1-Wire sensors.
// start() from a web handler; the scan runs in loop() (it owns the buses).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Discover {
void start();
void update();                 // from loop()
void toJson(JsonObject o);     // StateLock held
const char* i2cDriver(uint8_t addr);   // "ads1115", "bme280", ... or "" if unknown
}
