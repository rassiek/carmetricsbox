#include "Pins.h"
#include "Board.h"
#include "State.h"
#include <map>

namespace {
int blinkPin = -1;
uint32_t blinkUntil = 0, blinkLast = 0;
bool blinkLevel = false;

// GPIO -> what uses it, from the active settings.
std::map<int, String> usedPins() {
  std::map<int, String> m;
  auto add = [&](int p, const String& what) { if (p >= 0) { if (m.count(p)) m[p] += ", " + what; else m[p] = what; } };
  const SystemConfig& s = gConfig.sys;
  add(s.i2cSda, "I2C SDA"); add(s.i2cScl, "I2C SCL"); add(s.oneWirePin, "1-Wire");
  add(s.buzzerPin, "buzzer"); add(s.ledPin, "warning LED"); add(s.ignitionPin, "ignition sense");
  add(s.gpsRxPin, "GPS RX"); add(s.gpsTxPin, "GPS TX");
  if (s.canEnabled) { add(s.canTx, "CAN TX"); add(s.canRx, "CAN RX"); }
  if (s.rgbBuiltin) add(s.rgbBuiltinPin >= 0 ? s.rgbBuiltinPin : Board::current().fixed.rgb, "built-in RGB light");
  if (s.rgbExternal) { add(s.rgbR, "RGB red"); add(s.rgbG, "RGB green"); add(s.rgbB, "RGB blue"); }
  for (auto& c : gConfig.sensors) {
    if (!c.enabled) continue;
    if (c.type == SensorType::ADC || c.type == SensorType::RPM || c.type == SensorType::SWITCH) add(c.pin, "sensor '" + c.id + "'");
    if (c.type == SensorType::MAX6675) { add(c.pin, "'" + c.id + "' CS"); add(c.sckPin, "'" + c.id + "' SCK"); add(c.soPin, "'" + c.id + "' SO"); }
  }
  for (auto& r : gConfig.relays) {
    if (!r.enabled) continue;
    add(r.pin, "actuator '" + r.id + "'");
    if (r.type == ActType::HBRIDGE) add(r.pin2, "actuator '" + r.id + "' reverse");
  }
  return m;
}
}  // namespace

void Pins::toJson(JsonObject o) {
  const BoardProfile& b = Board::current();
  o["board"] = b.id; o["boardName"] = b.name; o["chip"] = b.chip; o["notes"] = b.notes;
  auto used = usedPins();
  JsonArray a = o["pins"].to<JsonArray>();
  JsonArray warn = o["warnings"].to<JsonArray>();
  for (int p = 0; p < 64; p++) {
    if (!Board::has(b.valid, p)) continue;
    JsonObject x = a.add<JsonObject>();
    x["gpio"] = p;
    JsonArray ab = x["abilities"].to<JsonArray>();
    ab.add(Board::has(b.output, p) ? "output" : "input_only");
    if (Board::has(b.adc1, p)) ab.add("analog");
    if (Board::has(b.rtc, p)) ab.add("wake");
    if (Board::has(b.caution, p)) ab.add("boot_pin");
    for (int i = 0; i < b.fixed.nRelays; i++) if (b.fixed.relays[i] == p) x["fixed"] = "relay " + String(i + 1);
    for (int i = 0; i < b.fixed.nInputs; i++) if (b.fixed.inputs[i] == p) x["fixed"] = "input " + String(i + 1);
    if (b.fixed.rgb == p) x["fixed"] = "built-in RGB LED";
    if (used.count(p)) {
      x["usedBy"] = used[p];
      if (used[p].indexOf(',') > 0) warn.add("GPIO " + String(p) + " is used twice: " + used[p]);
      if (Board::has(b.caution, p)) warn.add("GPIO " + String(p) + " (" + used[p] + ") is a boot pin: it must not be pulled the wrong way while the board starts");
    }
  }
  if (blinkPin >= 0) o["testing"] = blinkPin;
}

String Pins::test(int p, const String& mode, JsonObject r) {
  const BoardProfile& b = Board::current();
  if (!Board::has(b.valid, p)) return "GPIO " + String(p) + " doesn't exist on " + String(b.name);
  auto used = usedPins();
  if (mode == "read") {
    r["gpio"] = p;
    if (!used.count(p)) pinMode(p, INPUT);
    r["level"] = digitalRead(p);
    if (Board::has(b.adc1, p)) r["volts"] = roundf(analogReadMilliVolts(p)) / 1000.0f;
    if (used.count(p)) r["usedBy"] = used[p];
    return "";
  }
  if (mode == "blink") {
    if (!Board::has(b.output, p)) return "GPIO " + String(p) + " is input only";
    if (used.count(p)) return "GPIO " + String(p) + " is in use (" + used[p] + "): test it from its own page";
    if (blinkPin >= 0 && blinkPin != p) pinMode(blinkPin, INPUT);
    blinkPin = p; blinkUntil = millis() + 5000; blinkLast = 0; blinkLevel = false;
    pinMode(p, OUTPUT);
    r["gpio"] = p; r["seconds"] = 5;
    return "";
  }
  return "mode: blink or read";
}

void Pins::update() {
  if (blinkPin < 0) return;
  uint32_t now = millis();
  if ((int32_t)(now - blinkUntil) >= 0) { digitalWrite(blinkPin, LOW); pinMode(blinkPin, INPUT); blinkPin = -1; return; }
  if (now - blinkLast >= 250) { blinkLast = now; blinkLevel = !blinkLevel; digitalWrite(blinkPin, blinkLevel); }
}
