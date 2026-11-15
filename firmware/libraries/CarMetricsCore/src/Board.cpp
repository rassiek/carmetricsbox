#include "Board.h"
#include "State.h"

static constexpr uint64_t range(int a, int b) { return b < a ? 0 : (CM_BIT(b) | range(a, b - 1)); }

#if CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3 (WROOM-1/-2 modules). GPIO 26-32 = flash; 35-37 = octal PSRAM on R8 parts.
static constexpr uint64_t S3_ALL = range(0, 21) | range(35, 48);
static constexpr uint64_t S3_OCTAL = S3_ALL & ~range(35, 37);
static constexpr uint64_t S3_ADC1 = range(1, 10);
// 0/3/45/46 boot straps, 19/20 native USB, 43/44 console UART, 38/48 RGB LED on DevKitC
static constexpr uint64_t S3_CAUTION = CM_BIT(0) | CM_BIT(3) | CM_BIT(45) | CM_BIT(46) | CM_BIT(19) | CM_BIT(20) |
                                       CM_BIT(43) | CM_BIT(44) | CM_BIT(38) | CM_BIT(48);
static const BoardPins S3_PINS = {8, 9, 15, 16, 17, {4, 5, 6, 7}, 12, 13, 14, {18, 21}};
static const BoardFixed S3_FIXED = {48, 0, {}, false, 0, {}, false};
static const BoardProfile kProfiles[] = {
  {"esp32s3_n8r8", "ESP32-S3 DevKitC-1 N8R8 (8 MB flash, 8 MB PSRAM)", "esp32s3", 8, 8,
   S3_OCTAL, S3_OCTAL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "Recommended main board. GPIO35-37 are used by the PSRAM. Analog inputs: GPIO1-10. Built-in RGB LED on GPIO48 (GPIO38 on DevKitC v1.1: set it under Hardware).", S3_FIXED},
  {"esp32s3_n16r8", "ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM)", "esp32s3", 16, 8,
   S3_OCTAL, S3_OCTAL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "Same pins as N8R8. The extra flash is kept for a bigger offline buffer in a later release.", S3_FIXED},
  {"esp32s3_quad", "ESP32-S3 with quad PSRAM or none (N4, N8, N8R2)", "esp32s3", 4, 0,
   S3_ALL, S3_ALL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "GPIO35-37 are free on these modules.", S3_FIXED},
};
#elif CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 (single core, RISC-V). 11-17 = flash (12-17 on most modules, 11 is VDD_SPI),
// 18/19 = native USB, 20/21 = console UART, ADC1 = 0-4 (ADC2 is not usable with Wi-Fi).
static constexpr uint64_t C3_ALL = range(0, 10) | range(18, 21);
static constexpr uint64_t C3_CAUTION = CM_BIT(2) | CM_BIT(8) | CM_BIT(9) | CM_BIT(18) | CM_BIT(19) | CM_BIT(20) | CM_BIT(21);
static const BoardPins C3_PINS = {6, 7, 10, 5, -1, {0, 1, 3, 4}, -1, -1, -1, {-1, -1}};
static const BoardFixed C3_FIXED = {-1, 0, {}, false, 0, {}, false};
static const BoardFixed C3M_FIXED = {8, 0, {}, false, 0, {}, false};
static const BoardProfile kProfiles[] = {
  {"esp32c3", "ESP32-C3 (SuperMini, DevKit, 4 MB)", "esp32c3", 4, 0,
   C3_ALL, C3_ALL, range(0, 4), C3_CAUTION, range(0, 5), C3_PINS,
   "Small single-core board. Analog inputs: GPIO0-4. GPIO 2, 8, 9 are boot pins (the SuperMini's LED is on GPIO8, lit on LOW).", C3_FIXED},
  {"esp32c3_devkitm", "ESP32-C3-DevKitM-1 (RGB LED on GPIO8)", "esp32c3", 4, 0,
   C3_ALL, C3_ALL, range(0, 4), C3_CAUTION, range(0, 5), C3_PINS,
   "Espressif's C3 board with an addressable RGB LED on GPIO8 (used as the status light).", C3M_FIXED},
};
#elif CONFIG_IDF_TARGET_ESP32C6
// ESP32-C6 (single core, RISC-V, Wi-Fi 6). 24-30 = flash, 12/13 = native USB,
// 16/17 = console UART, ADC1 = 0-6.
static constexpr uint64_t C6_ALL = range(0, 23);
static constexpr uint64_t C6_CAUTION = CM_BIT(4) | CM_BIT(5) | CM_BIT(8) | CM_BIT(9) | CM_BIT(15) | CM_BIT(12) | CM_BIT(13) | CM_BIT(16) | CM_BIT(17);
static const BoardPins C6_PINS = {22, 23, 10, 11, -1, {0, 1, 2, 3}, -1, -1, -1, {-1, -1}};
static const BoardFixed C6_FIXED = {8, 0, {}, false, 0, {}, false};
static const BoardProfile kProfiles[] = {
  {"esp32c6", "ESP32-C6 (DevKitC-1 and similar, 4 MB+)", "esp32c6", 4, 0,
   C6_ALL, C6_ALL, range(0, 6), C6_CAUTION, range(0, 7), C6_PINS,
   "Small single-core board. Analog inputs: GPIO0-6. Addressable RGB LED on GPIO8 on the DevKitC-1 (boot pin).", C6_FIXED},
};
#else
// Classic ESP32 (WROOM-32 / WROVER). 6-11 = flash, 34-39 input only, ADC1 = 32-39.
static constexpr uint64_t E_ALL = range(0, 39) & ~range(6, 11) & ~CM_BIT(20) & ~CM_BIT(24) & ~range(28, 31);
static constexpr uint64_t E_OUT = E_ALL & ~range(34, 39) & ~CM_BIT(0) & ~CM_BIT(12);
static constexpr uint64_t E_ADC1 = range(32, 39);
// 2/5/15 boot straps, 1/3 console UART
static constexpr uint64_t E_CAUTION = CM_BIT(2) | CM_BIT(5) | CM_BIT(15) | CM_BIT(1) | CM_BIT(3);
static constexpr uint64_t E_RTC = CM_BIT(0) | CM_BIT(2) | CM_BIT(4) | range(12, 15) | range(25, 27) | range(32, 39);
static const BoardPins E_PINS = {21, 22, 4, 18, 19, {32, 34, 35, 33}, 5, 17, 16, {26, 27}};
static const BoardPins WROVER_PINS = {21, 22, 4, 18, 19, {32, 34, 35, 33}, 14, 13, 25, {26, 27}};
static const BoardFixed E_FIXED = {-1, 0, {}, false, 0, {}, false};
// Ready-made relay boards. Pins from the ESPHome device pages and Tasmota templates.
static const BoardPins X8_PINS = {21, 22, 4, -1, 23, {34, 35, 36, 39}, -1, -1, -1, {32, 33}};
static const BoardFixed X8_FIXED = {-1, 8, {32, 33, 25, 26, 27, 14, 12, 13}, false, 0, {}, false};
static const BoardPins TRELAY_PINS = {26, 27, 4, -1, -1, {32, 33, 34, 35}, -1, -1, -1, {21, 19}};
static const BoardFixed TRELAY_FIXED = {-1, 4, {21, 19, 18, 5}, false, 0, {}, false};
static const BoardPins KC_PINS = {21, 22, 13, 18, -1, {32, 33, 34, 35}, -1, -1, -1, {2, 15}};
static const BoardFixed KC_FIXED = {-1, 4, {2, 15, 5, 4}, false, 4, {36, 39, 27, 14}, true};
static const BoardProfile kProfiles[] = {
  {"esp32_devkit", "ESP32 DevKit (WROOM-32, 4 MB)", "esp32", 4, 0,
   E_ALL, E_OUT, E_ADC1, E_CAUTION, E_RTC, E_PINS,
   "Your current board. GPIO34-39 are input only. Analog inputs: GPIO32-39.", E_FIXED},
  {"esp32_wrover", "ESP32 WROVER (4-8 MB flash, PSRAM)", "esp32", 4, 8,
   E_ALL & ~CM_BIT(16) & ~CM_BIT(17), E_OUT & ~CM_BIT(16) & ~CM_BIT(17), E_ADC1, E_CAUTION, E_RTC, WROVER_PINS,
   "GPIO16/17 are used by the PSRAM, so the EGT (MAX6675) pins move to SCK 14, CS 13, SO 25.", E_FIXED},
  {"relay_x8", "ESP32 Relay X8 (LC Technology, 8 relays, 7-30 V)", "esp32", 4, 0,
   E_ALL, E_OUT | CM_BIT(12), E_ADC1, E_CAUTION | CM_BIT(12), E_RTC, X8_PINS,
   "Relays 1-8 on GPIO 32, 33, 25, 26, 27, 14, 12, 13; LED on GPIO23. Relay 7 (GPIO12) is a boot pin: it stays off while the board starts. "
   "First flash with a 3.3 V USB-to-serial adapter, holding the GPIO0 button. Check on the bench whether a relay clicks on HIGH (default) or LOW.", X8_FIXED},
  {"t_relay", "LilyGO T-Relay (4 relays, WROVER)", "esp32", 4, 8,
   E_ALL & ~CM_BIT(16) & ~CM_BIT(17), E_OUT & ~CM_BIT(16) & ~CM_BIT(17), E_ADC1, E_CAUTION, E_RTC, TRELAY_PINS,
   "Relays K1-K4 on GPIO 21, 19, 18, 5. GPIO16/17 are used by the PSRAM. I2C moves to SDA 26, SCL 27.", TRELAY_FIXED},
  {"kc868_a4", "KinCony KC868-A4 (4 relays, 4 inputs, 4 analog)", "esp32", 4, 0,
   E_ALL, E_OUT, E_ADC1, E_CAUTION, E_RTC, KC_PINS,
   "Relays on GPIO 2, 15, 5, 4; opto inputs on GPIO 36, 39, 27, 14 (active low); analog in 32-35; buzzer GPIO18; 1-Wire GPIO13. "
   "GPIO 16/17 = RS232, 21/19 = 433 MHz, 22/23 = IR.", KC_FIXED},
};
#endif

