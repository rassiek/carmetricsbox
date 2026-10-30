#include "Config.h"
#include <Preferences.h>
#include <nvs_flash.h>
#include <map>
#include <memory>

// ---------------------------------------------------------------- enums <-> strings
const char* sensorTypeToStr(SensorType t) {
  switch (t) {
    case SensorType::DS18B20: return "ds18b20";
    case SensorType::MAX6675: return "max6675";
    case SensorType::MCP9600: return "mcp9600";
    case SensorType::BMP:     return "bmp";
    case SensorType::ADC:     return "adc";
    case SensorType::ADS1115: return "ads1115";
    default:                  return "unknown";
  }
}
SensorType sensorTypeFromStr(const String& s) {
  if (s == "ds18b20") return SensorType::DS18B20;
  if (s == "max6675") return SensorType::MAX6675;
  if (s == "mcp9600") return SensorType::MCP9600;
  if (s == "bmp")     return SensorType::BMP;
  if (s == "adc")     return SensorType::ADC;
  if (s == "ads1115") return SensorType::ADS1115;
  return SensorType::UNKNOWN;
}
const char* bmpFieldToStr(BmpField f) {
  switch (f) {
    case BmpField::PRESSURE: return "pressure";
    case BmpField::ALTITUDE: return "altitude";
    default:                 return "temperature";
  }
}
BmpField bmpFieldFromStr(const String& s) {
  if (s == "pressure") return BmpField::PRESSURE;
  if (s == "altitude") return BmpField::ALTITUDE;
  return BmpField::TEMPERATURE;
}
const char* oledStyleToStr(OledStyle s) {
  switch (s) {
    case OledStyle::TEXT: return "text";
    case OledStyle::BAR:  return "bar";
    default:              return "off";
  }
}
OledStyle oledStyleFromStr(const String& s) {
  if (s == "text") return OledStyle::TEXT;
  if (s == "bar")  return OledStyle::BAR;
  return OledStyle::OFF;
}

// ---------------------------------------------------------------- defaults
// Pin defaults follow the V3 wiring so existing harnesses keep working.
static SensorConfig ds(const char* id, const char* name, const char* lbl, OledStyle st,
                       float gmin, float gmax, float warn, float crit) {
  SensorConfig s;
  s.id = id; s.name = name; s.unit = "°C"; s.type = SensorType::DS18B20;
  s.oledLabel = lbl; s.oledStyle = st; s.gaugeMin = gmin; s.gaugeMax = gmax;
  s.warnHigh = warn; s.critHigh = crit; s.hysteresis = 1.0f; s.smoothing = 1.0f;
  return s;
}
static SensorConfig analog(const char* id, const char* name, const char* unit, int pin,
                           float divider, float inMin, float inMax, float outMin, float outMax) {
  SensorConfig s;
  s.id = id; s.name = name; s.unit = unit; s.type = SensorType::ADC; s.pin = pin;
  s.divider = divider; s.inMin = inMin; s.inMax = inMax; s.outMin = outMin; s.outMax = outMax;
  return s;
}

