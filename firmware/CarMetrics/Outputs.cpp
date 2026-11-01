#include "Outputs.h"
#include "Health.h"
#include "BlackBox.h"
#include "State.h"
#include <set>

// ============================================================== helpers
static void writeOut(int pin, bool active, bool activeLow) {
  if (pin < 0) return;
  digitalWrite(pin, (active ^ activeLow) ? HIGH : LOW);
}
// Set the level BEFORE switching to output so the pin never glitches the wrong way.
static void initOut(int pin, bool active, bool activeLow) {
  if (pin < 0) return;
  writeOut(pin, active, activeLow);
  pinMode(pin, OUTPUT);
  writeOut(pin, active, activeLow);
}
static void releasePin(int pin) {
  if (pin >= 0) pinMode(pin, INPUT);
}

// ============================================================== Alerts
namespace {
int aBuzzer = -1, aLed = -1;
bool aBuzzerLow = false, aLedLow = false;
uint32_t aLastEval = 0;
uint32_t engineAboveSince = 0, engineBelowSince = 0;
std::vector<uint8_t> mutedLevels;     // per-sensor level when mute was pressed
uint8_t prevOverall = LVL_OK;
uint32_t chirpUntil = 0;
uint32_t selfTestUntil = 0;

uint8_t evalLevel(const SensorConfig& c, const SensorRuntime& r, uint8_t current, bool engineRunning) {
  if (!c.enabled) return LVL_OK;
  if (r.status == ST_FAULT || r.status == ST_NOT_FOUND || r.status == ST_LINK_LOST) return c.faultAlarm ? LVL_FAULT : LVL_OK;
  if (r.status != ST_OK || isnan(r.value)) return LVL_OK;  // waiting / unassigned
  if (c.engineRunningOnly && !engineRunning) return LVL_OK;

  // Hysteresis: once a level is active, the value has to move back past the
  // threshold by `hysteresis` before that level clears.
  float h = c.hysteresis, v = r.value;
  bool inCrit = current == LVL_CRIT, inWarn = current >= LVL_WARN && current != LVL_FAULT;
  auto hi = [&](float th, bool active) { return !isnan(th) && v >= (active ? th - h : th); };
  auto lo = [&](float th, bool active) { return !isnan(th) && v <= (active ? th + h : th); };
  if (hi(c.critHigh, inCrit) || lo(c.critLow, inCrit)) return LVL_CRIT;
  if (hi(c.warnHigh, inWarn || inCrit) || lo(c.warnLow, inWarn || inCrit)) return LVL_WARN;
  return LVL_OK;
}

bool computeEngineRunning(uint32_t now) {
  const SystemConfig& s = gConfig.sys;
  if (!s.engineSensorId.length()) return true;
  int idx = findSensorIndex(s.engineSensorId);
  if (idx < 0) return true;
  float v; uint8_t st;
  { StateLock lk; v = gSensorRt[idx].value; st = gSensorRt[idx].status; }
  bool above = st == ST_OK && !isnan(v) && v > s.engineRunningAbove;
  if (st != ST_OK) return true;  // can't tell - fail safe: keep alarms active
  if (above) { engineBelowSince = 0; if (!engineAboveSince) engineAboveSince = now; }
  else { engineAboveSince = 0; if (!engineBelowSince) engineBelowSince = now; }
  bool running;
  { StateLock lk; running = gSys.engineRunning; }
  if (!running && above && now - engineAboveSince >= s.engineDelaySeconds * 1000UL) running = true;
  if (running && !above && now - engineBelowSince >= 3000) running = false;
  return running;
}
}  // namespace

void Alerts::begin() {
  const SystemConfig& s = gConfig.sys;
  if (aBuzzer != s.buzzerPin) releasePin(aBuzzer);
  if (aLed != s.ledPin) releasePin(aLed);
  aBuzzer = s.buzzerPin; aLed = s.ledPin;
  aBuzzerLow = s.buzzerActiveLow; aLedLow = s.ledActiveLow;
  initOut(aBuzzer, false, aBuzzerLow);
  initOut(aLed, false, aLedLow);
  StateLock lk;
  for (auto& r : gSensorRt) r.level = LVL_OK;
  mutedLevels.assign(gSensorRt.size(), LVL_OK);
  if (!selfTestUntil) selfTestUntil = millis() + 150;  // short beep + flash at power-up
}

void Alerts::mute(uint32_t ms) {
  StateLock lk;
  gSys.muted = true;
  gSys.muteUntilMs = millis() + ms;
  mutedLevels.resize(gSensorRt.size());
  for (size_t i = 0; i < gSensorRt.size(); i++) mutedLevels[i] = gSensorRt[i].level;
}
void Alerts::unmute() { StateLock lk; gSys.muted = false; }
void Alerts::chirp(uint32_t ms) { chirpUntil = millis() + ms; }

