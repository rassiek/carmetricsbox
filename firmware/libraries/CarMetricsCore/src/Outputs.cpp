#include "Outputs.h"
#include "Health.h"
#include "State.h"
#include "Pwm.h"
#include "Board.h"
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
void (*alarmHook)(const String& what) = nullptr;   // Alerts::onAlarm()
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
void Alerts::onAlarm(void (*fn)(const String& what)) { alarmHook = fn; }

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
        if (alarmHook) alarmHook(gConfig.sensors[i].name + ": " + (r.level == LVL_CRIT ? "critical" : "warning"));   // e.g. the black box records 30 s before and after
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

// ============================================================== Actuators
// Each update: what is wanted (safe state > manual override > rule), then how
// it is applied (limits, kick-start, ramp), then the pins.
namespace {
struct ActWork {                 // per actuator, not shared with other tasks
  std::vector<std::pair<float, float>> curve;
  bool ruleOn = false;           // THRESHOLD / CONDITION memory (hysteresis)
  int8_t lastSign = 0;           // H-bridge: direction last applied
  uint32_t dirHoldUntil = 0;     // H-bridge: pause at 0 before reversing
  uint32_t lastMs = 0;
  bool pwmOk = false;
};
std::vector<ActWork> work;
std::vector<int> aPins;          // pins in use as plain outputs
std::vector<int> pPins;          // pins in use as PWM
bool gLinkLost = false;
const uint8_t PWM_BITS = 10, SERVO_BITS = 14;

bool isPwm(const RelayConfig& c) { return c.type != ActType::RELAY; }

// "On" for each type.
float onValue(const RelayConfig& c) {
  switch (c.type) {
    case ActType::PWM: case ActType::HBRIDGE: return c.maxDuty;
    case ActType::SERVO: return c.servoMaxDeg;
    default: return 1;
  }
}
float offValue(const RelayConfig& c) { return c.type == ActType::SERVO ? c.servoMinDeg : 0; }
float safeValue(const RelayConfig& c) {
  if (c.type == ActType::RELAY) return c.safeState == 1 ? 1 : 0;
  if (c.type == ActType::SERVO) return c.safeState == 1 ? constrain((float)c.safeDuty, c.servoMinDeg, c.servoMaxDeg) : c.servoMinDeg;
  return c.safeState == 1 ? c.safeDuty : 0;
}

bool sensorValue(const String& id, float& v) {
  int k = findSensorIndex(id);
  if (k < 0 || k >= (int)gSensorRt.size()) return false;
  if (gSensorRt[k].status != ST_OK || isnan(gSensorRt[k].value)) return false;
  v = gSensorRt[k].value;
  return true;
}

// What the rule asks for; false = can't tell (sensor fault): use the safe value.
bool ruleWant(const RelayConfig& c, ActWork& w, float& want) {
  switch (c.rule) {
    case RuleType::THRESHOLD: {
      float v; if (!sensorValue(c.ruleSensor, v)) return false;
      bool high = c.onAt >= c.offAt;   // on when HIGH (fan) or when LOW (heater)
      if (high ? v >= c.onAt : v <= c.onAt) w.ruleOn = true;
      else if (high ? v <= c.offAt : v >= c.offAt) w.ruleOn = false;
      want = w.ruleOn ? onValue(c) : offValue(c);
      return true;
    }
    case RuleType::CURVE: {
      float v; if (!sensorValue(c.ruleSensor, v) || w.curve.size() < 2) return false;
      float y = curveLookup(w.curve, v);
      want = c.type == ActType::RELAY ? (y >= 50 ? 1 : 0) : y;
      return true;
    }
    case RuleType::CONDITION: {
      const String& k = c.condition;
      bool on;
      if (k == "running") on = gSys.engineRunning;
      else if (k == "not_running") on = !gSys.engineRunning;
      else if (k == "ignition") on = gSys.ignition == 1;
      else if (k == "warn") on = gSys.overallLevel == LVL_WARN || gSys.overallLevel == LVL_CRIT;
      else if (k == "crit") on = gSys.overallLevel == LVL_CRIT;
      else if (k == "fault") on = gSys.overallLevel == LVL_FAULT;
      else on = false;
      want = on ? onValue(c) : offValue(c);
      return true;
    }
    default: return false;
  }
}

uint32_t servoDuty(float deg) {   // 500..2500 us pulse in a 20 ms period
  float us = 500 + constrain(deg, 0.0f, 180.0f) / 180.0f * 2000;
  return (uint32_t)(us / 20000.0f * ((1UL << SERVO_BITS) - 1) + 0.5f);
}
uint32_t pctDuty(float pct, bool activeLow) {
  uint32_t top = (1UL << PWM_BITS) - 1;
  uint32_t d = (uint32_t)(constrain(pct, 0.0f, 100.0f) / 100.0f * top + 0.5f);
  return activeLow ? top - d : d;
}

void drive(const RelayConfig& c, RelayRuntime& rt, ActWork& w) {
  if (!w.pwmOk && isPwm(c)) return;
  switch (c.type) {
    case ActType::RELAY: writeOut(c.pin, rt.outputHigh, c.activeLow); break;
    case ActType::PWM: Pwm::write(c.pin, pctDuty(rt.output, c.activeLow)); break;
    case ActType::HBRIDGE:
      Pwm::write(c.pin, pctDuty(rt.output > 0 ? rt.output : 0, c.activeLow));
      Pwm::write(c.pin2, pctDuty(rt.output < 0 ? -rt.output : 0, c.activeLow));
      break;
    case ActType::SERVO: Pwm::write(c.pin, servoDuty(rt.output)); break;
  }
}

// Limits, kick-start, ramp and the H-bridge reversal pause.
void settle(const RelayConfig& c, RelayRuntime& rt, ActWork& w, float want, uint32_t now) {
  float dt = w.lastMs ? (now - w.lastMs) / 1000.0f : 0;
  w.lastMs = now;
  if (c.type == ActType::SERVO) {
    want = constrain(want, c.servoMinDeg, c.servoMaxDeg);
  } else if (c.type != ActType::RELAY) {
    float mag = fabsf(want);
    if (mag > 0.05f) mag = constrain(mag, (float)std::max<uint8_t>(c.minDuty, 1), (float)c.maxDuty);
    else mag = 0;
    if (c.type == ActType::PWM) want = mag;
    else {
      int8_t sign = want > 0.05f ? 1 : want < -0.05f ? -1 : 0;
      if (sign && w.lastSign && sign != w.lastSign && c.rampPerS <= 0 && rt.output != 0) {
        w.dirHoldUntil = now + 300;   // stop the motor before turning it the other way
        rt.output = 0;
      }
      if ((int32_t)(w.dirHoldUntil - now) > 0) mag = 0, sign = 0;
      want = sign * mag;
      if (sign) w.lastSign = sign;
    }
    // Kick-start: from standstill, full power for kickMs, then the requested value.
    if (rt.output == 0 && want != 0 && c.kickMs) rt.kickUntil = now + c.kickMs;
    if (want == 0) rt.kickUntil = 0;
    if (rt.kickUntil && (int32_t)(rt.kickUntil - now) > 0) {
      rt.output = want > 0 ? c.maxDuty : -(float)c.maxDuty;
      rt.on = true;
      return;
    }
    if (rt.kickUntil) { rt.kickUntil = 0; rt.output = want; }   // after a kick, settle at once
  }
  if (c.type == ActType::RELAY) {
    rt.on = want >= 0.5f;
    if (!rt.on) rt.outputHigh = false;
    else if (c.mode == RelayMode::ON_OFF || rt.blinkMs == 0) rt.outputHigh = true;
    else if (now - rt.lastToggleMs >= rt.blinkMs) { rt.outputHigh = !rt.outputHigh; rt.lastToggleMs = now; }
    rt.output = rt.on ? 1 : 0;
    return;
  }
  if (c.rampPerS > 0 && dt > 0) {
    float step = c.rampPerS * dt;
    if (fabsf(want - rt.output) <= step) rt.output = want;
    else rt.output += want > rt.output ? step : -step;
  } else rt.output = want;
  rt.on = c.type == ActType::SERVO ? true : rt.output != 0;
}
}  // namespace