void defaultSensors(std::vector<SensorConfig>& v) {
  v.clear();
  // 1-Wire temperatures - addresses are assigned from the Settings page after a bus scan
  v.push_back(ds("coolant_temp", "Coolant", "CT", OledStyle::BAR, 15, 120, 100, 105));
  v.push_back(ds("intake_temp", "Intake air", "I", OledStyle::TEXT, 0, 80, 55, 65));
  v.push_back(ds("gearbox_temp", "Gearbox", "G", OledStyle::TEXT, 0, 120, 90, 105));
  v.push_back(ds("transfer_case_temp", "Transfer case", "T", OledStyle::TEXT, 0, 120, 90, 105));

  // EGT thermocouple (MAX6675: SCK=5, CS=17, SO=16)
  SensorConfig egt;
  egt.id = "egt"; egt.name = "EGT"; egt.unit = "°C"; egt.type = SensorType::MAX6675;
  egt.sckPin = 5; egt.pin = 17; egt.soPin = 16; egt.offset = -8.0f; egt.smoothing = 0.5f;
  egt.gaugeMin = 0; egt.gaugeMax = 800; egt.warnHigh = 600; egt.critHigh = 700; egt.hysteresis = 10;
  egt.oledStyle = OledStyle::BAR; egt.oledLabel = "EGT";
  v.push_back(egt);

  // Pressure senders: 5 V, 0.5-4.5 V out, through a 10k/20k divider (x1.5)
  SensorConfig boost = analog("boost_pressure", "Boost", "psi", 32, 1.5f, 0.5f, 4.5f, 0, 30);
  boost.decimals = 1; boost.gaugeMin = 0; boost.gaugeMax = 25; boost.warnHigh = 17; boost.critHigh = 20;
  boost.hysteresis = 0.5f; boost.faultLowV = 0.2f; boost.faultHighV = 4.8f;
  boost.oledStyle = OledStyle::BAR; boost.oledLabel = "BST";
  v.push_back(boost);

  SensorConfig oil = analog("oil_pressure", "Oil pressure", "psi", 34, 1.5f, 0.5f, 4.5f, 0, 100);
  oil.gaugeMin = 0; oil.gaugeMax = 100; oil.warnLow = 15; oil.critLow = 7; oil.warnHigh = 85;
  oil.hysteresis = 2; oil.faultLowV = 0.2f; oil.faultHighV = 4.8f; oil.engineRunningOnly = true;
  oil.oledStyle = OledStyle::BAR; oil.oledLabel = "OIL";
  v.push_back(oil);

  // Battery voltage via B25 module (30k/7.5k = x5)
  SensorConfig bat = analog("bat_voltage", "Starter battery", "V", 35, 5.0f, 0, 1, 0, 1);
  bat.decimals = 1; bat.gaugeMin = 8; bat.gaugeMax = 16;
  bat.warnLow = 11.8f; bat.critLow = 11.2f; bat.warnHigh = 14.8f; bat.critHigh = 15.2f; bat.hysteresis = 0.2f;
  bat.oledStyle = OledStyle::TEXT; bat.oledLabel = "B";
  v.push_back(bat);

  SensorConfig aux = analog("aux_voltage", "Aux battery", "V", 33, 5.0f, 0, 1, 0, 1);
  aux.enabled = false; aux.decimals = 1; aux.gaugeMin = 8; aux.gaugeMax = 16;
  aux.warnLow = 11.8f; aux.critLow = 11.2f; aux.hysteresis = 0.2f;
  aux.oledStyle = OledStyle::TEXT; aux.oledLabel = "A";
  v.push_back(aux);

  // Cabin BMP180/BMP280 (auto-detected on I2C)
  SensorConfig cab;
  cab.id = "cabin_temp"; cab.name = "Cabin"; cab.unit = "°C"; cab.type = SensorType::BMP;
  cab.bmpField = BmpField::TEMPERATURE; cab.gaugeMin = 0; cab.gaugeMax = 50; cab.warnHigh = 40;
  cab.oledStyle = OledStyle::TEXT; cab.oledLabel = "C";
  v.push_back(cab);

  SensorConfig alt;
  alt.id = "altitude"; alt.name = "Altitude"; alt.unit = "m"; alt.type = SensorType::BMP;
  alt.bmpField = BmpField::ALTITUDE; alt.gaugeMin = 0; alt.gaugeMax = 3000; alt.smoothing = 0.2f;
  alt.oledStyle = OledStyle::TEXT; alt.oledLabel = "^";
  v.push_back(alt);
}

