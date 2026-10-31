#include "Sensors.h"
#ifdef CM_EMU_SELFTEST
void selftestFakeSensors();
#endif
#include "State.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <max6675.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MCP9600.h>
#include <map>
#include <memory>
#include <array>

namespace {

// ---------------------------------------------------------------- 1-Wire
OneWire* ow = nullptr;
DallasTemperature* dallas = nullptr;
int owPin = -1;
bool dsPending = false;
uint32_t dsRequestedAt = 0, dsLastCycle = 0;
uint16_t dsConvMs = 375;
std::vector<std::array<uint8_t, 8>> dsAddr;   // per sensor index
bool scanRequested = true;                     // scan once at boot

struct ScanResult { String address; float tempC; };
std::vector<ScanResult> scanResults;            // guarded by StateLock
uint32_t lastScanMs = 0;

// ---------------------------------------------------------------- MAX6675
std::vector<std::unique_ptr<MAX6675>> tc;       // per sensor index
uint32_t tcLast = 0;

// ---------------------------------------------------------------- BMP280 / BMP180
Adafruit_BMP280 bmp280(&Wire);
Adafruit_BMP085 bmp180;
enum { BMP_NONE, BMP_280, BMP_180 } bmpKind = BMP_NONE;
bool bmpProbed = false;
uint32_t bmpLast = 0;

// ---------------------------------------------------------------- MCP9600 / MCP9601
// I2C thermocouple amp: no extra GPIOs, sits on the OLED/BMP bus. The MCP9601
// variant flags open/short probes (status 0x10/0x20); on the MCP9600, 0x10 means
// "input out of range", which is also a fault.
struct Mcp { Adafruit_MCP9600 dev; bool found = false; };
std::map<uint8_t, std::unique_ptr<Mcp>> mcp;
uint32_t mcpLast = 0;

// ---------------------------------------------------------------- ADS1115
struct Ads { Adafruit_ADS1115 dev; bool found = false; };
std::map<uint8_t, std::unique_ptr<Ads>> ads;
uint32_t analogLast = 0;

uint32_t lastBeginMs = 0;
std::vector<std::vector<std::pair<float, float>>> curves;   // parsed "curve" per sensor index

// ---------------------------------------------------------------- RPM (pulse counting)
// The alternator W terminal gives an AC signal proportional to engine speed; a
// conditioning stage (opto-coupler or comparator) turns it into 3.3 V pulses.
struct RpmSlot {
  int pin = -1;
  volatile uint32_t count = 0, lastUs = 0;
  uint32_t seenCount = 0, seenUs = 0, lastChangeMs = 0;
  float hz = 0;
};
RpmSlot rpmSlots[4];
void IRAM_ATTR rpmIsr(void* arg) {
  RpmSlot* s = (RpmSlot*)arg;
  uint32_t t = micros();
  if (t - s->lastUs < 120) return;   // ignore ringing (> ~8 kHz)
  s->lastUs = t;
  s->count++;
}
std::vector<int> rpmSlotOf;   // per sensor index, -1 = none
uint32_t rpmLast = 0;

// ---------------------------------------------------------------- switches
struct SwitchState { uint8_t stable = 0, same = 0; int last = -1; };
std::vector<SwitchState> sw;
uint32_t swLast = 0;

// ---------------------------------------------------------------- I2C helpers (INA226, MPU-6050, BME280 humidity)
bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t v) { Wire.beginTransmission(addr); Wire.write(reg); Wire.write(v); return Wire.endTransmission() == 0; }
bool i2cWrite16(uint8_t addr, uint8_t reg, uint16_t v) { Wire.beginTransmission(addr); Wire.write(reg); Wire.write(v >> 8); Wire.write(v & 0xFF); return Wire.endTransmission() == 0; }

std::map<uint8_t, bool> inaFound, imuFound;
std::map<uint8_t, std::pair<float, float>> imuAngles;   // roll, pitch per address
uint32_t inaLast = 0, imuLast = 0;