static const size_t kCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

const char* Board::chip() {
#if CONFIG_IDF_TARGET_ESP32S3
  return "esp32s3";
#elif CONFIG_IDF_TARGET_ESP32C3
  return "esp32c3";
#elif CONFIG_IDF_TARGET_ESP32C6
  return "esp32c6";
#else
  return "esp32";
#endif
}

const BoardProfile* Board::find(const String& id) {
  for (size_t i = 0; i < kCount; i++) if (id == kProfiles[i].id) return &kProfiles[i];
  if (id == "custom") return custom();
  return nullptr;
}

// ---------------------------------------------------------------- custom board
namespace {
BoardProfile sCustom = {};
String sCustomName, sCustomNotes;
bool sCustomLoaded = false, sHasCustom = false;

uint64_t chipMask(uint64_t BoardProfile::*m) { uint64_t r = 0; for (size_t i = 0; i < kCount; i++) r |= kProfiles[i].*m; return r; }

// [1,2,5] or "1-3, 5"
bool maskFrom(JsonVariantConst v, uint64_t& out, const char* what, String& err) {
  out = 0;
  if (v.isNull()) return true;
  auto add = [&](long p) { if (p < 0 || p > 63) { err = String(what) + ": GPIO " + p + " does not exist"; return false; } out |= CM_BIT(p); return true; };
  if (v.is<JsonArrayConst>()) { for (JsonVariantConst x : v.as<JsonArrayConst>()) { if (!x.is<int>()) { err = String(what) + ": pin numbers only"; return false; } if (!add(x.as<int>())) return false; } return true; }
  if (!v.is<const char*>()) { err = String(what) + ": a list of pins"; return false; }
  String t = v.as<const char*>(); t.replace(" ", "");
  int i = 0;
  while (i < (int)t.length()) {
    int c = t.indexOf(',', i); if (c < 0) c = t.length();
    String part = t.substring(i, c); i = c + 1;
    if (!part.length()) continue;
    int d = part.indexOf('-');
    char* e; long a = strtol(part.c_str(), &e, 10);
    if (e == part.c_str()) { err = String(what) + ": '" + part + "' is not a pin"; return false; }
    long b = a;
    if (d > 0) { const char* q = part.c_str() + d + 1; b = strtol(q, &e, 10); if (e == q || *e) { err = String(what) + ": '" + part + "' is not a pin range"; return false; } }
    else if (*e) { err = String(what) + ": '" + part + "' is not a pin"; return false; }
    if (b < a || b - a > 63) { err = String(what) + ": '" + part + "' is not a pin range"; return false; }
    for (long p = a; p <= b; p++) if (!add(p)) return false;
  }
  return true;
}

bool parseCustom(JsonVariantConst v, BoardProfile& p, String& name, String& notes, String& err) {
  if (!v.is<JsonObjectConst>()) { err = "A board definition (JSON object)"; return false; }
  String chip = v["chip"] | Board::chip();
  if (chip != Board::chip()) { err = "chip: this firmware is for " + String(Board::chip()) + ", not " + chip; return false; }
  name = v["name"] | "";
  name.trim();
  if (!name.length() || name.length() > 60) { err = "name: 1-60 characters"; return false; }
  notes = v["notes"] | "";
  if (notes.length() > 300) { err = "notes: at most 300 characters"; return false; }
  int flash = v["flashMB"] | 4, psram = v["psramMB"] | 0;
  if (flash < 4 || flash > 32 || psram < 0 || psram > 32) { err = "flashMB 4-32, psramMB 0-32"; return false; }
  p = sCustom; p.id = "custom"; p.chip = Board::chip(); p.flashMB = flash; p.psramMB = psram;
  if (!maskFrom(v["valid"], p.valid, "valid", err) || !maskFrom(v["output"], p.output, "output", err) || !maskFrom(v["adc1"], p.adc1, "adc1", err) ||
      !maskFrom(v["caution"], p.caution, "caution", err) || !maskFrom(v["rtc"], p.rtc, "rtc", err)) return false;
  // Only what this chip has (never its flash or PSRAM pins), and each list inside "valid".
  uint64_t bad;
  if ((bad = p.valid & ~chipMask(&BoardProfile::valid))) { err = "valid: GPIO " + Board::pinList(bad) + " can't be used on " + Board::chip() + " (flash, PSRAM or missing)"; return false; }
  if (!p.valid) { err = "valid: list the GPIOs the board brings out"; return false; }
  if ((bad = p.output & ~(p.valid & chipMask(&BoardProfile::output)))) { err = "output: GPIO " + Board::pinList(bad) + " can't drive an output"; return false; }
  if ((bad = p.adc1 & ~(p.valid & chipMask(&BoardProfile::adc1)))) { err = "adc1: GPIO " + Board::pinList(bad) + " is not an ADC1 input on this chip"; return false; }
  if ((bad = p.rtc & ~(p.valid & chipMask(&BoardProfile::rtc)))) { err = "rtc: GPIO " + Board::pinList(bad) + " can't wake the chip"; return false; }
  p.caution &= p.valid;
  p.caution |= chipMask(&BoardProfile::caution) & p.valid;   // the chip's boot / USB / UART pins stay marked
  JsonVariantConst d = v["defaults"];
  auto pin = [&](const char* k, bool out, int8_t& to) {
    int x = d[k] | -1; to = x;
    if (x < 0) { to = -1; return true; }
    if (!Board::has(out ? p.output : p.valid, x)) { err = String("defaults.") + k + ": GPIO " + x + (out ? " can't drive an output" : " is not on the board"); return false; }
    return true;
  };
  p.pins = {};
  for (int8_t& a : p.pins.analog) a = -1;
  if (!pin("i2cSda", true, p.pins.sda) || !pin("i2cScl", true, p.pins.scl) || !pin("oneWirePin", true, p.pins.oneWire) || !pin("buzzerPin", true, p.pins.buzzer) ||
      !pin("ledPin", true, p.pins.led) || !pin("egtSck", true, p.pins.egtSck) || !pin("egtCs", true, p.pins.egtCs) || !pin("egtSo", false, p.pins.egtSo)) return false;
  p.pins.relay[0] = p.pins.relay[1] = -1;
  JsonVariantConst f = v["fixed"];
  p.fixed = {};
  p.fixed.rgb = f["rgb"] | -1;
  if (p.fixed.rgb >= 0 && !Board::has(p.output, p.fixed.rgb)) { err = "fixed.rgb: GPIO " + String(p.fixed.rgb) + " can't drive an output"; return false; }
  JsonArrayConst r = f["relays"].as<JsonArrayConst>(), in = f["inputs"].as<JsonArrayConst>();
  if (r.size() > 8 || in.size() > 4) { err = "fixed: at most 8 relays and 4 inputs"; return false; }
  for (JsonVariantConst x : r) {
    int g = x | -1;
    if (!Board::has(p.output, g)) { err = "fixed.relays: GPIO " + String(g) + " can't drive an output"; return false; }
    p.fixed.relays[p.fixed.nRelays++] = g;
  }
  for (JsonVariantConst x : in) {
    int g = x | -1;
    if (!Board::has(p.valid, g)) { err = "fixed.inputs: GPIO " + String(g) + " is not on the board"; return false; }
    p.fixed.inputs[p.fixed.nInputs++] = g;
  }
  p.fixed.relaysActiveLow = f["relaysActiveLow"] | false;
  p.fixed.inputsActiveLow = f["inputsActiveLow"] | true;
  // relay defaults: the first two fixed relays
  for (int i = 0; i < 2 && i < p.fixed.nRelays; i++) p.pins.relay[i] = p.fixed.relays[i];
  return true;
}

void useCustom(const BoardProfile& p, const String& name, const String& notes) {
  sCustomName = name; sCustomNotes = notes;
  sCustom = p; sCustom.id = "custom"; sCustom.name = sCustomName.c_str(); sCustom.notes = sCustomNotes.c_str();
  sHasCustom = true;
}

void loadOnce() {
  if (sCustomLoaded) return;
  sCustomLoaded = true;
  String js;
  if (!Board::customAllowed() || !loadCustomBoard(js)) return;
  JsonDocument d; String err, name, notes; BoardProfile p;
  if (deserializeJson(d, js) || !parseCustom(d.as<JsonVariantConst>(), p, name, notes, err)) { Serial.printf("[board] custom board ignored: %s\n", err.c_str()); return; }
  useCustom(p, name, notes);
}
}  // namespace

