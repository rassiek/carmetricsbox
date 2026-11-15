#include "Discover.h"
#include "Sensors.h"
#include "State.h"
#include "Board.h"
#include <Wire.h>

namespace {
struct I2cHit { uint8_t addr; const char* driver; };
struct AnalogHit { String source; float volts; String assignedTo; };
bool requested = false, running = false;
uint32_t doneMs = 0;
std::vector<I2cHit> i2c;          // guarded by StateLock
std::vector<AnalogHit> analog;

// ADS1115 single-shot read, +-6.144 V range (5 V modules), ~8 ms per channel.
float adsVolts(uint8_t addr, int ch) {
  uint16_t cfg = 0x8000 | ((0x4 + ch) << 12) | (0 << 9) | 0x0100 | (0x4 << 5) | 0x0003;   // OS, AINx vs GND, FSR 6.144, single, 128 SPS
  Wire.beginTransmission(addr); Wire.write(1); Wire.write(cfg >> 8); Wire.write(cfg & 0xFF);
  if (Wire.endTransmission() != 0) return NAN;
  delay(9);
  Wire.beginTransmission(addr); Wire.write(0);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)addr, 2) != 2) return NAN;
  int16_t raw = (Wire.read() << 8) | Wire.read();
  return raw * 6.144f / 32768.0f;
}

String analogOwner(const String& src) {
  for (auto& c : gConfig.sensors) {
    if (!c.enabled) continue;
    if (src.startsWith("gpio") && c.type == SensorType::ADC && c.pin == src.substring(4).toInt()) return c.id;
    if (c.type == SensorType::ADS1115) {
      char k[24]; snprintf(k, sizeof k, "ads1115@0x%02X/%d", c.adsAddress, c.adsChannel);
      if (src == k) return c.id;
    }
  }
  return "";
}
}  // namespace

const char* Discover::i2cDriver(uint8_t a) {
  if (a >= 0x48 && a <= 0x4B) return "ads1115";
  if (a == 0x76 || a == 0x77) return "bme280";
  if (a == 0x68 || a == 0x69) return "mpu6050";
  if (a >= 0x60 && a <= 0x67) return "mcp9600";
  if (a >= 0x40 && a <= 0x45) return "ina226";
  if (a == 0x3C || a == 0x3D) return "oled";
  return "";
}

void Discover::start() {
  StateLock lk;
  requested = true;
  Sensors::requestScan();   // 1-Wire too
}

void Discover::update() {
  if (!requested) return;
  { StateLock lk; requested = false; running = true; }
  std::vector<I2cHit> hits;
  std::vector<AnalogHit> an;
  int sda, scl; { StateLock lk; sda = gConfig.sys.i2cSda; scl = gConfig.sys.i2cScl; }
  if (sda >= 0 && scl >= 0) {
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) hits.push_back({a, i2cDriver(a)});
    }
    for (auto& h : hits) {
      if (strcmp(h.driver, "ads1115") != 0) continue;
      for (int ch = 0; ch < 4; ch++) {
        char src[24]; snprintf(src, sizeof src, "ads1115@0x%02X/%d", h.addr, ch);
        an.push_back({src, adsVolts(h.addr, ch), ""});
      }
    }
  }
  // Free ADC1 pins of this board (those that work with Wi-Fi on).
  const BoardProfile& b = Board::current();
  for (int p = 0; p < 64; p++) {
    if (!Board::has(b.adc1, p) || !Board::has(b.valid, p)) continue;
    an.push_back({"gpio" + String(p), analogReadMilliVolts(p) / 1000.0f, ""});
  }
  StateLock lk;
  for (auto& x : an) x.assignedTo = analogOwner(x.source);
  i2c = hits; analog = an;
  running = false; doneMs = millis();
  Serial.printf("[discover] %u I2C device(s), %u analog source(s)\n", (unsigned)i2c.size(), (unsigned)analog.size());
}

void Discover::toJson(JsonObject o) {
  o["running"] = running || requested;
  o["ageMs"] = doneMs ? (int32_t)(millis() - doneMs) : -1;
  JsonArray a = o["i2c"].to<JsonArray>();
  for (auto& h : i2c) {
    JsonObject x = a.add<JsonObject>();
    char s[6]; snprintf(s, sizeof s, "0x%02X", h.addr);
    x["addr"] = s; x["driver"] = h.driver;
  }
  Sensors::scanToJson(o["onewire"].to<JsonObject>());
  JsonArray an = o["analog"].to<JsonArray>();
  for (auto& h : analog) {
    JsonObject x = an.add<JsonObject>();
    x["source"] = h.source;
    if (isnan(h.volts)) x["volts"] = nullptr; else x["volts"] = roundf(h.volts * 1000) / 1000;
    x["assignedTo"] = h.assignedTo;
  }
}