// BME280 humidity (the BMP280 library reads its temperature and pressure)
struct BmeCal { uint16_t T1; int16_t T2, T3; uint8_t H1, H3; int16_t H2, H4, H5; int8_t H6; };
uint8_t bmeAddr = 0; BmeCal bmeCal; float bmeHum = NAN;
float bmeReadHumidity();

// ---------------------------------------------------------------- computed
struct CompState { float prev = NAN; uint32_t prevMs = 0; float warnSeconds = 0; uint32_t lastMs = 0; };
std::vector<CompState> comp;
uint32_t compLast = 0;

bool parseAddr(const String& hex, std::array<uint8_t, 8>& out) {
  if (hex.length() != 16) return false;
  for (int i = 0; i < 8; i++) {
    char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end;
    long v = strtol(b, &end, 16);
    if (*end) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}
String addrToHex(const uint8_t* a) {
  char buf[17];
  for (int i = 0; i < 8; i++) sprintf(buf + 2 * i, "%02X", a[i]);
  return String(buf);
}

// ---------------------------------------------------------------- result helpers
void setOk(size_t i, float v, float volts = NAN) {
  const SensorConfig& c = gConfig.sensors[i];
  v += c.offset;
  StateLock lk;
  SensorRuntime& r = gSensorRt[i];
  if (r.status == ST_OK && !isnan(r.value) && c.smoothing < 1.0f) v = r.value + c.smoothing * (v - r.value);
  r.value = v;
  r.volts = volts;
  r.status = ST_OK;
  r.fails = 0;
  r.lastOkMs = millis();
}

// Transient failures are tolerated: status only changes after `tolerance` misses in a row.
void setFail(size_t i, uint8_t status, float volts = NAN, uint8_t tolerance = 3) {
  StateLock lk;
  SensorRuntime& r = gSensorRt[i];
  r.volts = volts;
  if (r.fails < 255) r.fails++;
  if (r.fails >= tolerance || r.status != ST_OK) {
    r.status = status;
    r.value = NAN;
  }
}

void setStatus(size_t i, uint8_t status) {
  StateLock lk;
  gSensorRt[i].status = status;
  gSensorRt[i].value = NAN;
}

float toCalibrated(const SensorConfig& c, float sensorVolts) {
  return c.outMin + (sensorVolts - c.inMin) * (c.outMax - c.outMin) / (c.inMax - c.inMin);
}

void handleAnalog(size_t i, float pinVolts) {
  const SensorConfig& c = gConfig.sensors[i];
  float sv = pinVolts * c.divider;
  if ((!isnan(c.faultLowV) && sv < c.faultLowV) || (!isnan(c.faultHighV) && sv > c.faultHighV)) {
    setFail(i, ST_FAULT, sv);
    return;
  }
  if (i < curves.size() && !curves[i].empty()) { setOk(i, curveLookup(curves[i], sv), sv); return; }   // non-linear sender
  // Clamp to the sensor's output range: a sender a little under 0.5 V is 0 psi, not -6 psi.
  float v = toCalibrated(c, sv);
  float lo = min(c.outMin, c.outMax), hi = max(c.outMin, c.outMax);
  bool ranged = !(c.inMin == 0 && c.inMax == 1 && c.outMin == 0 && c.outMax == 1);  // identity map = plain volts
  if (ranged) v = constrain(v, lo, hi);
  setOk(i, v, sv);
}

// ---------------------------------------------------------------- per-family update
void updateDs18b20(uint32_t now) {
  if (!dallas) return;
  bool any = false;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (c.enabled && c.type == SensorType::DS18B20 && c.address.length()) any = true;
  }
  if (!any && !scanRequested) return;

  if (!dsPending && now - dsLastCycle >= 1000) {
    dallas->requestTemperatures();  // non-blocking (setWaitForConversion(false))
    dsRequestedAt = now;
    dsLastCycle = now;
    dsPending = true;
    return;
  }
  if (!dsPending || now - dsRequestedAt < (uint32_t)dsConvMs + 20) return;
  dsPending = false;

  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::DS18B20 || !c.address.length()) continue;
    float t = dallas->getTempC(dsAddr[i].data());
    float prev;
    { StateLock lk; prev = gSensorRt[i].value; }
    // 85.0 is the DS18B20 power-on value; only trust it if we were already close to 85.
    bool powerOnValue = (t == 85.0f) && !(fabsf(prev - 85.0f) < 5.0f);
    if (t == DEVICE_DISCONNECTED_C || t < -55 || t > 125 || powerOnValue) {
      setFail(i, ST_NOT_FOUND);
    } else {
      setOk(i, t);
    }
  }

  if (scanRequested) {
    scanRequested = false;
    std::vector<ScanResult> found;
    uint8_t a[8];
    ow->reset_search();
    while (ow->search(a) && found.size() < 32) {
      if (OneWire::crc8(a, 7) != a[7]) continue;
      if (a[0] != 0x28 && a[0] != 0x10 && a[0] != 0x22) continue;  // temperature sensor families
      float t = dallas->getTempC(a);  // value from the conversion we just did
      found.push_back({addrToHex(a), t == DEVICE_DISCONNECTED_C ? NAN : t});
    }
    dallas->setResolution(gConfig.sys.oneWireResolution);  // also applies to newly found devices
    StateLock lk;
    scanResults = found;
    lastScanMs = millis();
    Serial.printf("[1w] scan found %u sensor(s)\n", (unsigned)found.size());
  }
}

