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
  float alt = isnan(p) ? NAN : 44330.0f * (1.0f - powf(p / gConfig.sys.seaLevelHpa, 0.1903f));

  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    if (!c.enabled || c.type != SensorType::BMP) continue;
    if (bmpKind == BMP_NONE) { setStatus(i, ST_NOT_FOUND); continue; }
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
    StateLock lk;
    gSys.bmpModel = bmpKind == BMP_280 ? "BMP280" : bmpKind == BMP_180 ? "BMP180" : "none";
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
  ::selftestFakeSensors(); return;
#endif
  updateAnalog(now);
  updateDs18b20(now);
  updateMax6675(now);
  updateMcp9600(now);
  updateBmp(now);
}

void Sensors::requestScan() { scanRequested = true; }

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