void defaultRelays(std::vector<RelayConfig>& v) {
  v.clear();
  // Disabled until you confirm wiring and whether your relay board is active-low.
  RelayConfig r1; r1.id = "relay1"; r1.name = "Relay 1"; r1.pin = 26; r1.enabled = false; r1.activeLow = true;
  RelayConfig r2; r2.id = "relay2"; r2.name = "Relay 2"; r2.pin = 27; r2.enabled = false; r2.activeLow = true;
  v.push_back(r1); v.push_back(r2);
}

void configDefaults(Config& c) {
  c.sys = SystemConfig();
  defaultSensors(c.sensors);
  defaultRelays(c.relays);
}

// ---------------------------------------------------------------- JSON helpers
static void putF(JsonObject o, const char* k, float v) {
  if (isnan(v)) o[k] = nullptr; else o[k] = v;
}
static bool hasKey(JsonObjectConst o, const char* k) {
  for (JsonPairConst p : o) if (strcmp(p.key().c_str(), k) == 0) return true;
  return false;
}
static float getF(JsonObjectConst o, const char* k, float def) {
  JsonVariantConst v = o[k];
  if (v.isNull()) return hasKey(o, k) ? NAN : def;  // explicit null disables
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  if (v.is<const char*>()) {
    String s = v.as<const char*>(); s.trim();
    if (s.length() == 0) return NAN;
    return s.toFloat();
  }
  return def;
}
static int getI(JsonObjectConst o, const char* k, int def) {
  JsonVariantConst v = o[k];
  if (v.isNull()) return def;
  if (v.is<const char*>()) { String s = v.as<const char*>(); s.trim(); return s.length() ? (int)strtol(s.c_str(), nullptr, 0) : def; }
  return v.as<int>();
}
static bool getB(JsonObjectConst o, const char* k, bool def) {
  JsonVariantConst v = o[k];
  return v.isNull() ? def : v.as<bool>();
}
static String getS(JsonObjectConst o, const char* k, const String& def) {
  JsonVariantConst v = o[k];
  return v.isNull() ? def : String(v.as<const char*>());
}

static bool validId(const String& id) {
  if (id.length() == 0 || id.length() > 31) return false;
  for (char ch : id) if (!(isdigit(ch) || (ch >= 'a' && ch <= 'z') || ch == '_')) return false;
  return true;
}

// ---------------------------------------------------------------- to JSON
void sensorToJson(const SensorConfig& s, JsonObject o) {
  o["id"] = s.id; o["name"] = s.name; o["unit"] = s.unit;
  o["type"] = sensorTypeToStr(s.type); o["enabled"] = s.enabled;
  o["address"] = s.address; o["pin"] = s.pin; o["sckPin"] = s.sckPin; o["soPin"] = s.soPin;
  o["adsAddress"] = s.adsAddress; o["adsChannel"] = s.adsChannel; o["mcpAddress"] = s.mcpAddress;
  o["bmpField"] = bmpFieldToStr(s.bmpField);
  putF(o, "divider", s.divider); putF(o, "inMin", s.inMin); putF(o, "inMax", s.inMax);
  putF(o, "outMin", s.outMin); putF(o, "outMax", s.outMax);
  putF(o, "faultLowV", s.faultLowV); putF(o, "faultHighV", s.faultHighV);
  putF(o, "offset", s.offset); putF(o, "smoothing", s.smoothing);
  o["decimals"] = s.decimals; putF(o, "gaugeMin", s.gaugeMin); putF(o, "gaugeMax", s.gaugeMax);
  o["oledStyle"] = oledStyleToStr(s.oledStyle); o["oledLabel"] = s.oledLabel;
  putF(o, "warnHigh", s.warnHigh); putF(o, "critHigh", s.critHigh);
  putF(o, "warnLow", s.warnLow); putF(o, "critLow", s.critLow);
  putF(o, "hysteresis", s.hysteresis); o["engineRunningOnly"] = s.engineRunningOnly;
}