void updateMax6675(uint32_t now) {
  if (now - tcLast < 500) return;  // MAX6675 needs >= 220 ms between conversions
  tcLast = now;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    if (!tc[i]) continue;
    float t = tc[i]->readCelsius();
    if (isnan(t)) setFail(i, ST_FAULT, NAN, 2);         // NAN = thermocouple open circuit
    else if (t == 0.0f) setFail(i, ST_NOT_FOUND, NAN, 3);  // all-zero read = no MAX6675 answering
    else setOk(i, t);
  }
}

void updateMcp9600(uint32_t now) {
  if (now - mcpLast < 250) return;
  mcpLast = now;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::MCP9600) continue;
    auto it = mcp.find(c.mcpAddress);
    if (it == mcp.end() || !it->second->found) { setStatus(i, ST_NOT_FOUND); continue; }
    uint8_t st = it->second->dev.getStatus();
    float t = it->second->dev.readThermocouple();
    if ((st & 0x30) || isnan(t) || t < -200 || t > 1372) setFail(i, ST_FAULT, NAN, 2);
    else setOk(i, t);
  }
}

void updateBmp(uint32_t now) {
  if (now - bmpLast < 1000) return;
  bmpLast = now;
  bool any = false;
  for (auto& c : gConfig.sensors) if (c.enabled && c.type == SensorType::BMP) any = true;
  if (!any) return;

  float t = NAN, p = NAN;  // °C, hPa
  if (bmpKind == BMP_280) { t = bmp280.readTemperature(); p = bmp280.readPressure() / 100.0f; }
  else if (bmpKind == BMP_180) { t = bmp180.readTemperature(); p = bmp180.readPressure() / 100.0f; }
  float sl; { StateLock lk; sl = effectiveSeaLevelHpa(); gSys.stationHpa = (p > 300 && p < 1200) ? p : NAN; }
  float alt = isnan(p) ? NAN : 44330.0f * (1.0f - powf(p / sl, 0.1903f));
  bmeHum = bmeReadHumidity();

  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::BMP) continue;
    if (bmpKind == BMP_NONE || (c.bmpField == BmpField::HUMIDITY && !bmeAddr)) { setStatus(i, ST_NOT_FOUND); continue; }
    if (c.bmpField == BmpField::HUMIDITY) { if (isnan(bmeHum)) setFail(i, ST_FAULT); else setOk(i, bmeHum); continue; }
    float v = c.bmpField == BmpField::PRESSURE ? p : c.bmpField == BmpField::ALTITUDE ? alt : t;
    if (isnan(v) || p < 300 || p > 1200) setFail(i, ST_FAULT);
    else setOk(i, v);
  }
}