bool Relays::linkIsLost() { return gLinkLost; }
void Relays::linkLost(bool lost) {
  if (gLinkLost != lost) Serial.printf("[act] link to the main board %s\n", lost ? "lost: manual outputs to their safe state" : "back");
  gLinkLost = lost;
}

float Relays::value(size_t i) {
  if (i >= gRelayRt.size() || i >= gConfig.relays.size()) return NAN;
  if (gConfig.relays[i].type == ActType::RELAY) return gRelayRt[i].on ? 1 : 0;
  return roundf(gRelayRt[i].output * 10) / 10;
}

void Relays::allOff() {
  StateLock lk;
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    RelayRuntime& rt = gRelayRt[i];
    rt.on = false; rt.outputHigh = false; rt.output = offValue(c);
    if (!c.enabled) continue;
    if (c.type == ActType::RELAY) writeOut(c.pin, false, c.activeLow);
    else if (c.type == ActType::SERVO) Pwm::write(c.pin, 0);   // no pulses: the servo goes limp
    else { Pwm::write(c.pin, pctDuty(0, c.activeLow)); if (c.pin2 >= 0) Pwm::write(c.pin2, pctDuty(0, c.activeLow)); }
  }
}

void Relays::begin() {
  // Carry over state for actuators that kept the same id, type and pins.
  std::vector<RelayRuntime> next(gConfig.relays.size());
  std::vector<ActWork> nextWork(gConfig.relays.size());
  static std::vector<RelayConfig> prevCfg;
  std::vector<RelayRuntime> prevRt;
  { StateLock lk; prevRt = gRelayRt; }

  std::set<int> outPins, pwmPins;
  for (auto& r : gConfig.relays) {
    if (!r.enabled || r.pin < 0) continue;
    if (!isPwm(r)) outPins.insert(r.pin);
    else { pwmPins.insert(r.pin); if (r.type == ActType::HBRIDGE && r.pin2 >= 0) pwmPins.insert(r.pin2); }
  }
  for (int p : pPins) if (!pwmPins.count(p)) Pwm::detach(p);
  for (int p : aPins) if (!outPins.count(p) && !pwmPins.count(p)) releasePin(p);
  aPins.assign(outPins.begin(), outPins.end());
  pPins.assign(pwmPins.begin(), pwmPins.end());

  uint32_t now = millis();
  bool safe = Health::safeMode();   // safe mode: every actuator starts and stays off
  for (size_t i = 0; i < gConfig.relays.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    RelayRuntime& rt = next[i];
    ActWork& w = nextWork[i];
    if (c.rule == RuleType::CURVE) parseCurve(c.curve, w.curve);
    rt.target = c.defaultOn ? onValue(c) : offValue(c);
    rt.on = c.defaultOn && !safe;
    rt.output = c.type == ActType::RELAY ? 0 : offValue(c);
    rt.blinkMs = c.blinkMs;
    for (size_t j = 0; j < prevCfg.size() && j < prevRt.size(); j++) {
      const RelayConfig& p = prevCfg[j];
      if (p.id != c.id || p.pin != c.pin || p.type != c.type || p.pin2 != c.pin2 || p.activeLow != c.activeLow) continue;
      rt.target = prevRt[j].target; rt.on = prevRt[j].on; rt.output = prevRt[j].output;
      rt.manual = prevRt[j].manual; rt.manualUntil = prevRt[j].manualUntil;
      if (c.mode == p.mode) rt.blinkMs = prevRt[j].blinkMs;
      if (j < work.size()) { w.ruleOn = work[j].ruleOn; w.lastSign = work[j].lastSign; }
    }
    if (!c.enabled || safe) { rt.on = false; rt.target = offValue(c); rt.output = c.type == ActType::RELAY ? 0 : offValue(c); rt.manual = false; }
    rt.outputHigh = c.type == ActType::RELAY && rt.on;
    rt.lastToggleMs = now;
    if (!c.enabled) continue;
    if (c.type == ActType::RELAY) { initOut(c.pin, rt.outputHigh, c.activeLow); continue; }
    uint32_t f = c.type == ActType::SERVO ? 50 : c.freqHz;
    uint8_t b = c.type == ActType::SERVO ? SERVO_BITS : PWM_BITS;
    w.pwmOk = Pwm::attach(c.pin, f, b) && (c.type != ActType::HBRIDGE || Pwm::attach(c.pin2, f, b));
    if (!w.pwmOk) Serial.printf("[act] %s: no free PWM channel / timer for %lu Hz\n", c.id.c_str(), (unsigned long)f);
    if (safe && c.type == ActType::SERVO) continue;   // safe mode: no servo pulses
    drive(c, rt, w);
  }
  prevCfg = gConfig.relays;
  StateLock lk;
  gRelayRt = next;
  work = nextWork;
}

