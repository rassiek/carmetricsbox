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
static const BoardProfile kProfiles[] = {
  {"esp32s3_n8r8", "ESP32-S3 DevKitC-1 N8R8 (8 MB flash, 8 MB PSRAM)", "esp32s3", 8, 8,
   S3_OCTAL, S3_OCTAL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "Recommended main board. GPIO35-37 are used by the PSRAM. Analog inputs: GPIO1-10."},
  {"esp32s3_n16r8", "ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM)", "esp32s3", 16, 8,
   S3_OCTAL, S3_OCTAL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "Same pins as N8R8. The extra flash is kept for a bigger offline buffer in a later release."},
  {"esp32s3_quad", "ESP32-S3 with quad PSRAM or none (N4, N8, N8R2)", "esp32s3", 4, 0,
   S3_ALL, S3_ALL & ~CM_BIT(0), S3_ADC1, S3_CAUTION, range(0, 21), S3_PINS,
   "GPIO35-37 are free on these modules."},
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
static const BoardProfile kProfiles[] = {
  {"esp32_devkit", "ESP32 DevKit (WROOM-32, 4 MB)", "esp32", 4, 0,
   E_ALL, E_OUT, E_ADC1, E_CAUTION, E_RTC, E_PINS,
   "Your current board. GPIO34-39 are input only. Analog inputs: GPIO32-39."},
  {"esp32_wrover", "ESP32 WROVER (4-8 MB flash, PSRAM)", "esp32", 4, 8,
   E_ALL & ~CM_BIT(16) & ~CM_BIT(17), E_OUT & ~CM_BIT(16) & ~CM_BIT(17), E_ADC1, E_CAUTION, E_RTC, WROVER_PINS,
   "GPIO16/17 are used by the PSRAM, so the EGT (MAX6675) pins move to SCK 14, CS 13, SO 25."},
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
}

void Board::listToJson(JsonArray a) {
  for (size_t i = 0; i < kCount; i++) profileToJson(kProfiles[i], a.add<JsonObject>());
}