bool Board::customAllowed() { return gFirmwareRole == "worker"; }

const BoardProfile* Board::custom() {
  if (!customAllowed()) return nullptr;
  loadOnce();
  return sHasCustom ? &sCustom : nullptr;
}

bool Board::setCustom(JsonVariantConst v, String& err) {
  if (!customAllowed()) { err = "Custom boards are for worker firmware; main boards use the built-in ESP32-S3 profiles"; return false; }
  loadOnce();
  BoardProfile p; String name, notes;
  if (!parseCustom(v, p, name, notes, err)) return false;
  String js; serializeJson(v, js);
  if (js.length() > 1500) { err = "Board definition too long"; return false; }
  StateLock lk;
  bool inUse = gConfig.sys.board == "custom";
  BoardProfile oldP = sCustom; String oldName = sCustomName, oldNotes = sCustomNotes; bool had = sHasCustom;
  useCustom(p, name, notes);
  if (inUse && !validateConfig(gConfig, err)) {   // the settings in use must still fit this board
    err = "The current settings don't fit this definition: " + err;
    if (had) useCustom(oldP, oldName, oldNotes); else sHasCustom = false;
    return false;
  }
  if (!saveCustomBoard(js)) { err = "Could not save the board definition"; if (had) useCustom(oldP, oldName, oldNotes); else sHasCustom = false; return false; }
  return true;
}

