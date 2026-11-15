// CarMetrics - the board-to-board links of this board (ESP-NOW and/or CAN) and the
// small helpers both the main and the worker firmware need around them.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "BoardNet.h"
#include "EspNowLink.h"
#include "CanLink.h"
#include "Config.h"

namespace Radio {
// ESP-NOW unless only CAN is wanted (main boards always use ESP-NOW), CAN when
// enabled in the settings. False when no link could be started.
bool begin(bool mainBoard);
bool ok();
MultiLink& links();
EspNowLink& espnow();
bool usingRadio();
bool usingCan();
void toJson(JsonObject o);                   // "link" (type, channel, counters) and "can"

uint8_t myChip();                            // Proto::CHIP_*
const char* chipName(uint8_t chip);
uint8_t chipOfImage(const uint8_t* hdr);     // from an ESP32 firmware image header
BoardNet::Identity identity(uint8_t role);
String macStr(const uint8_t* m);
bool parseMac(const String& s, uint8_t out[6]);

// Actuator commands over the link: JSON (as Relays::command) <-> radio command.
String jsonToCommand(JsonVariantConst body, Proto::Command& c);
void commandToJson(const Proto::Command& c, const RelayConfig& r, JsonObject o);
}