void updateAnalog(uint32_t now) {
  if (now - analogLast < 100) return;
  analogLast = now;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled) continue;
    if (c.type == SensorType::ADC) {
      uint32_t mv = 0;
      for (int k = 0; k < 8; k++) mv += analogReadMilliVolts(c.pin);  // calibrated by eFuse
      handleAnalog(i, mv / 8000.0f);
    } else if (c.type == SensorType::ADS1115) {
      auto it = ads.find(c.adsAddress);
      if (it == ads.end() || !it->second->found) { setStatus(i, ST_NOT_FOUND); continue; }
      int16_t raw = it->second->dev.readADC_SingleEnded(c.adsChannel);
      handleAnalog(i, it->second->dev.computeVolts(raw));
    }
  }
}

void updateRpm(uint32_t now) {
  if (now - rpmLast < 250) return;
  rpmLast = now;
  for (size_t i = 0; i < gConfig.sensors.size() && i < rpmSlotOf.size(); i++) {
    if (rpmSlotOf[i] < 0) continue;
    RpmSlot& s = rpmSlots[rpmSlotOf[i]];
    noInterrupts();
    uint32_t cnt = s.count, us = s.lastUs;
    interrupts();
    if (cnt != s.seenCount) {
      if (s.seenUs && us != s.seenUs) s.hz = (cnt - s.seenCount) * 1e6f / (float)(us - s.seenUs);
      s.seenCount = cnt; s.seenUs = us; s.lastChangeMs = now;
    } else if (now - s.lastChangeMs > 600) s.hz = 0;   // no pulses: engine stopped
    setOk(i, s.hz * 60.0f / gConfig.sensors[i].pulsesPerRev, s.hz);   // "volts" carries Hz for calibration
  }
}

void updateSwitches(uint32_t now) {
  if (now - swLast < 50) return;
  swLast = now;
  for (size_t i = 0; i < gConfig.sensors.size() && i < sw.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::SWITCH) continue;
    int active = (digitalRead(c.pin) == LOW) == c.activeLow;
    SwitchState& st = sw[i];
    if (active == st.last) { if (st.same < 255) st.same++; } else { st.same = 0; st.last = active; }
    if (st.same >= 3) setOk(i, active);   // 150 ms debounce
  }
}

void updateGpsSensors() {
  GpsRuntime g; { StateLock lk; g = gSys.gps; }
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::GPS) continue;
    if (!g.fitted) { setStatus(i, ST_NOT_FOUND); continue; }
    float v = NAN;
    if (c.field == "sats") v = g.sats;
    else if (!g.fix) { setStatus(i, ST_WAITING); continue; }
    else if (c.field == "speed") v = g.speedKmh;
    else if (c.field == "altitude") v = g.altM;
    else if (c.field == "heading") v = g.heading;
    else if (c.field == "hdop") v = g.hdop;
    if (isnan(v)) setStatus(i, ST_WAITING); else setOk(i, v);
  }
}

bool inaProbe(uint8_t a) {
  uint8_t id[2];
  bool ok = i2cRead(a, 0xFE, id, 2) && id[0] == 0x54 && id[1] == 0x49;   // "TI"
  if (ok) i2cWrite16(a, 0x00, 0x4527);   // 16 samples, 1.1 ms conversions, continuous
  return ok;
}