void relayToJson(const RelayConfig& r, JsonObject o) {
  o["id"] = r.id; o["name"] = r.name; o["pin"] = r.pin;
  o["mode"] = r.mode == RelayMode::BLINK ? "blink" : "on_off";
  o["activeLow"] = r.activeLow; o["defaultOn"] = r.defaultOn;
  o["blinkMs"] = r.blinkMs; o["enabled"] = r.enabled;
}

void systemToJson(const SystemConfig& s, JsonObject o, bool includeSecrets) {
  o["apSsid"] = s.apSsid; o["apPass"] = includeSecrets ? s.apPass : "";
  o["staSsid"] = s.staSsid; o["staPass"] = includeSecrets ? s.staPass : "";
  o["hostname"] = s.hostname; o["adminPass"] = includeSecrets ? s.adminPass : "";
  o["i2cSda"] = s.i2cSda; o["i2cScl"] = s.i2cScl; o["i2cKhz"] = s.i2cKhz;
  o["oneWirePin"] = s.oneWirePin; o["oneWireResolution"] = s.oneWireResolution;
  o["oledType"] = s.oledType; o["oledFlip"] = s.oledFlip; o["oledClock"] = s.oledClock;
  o["oledPageSeconds"] = s.oledPageSeconds; o["oledContrast"] = s.oledContrast;
  o["buzzerPin"] = s.buzzerPin; o["buzzerActiveLow"] = s.buzzerActiveLow;
  o["ledPin"] = s.ledPin; o["ledActiveLow"] = s.ledActiveLow;
  o["engineSensorId"] = s.engineSensorId; putF(o, "engineRunningAbove", s.engineRunningAbove);
  o["engineDelaySeconds"] = s.engineDelaySeconds;
  putF(o, "seaLevelHpa", s.seaLevelHpa); o["historyIntervalSeconds"] = s.historyIntervalSeconds;
  o["tzOffsetMinutes"] = s.tzOffsetMinutes;
}

void configToJson(const Config& c, JsonObject o, bool includeSecrets) {
  o["version"] = FW_VERSION;
  systemToJson(c.sys, o["system"].to<JsonObject>(), includeSecrets);
  JsonArray sa = o["sensors"].to<JsonArray>();
  for (auto& s : c.sensors) sensorToJson(s, sa.add<JsonObject>());
  JsonArray ra = o["relays"].to<JsonArray>();
  for (auto& r : c.relays) relayToJson(r, ra.add<JsonObject>());
}

