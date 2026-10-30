#include "Display.h"
#include "State.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

namespace {

struct Item {
  char label[4];
  char value[10];
  char unit[2];
  OledStyle style;
  float frac;          // bar fill 0..1, NAN = no value
  float ticks[4];      // threshold positions 0..1, NAN = none
  uint8_t level;
};

struct Snapshot {
  std::vector<Item> items;
  String ssid, ip;
  int ota = -1;
  bool clock = false;
  uint16_t pageSeconds = 4;
  bool timeSynced = false;
  int tzOffset = 0;
};

volatile bool reconfigureRequested = true;
U8G2* oled = nullptr;
String currentType;
uint32_t bootMs = 0;

float toFrac(const SensorConfig& c, float v) {
  if (isnan(v)) return NAN;
  float f = (v - c.gaugeMin) / (c.gaugeMax - c.gaugeMin);
  return constrain(f, 0.0f, 1.0f);
}

void takeSnapshot(Snapshot& s) {
  StateLock lk;
  s.items.clear();
  s.ssid = gConfig.sys.apSsid;
  s.ip = gSys.ip.length() ? gSys.ip : gSys.apIp;
  s.ota = gSys.otaProgress;
  s.clock = gConfig.sys.oledClock;
  s.pageSeconds = gConfig.sys.oledPageSeconds;
  s.timeSynced = gSys.timeSynced;
  s.tzOffset = gConfig.sys.tzOffsetMinutes;
  for (size_t i = 0; i < gConfig.sensors.size() && i < gSensorRt.size(); i++) {
    const SensorConfig& c = gConfig.sensors[i];
    const SensorRuntime& r = gSensorRt[i];
    if (!c.enabled || c.oledStyle == OledStyle::OFF) continue;
    Item it;
    String lbl = c.oledLabel.length() ? c.oledLabel : c.name.substring(0, 3);
    strlcpy(it.label, lbl.c_str(), sizeof(it.label));
    it.unit[0] = 0; it.unit[1] = 0;
    if (c.unit == "°C" || c.unit == "C") it.unit[0] = 'c';
    else if (c.unit == "V" || c.unit == "v") it.unit[0] = 'v';
    else if (c.unit == "m") it.unit[0] = 'm';
    it.style = c.oledStyle;
    it.level = r.level;
    it.frac = NAN;
    switch (r.status) {
      case ST_OK:
        dtostrf(r.value, 1, c.decimals, it.value);
        it.frac = toFrac(c, r.value);
        break;
      case ST_FAULT:     strcpy(it.value, "ERR"); break;
      case ST_NOT_FOUND: strcpy(it.value, "--"); break;
      case ST_UNASSIGNED: strcpy(it.value, "?"); break;
      default:           strcpy(it.value, ".."); break;
    }
    float th[4] = {c.warnLow, c.critLow, c.warnHigh, c.critHigh};
    for (int k = 0; k < 4; k++) it.ticks[k] = isnan(th[k]) ? NAN : toFrac(c, th[k]);
    s.items.push_back(it);
  }
}

#ifdef CM_EMU_SELFTEST
class U8G2Virtual : public U8G2 {
 public:
  explicit U8G2Virtual(bool big) {
    if (big) u8g2_Setup_ssd1327_ea_w128128_f(&u8g2, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb);
    else u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb);
  }
};
volatile bool dumpRequested = false;
#endif

