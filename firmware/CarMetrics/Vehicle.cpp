#include "Vehicle.h"
#include "State.h"
#include "Outputs.h"
#include "BlackBox.h"
#include "Health.h"
#include "Cloud.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

namespace {
int ignPin = -1;
int ignLast = -1; uint8_t ignSame = 0;
uint32_t ignOffSinceMs = 0, ignLastRead = 0;
uint32_t stoppedSinceMs = 0, turboLast = 0;
uint8_t prevPhase = TURBO_OFF;
bool prevEngine = false;

float sensorValue(const String& id, bool& ok) {
  ok = false;
  if (!id.length()) return NAN;
  int i = findSensorIndex(id);
  if (i < 0) return NAN;
  StateLock lk;
  ok = gSensorRt[i].status == ST_OK && !isnan(gSensorRt[i].value);
  return gSensorRt[i].value;
}

void goToSleep() {
  const SystemConfig& s = gConfig.sys;
  Serial.printf("[sleep] key off for %u min: deep sleep until the ignition comes on\n", s.sleepAfterMinutes);
  Relays::allOff();
  Cloud::prepareSleep(8000);   // RAM-buffered upload data -> flash
  delay(200);
  rtc_gpio_pullup_dis((gpio_num_t)ignPin);
  rtc_gpio_pulldown_dis((gpio_num_t)ignPin);
  if (s.ignitionActiveLow) rtc_gpio_pullup_en((gpio_num_t)ignPin); else rtc_gpio_pulldown_en((gpio_num_t)ignPin);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)ignPin, s.ignitionActiveLow ? 0 : 1);
  Serial.flush();
  esp_deep_sleep_start();
}

void updateIgnition(uint32_t now) {
  if (ignPin < 0 || now - ignLastRead < 50) return;
  ignLastRead = now;
  int on = (digitalRead(ignPin) == LOW) == gConfig.sys.ignitionActiveLow;
  if (on == ignLast) { if (ignSame < 255) ignSame++; } else { ignSame = 0; ignLast = on; }
  if (ignSame < 4) return;   // 200 ms debounce
  int8_t prev;
  { StateLock lk; prev = gSys.ignition; gSys.ignition = on; }
  if (prev != on) Serial.printf("[ign] key %s\n", on ? "on" : "off");
  if (on) ignOffSinceMs = 0;
  else if (!ignOffSinceMs) ignOffSinceMs = now;
  // Deep sleep after key-off; postponed while someone has the web app open
  uint16_t mins = gConfig.sys.sleepAfterMinutes;
  if (!on && mins && ignOffSinceMs && now - ignOffSinceMs > mins * 60000UL && !Health::safeMode()) {
    static uint32_t postponedUntil = 0;
    uint32_t clients = WiFi.softAPgetStationNum();
    if (clients && now - ignOffSinceMs < (mins + 30) * 60000UL) {
      if (now > postponedUntil) { postponedUntil = now + 60000; Serial.println("[sleep] postponed: someone is connected"); }
      return;
    }
    goToSleep();
  }
}

void updateTurbo(uint32_t now) {
  if (now - turboLast < 250) return;
  turboLast = now;
  const TurboConfig& t = gConfig.turbo;
  bool engine; int8_t ign;
  { StateLock lk; engine = gSys.engineRunning; ign = gSys.ignition; }
  if (!t.enabled) { StateLock lk; gSys.turbo.phase = TURBO_OFF; return; }

  bool egtOk, rpmOk, spdOk;
  float egt = sensorValue(t.egtId, egtOk);
  float rpm = sensorValue(t.rpmId, rpmOk), spd = sensorValue(t.speedId, spdOk);

  // Engine stopped (or key turned off) while the turbo was still hot: log it.
  bool stoppedEngine = (prevEngine && !engine) || ign == 0;
  if (stoppedEngine && prevPhase == TURBO_COOLING) {
    String why = "hot shutdown: EGT " + (egtOk ? String(egt, 0) + " C" : String("?"));
    Serial.printf("[turbo] %s\n", why.c_str());
    BlackBox::trigger(why);
    StateLock lk; gSys.turbo.hotShutdowns++;
  }
  prevEngine = engine;

  uint8_t phase = TURBO_OFF; int32_t left = 0;
  if (engine && ign != 0) {
    bool cool = !egtOk || egt < t.safeEgt;
    if (!t.speedId.length() && !t.rpmId.length()) {
      // No speed or RPM source: we can't tell driving from idling, so just track EGT.
      phase = !cool ? TURBO_COOLING : prevPhase == TURBO_OFF ? TURBO_OFF : TURBO_OK;
    } else if ((!spdOk || spd < 3) && (!rpmOk || rpm < t.idleRpm)) {
      if (!stoppedSinceMs) stoppedSinceMs = now;
      int32_t elapsed = (now - stoppedSinceMs) / 1000;
      left = max<int32_t>(0, (int32_t)t.minIdleSeconds - elapsed);
      phase = (cool && left == 0) ? TURBO_OK : TURBO_COOLING;
    } else stoppedSinceMs = 0;
  } else stoppedSinceMs = 0;

  if (phase == TURBO_OK && prevPhase == TURBO_COOLING && t.beep) Alerts::chirp(150);
  prevPhase = phase;
  StateLock lk;
  gSys.turbo.phase = phase; gSys.turbo.leftS = left; gSys.turbo.egt = egtOk ? egt : NAN;
}
}  // namespace

void Vehicle::begin() {
  const SystemConfig& s = gConfig.sys;
  if (ignPin != s.ignitionPin) {
    ignPin = s.ignitionPin;
    ignLast = -1; ignSame = 0; ignOffSinceMs = 0;
    StateLock lk; gSys.ignition = ignPin < 0 ? -1 : gSys.ignition;
  }
  if (ignPin >= 0) {
    if (rtc_gpio_is_valid_gpio((gpio_num_t)ignPin)) rtc_gpio_deinit((gpio_num_t)ignPin);   // after waking from sleep
    pinMode(ignPin, s.ignitionActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) Serial.println("[sleep] woke up: ignition on");
}

void Vehicle::update() {
  uint32_t now = millis();
  updateIgnition(now);
  updateTurbo(now);
}