// ---------------------------------------------------------------- from JSON
static bool sensorFromJson(JsonObjectConst o, SensorConfig& s, String& err) {
  s.id = getS(o, "id", ""); s.id.trim(); s.id.toLowerCase();
  if (!validId(s.id)) { err = "Sensor id '" + s.id + "' must be 1-31 chars of a-z, 0-9, _"; return false; }
  s.name = getS(o, "name", s.id); if (s.name.length() == 0) s.name = s.id;
  s.unit = getS(o, "unit", "");
  s.type = sensorTypeFromStr(getS(o, "type", ""));
  if (s.type == SensorType::UNKNOWN) { err = "Sensor '" + s.id + "': unknown type"; return false; }
  s.enabled = getB(o, "enabled", true);
  s.address = getS(o, "address", ""); s.address.trim(); s.address.toUpperCase();
  s.pin = getI(o, "pin", -1); s.sckPin = getI(o, "sckPin", -1); s.soPin = getI(o, "soPin", -1);
  s.adsAddress = getI(o, "adsAddress", 0x48); s.adsChannel = getI(o, "adsChannel", 0);
  s.mcpAddress = getI(o, "mcpAddress", 0x67);
  s.bmpField = bmpFieldFromStr(getS(o, "bmpField", "temperature"));
  s.divider = getF(o, "divider", 1); s.inMin = getF(o, "inMin", 0); s.inMax = getF(o, "inMax", 1);
  s.outMin = getF(o, "outMin", 0); s.outMax = getF(o, "outMax", 1);
  s.faultLowV = getF(o, "faultLowV", NAN); s.faultHighV = getF(o, "faultHighV", NAN);
  s.offset = getF(o, "offset", 0); s.smoothing = getF(o, "smoothing", 0.3f);
  s.decimals = constrain(getI(o, "decimals", 0), 0, 3);
  s.gaugeMin = getF(o, "gaugeMin", 0); s.gaugeMax = getF(o, "gaugeMax", 100);
  s.oledStyle = oledStyleFromStr(getS(o, "oledStyle", "text"));
  s.oledLabel = getS(o, "oledLabel", ""); if (s.oledLabel.length() > 3) s.oledLabel = s.oledLabel.substring(0, 3);
  s.warnHigh = getF(o, "warnHigh", NAN); s.critHigh = getF(o, "critHigh", NAN);
  s.warnLow = getF(o, "warnLow", NAN); s.critLow = getF(o, "critLow", NAN);
  s.hysteresis = getF(o, "hysteresis", 1); if (isnan(s.hysteresis) || s.hysteresis < 0) s.hysteresis = 0;
  s.engineRunningOnly = getB(o, "engineRunningOnly", false);

  // per-type checks
  if (s.type == SensorType::DS18B20 && s.address.length() && s.address.length() != 16) {
    err = "Sensor '" + s.id + "': 1-Wire address must be 16 hex characters"; return false;
  }
  if (s.type == SensorType::ADC || s.type == SensorType::ADS1115) {
    if (isnan(s.divider) || s.divider <= 0) { err = "Sensor '" + s.id + "': divider must be > 0"; return false; }
    if (isnan(s.inMin) || isnan(s.inMax) || s.inMax == s.inMin) { err = "Sensor '" + s.id + "': input range is empty"; return false; }
    if (isnan(s.outMin) || isnan(s.outMax)) { err = "Sensor '" + s.id + "': output range missing"; return false; }
  }
  if (s.type == SensorType::MCP9600 && (s.mcpAddress < 0x60 || s.mcpAddress > 0x67)) {
    err = "Sensor '" + s.id + "': MCP9600 address must be 0x60-0x67"; return false;
  }
  if (s.type == SensorType::ADS1115) {
    if (s.adsAddress < 0x48 || s.adsAddress > 0x4B) { err = "Sensor '" + s.id + "': ADS1115 address must be 0x48-0x4B"; return false; }
    if (s.adsChannel > 3) { err = "Sensor '" + s.id + "': ADS1115 channel must be 0-3"; return false; }
  }
  if (isnan(s.gaugeMin) || isnan(s.gaugeMax) || s.gaugeMax <= s.gaugeMin) { err = "Sensor '" + s.id + "': gauge max must be above gauge min"; return false; }
  if (isnan(s.smoothing) || s.smoothing <= 0 || s.smoothing > 1) s.smoothing = 1;
  if (isnan(s.offset)) s.offset = 0;
  return true;
}

bool sensorsFromJson(JsonVariantConst v, std::vector<SensorConfig>& out, String& err) {
  if (!v.is<JsonArrayConst>()) { err = "Expected an array of sensors"; return false; }
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.size() > MAX_SENSORS) { err = "At most " + String(MAX_SENSORS) + " sensors"; return false; }
  std::vector<SensorConfig> tmp;
  for (JsonObjectConst o : a) {
    SensorConfig s;
    if (!sensorFromJson(o, s, err)) return false;
    tmp.push_back(s);
  }
  out = tmp;
  return true;
}

