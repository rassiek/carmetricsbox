// CarMetrics - alerts (buzzer / warning LED) and user relays. Called from loop().
#pragma once
#include <Arduino.h>

namespace Alerts {
void begin();                    // (re)configure buzzer/LED pins from gConfig
void update();                   // evaluate thresholds, drive outputs (non-blocking)
void mute(uint32_t ms);          // silence the buzzer; a NEW or worse alert un-mutes
void unmute();
void chirp(uint32_t ms);         // short beep (turbo timer "OK to switch off")
}

namespace Relays {
void begin();                    // (re)configure pins, keeps state of unchanged relays
void update();                   // blink handling
void allOff();                   // before deep sleep
// Returns empty string on success, otherwise an error message.
String control(const String& id, int on /* -1 = unchanged */, long blinkMs /* -1 = unchanged */);
}