void Relays::update() {
  uint32_t now = millis();
  bool safeMode = Health::safeMode();
  StateLock lk;
  for (size_t i = 0; i < gConfig.relays.size() && i < gRelayRt.size() && i < work.size(); i++) {
    const RelayConfig& c = gConfig.relays[i];
    RelayRuntime& rt = gRelayRt[i];
    ActWork& w = work[i];
    if (!c.enabled) continue;
    if (safeMode) { rt.by = "safe"; continue; }   // begin() left everything off

    if (rt.manual && rt.manualUntil && (int32_t)(now - rt.manualUntil) >= 0) {
      rt.manual = false; rt.manualUntil = 0;
      if (c.rule == RuleType::MANUAL) rt.target = c.defaultOn ? onValue(c) : offValue(c);
    }
    float want;
    if (rt.manual || c.rule == RuleType::MANUAL) {
      // Set by a person: on an extra board that lost its main board, go to the
      // safe state (2 = keep as it is). Rules keep running on their own sensors.
      if (gLinkLost && c.safeState != 2) { want = safeValue(c); rt.by = "safe"; }
      else { want = rt.target; rt.by = "manual"; }
    } else if (ruleWant(c, w, want)) rt.by = "rule";
    else { want = c.safeState == 2 ? rt.output : safeValue(c); rt.by = "safe"; }   // its sensor failed

    settle(c, rt, w, want, now);
    drive(c, rt, w);
  }
}