bool Board::clearCustom(String& err) {
  StateLock lk;
  if (gConfig.sys.board == "custom") { err = "The board in use: choose another board type first"; return false; }
  if (!saveCustomBoard("")) { err = "Could not remove the board definition"; return false; }
  sHasCustom = false;
  return true;
}

const BoardProfile& Board::detected() {
  // Builds for ready-made boards start with that board's profile.
#if defined(CM_RELAY_X8)
  if (const BoardProfile* p = find("relay_x8")) return *p;
#elif defined(CM_KC868_A4)
  if (const BoardProfile* p = find("kc868_a4")) return *p;
#elif defined(CM_T_RELAY)
  if (const BoardProfile* p = find("t_relay")) return *p;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
  return ESP.getPsramSize() >= 4 * 1024 * 1024 ? (ESP.getFlashChipSize() >= 16UL * 1024 * 1024 ? kProfiles[1] : kProfiles[0]) : kProfiles[2];
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
  return kProfiles[0];
#else
  return ESP.getPsramSize() > 0 ? kProfiles[1] : kProfiles[0];
#endif
}

const BoardProfile& Board::resolve(const String& id) {
  const BoardProfile* p = find(id);
  return p ? *p : detected();
}

const BoardProfile& Board::current() { return resolve(gConfig.sys.board); }

