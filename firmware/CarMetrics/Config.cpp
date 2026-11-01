#include "Config.h"
#include "Board.h"
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
    case SensorType::RPM:     return "rpm";
    case SensorType::SWITCH:  return "switch";
    case SensorType::GPS:     return "gps";
    case SensorType::INA226:  return "ina226";
    case SensorType::IMU:     return "imu";
    case SensorType::COMPUTED: return "computed";
    case SensorType::REMOTE:   return "remote";
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
  if (s == "rpm")     return SensorType::RPM;
  if (s == "switch")  return SensorType::SWITCH;
  if (s == "gps")     return SensorType::GPS;
  if (s == "ina226")  return SensorType::INA226;
  if (s == "imu")     return SensorType::IMU;
  if (s == "computed") return SensorType::COMPUTED;
  if (s == "remote")   return SensorType::REMOTE;
  return SensorType::UNKNOWN;
}
const char* bmpFieldToStr(BmpField f) {
  switch (f) {
    case BmpField::PRESSURE: return "pressure";
    case BmpField::ALTITUDE: return "altitude";
    case BmpField::HUMIDITY: return "humidity";
    default:                 return "temperature";
  }
}
BmpField bmpFieldFromStr(const String& s) {
  if (s == "pressure") return BmpField::PRESSURE;
  if (s == "altitude") return BmpField::ALTITUDE;
  if (s == "humidity") return BmpField::HUMIDITY;
  return BmpField::TEMPERATURE;
}
const char* oledStyleToStr(OledStyle s) {
  switch (s) {
    case OledStyle::TEXT: return "text";
    case OledStyle::BAR:  return "bar";
    case OledStyle::MARKER: return "marker";
    default:              return "off";
  }
}
OledStyle oledStyleFromStr(const String& s) {
  if (s == "text") return OledStyle::TEXT;
  if (s == "bar")  return OledStyle::BAR;
  if (s == "marker") return OledStyle::MARKER;
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

void defaultSensors(std::vector<SensorConfig>& v, const BoardProfile& b) {
  const BoardPins& P = b.pins;
  v.clear();
  // 1-Wire temperatures - addresses are assigned from the Settings page after a bus scan
  // OLED labels (3 characters max): CLT coolant, IT intake, GBT gearbox, TCT transfer case, CT cabin
  v.push_back(ds("coolant_temp", "Coolant", "CLT", OledStyle::MARKER, 15, 120, 100, 105));
  v.push_back(ds("intake_temp", "Intake air", "IT", OledStyle::TEXT, 0, 80, 55, 65));
  v.push_back(ds("gearbox_temp", "Gearbox", "GBT", OledStyle::TEXT, 0, 120, 90, 105));
  v.push_back(ds("transfer_case_temp", "Transfer case", "TCT", OledStyle::TEXT, 0, 120, 90, 105));

  // EGT thermocouple (MAX6675; classic ESP32 DevKit: SCK=5, CS=17, SO=16)
  SensorConfig egt;
  egt.id = "egt"; egt.name = "EGT"; egt.unit = "°C"; egt.type = SensorType::MAX6675;
  egt.sckPin = P.egtSck; egt.pin = P.egtCs; egt.soPin = P.egtSo; egt.offset = 0;
  egt.faultAlarm = false;  // unplugged probe shows NOT FOUND without sounding the alarm
  egt.smoothing = 0.5f;
  egt.gaugeMin = 0; egt.gaugeMax = 800; egt.warnHigh = 600; egt.critHigh = 700; egt.hysteresis = 10;
  egt.oledStyle = OledStyle::MARKER; egt.oledLabel = "EGT";
  v.push_back(egt);

  // Pressure senders: 5 V, 0.5-4.5 V out, through a 10k/20k divider (x1.5)
  SensorConfig boost = analog("boost_pressure", "Boost", "psi", P.analog[0], 1.5f, 0.5f, 4.5f, 0, 30);
  boost.decimals = 1; boost.gaugeMin = 0; boost.gaugeMax = 25; boost.warnHigh = 17; boost.critHigh = 20;
  boost.hysteresis = 0.5f; boost.faultLowV = 0.35f; boost.faultHighV = 4.8f;
  boost.oledStyle = OledStyle::MARKER; boost.oledLabel = "BST";
  v.push_back(boost);

  SensorConfig oil = analog("oil_pressure", "Oil pressure", "psi", P.analog[1], 1.5f, 0.5f, 4.5f, 0, 100);
  oil.gaugeMin = 0; oil.gaugeMax = 100; oil.warnLow = 15; oil.critLow = 7; oil.warnHigh = 85;
  oil.hysteresis = 2; oil.faultLowV = 0.35f; oil.faultHighV = 4.8f; oil.engineRunningOnly = true;
  oil.oledStyle = OledStyle::MARKER; oil.oledLabel = "OIL";
  v.push_back(oil);

  // Battery voltage via B25 module (30k/7.5k = x5)
  SensorConfig bat = analog("bat_voltage", "Starter battery", "V", P.analog[2], 5.0f, 0, 1, 0, 1);
  bat.decimals = 1; bat.gaugeMin = 8; bat.gaugeMax = 16;
  bat.warnLow = 11.8f; bat.critLow = 11.2f; bat.warnHigh = 14.8f; bat.critHigh = 15.2f; bat.hysteresis = 0.2f;
  bat.oledStyle = OledStyle::TEXT; bat.oledLabel = "B";
  v.push_back(bat);

  SensorConfig aux = analog("aux_voltage", "Aux battery", "V", P.analog[3], 5.0f, 0, 1, 0, 1);
  aux.enabled = false; aux.decimals = 1; aux.gaugeMin = 8; aux.gaugeMax = 16;
  aux.warnLow = 11.8f; aux.critLow = 11.2f; aux.hysteresis = 0.2f;
  aux.oledStyle = OledStyle::TEXT; aux.oledLabel = "A";
  v.push_back(aux);

  // Cabin BMP180/BMP280 (auto-detected on I2C)
  SensorConfig cab;
  cab.id = "cabin_temp"; cab.name = "Cabin"; cab.unit = "°C"; cab.type = SensorType::BMP;
  cab.bmpField = BmpField::TEMPERATURE; cab.gaugeMin = 0; cab.gaugeMax = 50; cab.warnHigh = 40;
  cab.oledStyle = OledStyle::TEXT; cab.oledLabel = "CT";
  v.push_back(cab);

  SensorConfig alt;
  alt.id = "altitude"; alt.name = "Altitude"; alt.unit = "m"; alt.type = SensorType::BMP;
  alt.bmpField = BmpField::ALTITUDE; alt.gaugeMin = 0; alt.gaugeMax = 3000; alt.smoothing = 0.2f;
  alt.oledStyle = OledStyle::TEXT; alt.oledLabel = "^";
  v.push_back(alt);
}

void defaultRelays(std::vector<RelayConfig>& v, const BoardProfile& b) {
  v.clear();
  // Disabled until you confirm wiring and whether your relay board is active-low.
  RelayConfig r1; r1.id = "relay1"; r1.name = "Relay 1"; r1.pin = b.pins.relay[0]; r1.enabled = false; r1.activeLow = true;
  RelayConfig r2; r2.id = "relay2"; r2.name = "Relay 2"; r2.pin = b.pins.relay[1]; r2.enabled = false; r2.activeLow = true;
  v.push_back(r1); v.push_back(r2);
}

void configDefaults(Config& c) { configDefaults(c, Board::detected()); }

static void sysPins(SystemConfig& s, const BoardProfile& b) {
  s.i2cSda = b.pins.sda; s.i2cScl = b.pins.scl; s.oneWirePin = b.pins.oneWire;
  s.buzzerPin = b.pins.buzzer; s.ledPin = b.pins.led;
}

void configDefaults(Config& c, const BoardProfile& b) {
  c.sys = SystemConfig();
  sysPins(c.sys, b);
  c.weather = WeatherConfig();
  WeatherTown nbo; nbo.name = "Nairobi"; nbo.lat = -1.2864f; nbo.lon = 36.8172f;
  c.weather.towns.push_back(nbo);
  c.cloud = CloudConfig();
  defaultSensors(c.sensors, b);
  defaultRelays(c.relays, b);
}

// Re-map pins after switching board: system pins and the default sensors/relays
// (matched by id) move to the new board's defaults; user-added items keep theirs.
void applyBoardPins(Config& c, const BoardProfile& b) {
  sysPins(c.sys, b);
  std::vector<SensorConfig> ds; defaultSensors(ds, b);
  for (auto& s : c.sensors)
    for (auto& d : ds)
      if (d.id == s.id && d.type == s.type) { s.pin = d.pin; s.sckPin = d.sckPin; s.soPin = d.soPin; }
  std::vector<RelayConfig> dr; defaultRelays(dr, b);
  for (auto& r : c.relays) for (auto& d : dr) if (d.id == r.id) r.pin = d.pin;
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
  if (s.type == SensorType::REMOTE) { o["board"] = s.remoteBoard; o["remoteIndex"] = s.remoteIndex; o["remoteId"] = s.remoteId; }
  o["address"] = s.address; o["pin"] = s.pin; o["sckPin"] = s.sckPin; o["soPin"] = s.soPin;
  o["adsAddress"] = s.adsAddress; o["adsChannel"] = s.adsChannel; o["mcpAddress"] = s.mcpAddress;
  o["bmpField"] = bmpFieldToStr(s.bmpField);
  o["i2cAddr"] = s.i2cAddr; o["field"] = s.field; putF(o, "pulsesPerRev", s.pulsesPerRev);
  o["activeLow"] = s.activeLow; putF(o, "shuntOhms", s.shuntOhms);
  o["formula"] = s.formula; o["srcA"] = s.srcA; o["srcB"] = s.srcB; o["curve"] = s.curve;
  putF(o, "divider", s.divider); putF(o, "inMin", s.inMin); putF(o, "inMax", s.inMax);
  putF(o, "outMin", s.outMin); putF(o, "outMax", s.outMax);
  putF(o, "faultLowV", s.faultLowV); putF(o, "faultHighV", s.faultHighV);
  putF(o, "offset", s.offset); putF(o, "smoothing", s.smoothing);
  o["decimals"] = s.decimals; putF(o, "gaugeMin", s.gaugeMin); putF(o, "gaugeMax", s.gaugeMax);
  o["oledStyle"] = oledStyleToStr(s.oledStyle); o["oledLabel"] = s.oledLabel;
  putF(o, "warnHigh", s.warnHigh); putF(o, "critHigh", s.critHigh);
  putF(o, "warnLow", s.warnLow); putF(o, "critLow", s.critLow);
  putF(o, "hysteresis", s.hysteresis); o["engineRunningOnly"] = s.engineRunningOnly; o["faultAlarm"] = s.faultAlarm;
}

void relayToJson(const RelayConfig& r, JsonObject o) {
  o["id"] = r.id; o["name"] = r.name; o["pin"] = r.pin;
  o["mode"] = r.mode == RelayMode::BLINK ? "blink" : "on_off";
  o["activeLow"] = r.activeLow; o["defaultOn"] = r.defaultOn;
  o["blinkMs"] = r.blinkMs; o["enabled"] = r.enabled;
  o["safeState"] = r.safeState == 1 ? "on" : r.safeState == 2 ? "hold" : "off";
}

void systemToJson(const SystemConfig& s, JsonObject o, bool includeSecrets) {
  o["board"] = s.board; o["role"] = s.role; o["boardEncrypt"] = s.boardEncrypt;
  o["canEnabled"] = s.canEnabled; o["canTx"] = s.canTx; o["canRx"] = s.canRx; o["canKbps"] = s.canKbps;
  o["apSsid"] = s.apSsid; o["apPass"] = includeSecrets ? s.apPass : "";
  JsonArray nets = o["networks"].to<JsonArray>();
  for (auto& n : s.networks) {
    JsonObject x = nets.add<JsonObject>();
    x["ssid"] = n.ssid; x["pass"] = includeSecrets ? n.pass : ""; x["passSet"] = n.pass.length() > 0; x["priority"] = n.priority;
  }
  o["scanWhileApInUse"] = s.scanWhileApInUse;
  o["ignitionPin"] = s.ignitionPin; o["ignitionActiveLow"] = s.ignitionActiveLow; o["sleepAfterMinutes"] = s.sleepAfterMinutes;
  o["gpsRxPin"] = s.gpsRxPin; o["gpsTxPin"] = s.gpsTxPin; o["gpsBaud"] = s.gpsBaud;
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
  o["schema"] = CONFIG_SCHEMA;
  systemToJson(c.sys, o["system"].to<JsonObject>(), includeSecrets);
  JsonArray sa = o["sensors"].to<JsonArray>();
  for (auto& s : c.sensors) sensorToJson(s, sa.add<JsonObject>());
  JsonArray ra = o["relays"].to<JsonArray>();
  for (auto& r : c.relays) relayToJson(r, ra.add<JsonObject>());
  weatherToJson(c.weather, o["weather"].to<JsonObject>());
  turboToJson(c.turbo, o["turbo"].to<JsonObject>());
  layoutToJson(c.layout, o["layout"].to<JsonObject>());
  JsonObject bb = o["blackbox"].to<JsonObject>(); bb["enabled"] = c.blackbox.enabled; bb["keepEvents"] = c.blackbox.keepEvents;
  cloudToJson(c.cloud, o["cloud"].to<JsonObject>(), includeSecrets);
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
  s.i2cAddr = getI(o, "i2cAddr", 0); s.field = getS(o, "field", "");
  s.pulsesPerRev = getF(o, "pulsesPerRev", 10); s.activeLow = getB(o, "activeLow", true);
  s.shuntOhms = getF(o, "shuntOhms", 0.001f);
  s.formula = getS(o, "formula", ""); s.srcA = getS(o, "srcA", ""); s.srcB = getS(o, "srcB", "");
  s.curve = getS(o, "curve", ""); s.curve.trim();
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
  s.faultAlarm = getB(o, "faultAlarm", true);

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
  auto oneOf = [&](const char* const* list, size_t n) { for (size_t k = 0; k < n; k++) if (s.field == list[k]) return true; return false; };
  if (s.type == SensorType::GPS) {
    static const char* const f[] = {"speed", "altitude", "sats", "heading", "hdop"};
    if (!oneOf(f, 5)) { err = "Sensor '" + s.id + "': GPS field must be speed, altitude, sats, heading or hdop"; return false; }
  }
  if (s.type == SensorType::INA226) {
    static const char* const f[] = {"current", "voltage", "power"};
    if (!oneOf(f, 3)) { err = "Sensor '" + s.id + "': INA226 field must be current, voltage or power"; return false; }
    if (!s.i2cAddr) s.i2cAddr = 0x40;
    if (s.i2cAddr < 0x40 || s.i2cAddr > 0x4F) { err = "Sensor '" + s.id + "': INA226 address must be 0x40-0x4F"; return false; }
    if (isnan(s.shuntOhms) || s.shuntOhms <= 0) { err = "Sensor '" + s.id + "': shunt resistance must be > 0"; return false; }
  }
  if (s.type == SensorType::IMU) {
    if (s.field != "roll" && s.field != "pitch") { err = "Sensor '" + s.id + "': IMU field must be roll or pitch"; return false; }
    if (!s.i2cAddr) s.i2cAddr = 0x68;
    if (s.i2cAddr != 0x68 && s.i2cAddr != 0x69) { err = "Sensor '" + s.id + "': IMU address must be 0x68 or 0x69"; return false; }
  }
  if (s.type == SensorType::RPM && (isnan(s.pulsesPerRev) || s.pulsesPerRev <= 0)) { err = "Sensor '" + s.id + "': pulses per revolution must be > 0"; return false; }
  if (s.type == SensorType::REMOTE) {
    s.remoteBoard = getI(o, "board", 0); s.remoteIndex = getI(o, "remoteIndex", 0); s.remoteId = getS(o, "remoteId", "");
    if (s.remoteBoard < 1 || s.remoteBoard > 14) { err = "Sensor '" + s.id + "': remote board must be 1-14"; return false; }
  }
  if (s.type == SensorType::COMPUTED) {
    static const char* const f[] = {"diff", "sum", "rate", "time_in_warn", "curve_margin"};
    bool ok = false; for (auto x : f) if (s.formula == x) ok = true;
    if (!ok) { err = "Sensor '" + s.id + "': formula must be diff, sum, rate, time_in_warn or curve_margin"; return false; }
  }
  if (s.curve.length()) {
    std::vector<std::pair<float, float>> c;
    if (!parseCurve(s.curve, c)) { err = "Sensor '" + s.id + "': curve must be 2-16 'x:y' pairs with rising x, e.g. 0.5:0,2.5:40,4.5:100"; return false; }
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
    String safe = getS(o, "safeState", "off");
    r.safeState = safe == "on" ? 1 : safe == "hold" ? 2 : 0;
    tmp.push_back(r);
  }
  out = tmp;
  return true;
}

bool systemFromJson(JsonVariantConst v, SystemConfig& s, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected a system settings object"; return false; }
  JsonObjectConst o = v.as<JsonObjectConst>();
  s.board = getS(o, "board", s.board); s.board.trim();
  if (s.board != "auto" && !Board::find(s.board)) { err = "Unknown board '" + s.board + "' for this firmware (" + Board::chip() + ")"; return false; }
  s.role = getS(o, "role", s.role); s.role.trim();
  s.boardEncrypt = getB(o, "boardEncrypt", s.boardEncrypt);
  s.canEnabled = getB(o, "canEnabled", s.canEnabled); s.canTx = getI(o, "canTx", s.canTx); s.canRx = getI(o, "canRx", s.canRx);
  s.canKbps = getI(o, "canKbps", s.canKbps);
  if (s.canKbps != 125 && s.canKbps != 250 && s.canKbps != 500 && s.canKbps != 1000) { err = "CAN speed must be 125, 250, 500 or 1000 kbit/s"; return false; }
  if (s.role != "main" && s.role != "sensor" && s.role != "relay" && s.role != "sensor_relay") { err = "Unknown board role '" + s.role + "'"; return false; }
  s.apSsid = getS(o, "apSsid", s.apSsid);
  String p = getS(o, "apPass", ""); if (p.length()) s.apPass = p;
  if (o["networks"].is<JsonArrayConst>()) {
    std::vector<WifiNetwork> nets;
    for (JsonObjectConst x : o["networks"].as<JsonArrayConst>()) {
      WifiNetwork n; n.ssid = getS(x, "ssid", ""); n.pass = getS(x, "pass", "");
      n.priority = constrain(getI(x, "priority", 5), 1, 9);
      if (!n.ssid.length()) continue;
      if (n.ssid.length() > 32) { err = "Wi-Fi name '" + n.ssid + "' is longer than 32 characters"; return false; }
      if (!n.pass.length()) for (auto& old : s.networks) if (old.ssid == n.ssid) n.pass = old.pass;   // blank = keep
      if (n.pass.length() && n.pass.length() < 8) { err = "Password for '" + n.ssid + "' must be at least 8 characters (or empty for an open network)"; return false; }
      nets.push_back(n);
    }
    if (nets.size() > MAX_WIFI_NETWORKS) { err = "At most " + String(MAX_WIFI_NETWORKS) + " Wi-Fi networks"; return false; }
    s.networks = nets;
  }
  // v4 single network ("Also join network") becomes the first saved network
  String legacy = getS(o, "staSsid", "");
  if (legacy.length()) {
    bool have = false; for (auto& n : s.networks) if (n.ssid == legacy) have = true;
    if (!have && s.networks.size() < MAX_WIFI_NETWORKS) { WifiNetwork n; n.ssid = legacy; n.pass = getS(o, "staPass", ""); s.networks.push_back(n); }
  }
  s.staSsid = ""; s.staPass = "";
  s.scanWhileApInUse = getB(o, "scanWhileApInUse", s.scanWhileApInUse);
  s.ignitionPin = getI(o, "ignitionPin", s.ignitionPin); s.ignitionActiveLow = getB(o, "ignitionActiveLow", s.ignitionActiveLow);
  s.sleepAfterMinutes = constrain(getI(o, "sleepAfterMinutes", s.sleepAfterMinutes), 0, 1440);
  s.gpsRxPin = getI(o, "gpsRxPin", s.gpsRxPin); s.gpsTxPin = getI(o, "gpsTxPin", s.gpsTxPin);
  s.gpsBaud = getI(o, "gpsBaud", s.gpsBaud);
  if (s.gpsBaud != 4800 && s.gpsBaud != 9600 && s.gpsBaud != 19200 && s.gpsBaud != 38400 && s.gpsBaud != 57600 && s.gpsBaud != 115200) s.gpsBaud = 9600;
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

// ---------------------------------------------------------------- curves
bool parseCurve(const String& str, std::vector<std::pair<float, float>>& out) {
  out.clear();
  int start = 0;
  while (start < (int)str.length()) {
    int comma = str.indexOf(',', start);
    String part = str.substring(start, comma < 0 ? str.length() : comma);
    part.trim();
    int colon = part.indexOf(':');
    if (colon <= 0) return false;
    char* e1; char* e2;
    String xs = part.substring(0, colon), ys = part.substring(colon + 1);
    float x = strtof(xs.c_str(), &e1), y = strtof(ys.c_str(), &e2);
    if (*e1 || *e2 || isnan(x) || isnan(y)) return false;
    if (!out.empty() && x <= out.back().first) return false;
    out.push_back({x, y});
    if (comma < 0) break;
    start = comma + 1;
  }
  return out.size() >= 2 && out.size() <= 16;
}

float curveLookup(const std::vector<std::pair<float, float>>& c, float x) {
  if (c.empty() || isnan(x)) return NAN;
  if (x <= c.front().first) return c.front().second;
  for (size_t i = 1; i < c.size(); i++)
    if (x <= c[i].first) return c[i - 1].second + (x - c[i - 1].first) * (c[i].second - c[i - 1].second) / (c[i].first - c[i - 1].first);
  return c.back().second;
}

// ---------------------------------------------------------------- turbo / layout
void turboToJson(const TurboConfig& t, JsonObject o) {
  o["enabled"] = t.enabled; o["egtId"] = t.egtId; putF(o, "safeEgt", t.safeEgt); o["minIdleSeconds"] = t.minIdleSeconds;
  o["rpmId"] = t.rpmId; putF(o, "idleRpm", t.idleRpm); o["speedId"] = t.speedId; o["beep"] = t.beep;
}
bool turboFromJson(JsonVariantConst v, TurboConfig& t, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected turbo timer settings"; return false; }
  JsonObjectConst o = v.as<JsonObjectConst>();
  t.enabled = getB(o, "enabled", t.enabled); t.egtId = getS(o, "egtId", t.egtId);
  t.safeEgt = getF(o, "safeEgt", t.safeEgt); t.minIdleSeconds = constrain(getI(o, "minIdleSeconds", t.minIdleSeconds), 0, 900);
  t.rpmId = getS(o, "rpmId", t.rpmId); t.idleRpm = getF(o, "idleRpm", t.idleRpm);
  t.speedId = getS(o, "speedId", t.speedId); t.beep = getB(o, "beep", t.beep);
  if (isnan(t.safeEgt) || t.safeEgt < 50 || t.safeEgt > 600) { err = "Safe EGT must be 50-600 °C"; return false; }
  if (isnan(t.idleRpm)) t.idleRpm = 1000;
  return true;
}
void layoutToJson(const LayoutConfig& l, JsonObject o) {
  JsonArray t = o["tiles"].to<JsonArray>();
  for (auto& x : l.tiles) { JsonObject j = t.add<JsonObject>(); j["id"] = x.id; j["size"] = x.size; j["widget"] = x.widget; }
  JsonArray d = o["oled"].to<JsonArray>();
  for (auto& x : l.oled) d.add(x);
  o["oledColumns"] = l.oledColumns;
}
bool layoutFromJson(JsonVariantConst v, LayoutConfig& l, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected a layout"; return false; }
  LayoutConfig n;
  for (JsonObjectConst x : v["tiles"].as<JsonArrayConst>()) {
    LayoutTile t; t.id = getS(x, "id", ""); t.size = constrain(getI(x, "size", 1), 1, 2); t.widget = getS(x, "widget", "bar");
    if (t.widget != "number" && t.widget != "bar" && t.widget != "spark") t.widget = "bar";
    if (t.id.length()) n.tiles.push_back(t);
  }
  for (JsonVariantConst x : v["oled"].as<JsonArrayConst>()) { String id = x.as<const char*>() ? x.as<const char*>() : ""; if (id.length()) n.oled.push_back(id); }
  if (n.tiles.size() > 64 || n.oled.size() > 64) { err = "Layout too long"; return false; }
  n.oledColumns = constrain(getI(v.as<JsonObjectConst>(), "oledColumns", 0), 0, 4);
  if (n.oledColumns == 1) n.oledColumns = 2;
  l = n;
  return true;
}

// ---------------------------------------------------------------- weather / cloud
void weatherToJson(const WeatherConfig& w, JsonObject o) {
  o["enabled"] = w.enabled; o["refTown"] = w.refTown; o["autoSeaLevel"] = w.autoSeaLevel;
  o["refreshMinutes"] = w.refreshMinutes;
  JsonArray a = o["towns"].to<JsonArray>();
  for (auto& t : w.towns) { JsonObject x = a.add<JsonObject>(); x["name"] = t.name; x["lat"] = t.lat; x["lon"] = t.lon; }
}

bool weatherFromJson(JsonVariantConst v, WeatherConfig& w, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected weather settings"; return false; }
  JsonObjectConst o = v.as<JsonObjectConst>();
  w.enabled = getB(o, "enabled", w.enabled);
  w.refTown = getS(o, "refTown", w.refTown);
  w.autoSeaLevel = getB(o, "autoSeaLevel", w.autoSeaLevel);
  w.refreshMinutes = constrain(getI(o, "refreshMinutes", w.refreshMinutes), 10, 180);
  if (!o["towns"].isNull()) {
    if (!o["towns"].is<JsonArrayConst>()) { err = "towns must be a list"; return false; }
    std::vector<WeatherTown> t;
    for (JsonObjectConst x : o["towns"].as<JsonArrayConst>()) {
      WeatherTown wt; wt.name = getS(x, "name", ""); wt.lat = getF(x, "lat", NAN); wt.lon = getF(x, "lon", NAN);
      if (!wt.name.length() || isnan(wt.lat) || isnan(wt.lon) || fabsf(wt.lat) > 90 || fabsf(wt.lon) > 180) { err = "Bad town entry"; return false; }
      t.push_back(wt);
    }
    if (t.size() > 12) { err = "At most 12 weather towns"; return false; }
    w.towns = t;
  }
  return true;
}

void cloudToJson(const CloudConfig& c, JsonObject o, bool includeSecrets) {
  o["enabled"] = c.enabled; o["protocol"] = c.protocol; o["intervalMs"] = c.intervalMs;
  o["uploadEverySec"] = c.uploadEverySec; o["bufferKB"] = c.bufferKB; o["onlyEngineRunning"] = c.onlyEngineRunning;
  o["sendSystem"] = c.sendSystem;
  o["zbxServer"] = c.zbxServer; o["zbxPort"] = c.zbxPort; o["zbxHost"] = c.zbxHost; o["zbxPacked"] = c.zbxPacked;
  o["zbxPskIdentity"] = c.zbxPskIdentity; o["zbxPsk"] = includeSecrets ? c.zbxPsk : "";
  o["zbxPskSet"] = c.zbxPsk.length() > 0;
  o["influxUrl"] = c.influxUrl; o["influxAuth"] = includeSecrets ? c.influxAuth : "";
  o["influxAuthSet"] = c.influxAuth.length() > 0; o["influxMeasurement"] = c.influxMeasurement;
}

bool cloudFromJson(JsonVariantConst v, CloudConfig& c, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "Expected cloud settings"; return false; }
  JsonObjectConst o = v.as<JsonObjectConst>();
  c.enabled = getB(o, "enabled", c.enabled);
  c.protocol = getS(o, "protocol", c.protocol);
  c.intervalMs = getI(o, "intervalMs", c.intervalMs);
  c.uploadEverySec = constrain(getI(o, "uploadEverySec", c.uploadEverySec), 10, 3600);
  c.bufferKB = constrain(getI(o, "bufferKB", c.bufferKB), 16, CM_MAX_BUFFER_KB);
  c.onlyEngineRunning = getB(o, "onlyEngineRunning", c.onlyEngineRunning);
  c.sendSystem = getB(o, "sendSystem", c.sendSystem);
  c.zbxServer = getS(o, "zbxServer", c.zbxServer); c.zbxServer.trim();
  c.zbxPort = constrain(getI(o, "zbxPort", c.zbxPort), 1, 65535);
  c.zbxHost = getS(o, "zbxHost", c.zbxHost); c.zbxHost.trim();
  c.zbxPacked = getB(o, "zbxPacked", c.zbxPacked);
  c.zbxPskIdentity = getS(o, "zbxPskIdentity", c.zbxPskIdentity);
  String p = getS(o, "zbxPsk", ""); if (p.length()) c.zbxPsk = p;
  if (getB(o, "zbxPskClear", false)) { c.zbxPsk = ""; c.zbxPskIdentity = ""; }
  c.influxUrl = getS(o, "influxUrl", c.influxUrl); c.influxUrl.trim();
  p = getS(o, "influxAuth", ""); if (p.length()) c.influxAuth = p;
  if (getB(o, "influxAuthClear", false)) c.influxAuth = "";
  c.influxMeasurement = getS(o, "influxMeasurement", c.influxMeasurement);

  static const uint32_t ok[] = {250, 1000, 5000, 10000, 30000, 60000, 300000};
  bool valid = false; for (auto x : ok) if (c.intervalMs == x) valid = true;
  if (!valid) { err = "Sample interval must be 250 ms, 1 s, 5 s, 10 s, 30 s, 1 min or 5 min"; return false; }
  if (c.protocol != "zabbix" && c.protocol != "influx") { err = "Protocol must be zabbix or influx"; return false; }
  if (c.zbxPsk.length()) {
    if (c.zbxPsk.length() < 32 || c.zbxPsk.length() % 2) { err = "Zabbix PSK must be hex, at least 32 characters"; return false; }
    for (char ch : c.zbxPsk) if (!isxdigit(ch)) { err = "Zabbix PSK must be hex"; return false; }
    if (!c.zbxPskIdentity.length()) { err = "Zabbix PSK identity is required with a PSK"; return false; }
  }
  if (c.enabled && c.protocol == "zabbix" && (!c.zbxServer.length() || !c.zbxHost.length())) { err = "Set the Zabbix server and host name"; return false; }
  if (c.enabled && c.protocol == "influx" && !(c.influxUrl.startsWith("http://") || c.influxUrl.startsWith("https://"))) {
    err = "Influx URL must start with http:// or https://"; return false;
  }
  if (!c.influxMeasurement.length()) c.influxMeasurement = "carmetrics";
  return true;
}

bool configFromJson(JsonVariantConst v, Config& out, String& err) {
  Config tmp = out;
  if (!v["system"].isNull() && !systemFromJson(v["system"], tmp.sys, err)) return false;
  if (!v["sensors"].isNull() && !sensorsFromJson(v["sensors"], tmp.sensors, err)) return false;
  if (!v["relays"].isNull() && !relaysFromJson(v["relays"], tmp.relays, err)) return false;
  if (!v["weather"].isNull() && !weatherFromJson(v["weather"], tmp.weather, err)) return false;
  if (!v["cloud"].isNull() && !cloudFromJson(v["cloud"], tmp.cloud, err)) return false;
  if (!v["turbo"].isNull() && !turboFromJson(v["turbo"], tmp.turbo, err)) return false;
  if (!v["layout"].isNull() && !layoutFromJson(v["layout"], tmp.layout, err)) return false;
  if (v["blackbox"].is<JsonObjectConst>()) {
    tmp.blackbox.enabled = v["blackbox"]["enabled"] | tmp.blackbox.enabled;
    tmp.blackbox.keepEvents = constrain((int)(v["blackbox"]["keepEvents"] | (int)tmp.blackbox.keepEvents), 1, CM_MAX_BLACKBOX);
  }
  if (!validateConfig(tmp, err)) return false;
  out = tmp;
  return true;
}

// ---------------------------------------------------------------- validation
// Pin rules come from the board profile (Board.cpp): which GPIOs exist on the
// module, which may drive outputs, and which are ADC1 (works with Wi-Fi on).

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
  const BoardProfile& board = Board::resolve(c.sys.board);
  auto pinExists = [&](int p) { return Board::has(board.valid, p); };
  auto pinCanOutput = [&](int p) { return Board::has(board.output, p); };
  auto pinIsAdc1 = [&](int p) { return Board::has(board.adc1, p); };
  PinBook pb;
  const SystemConfig& s = c.sys;
  auto needOut = [&](int pin, const String& what) -> bool {
    if (pin < 0) return true;
    if (!pinCanOutput(pin)) { err = what + ": GPIO " + String(pin) + " can't be used as an output on " + String(board.name) + " (use " + Board::pinList(board.output) + ")"; return false; }
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
        if (!pinIsAdc1(x.pin)) { err = "Sensor '" + x.id + "': analog pin must be an ADC1 pin on this board (GPIO " + Board::pinList(board.adc1) + "; ADC2 stops working with Wi-Fi)"; return false; }
        if (!pb.add(x.pin, who)) { err = pb.err; return false; }
        break;
      case SensorType::MAX6675:
        if (!pinCanOutput(x.pin) || !pinCanOutput(x.sckPin) || !pinExists(x.soPin)) { err = "Sensor '" + x.id + "': set valid SCK, CS and SO pins"; return false; }
        if (!pb.add(x.pin, who + " CS") || !pb.add(x.sckPin, who + " SCK", "max6675_sck") || !pb.add(x.soPin, who + " SO", "max6675_so")) { err = pb.err; return false; }
        break;
      case SensorType::RPM:
        if (!pinExists(x.pin)) { err = "Sensor '" + x.id + "': choose the GPIO the RPM signal is wired to"; return false; }
        if (!pb.add(x.pin, who)) { err = pb.err; return false; }
        break;
      case SensorType::SWITCH:
        if (!pinExists(x.pin)) { err = "Sensor '" + x.id + "': choose the switch input GPIO"; return false; }
        if (!pb.add(x.pin, who)) { err = pb.err; return false; }
        break;
      default: break;  // 1-Wire / I2C devices share the buses
    }
  }
  // computed sensors must reference existing sensors (and not themselves)
  for (auto& x : c.sensors) {
    if (x.type != SensorType::COMPUTED || !x.enabled) continue;
    auto exists = [&](const String& id) { if (id == x.id) return false; for (auto& y : c.sensors) if (y.id == id) return true; return false; };
    bool needB = x.formula == "diff" || x.formula == "sum" || x.formula == "curve_margin";
    if (!exists(x.srcA) || (needB && !exists(x.srcB))) { err = "Sensor '" + x.id + "': pick the sensor(s) it is computed from"; return false; }
    if (x.formula == "curve_margin" && !x.curve.length()) { err = "Sensor '" + x.id + "': enter the curve (e.g. 800:10,2000:25,3500:40)"; return false; }
  }
  if (s.ignitionPin >= 0) {
    if (!pinExists(s.ignitionPin)) { err = "Ignition sense: GPIO " + String(s.ignitionPin) + " doesn't exist on this board"; return false; }
    if (s.sleepAfterMinutes && !Board::has(board.rtc, s.ignitionPin)) { err = "Ignition sense: to wake from sleep, use an RTC GPIO (" + Board::pinList(board.rtc) + ")"; return false; }
    if (!pb.add(s.ignitionPin, "ignition sense")) { err = pb.err; return false; }
  }
  if (s.gpsRxPin >= 0 || s.gpsTxPin >= 0) {
    if (!pinExists(s.gpsRxPin)) { err = "GPS: set the RX pin (wired to the GPS TX)"; return false; }
    if (s.gpsTxPin >= 0 && !pinCanOutput(s.gpsTxPin)) { err = "GPS: TX pin can't be an output"; return false; }
    if (!pb.add(s.gpsRxPin, "GPS RX") || !pb.add(s.gpsTxPin, "GPS TX")) { err = pb.err; return false; }
  }
  if (s.canEnabled) {
    if (!pinExists(s.canRx) || !pinCanOutput(s.canTx)) { err = "CAN: set the TX pin (an output) and the RX pin"; return false; }
    if (!pb.add(s.canTx, "CAN TX") || !pb.add(s.canRx, "CAN RX")) { err = pb.err; return false; }
  }
  if (c.turbo.enabled) {
    auto exists = [&](const String& id) { for (auto& y : c.sensors) if (y.id == id) return true; return false; };
    if (!exists(c.turbo.egtId)) { err = "Turbo timer: EGT sensor '" + c.turbo.egtId + "' does not exist"; return false; }
    if (c.turbo.rpmId.length() && !exists(c.turbo.rpmId)) { err = "Turbo timer: RPM sensor does not exist"; return false; }
    if (c.turbo.speedId.length() && !exists(c.turbo.speedId)) { err = "Turbo timer: speed sensor does not exist"; return false; }
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

static uint32_t sWrites = 0;
uint32_t configWriteCount() { return sWrites; }
static const char* slotKey(CfgSlot s) { return s == CfgSlot::PREVIOUS ? "prev" : s == CfgSlot::GOOD ? "good" : "cfg"; }

static String readSlot(Preferences& prefs, const char* key) {
  size_t len = prefs.getBytesLength(key);
  if (!len) return String();
  std::unique_ptr<char[]> buf(new char[len + 1]);
  prefs.getBytes(key, buf.get(), len);
  buf[len] = 0;
  return String(buf.get());
}

bool hasConfigSlot(CfgSlot slot) {
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, true, NVS_PART)) return false;
  bool has = prefs.getBytesLength(slotKey(slot)) > 0;
  prefs.end();
  return has;
}

