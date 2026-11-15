// CarMetrics - alerts (buzzer / warning LED) and user relays. Called from loop().
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Alerts {
void begin();                    // (re)configure buzzer/LED pins from gConfig
void update();                   // evaluate thresholds, drive outputs (non-blocking)
void mute(uint32_t ms);          // silence the buzzer; a NEW or worse alert un-mutes
void unmute();
void chirp(uint32_t ms);         // short beep (turbo timer "OK to switch off")
void identify(uint32_t ms);      // "which board is this?": LED and status light flash, short beeps, OLED flashes (main)
bool identifying();
void onAlarm(void (*fn)(const String& what));   // called when a sensor goes to warning / critical
}

// Actuators (the stored list keeps the name "relays"): relay, PWM, H-bridge, servo.
namespace Relays {
void begin();                    // (re)configure pins, keeps state of unchanged actuators
void update();                   // rules, manual overrides, ramp, kick-start, blink
void allOff();                   // before deep sleep
void linkLost(bool lost);        // extra board: link to the main board lost / back (safe states)
bool linkIsLost();
// Returns empty string on success, otherwise an error message.
String control(const String& id, int on /* -1 = unchanged */, long blinkMs /* -1 = unchanged */);
// {"on":true} {"duty":60} {"direction":"reverse","speed":30} {"angle":45} {"auto":true}
// {"blinkMs":500}; optional "forMinutes" for a timed override.
String command(const String& id, JsonVariantConst body);
float value(size_t index);       // relay 0/1, PWM %, H-bridge signed %, servo degrees. Lock held.
}

// RGB status light: the S3's built-in WS2812 and/or an external RGB LED.
namespace StatusLight {
void begin();
void update();
void test(bool external);        // red, green, blue, 1 s each
}