bool Board::chipHasPin(int pin) { for (size_t i = 0; i < kCount; i++) if (has(kProfiles[i].valid, pin)) return true; return false; }
bool Board::chipCanOutput(int pin) { for (size_t i = 0; i < kCount; i++) if (has(kProfiles[i].output, pin)) return true; return false; }

String Board::pinList(uint64_t m) {
  String s;
  for (int p = 0; p < 64; p++) {
    if (!(m & CM_BIT(p))) continue;
    int q = p;
    while (q + 1 < 64 && (m & CM_BIT(q + 1))) q++;
    if (s.length()) s += ", ";
    s += q > p ? String(p) + "-" + String(q) : String(p);
    p = q;
  }
  return s;
}

static void maskToJson(uint64_t m, JsonArray a) { for (int p = 0; p < 64; p++) if (m & CM_BIT(p)) a.add(p); }

void Board::profileToJson(const BoardProfile& p, JsonObject o) {
  o["id"] = p.id; o["name"] = p.name; o["chip"] = p.chip; o["flashMB"] = p.flashMB; o["psramMB"] = p.psramMB;
  maskToJson(p.valid, o["valid"].to<JsonArray>());
  maskToJson(p.output, o["output"].to<JsonArray>());
  maskToJson(p.adc1, o["adc1"].to<JsonArray>());
  maskToJson(p.caution, o["caution"].to<JsonArray>());
  maskToJson(p.rtc, o["rtc"].to<JsonArray>());
  JsonObject d = o["defaults"].to<JsonObject>();
  d["i2cSda"] = p.pins.sda; d["i2cScl"] = p.pins.scl; d["oneWirePin"] = p.pins.oneWire;
  d["buzzerPin"] = p.pins.buzzer; d["ledPin"] = p.pins.led;
  d["egtSck"] = p.pins.egtSck; d["egtCs"] = p.pins.egtCs; d["egtSo"] = p.pins.egtSo;
  o["notes"] = p.notes;
  JsonObject f = o["fixed"].to<JsonObject>();
  if (p.fixed.rgb >= 0) f["rgb"] = p.fixed.rgb;
  JsonArray r = f["relays"].to<JsonArray>();
  for (int i = 0; i < p.fixed.nRelays; i++) r.add(p.fixed.relays[i]);
  f["relaysActiveLow"] = p.fixed.relaysActiveLow;
  JsonArray in = f["inputs"].to<JsonArray>();
  for (int i = 0; i < p.fixed.nInputs; i++) in.add(p.fixed.inputs[i]);
  f["inputsActiveLow"] = p.fixed.inputsActiveLow;
}

void Board::listToJson(JsonArray a) {
  for (size_t i = 0; i < kCount; i++) profileToJson(kProfiles[i], a.add<JsonObject>());
  if (const BoardProfile* c = custom()) profileToJson(*c, a.add<JsonObject>());
}