void updateIna(uint32_t now) {
  if (now - inaLast < 250) return;
  inaLast = now;
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::INA226) continue;
    if (!inaFound[c.i2cAddr]) { setStatus(i, ST_NOT_FOUND); continue; }
    uint8_t sh[2], bu[2];
    if (!i2cRead(c.i2cAddr, 0x01, sh, 2) || !i2cRead(c.i2cAddr, 0x02, bu, 2)) { setFail(i, ST_FAULT); continue; }
    float shuntV = (int16_t)((sh[0] << 8) | sh[1]) * 2.5e-6f, busV = ((bu[0] << 8) | bu[1]) * 1.25e-3f;
    float amps = shuntV / c.shuntOhms;
    setOk(i, c.field == "voltage" ? busV : c.field == "power" ? busV * amps : amps);
  }
}

bool imuProbe(uint8_t a) {
  uint8_t who;
  if (!i2cRead(a, 0x75, &who, 1) || who == 0 || who == 0xFF) return false;
  i2cWrite8(a, 0x6B, 0x00);   // wake up
  i2cWrite8(a, 0x1A, 0x05);   // 10 Hz low-pass: smooth out engine vibration
  i2cWrite8(a, 0x1C, 0x00);   // +-2 g
  return true;
}

void updateImu(uint32_t now) {
  if (now - imuLast < 200) return;
  imuLast = now;
  for (auto& f : imuFound) {
    if (!f.second) continue;
    uint8_t b[6];
    if (!i2cRead(f.first, 0x3B, b, 6)) { imuAngles[f.first] = {NAN, NAN}; continue; }
    float ax = (int16_t)((b[0] << 8) | b[1]), ay = (int16_t)((b[2] << 8) | b[3]), az = (int16_t)((b[4] << 8) | b[5]);
    imuAngles[f.first] = {atan2f(ay, az) * 57.2958f, atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f};
  }
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::IMU) continue;
    if (!imuFound[c.i2cAddr]) { setStatus(i, ST_NOT_FOUND); continue; }
    auto a = imuAngles[c.i2cAddr];
    float v = c.field == "pitch" ? a.second : a.first;
    if (isnan(v)) setFail(i, ST_FAULT); else setOk(i, v);
  }
}

void bmeProbe() {
  for (uint8_t a : {0x76, 0x77}) {
    uint8_t id;
    if (!i2cRead(a, 0xD0, &id, 1) || id != 0x60) continue;
    uint8_t t[6], h1, h[7];
    if (!i2cRead(a, 0x88, t, 6) || !i2cRead(a, 0xA1, &h1, 1) || !i2cRead(a, 0xE1, h, 7)) return;
    bmeCal.T1 = t[0] | (t[1] << 8); bmeCal.T2 = t[2] | (t[3] << 8); bmeCal.T3 = t[4] | (t[5] << 8);
    bmeCal.H1 = h1; bmeCal.H2 = h[0] | (h[1] << 8); bmeCal.H3 = h[2];
    bmeCal.H4 = (int16_t)((h[3] << 4) | (h[4] & 0x0F)); bmeCal.H5 = (int16_t)((h[5] << 4) | (h[4] >> 4)); bmeCal.H6 = (int8_t)h[6];
    uint8_t meas;
    i2cWrite8(a, 0xF2, 0x01);                                       // humidity x1 ...
    if (i2cRead(a, 0xF4, &meas, 1)) i2cWrite8(a, 0xF4, meas);       // ... takes effect after ctrl_meas is written
    bmeAddr = a;
    Serial.printf("[bme] BME280 humidity at 0x%02X\n", a);
    return;
  }
}

