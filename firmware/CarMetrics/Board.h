// CarMetrics - board profiles: which GPIOs exist, can drive outputs, read analog
// with Wi-Fi on, or should be avoided; plus default pins for each board.
//
// One firmware binary is built per chip (classic ESP32 or ESP32-S3). A profile
// fine-tunes it for the exact module (e.g. WROVER loses GPIO16/17 to PSRAM).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define CM_BIT(p) (1ULL << (p))

struct BoardPins {      // defaults used for a fresh config on this board
  int8_t sda, scl, oneWire, buzzer, led;
  int8_t analog[4];     // boost, oil, battery, aux battery
  int8_t egtSck, egtCs, egtSo;
  int8_t relay[2];
};

struct BoardProfile {
  const char* id;
  const char* name;
  const char* chip;      // "esp32" | "esp32s3"
  uint8_t flashMB, psramMB;
  uint64_t valid;        // GPIO exists and is free on this module
  uint64_t output;       // may be driven (relays, buzzer, buses)
  uint64_t adc1;         // analog input that works while Wi-Fi is on
  uint64_t caution;      // boot-strapping / USB / UART pins: usable, but read the note
  uint64_t rtc;          // can wake the chip from deep sleep (ignition sense)
  BoardPins pins;
  const char* notes;
};

namespace Board {
const char* chip();                         // chip this firmware was built for
const BoardProfile* find(const String& id); // nullptr if unknown or other chip
const BoardProfile& resolve(const String& id);  // "auto"/unknown -> detected profile
const BoardProfile& detected();             // best guess from chip + PSRAM
const BoardProfile& current();              // profile of the active config
void listToJson(JsonArray a);               // profiles for this chip
void profileToJson(const BoardProfile& p, JsonObject o);
String pinList(uint64_t mask);              // "1-10, 12" style
inline bool has(uint64_t mask, int pin) { return pin >= 0 && pin < 64 && (mask & CM_BIT(pin)); }
}