bool relaysFromJson(JsonVariantConst v, std::vector<RelayConfig>& out, String& err) {
  if (!v.is<JsonArrayConst>()) { err = "Expected an array of relays"; return false; }
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.size() > MAX_RELAYS) { err = "At most " + String(MAX_RELAYS) + " relays"; return false; }
  std::vector<RelayConfig> tmp;
  for (JsonObjectConst o : a) {
    RelayConfig r;
    r.id = getS(o, "id", ""); r.id.trim(); r.id.toLowerCase();
    if (!validId(r.id)) { err = "Relay id '" + r.id + "' must be 1-31 chars of a-z, 0-9, _"; return false; }
    r.name = getS(o, "name", r.id); if (r.name.length() == 0) r.name = r.id;
    r.pin = getI(o, "pin", -1);
    r.mode = getS(o, "mode", "on_off") == "blink" ? RelayMode::BLINK : RelayMode::ON_OFF;
    r.activeLow = getB(o, "activeLow", false); r.defaultOn = getB(o, "defaultOn", false);
    r.blinkMs = constrain(getI(o, "blinkMs", 500), 0, 60000); r.enabled = getB(o, "enabled", true);
    tmp.push_back(r);
  }
  out = tmp;
  return true;
}

bool systemFromJson(JsonVariantConst v, SystemConfig& s, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected a system settings object"; return false; }
  JsonObjectConst o = v.as<JsonObjectConst>();
  s.apSsid = getS(o, "apSsid", s.apSsid);
  String p = getS(o, "apPass", ""); if (p.length()) s.apPass = p;
  s.staSsid = getS(o, "staSsid", s.staSsid);
  p = getS(o, "staPass", ""); if (p.length()) s.staPass = p;
  if (s.staSsid.length() == 0) s.staPass = "";
  s.hostname = getS(o, "hostname", s.hostname);
  p = getS(o, "adminPass", ""); if (p.length()) s.adminPass = p;
  s.i2cSda = getI(o, "i2cSda", s.i2cSda); s.i2cScl = getI(o, "i2cScl", s.i2cScl);
  s.i2cKhz = getI(o, "i2cKhz", s.i2cKhz) <= 100 ? 100 : 400;
  s.oneWirePin = getI(o, "oneWirePin", s.oneWirePin);
  s.oneWireResolution = constrain(getI(o, "oneWireResolution", s.oneWireResolution), 9, 12);
  s.oledType = getS(o, "oledType", s.oledType);
  s.oledFlip = getB(o, "oledFlip", s.oledFlip); s.oledClock = getB(o, "oledClock", s.oledClock);
  s.oledPageSeconds = constrain(getI(o, "oledPageSeconds", s.oledPageSeconds), 1, 60);
  s.oledContrast = constrain(getI(o, "oledContrast", s.oledContrast), 0, 255);
  s.buzzerPin = getI(o, "buzzerPin", s.buzzerPin); s.buzzerActiveLow = getB(o, "buzzerActiveLow", s.buzzerActiveLow);
  s.ledPin = getI(o, "ledPin", s.ledPin); s.ledActiveLow = getB(o, "ledActiveLow", s.ledActiveLow);
  s.engineSensorId = getS(o, "engineSensorId", s.engineSensorId);
  s.engineRunningAbove = getF(o, "engineRunningAbove", s.engineRunningAbove);
  s.engineDelaySeconds = constrain(getI(o, "engineDelaySeconds", s.engineDelaySeconds), 0, 600);
  s.seaLevelHpa = getF(o, "seaLevelHpa", s.seaLevelHpa);
  if (isnan(s.seaLevelHpa) || s.seaLevelHpa < 800 || s.seaLevelHpa > 1100) s.seaLevelHpa = 1013.25f;
  s.historyIntervalSeconds = constrain(getI(o, "historyIntervalSeconds", s.historyIntervalSeconds), 1, 600);
  s.tzOffsetMinutes = constrain(getI(o, "tzOffsetMinutes", s.tzOffsetMinutes), -720, 840);

  if (s.apSsid.length() == 0 || s.apSsid.length() > 32) { err = "Access point name must be 1-32 characters"; return false; }
  if (s.apPass.length() < 8) { err = "Access point password must be at least 8 characters"; return false; }
  if (s.adminPass.length() < 4) { err = "Admin password must be at least 4 characters"; return false; }
  static const char* oleds[] = {"none", "ssd1306_128x64", "sh1106_128x64", "ssd1327_128x128", "ssd1327_ws_128x128"};
  bool ok = false; for (auto t : oleds) if (s.oledType == t) ok = true;
  if (!ok) { err = "Unknown OLED type"; return false; }
  return true;
}

