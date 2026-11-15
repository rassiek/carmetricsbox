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
static const BoardFixed S3_FIXED = {48, 0, {}, false, 0, {}, false, "main"};
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
static const BoardFixed E_FIXED = {-1, 0, {}, false, 0, {}, false, "main"};
// Ready-made relay boards. Pins from the ESPHome device pages and Tasmota templates.
static const BoardPins X8_PINS = {21, 22, 4, -1, 23, {34, 35, 36, 39}, -1, -1, -1, {32, 33}};
static const BoardFixed X8_FIXED = {-1, 8, {32, 33, 25, 26, 27, 14, 12, 13}, false, 0, {}, false, "relay"};
static const BoardPins TRELAY_PINS = {26, 27, 4, -1, -1, {32, 33, 34, 35}, -1, -1, -1, {21, 19}};
static const BoardFixed TRELAY_FIXED = {-1, 4, {21, 19, 18, 5}, false, 0, {}, false, "relay"};
static const BoardPins KC_PINS = {21, 22, 13, 18, -1, {32, 33, 34, 35}, -1, -1, -1, {2, 15}};
static const BoardFixed KC_FIXED = {-1, 4, {2, 15, 5, 4}, false, 4, {36, 39, 27, 14}, true, "sensor_relay"};
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
#else
  return "esp32";
#endif
}

const BoardProfile* Board::find(const String& id) {
  for (size_t i = 0; i < kCount; i++) if (id == kProfiles[i].id) return &kProfiles[i];
  return nullptr;
}

const BoardProfile& Board::detected() {
#ifdef CM_RELAY_X8
  if (const BoardProfile* p = find("relay_x8")) return *p;   // the relay-x8 build
#endif
#if CONFIG_IDF_TARGET_ESP32S3
  return ESP.getPsramSize() >= 4 * 1024 * 1024 ? (ESP.getFlashChipSize() >= 16UL * 1024 * 1024 ? kProfiles[1] : kProfiles[0]) : kProfiles[2];
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
  f["role"] = p.fixed.role;
}

void Board::listToJson(JsonArray a) {
  for (size_t i = 0; i < kCount; i++) profileToJson(kProfiles[i], a.add<JsonObject>());
}