void Alerts::update() {
  uint32_t now = millis();
  if (now - aLastEval >= 100) {
    aLastEval = now;
    bool running = computeEngineRunning(now);
    StateLock lk;
    gSys.engineRunning = running;
    uint8_t overall = LVL_OK;
    bool worse = false;
    mutedLevels.resize(gSensorRt.size(), LVL_OK);
    for (size_t i = 0; i < gConfig.sensors.size(); i++) {
      SensorRuntime& r = gSensorRt[i];
      uint8_t before = r.level;
      r.level = evalLevel(gConfig.sensors[i], r, r.level, running);
      if ((r.level == LVL_WARN || r.level == LVL_CRIT) && r.level > before)
        BlackBox::trigger(gConfig.sensors[i].id + " " + alertLevelStr(r.level));   // records 30 s before and after
      if (r.level > overall) overall = r.level;
      if (r.level > mutedLevels[i]) worse = true;
    }
    if (gSys.muted && (worse || (int32_t)(now - gSys.muteUntilMs) >= 0)) gSys.muted = false;
    if (overall == LVL_WARN && prevOverall < LVL_WARN && !gSys.muted) chirpUntil = now + 120;
    prevOverall = overall;
    gSys.overallLevel = overall;
  }

  uint8_t lvl; bool muted;
  { StateLock lk; lvl = gSys.overallLevel; muted = gSys.muted; }
  bool buzz = false, led = false;
  switch (lvl) {
    case LVL_CRIT:  buzz = (now / 150) % 2 == 0; led = buzz; break;
    case LVL_FAULT: { uint32_t ph = now % 20000; buzz = ph < 80 || (ph > 160 && ph < 240); led = (now / 1000) % 2 == 0; break; }
    case LVL_WARN:  led = (now / 500) % 2 == 0; break;
    default: break;
  }
  if (muted) buzz = false;
  if ((int32_t)(chirpUntil - now) > 0) buzz = true;
  if ((int32_t)(selfTestUntil - now) > 0) { buzz = true; led = true; }
  writeOut(aBuzzer, buzz, aBuzzerLow);
  writeOut(aLed, led, aLedLow);
}

// ============================================================== Relays
void Relays::allOff() {
  StateLock lk;
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    gRelayRt[i].on = false; gRelayRt[i].outputHigh = false;
    if (gConfig.relays[i].enabled) writeOut(gConfig.relays[i].pin, false, gConfig.relays[i].activeLow);
  }
}

namespace {
std::vector<int> rPins;   // pins currently driven
}

void Relays::begin() {
  // Carry over state for relays that kept the same id and pin.
  std::vector<RelayRuntime> next(gConfig.relays.size());
  static std::vector<RelayConfig> prevCfg;
  std::vector<RelayRuntime> prevRt;
  { StateLock lk; prevRt = gRelayRt; }

  std::set<int> newPins;
  for (auto& r : gConfig.relays) if (r.enabled && r.pin >= 0) newPins.insert(r.pin);
  for (int p : rPins) if (!newPins.count(p)) releasePin(p);
  rPins.assign(newPins.begin(), newPins.end());

  uint32_t now = millis();
  for (size_t i = 0; i < gConfig.relays.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    RelayRuntime& rt = next[i];
    rt.on = c.defaultOn && !Health::safeMode();   // safe mode: every relay starts and stays off
    rt.blinkMs = c.blinkMs;
    for (size_t j = 0; j < prevCfg.size() && j < prevRt.size(); j++)
      if (prevCfg[j].id == c.id && prevCfg[j].pin == c.pin && prevCfg[j].activeLow == c.activeLow) {
        rt.on = prevRt[j].on;
        if (c.mode == prevCfg[j].mode) rt.blinkMs = prevRt[j].blinkMs;
      }
    if (!c.enabled || Health::safeMode()) rt.on = false;
    rt.outputHigh = rt.on;
    rt.lastToggleMs = now;
    if (c.enabled) initOut(c.pin, rt.outputHigh, c.activeLow);
  }
  prevCfg = gConfig.relays;
  StateLock lk;
  gRelayRt = next;
}

void Relays::update() {
  uint32_t now = millis();
  StateLock lk;
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    RelayRuntime& rt = gRelayRt[i];
    if (!c.enabled) continue;
    bool level;
    if (!rt.on) level = false;
    else if (c.mode == RelayMode::ON_OFF || rt.blinkMs == 0) level = true;
    else {
      level = rt.outputHigh;
      if (now - rt.lastToggleMs >= rt.blinkMs) { level = !level; rt.lastToggleMs = now; }
    }
    rt.outputHigh = level;
    writeOut(c.pin, level, c.activeLow);
  }
}

String Relays::control(const String& id, int on, long blinkMs) {
  if (Health::safeMode()) return "Safe mode: relays are switched off until you leave safe mode";
  StateLock lk;
  int i = findRelayIndex(id);
  if (i < 0) return "Unknown relay '" + id + "'";
  const RelayConfig& c = gConfig.relays[i];
  if (!c.enabled) return "Relay '" + id + "' is disabled";
  RelayRuntime& rt = gRelayRt[i];
  if (blinkMs >= 0) {
    if (c.mode != RelayMode::BLINK) return "Relay '" + id + "' is not in blink mode";
    rt.blinkMs = constrain(blinkMs, 0L, 60000L);
  }
  if (on >= 0) { rt.on = on != 0; rt.lastToggleMs = millis(); rt.outputHigh = rt.on; }
  Serial.printf("[relay] %s on=%d blink=%lu\n", id.c_str(), rt.on, (unsigned long)rt.blinkMs);
  return "";
}