bool configFromJson(JsonVariantConst v, Config& out, String& err) {
  Config tmp = out;
  if (!v["system"].isNull() && !systemFromJson(v["system"], tmp.sys, err)) return false;
  if (!v["sensors"].isNull() && !sensorsFromJson(v["sensors"], tmp.sensors, err)) return false;
  if (!v["relays"].isNull() && !relaysFromJson(v["relays"], tmp.relays, err)) return false;
  if (!validateConfig(tmp, err)) return false;
  out = tmp;
  return true;
}

// ---------------------------------------------------------------- validation
// ESP32 (WROOM/WROVER) GPIO facts:
//  6-11 = flash, 20/24/28-31 don't exist, 34-39 are input only,
//  0 and 12 are boot straps that break booting if driven wrong.
//  Only ADC1 (32-39) works while Wi-Fi is on.
static bool pinExists(int p) { return p >= 0 && p <= 39 && !(p >= 6 && p <= 11) && p != 20 && p != 24 && !(p >= 28 && p <= 31); }
static bool pinCanOutput(int p) { return pinExists(p) && p < 34 && p != 0 && p != 12; }
static bool pinIsAdc1(int p) { return p >= 32 && p <= 39; }

namespace {
struct PinBook {
  std::map<int, std::pair<String, String>> used;  // pin -> (owner, shareKey)
  String err;
  bool add(int pin, const String& owner, const String& shareKey = "") {
    if (pin < 0) return true;
    auto it = used.find(pin);
    if (it != used.end()) {
      if (shareKey.length() && it->second.second == shareKey) return true;
      err = "GPIO " + String(pin) + " is used by both " + it->second.first + " and " + owner;
      return false;
    }
    used[pin] = {owner, shareKey};
    return true;
  }
};
}

bool validateConfig(const Config& c, String& err) {
  PinBook pb;
  const SystemConfig& s = c.sys;
  auto needOut = [&](int pin, const String& what) -> bool {
    if (pin < 0) return true;
    if (!pinCanOutput(pin)) { err = what + ": GPIO " + String(pin) + " can't be used as an output (use 2-5, 13-19, 21-23, 25-27, 32-33)"; return false; }
    return true;
  };
  if (!needOut(s.i2cSda, "I2C SDA") || !needOut(s.i2cScl, "I2C SCL") || !needOut(s.oneWirePin, "1-Wire") ||
      !needOut(s.buzzerPin, "Buzzer") || !needOut(s.ledPin, "Warning LED")) return false;
  if (s.i2cSda < 0 || s.i2cScl < 0 || s.oneWirePin < 0) { err = "I2C and 1-Wire pins are required"; return false; }
  if (!pb.add(s.i2cSda, "I2C SDA") || !pb.add(s.i2cScl, "I2C SCL") || !pb.add(s.oneWirePin, "1-Wire bus") ||
      !pb.add(s.buzzerPin, "buzzer") || !pb.add(s.ledPin, "warning LED")) { err = pb.err; return false; }

  std::map<String, int> ids;
  for (auto& x : c.sensors) {
    if (ids.count(x.id)) { err = "Duplicate id '" + x.id + "'"; return false; }
    ids[x.id] = 1;
    if (!x.enabled) continue;
    String who = "sensor '" + x.id + "'";
    switch (x.type) {
      case SensorType::ADC:
        if (!pinIsAdc1(x.pin)) { err = "Sensor '" + x.id + "': analog pin must be GPIO 32-39 (ADC1; ADC2 stops working with Wi-Fi)"; return false; }
        if (!pb.add(x.pin, who)) { err = pb.err; return false; }
        break;
      case SensorType::MAX6675:
        if (!pinCanOutput(x.pin) || !pinCanOutput(x.sckPin) || !pinExists(x.soPin)) { err = "Sensor '" + x.id + "': set valid SCK, CS and SO pins"; return false; }
        if (!pb.add(x.pin, who + " CS") || !pb.add(x.sckPin, who + " SCK", "max6675_sck") || !pb.add(x.soPin, who + " SO", "max6675_so")) { err = pb.err; return false; }
        break;
      default: break;  // 1-Wire / I2C devices share the buses
    }
  }
  for (auto& r : c.relays) {
    if (ids.count(r.id)) { err = "Duplicate id '" + r.id + "'"; return false; }
    ids[r.id] = 1;
    if (!r.enabled) continue;
    if (!needOut(r.pin, "Relay '" + r.id + "'")) return false;
    if (!pb.add(r.pin, "relay '" + r.id + "'")) { err = pb.err; return false; }
  }
  if (s.engineSensorId.length()) {
    bool found = false;
    for (auto& x : c.sensors) if (x.id == s.engineSensorId) found = true;
    if (!found) { err = "Engine-running sensor '" + s.engineSensorId + "' does not exist"; return false; }
  }
  return true;
}