U8G2* makeDriver(const String& type, bool flip) {
  const u8g2_cb_t* rot = flip ? U8G2_R2 : U8G2_R0;
  // Pins are NONE: Wire is already started by main with the configured SDA/SCL.
  if (type == "ssd1306_128x64")     return new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(rot, U8X8_PIN_NONE);
  if (type == "sh1106_128x64")      return new U8G2_SH1106_128X64_NONAME_F_HW_I2C(rot, U8X8_PIN_NONE);
  if (type == "ssd1327_128x128")    return new U8G2_SSD1327_EA_W128128_F_HW_I2C(rot, U8X8_PIN_NONE);
  if (type == "ssd1327_ws_128x128") return new U8G2_SSD1327_WS_128X128_F_HW_I2C(rot, U8X8_PIN_NONE);
  return nullptr;
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void doReconfigure() {
  String type; bool flip; uint8_t contrast;
  { StateLock lk; type = gConfig.sys.oledType; flip = gConfig.sys.oledFlip; contrast = gConfig.sys.oledContrast; }
  if (oled) { oled->clearDisplay(); oled->setPowerSave(1); delete oled; oled = nullptr; }
  currentType = type;
  String status = "off";
  if (type != "none") {
#ifdef CM_EMU_SELFTEST
    oled = new U8G2Virtual(type.indexOf("128x128") > 0);
    oled->begin();
    status = "ok";
    bool at3c = true, at3d = false;
    if (false) {}
#else
    bool at3c = i2cPresent(0x3C), at3d = i2cPresent(0x3D);
    if (!at3c && !at3d) status = "not found on I2C";
#endif
    else {
      oled = makeDriver(type, flip);
      if (oled) {
        if (at3d && !at3c) oled->setI2CAddress(0x3D * 2);
        oled->setBusClock(400000);
        oled->begin();
        oled->setContrast(contrast);
        status = "ok";
      }
    }
  }
  StateLock lk;
  gSys.oledStatus = status;
  Serial.printf("[oled] %s: %s\n", type.c_str(), status.c_str());
}

// ------------------------------------------------------------------ drawing
void drawCellText(int x, int y, int w, int h, const Item& it, bool invert) {
  // label left, value (+unit if it fits) right-aligned, 2 px gutter between cells
  if (invert) { oled->setDrawColor(1); oled->drawBox(x, y, w - 2, h); oled->setDrawColor(0); }
  int base = y + h - 3;
  oled->setCursor(x + 1, base);
  oled->print(it.label);
  bool numeric = isdigit((unsigned char)it.value[0]) || it.value[0] == '-' && isdigit((unsigned char)it.value[1]);
  char txt[14];
  snprintf(txt, sizeof(txt), "%s%s", it.value, numeric ? it.unit : "");
  int lw = oled->getStrWidth(it.label) + 3;
  if (oled->getStrWidth(txt) > w - 3 - lw) strlcpy(txt, it.value, sizeof(txt));
  oled->setCursor(x + w - 3 - oled->getStrWidth(txt), base);
  oled->print(txt);
  oled->setDrawColor(1);
}

void drawBarRow(int y, int h, int W, const Item& it, bool invert) {
  int cw = oled->getMaxCharWidth();
  int labelW = cw * 3 + 3, valueW = cw * 4 + 5;
  int bx = labelW, bw = W - labelW - valueW;
  int barH = h - 4, by = y + 2;
  int base = y + h - 2 - (h > 14 ? 2 : 0);
  if (invert) { oled->drawBox(0, y, labelW - 2, h); oled->setDrawColor(0); }
  oled->setCursor(0, base); oled->print(it.label);
  oled->setDrawColor(1);
  oled->drawFrame(bx, by, bw, barH);
  if (!isnan(it.frac)) oled->drawBox(bx + 1, by + 1, (int)((bw - 2) * it.frac), barH - 2);
  for (int k = 0; k < 4; k++) {
    if (isnan(it.ticks[k])) continue;
    int tx = bx + (int)((bw - 1) * it.ticks[k]);
    oled->drawVLine(tx, by - 2, 2);
    oled->drawVLine(tx, by + barH, 2);
  }
  int vw = oled->getStrWidth(it.value);
  oled->setCursor(W - vw, base);
  oled->print(it.value);
}

struct Row { bool bar; int a, b; int h; };  // items [a,b)

void render(const Snapshot& s, uint32_t now) {
  int W = oled->getDisplayWidth(), H = oled->getDisplayHeight();
  bool large = H > 64;
  oled->clearBuffer();
  oled->setFont(large ? u8g2_font_7x13_tr : u8g2_font_6x10_tr);
  oled->setFontMode(1);

  if (s.ota >= 0) {
    oled->setCursor(0, 14); oled->print("Updating...");
    oled->drawFrame(0, 24, W, 10); oled->drawBox(1, 25, (W - 2) * s.ota / 100, 8);
    oled->setCursor(0, 50); oled->print(s.ota); oled->print('%');
    oled->sendBuffer();
    return;
  }
  if (now - bootMs < 3000 || s.items.empty()) {
    oled->setCursor(0, 12); oled->print("CarMetrics " FW_VERSION);
    oled->setCursor(0, 28); oled->print("WiFi: "); oled->print(s.ssid);
    oled->setCursor(0, 42); oled->print(s.ip);
    if (s.items.empty()) { oled->setCursor(0, 58); oled->print("No OLED sensors"); }
    oled->sendBuffer();
    return;
  }

  // Build the list of cells: optional clock + text items, then bars.
  std::vector<Item> texts, bars;
  if (s.clock && s.timeSynced) {
    Item c{}; time_t t = time(nullptr) + s.tzOffset * 60; struct tm tm; gmtime_r(&t, &tm);
    snprintf(c.value, sizeof(c.value), "%02d:%02d", tm.tm_hour, tm.tm_min);
    c.label[0] = 0; c.style = OledStyle::TEXT; c.level = LVL_OK; c.frac = NAN;
    texts.push_back(c);
  }
  for (auto& it : s.items) (it.style == OledStyle::BAR ? bars : texts).push_back(it);

  int textH = large ? 14 : 10, barH = large ? 17 : 11;
  std::vector<Row> rows;
  size_t cols = large ? 2 : 3;  // 128 px wide: 2 roomy columns on the tall screen, 3 on 128x64
  for (size_t i = 0; i < texts.size(); i += cols) rows.push_back({false, (int)i, (int)min(texts.size(), i + cols), textH});
  for (size_t i = 0; i < bars.size(); i++) rows.push_back({true, (int)i, (int)i + 1, barH});

  // Paginate
  std::vector<std::pair<size_t, size_t>> pages;
  size_t start = 0; int used = 0;
  for (size_t r = 0; r < rows.size(); r++) {
    if (used + rows[r].h > H && r > start) { pages.push_back({start, r}); start = r; used = 0; }
    used += rows[r].h;
  }
  pages.push_back({start, rows.size()});
  size_t page = (now / (s.pageSeconds * 1000UL)) % pages.size();

  bool blink = (now / 400) % 2;
  int y = 0;
  for (size_t r = pages[page].first; r < pages[page].second; r++) {
    const Row& row = rows[r];
    if (row.bar) {
      const Item& it = bars[row.a];
      bool inv = it.level == LVL_WARN || (it.level == LVL_CRIT && blink);
      drawBarRow(y, row.h, W, it, inv);
    } else {
      int cw = W / (large ? 2 : 3);
      for (int k = row.a; k < row.b; k++) {
        const Item& it = texts[k];
        bool inv = it.level == LVL_WARN || (it.level == LVL_CRIT && blink);
        drawCellText((k - row.a) * cw, y, cw, row.h, it, inv);
      }
    }
    y += row.h;
  }
  if (pages.size() > 1) {  // page dots
    for (size_t p = 0; p < pages.size(); p++) {
      int dx = W - 4 * (int)(pages.size() - p);
      if (p == page) oled->drawBox(dx, H - 2, 2, 2); else oled->drawPixel(dx, H - 1);
    }
  }
  oled->sendBuffer();
}

void task(void*) {
  Snapshot snap;
  for (;;) {
    if (reconfigureRequested) { reconfigureRequested = false; doReconfigure(); }
    if (oled) {
      takeSnapshot(snap);
      render(snap, millis());
#ifdef CM_EMU_SELFTEST
      if (dumpRequested) { dumpRequested = false; Serial.println("<<<PBM"); oled->writeBufferPBM(Serial); Serial.println(">>>PBM"); }
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(snap.ota >= 0 ? 200 : 400));
  }
}
}  // namespace

void Display::start() {
  bootMs = millis();
  xTaskCreatePinnedToCore(task, "oled", 6144, nullptr, 1, nullptr, 0);
}

void Display::reconfigure() { reconfigureRequested = true; }
#ifdef CM_EMU_SELFTEST
void Display::dumpFrame() { dumpRequested = true; }
#endif
