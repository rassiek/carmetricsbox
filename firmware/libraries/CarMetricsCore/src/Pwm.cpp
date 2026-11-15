#include "Pwm.h"

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define CM_PWM_CORE3 1
#endif

namespace {
struct Slot { int8_t pin = -1; uint32_t freq = 0; uint8_t bits = 0; uint32_t duty = 0xFFFFFFFF; };
Slot slots[CM_PWM_MAX];   // core 2: slot i = LEDC channel 2*i (one timer each)

Slot* find(int pin) {
  for (auto& s : slots) if (s.pin == pin && pin >= 0) return &s;
  return nullptr;
}
}  // namespace

bool Pwm::attached(int pin) { return find(pin) != nullptr; }
uint8_t Pwm::bits(int pin) { Slot* s = find(pin); return s ? s->bits : 0; }

bool Pwm::attach(int pin, uint32_t freqHz, uint8_t bits) {
  if (pin < 0) return false;
  Slot* s = find(pin);
  if (s && s->freq == freqHz && s->bits == bits) return true;
  if (s) detach(pin);
  for (size_t i = 0; i < CM_PWM_MAX; i++) {
    if (slots[i].pin >= 0) continue;
#ifdef CM_PWM_CORE3
    if (!ledcAttach(pin, freqHz, bits)) return false;
#else
    if (!ledcSetup(i * 2, freqHz, bits)) return false;
    ledcAttachPin(pin, i * 2);
#endif
    slots[i].pin = pin; slots[i].freq = freqHz; slots[i].bits = bits; slots[i].duty = 0xFFFFFFFF;
    write(pin, 0);
    return true;
  }
  return false;
}

void Pwm::write(int pin, uint32_t duty) {
  for (size_t i = 0; i < CM_PWM_MAX; i++) {
    Slot& s = slots[i];
    if (s.pin != pin || pin < 0) continue;
    uint32_t top = (1UL << s.bits) - 1;
    if (duty > top) duty = top;
    if (duty == s.duty) return;
    s.duty = duty;
#ifdef CM_PWM_CORE3
    ledcWrite(pin, duty);
#else
    ledcWrite(i * 2, duty);
#endif
    return;
  }
}

void Pwm::detach(int pin) {
  Slot* s = find(pin);
  if (!s) return;
#ifdef CM_PWM_CORE3
  ledcDetach(pin);
#else
  ledcDetachPin(pin);
#endif
  pinMode(pin, INPUT);
  *s = Slot();
}