// ---------------------------------------------------------------- persistence (NVS)
// NVS writes are atomic: a power cut mid-save leaves the previous value intact.
static const char* NVS_NS = "carmetrics";
static const char* NVS_PART = "cfg";   // dedicated partition, see partitions.csv

static bool initCfgPartition() {
  static bool done = false;
  if (done) return true;
  esp_err_t e = nvs_flash_init_partition(NVS_PART);
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[cfg] settings partition needs erasing");
    nvs_flash_erase_partition(NVS_PART);
    e = nvs_flash_init_partition(NVS_PART);
  }
  if (e != ESP_OK) { Serial.printf("[cfg] settings partition unavailable (%s) - check partitions.csv\n", esp_err_to_name(e)); return false; }
  done = true;
  return true;
}

bool loadConfig(Config& c) {
  configDefaults(c);
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, true, NVS_PART)) { Serial.println("[cfg] no saved config, using defaults"); return false; }
  size_t len = prefs.getBytesLength("cfg");
  if (len == 0) { prefs.end(); Serial.println("[cfg] no saved config, using defaults"); return false; }
  std::unique_ptr<char[]> buf(new char[len + 1]);
  prefs.getBytes("cfg", buf.get(), len);
  buf[len] = 0;
  prefs.end();

  JsonDocument doc;
  if (deserializeJson(doc, buf.get(), len)) { Serial.println("[cfg] saved config corrupt, using defaults"); return false; }
  // Start from defaults for the system block so new settings get sane values,
  // but take sensor/relay lists exactly as saved.
  Config tmp; configDefaults(tmp);
  tmp.sensors.clear(); tmp.relays.clear();
  String err;
  if (!configFromJson(doc.as<JsonVariantConst>(), tmp, err)) {
    Serial.printf("[cfg] saved config invalid (%s), using defaults\n", err.c_str());
    return false;
  }
  c = tmp;
  Serial.printf("[cfg] loaded: %u sensors, %u relays\n", (unsigned)c.sensors.size(), (unsigned)c.relays.size());
  return true;
}

bool saveConfig(const Config& c) {
  JsonDocument doc;
  configToJson(c, doc.to<JsonObject>(), true);
  String out;
  serializeJson(doc, out);
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, false, NVS_PART)) { Serial.println("[cfg] NVS open failed"); return false; }
  size_t n = prefs.putBytes("cfg", out.c_str(), out.length());
  prefs.end();
  bool ok = n == out.length();
  Serial.printf("[cfg] save %s (%u bytes)\n", ok ? "ok" : "FAILED", (unsigned)out.length());
  return ok;
}