// Bosch BME280 datasheet compensation (integer version)
float bmeReadHumidity() {
  uint8_t b[8];
  if (!bmeAddr || !i2cRead(bmeAddr, 0xF7, b, 8)) return NAN;
  int32_t adcT = ((int32_t)b[3] << 12) | ((int32_t)b[4] << 4) | (b[5] >> 4);
  int32_t adcH = ((int32_t)b[6] << 8) | b[7];
  if (adcH == 0x8000) return NAN;
  int32_t v1 = ((((adcT >> 3) - ((int32_t)bmeCal.T1 << 1))) * bmeCal.T2) >> 11;
  int32_t v2 = (((((adcT >> 4) - (int32_t)bmeCal.T1) * ((adcT >> 4) - (int32_t)bmeCal.T1)) >> 12) * bmeCal.T3) >> 14;
  int32_t x = (v1 + v2) - 76800;
  x = (((((adcH << 14) - ((int32_t)bmeCal.H4 << 20) - ((int32_t)bmeCal.H5 * x)) + 16384) >> 15) *
       (((((((x * (int32_t)bmeCal.H6) >> 10) * (((x * (int32_t)bmeCal.H3) >> 11) + 32768)) >> 10) + 2097152) * (int32_t)bmeCal.H2 + 8192) >> 14));
  x = x - (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)bmeCal.H1) >> 4);
  x = x < 0 ? 0 : x > 419430400 ? 419430400 : x;
  return (x >> 12) / 1024.0f;
}

void updateComputed(uint32_t now) {
  if (now - compLast < 250) return;
  compLast = now;
  for (size_t i = 0; i < gConfig.sensors.size() && i < comp.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::COMPUTED) continue;
    int ia = findSensorIndex(c.srcA), ib = findSensorIndex(c.srcB);
    float a = NAN, b = NAN; bool okA = false, okB = false; uint8_t lvlA = LVL_OK;
    {
      StateLock lk;
      if (ia >= 0) { okA = gSensorRt[ia].status == ST_OK; a = gSensorRt[ia].value; lvlA = gSensorRt[ia].level; }
      if (ib >= 0) { okB = gSensorRt[ib].status == ST_OK; b = gSensorRt[ib].value; }
    }
    CompState& st = comp[i];
    float dt = st.lastMs ? (now - st.lastMs) / 1000.0f : 0;
    st.lastMs = now;
    if (c.formula == "time_in_warn") {
      if (lvlA == LVL_WARN || lvlA == LVL_CRIT) st.warnSeconds += dt;
      setOk(i, st.warnSeconds / 60.0f);
      continue;
    }
    if (!okA || isnan(a)) { setStatus(i, ST_WAITING); continue; }
    if (c.formula == "rate") {   // change per minute, measured over >= 5 s
      if (isnan(st.prev)) { st.prev = a; st.prevMs = now; setStatus(i, ST_WAITING); continue; }
      if (now - st.prevMs >= 5000) {
        float r = (a - st.prev) * 60000.0f / (now - st.prevMs);
        st.prev = a; st.prevMs = now;
        setOk(i, r);
      }
      continue;
    }
    if (!okB || isnan(b)) { setStatus(i, ST_WAITING); continue; }
    if (c.formula == "diff") setOk(i, a - b);
    else if (c.formula == "sum") setOk(i, a + b);
    else if (c.formula == "curve_margin" && i < curves.size()) setOk(i, a - curveLookup(curves[i], b));
  }
}

}  // namespace

