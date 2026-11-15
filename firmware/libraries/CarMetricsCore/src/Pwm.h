// CarMetrics - PWM outputs on the LEDC peripheral, the same calls on both Arduino
// cores (core 2: channel numbers; core 3: pin numbers). Up to 8 outputs; each
// gets its own timer on core 2 (even channels), so frequencies don't clash.
#pragma once
#include <Arduino.h>
#include <soc/soc_caps.h>

// PWM outputs this chip can run at once (ESP32 / S3: 8 used here, C3 / C6: 6).
#if SOC_LEDC_CHANNEL_NUM < 8
#define CM_PWM_MAX SOC_LEDC_CHANNEL_NUM
#else
#define CM_PWM_MAX 8
#endif

namespace Pwm {
// (Re)attach a pin. Keeps running without a glitch when freq and bits are unchanged.
// Returns false when no channel / timer is free.
bool attach(int pin, uint32_t freqHz, uint8_t bits);
void write(int pin, uint32_t duty);          // 0 .. (1 << bits) - 1
void detach(int pin);                        // pin back to a plain input
bool attached(int pin);
uint8_t bits(int pin);                       // 0 if not attached
}