namespace {
// Lock held.
String apply(int i, JsonVariantConst b) {
  const RelayConfig& c = gConfig.relays[i];
  RelayRuntime& rt = gRelayRt[i];
  if (!c.enabled) return "'" + c.name + "' is disabled";
  if (b["auto"] | false) {
    rt.manual = false; rt.manualUntil = 0;
    if (c.rule == RuleType::MANUAL) rt.target = c.defaultOn ? onValue(c) : offValue(c);
    return "";
  }
  float t = NAN;
  if (!b["blinkMs"].isNull()) {
    if (c.mode != RelayMode::BLINK) return "'" + c.name + "' is not in blink mode";
    rt.blinkMs = constrain((long)(b["blinkMs"] | 0), 0L, 60000L);
    if (b["on"].isNull()) t = rt.target;
  }
  if (!b["on"].isNull()) t = (b["on"] | false) ? (c.type == ActType::RELAY || rt.target == 0 || c.type == ActType::SERVO ? onValue(c) : rt.target) : offValue(c);
  switch (c.type) {
    case ActType::RELAY: break;
    case ActType::PWM:
      if (!b["duty"].isNull()) t = constrain(b["duty"].as<float>(), 0.0f, 100.0f);
      break;
    case ActType::HBRIDGE: {
      if (!b["duty"].isNull()) t = constrain(b["duty"].as<float>(), -100.0f, 100.0f);
      String dir = b["direction"] | "";
      float sp = b["speed"].isNull() ? fabsf(isnan(t) ? rt.target : t) : constrain(b["speed"].as<float>(), 0.0f, 100.0f);
      if (sp == 0 && dir.length() && dir != "stop") sp = c.maxDuty;
      if (dir == "forward") t = sp;
      else if (dir == "reverse") t = -sp;
      else if (dir == "stop") t = 0;
      else if (dir.length()) return "direction: forward, reverse or stop";
      else if (!b["speed"].isNull()) t = (rt.target < 0 ? -1 : 1) * sp;
      break;
    }
    case ActType::SERVO:
      if (!b["angle"].isNull()) {
        float a = b["angle"].as<float>();
        if (a < c.servoMinDeg || a > c.servoMaxDeg) return "Angle must be " + String(c.servoMinDeg, 0) + "-" + String(c.servoMaxDeg, 0);
        t = a;
      }
      break;
  }
  if (isnan(t)) return "Nothing to do: send on, duty, direction / speed, angle or auto";
  float minutes = b["forMinutes"] | 0.0f;
  rt.target = t;
  rt.manual = true;
  rt.manualUntil = minutes > 0 ? (millis() + (uint32_t)(std::min(minutes, 1440.0f) * 60000UL)) | 1 : 0;
  if (c.type == ActType::RELAY) { rt.lastToggleMs = millis(); rt.on = rt.outputHigh = t >= 0.5f; }
  Serial.printf("[act] %s -> %.1f%s\n", c.id.c_str(), t, minutes > 0 ? " (timed)" : "");
  return "";
}
}  // namespace

String Relays::command(const String& id, JsonVariantConst body) {
  if (Health::safeMode()) return "Safe mode: actuators are off until you leave safe mode";
  StateLock lk;
  int i = findRelayIndex(id);
  if (i < 0 || i >= (int)gRelayRt.size()) return "Unknown actuator '" + id + "'";
  return apply(i, body);
}