// ---------------------------------------------------------------- public
void Sensors::begin() {
  const Config& cfg = gConfig;
  size_t n = cfg.sensors.size();
  lastBeginMs = millis();

  // 1-Wire bus
  if (!ow || owPin != cfg.sys.oneWirePin) {
    delete dallas; delete ow;
    owPin = cfg.sys.oneWirePin;
    ow = new OneWire(owPin);
    dallas = new DallasTemperature(ow);
    dallas->begin();
  }
  dallas->setWaitForConversion(false);
  dallas->setResolution(cfg.sys.oneWireResolution);
  dsConvMs = 750 / (1 << (12 - cfg.sys.oneWireResolution));
  dsPending = false;
  dsAddr.assign(n, {});
  for (size_t i = 0; i < n; i++) {
    const SensorConfig& c = cfg.sensors[i];
    if (c.type == SensorType::DS18B20 && c.address.length()) parseAddr(c.address, dsAddr[i]);
  }

  // Thermocouples
  tc.clear();
  tc.resize(n);
  for (size_t i = 0; i < n; i++) {
    const SensorConfig& c = cfg.sensors[i];
    if (c.enabled && c.type == SensorType::MAX6675) tc[i].reset(new MAX6675(c.sckPin, c.pin, c.soPin));
  }

  // BMP (probe once; I2C is already started by main)
  bool wantBmp = false;
  for (auto& c : cfg.sensors) if (c.enabled && c.type == SensorType::BMP) wantBmp = true;
  if (wantBmp && !bmpProbed) {
    bmpProbed = true;
    if (bmp280.begin(0x76, 0x58) || bmp280.begin(0x77, 0x58) || bmp280.begin(0x76, 0x60) || bmp280.begin(0x77, 0x60)) {
      bmpKind = BMP_280;
      bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16,
                         Adafruit_BMP280::FILTER_X16, Adafruit_BMP280::STANDBY_MS_500);
    } else if (bmp180.begin(BMP085_STANDARD, &Wire)) {
      bmpKind = BMP_180;
    }
    if (bmpKind == BMP_280) bmeProbe();
    StateLock lk;
    gSys.bmpModel = bmpKind == BMP_280 ? (bmeAddr ? "BME280" : "BMP280") : bmpKind == BMP_180 ? "BMP180" : "none";
    Serial.printf("[bmp] %s\n", gSys.bmpModel.c_str());
  }

  // MCP9600 / MCP9601 thermocouple amplifiers
  for (auto& c : cfg.sensors) {
    if (!c.enabled || c.type != SensorType::MCP9600 || mcp.count(c.mcpAddress)) continue;
    std::unique_ptr<Mcp> m(new Mcp());
    m->found = m->dev.begin(c.mcpAddress, &Wire);
    if (m->found) {
      m->dev.setThermocoupleType(MCP9600_TYPE_K);
      m->dev.setADCresolution(MCP9600_ADCRESOLUTION_16);  // ~80 ms per conversion, 0.0625 C
      m->dev.setFilterCoefficient(2);
      m->dev.enable(true);
    }
    Serial.printf("[mcp9600] 0x%02X %s\n", c.mcpAddress, m->found ? "found" : "NOT found");
    mcp[c.mcpAddress] = std::move(m);
  }

  // ADS1115 modules
  for (auto& c : cfg.sensors) {
    if (!c.enabled || c.type != SensorType::ADS1115 || ads.count(c.adsAddress)) continue;
    std::unique_ptr<Ads> a(new Ads());
    a->found = a->dev.begin(c.adsAddress, &Wire);
    if (a->found) {
      a->dev.setGain(GAIN_ONE);                  // +/-4.096 V range; inputs must stay below 3.3 V
      a->dev.setDataRate(RATE_ADS1115_860SPS);   // ~1.2 ms per conversion
    }
    Serial.printf("[ads] 0x%02X %s\n", c.adsAddress, a->found ? "found" : "NOT found");
    ads[c.adsAddress] = std::move(a);
  }

  // Curves (analog lookup tables, curve_margin)
  curves.assign(n, {});
  for (size_t i = 0; i < n; i++) if (cfg.sensors[i].curve.length()) parseCurve(cfg.sensors[i].curve, curves[i]);

  // RPM inputs
  for (auto& s : rpmSlots) if (s.pin >= 0) { detachInterrupt(s.pin); s.pin = -1; }
  rpmSlotOf.assign(n, -1);
  int slot = 0;
  for (size_t i = 0; i < n && slot < 4; i++) {
    const SensorConfig& c = cfg.sensors[i];
    if (!c.enabled || c.type != SensorType::RPM) continue;
    RpmSlot& s = rpmSlots[slot];
    s.pin = c.pin; s.count = 0; s.lastUs = 0; s.seenCount = 0; s.seenUs = 0; s.hz = 0; s.lastChangeMs = millis();
    pinMode(c.pin, INPUT);
    attachInterruptArg(digitalPinToInterrupt(c.pin), rpmIsr, &s, RISING);
    rpmSlotOf[i] = slot++;
  }

  // Switch inputs
  sw.assign(n, SwitchState());
  for (auto& c : cfg.sensors)
    if (c.enabled && c.type == SensorType::SWITCH) pinMode(c.pin, c.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);

  // INA226 / IMU (probe each address once per config)
  inaFound.clear(); imuFound.clear(); imuAngles.clear();
  for (auto& c : cfg.sensors) {
    if (!c.enabled) continue;
    if (c.type == SensorType::INA226 && !inaFound.count(c.i2cAddr)) {
      inaFound[c.i2cAddr] = inaProbe(c.i2cAddr);
      Serial.printf("[ina226] 0x%02X %s\n", c.i2cAddr, inaFound[c.i2cAddr] ? "found" : "NOT found");
    }
    if (c.type == SensorType::IMU && !imuFound.count(c.i2cAddr)) {
      imuFound[c.i2cAddr] = imuProbe(c.i2cAddr);
      Serial.printf("[imu] 0x%02X %s\n", c.i2cAddr, imuFound[c.i2cAddr] ? "found" : "NOT found");
    }
  }
  comp.assign(n, CompState());

  // ESP32 ADC pins
  for (auto& c : cfg.sensors)
    if (c.enabled && c.type == SensorType::ADC) analogSetPinAttenuation(c.pin, ADC_11db);  // 0-3.1 V

  // Initial statuses
  StateLock lk;
  for (size_t i = 0; i < n; i++) {
    const SensorConfig& c = cfg.sensors[i];
    SensorRuntime& r = gSensorRt[i];
    if (!c.enabled) r.status = ST_DISABLED;
    else if (c.type == SensorType::DS18B20 && !c.address.length()) r.status = ST_UNASSIGNED;
    else if (r.status == ST_DISABLED || r.status == ST_UNASSIGNED) r.status = ST_WAITING;
  }
}