bool loadConfig(Config& c, CfgSlot slot) {
  configDefaults(c);
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, true, NVS_PART)) { Serial.println("[cfg] no saved config, using defaults"); return false; }
  String json = readSlot(prefs, slotKey(slot));
  prefs.end();
  if (!json.length()) { Serial.printf("[cfg] no saved '%s' config, using defaults\n", slotKey(slot)); return false; }

  JsonDocument doc;
  if (deserializeJson(doc, json)) { Serial.println("[cfg] saved config corrupt, using defaults"); return false; }
  int schema = doc["schema"] | 1;   // v4 configs have no schema field: upgraded by filling new settings with defaults
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
  Serial.printf("[cfg] loaded '%s' (schema %d): %u sensors, %u relays\n", slotKey(slot), schema, (unsigned)c.sensors.size(), (unsigned)c.relays.size());
  return true;
}

static String configString(const Config& c) {
  JsonDocument doc;
  configToJson(c, doc.to<JsonObject>(), true);
  String out;
  serializeJson(doc, out);
  return out;
}

bool saveConfig(const Config& c) {
  String out = configString(c);
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, false, NVS_PART)) { Serial.println("[cfg] NVS open failed"); return false; }
  String old = readSlot(prefs, "cfg");
  if (old == out) { prefs.end(); return true; }          // unchanged: no flash write
  if (old.length()) { prefs.putBytes("prev", old.c_str(), old.length()); sWrites++; }
  size_t n = prefs.putBytes("cfg", out.c_str(), out.length());
  sWrites++;
  prefs.end();
  bool ok = n == out.length();
  Serial.printf("[cfg] save %s (%u bytes)\n", ok ? "ok" : "FAILED", (unsigned)out.length());
  return ok;
}

bool markConfigGood() {
  Preferences prefs;
  if (!initCfgPartition() || !prefs.begin(NVS_NS, false, NVS_PART)) return false;
  String cur = readSlot(prefs, "cfg");
  bool changed = cur.length() && cur != readSlot(prefs, "good");
  if (changed) { prefs.putBytes("good", cur.c_str(), cur.length()); sWrites++; Serial.println("[cfg] current config marked good"); }
  prefs.end();
  return changed;
}