String Relays::control(const String& id, int on, long blinkMs) {
  JsonDocument d;
  if (on >= 0) d["on"] = on != 0;
  if (blinkMs >= 0) d["blinkMs"] = blinkMs;
  return command(id, d.as<JsonVariantConst>());
}

// ============================================================== Status light
// green ok · amber warning · amber flashing sensor fault · red flashing critical
// (steady red when muted) · blue flashing: extra board without its main board ·
// purple: safe mode.
namespace {
int slBuiltin = -1;
int slR = -1, slG = -1, slB = -1;
bool slAnode = false;
uint8_t slBright = 15, slExtBright = 80;
uint32_t slLast = 0, slTestUntil = 0;
bool slTestExt = false;
uint32_t slShown = 0xFFFFFFFF, slShownExt = 0xFFFFFFFF;

void builtinWrite(uint32_t rgb) {
  if (slBuiltin < 0 || rgb == slShown) return;
  slShown = rgb;
  uint8_t r = ((rgb >> 16) & 255) * slBright / 100, g = ((rgb >> 8) & 255) * slBright / 100, b = (rgb & 255) * slBright / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(slBuiltin, r, g, b);
#else
  neopixelWrite(slBuiltin, r, g, b);
#endif
}
void externalWrite(uint32_t rgb) {
  if (slR < 0 || rgb == slShownExt) return;
  slShownExt = rgb;
  int pins[3] = {slR, slG, slB};
  for (int k = 0; k < 3; k++) {
    uint32_t v = ((rgb >> (16 - 8 * k)) & 255) * slExtBright / 100;
    Pwm::write(pins[k], slAnode ? 255 - v : v);
  }
}
}  // namespace

void StatusLight::begin() {
  const SystemConfig& s = gConfig.sys;
  for (int p : {slR, slG, slB}) if (p >= 0) Pwm::detach(p);
  slR = slG = slB = -1;
  int bpin = -1;
  if (s.rgbBuiltin) {
    bpin = s.rgbBuiltinPin >= 0 ? s.rgbBuiltinPin : Board::current().fixed.rgb;   // S3 DevKitC: 48 (v1.1: 38)
  }
  if (slBuiltin >= 0 && slBuiltin != bpin) { slShown = 0xFFFFFFFF; builtinWrite(0); }
  slBuiltin = bpin; slShown = 0xFFFFFFFF;
  slBright = s.rgbBuiltinBright; slExtBright = s.rgbExtBright; slAnode = s.rgbCommonAnode;
  if (s.rgbExternal && s.rgbR >= 0 && s.rgbG >= 0 && s.rgbB >= 0) {
    if (Pwm::attach(s.rgbR, 5000, 8) && Pwm::attach(s.rgbG, 5000, 8) && Pwm::attach(s.rgbB, 5000, 8)) {
      slR = s.rgbR; slG = s.rgbG; slB = s.rgbB;
    } else Serial.println("[light] no free PWM channels for the external RGB light");
  }
  slShownExt = 0xFFFFFFFF;
  slLast = 0;
}

void StatusLight::test(bool external) { slTestUntil = millis() + 3000; slTestExt = external; slShown = slShownExt = 0xFFFFFFFF; }

void StatusLight::update() {
  uint32_t now = millis();
  if (now - slLast < 50) return;
  slLast = now;
  if (slBuiltin < 0 && slR < 0) return;
  const uint32_t RED = 0xFF0000, GREEN = 0x00FF00, AMBER = 0xFF7000, BLUE = 0x0030FF, PURPLE = 0xA000FF, OFF = 0;
  uint32_t c;
  if ((int32_t)(slTestUntil - now) > 0) {
    uint32_t ph = 2 - (slTestUntil - now) / 1000;
    uint32_t col = ph == 0 ? RED : ph == 1 ? GREEN : BLUE;
    if (slTestExt) { externalWrite(col); builtinWrite(OFF); } else { builtinWrite(col); externalWrite(OFF); }
    return;
  }
  uint8_t lvl; bool muted;
  { StateLock lk; lvl = gSys.overallLevel; muted = gSys.muted; }
  bool slow = (now / 500) % 2 == 0, fast = (now / 150) % 2 == 0;
  if (Health::safeMode()) c = PURPLE;
  else if (lvl == LVL_CRIT) c = muted || fast ? RED : OFF;
  else if (lvl == LVL_FAULT) c = slow ? AMBER : OFF;
  else if (lvl == LVL_WARN) c = AMBER;
  else if (gLinkLost) c = slow ? BLUE : OFF;
  else c = GREEN;
  builtinWrite(c);
  externalWrite(c);
}