void Sensors::update() {
  uint32_t now = millis();
#ifdef CM_EMU_SELFTEST
  ::selftestFakeSensors(); updateGpsSensors(); updateComputed(now); return;
#endif
  updateAnalog(now);
  updateDs18b20(now);
  updateMax6675(now);
  updateMcp9600(now);
  updateBmp(now);
  updateRpm(now);
  updateSwitches(now);
  updateIna(now);
  updateImu(now);
  updateGpsSensors();
  updateComputed(now);   // last: uses the values above
}

void Sensors::requestScan() { scanRequested = true; }

float Sensors::rpmHz(const String& id) {
  int i = findSensorIndex(id);
  if (i < 0 || i >= (int)rpmSlotOf.size() || rpmSlotOf[i] < 0) return NAN;
  return rpmSlots[rpmSlotOf[i]].hz;
}

void Sensors::scanToJson(JsonObject o) {
  o["lastScanAgeMs"] = lastScanMs ? (int32_t)(millis() - lastScanMs) : -1;
  o["scanPending"] = scanRequested;
  JsonArray a = o["devices"].to<JsonArray>();
  for (auto& r : scanResults) {
    JsonObject d = a.add<JsonObject>();
    d["address"] = r.address;
    if (isnan(r.tempC)) d["tempC"] = nullptr; else d["tempC"] = r.tempC;
    String owner;
    for (auto& c : gConfig.sensors)
      if (c.type == SensorType::DS18B20 && c.address.equalsIgnoreCase(r.address)) owner = c.id;
    d["assignedTo"] = owner;
  }
}
