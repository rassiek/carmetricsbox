#include "Display.h"
#include "Health.h"
#include "State.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>
#include <algorithm>

namespace {

struct Item {
  char label[4];
  char value[10];
  char unit[2];
  OledStyle style;
  float frac;          // bar fill 0..1, NAN = no value
  float ticks[4];      // threshold positions 0..1, NAN = none
  uint8_t level;
  bool pageBreak = false;   // layout: start a new page here
};

struct Snapshot {
  std::vector<Item> items;
  String ssid, ip;
  int ota = -1;
  bool clock = false;
  uint16_t pageSeconds = 4;
  uint8_t columns = 0;      // text values per line, 0 = automatic
  bool timeSynced = false;
  int tzOffset = 0;
  uint8_t turboPhase = TURBO_OFF, overall = LVL_OK;
  int32_t turboLeft = 0;
  float turboEgt = NAN, turboSafe = 0;
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
  s.columns = gConfig.layout.oledColumns;
  s.timeSynced = gSys.timeSynced;
  s.tzOffset = gConfig.sys.tzOffsetMinutes;
  s.turboPhase = gSys.turbo.phase; s.turboLeft = gSys.turbo.leftS; s.turboEgt = gSys.turbo.egt;
  s.turboSafe = gConfig.turbo.safeEgt; s.overall = gSys.overallLevel;
  // Order: the OLED layout if one is set ("|" = page break), otherwise the sensor list.
  std::vector<int> order;
  std::vector<bool> breakBefore;
  if (gConfig.layout.oled.empty()) {
    for (size_t i = 0; i < gConfig.sensors.size(); i++) { order.push_back(i); breakBefore.push_back(false); }
  } else {
    bool brk = false;
    for (auto& id : gConfig.layout.oled) {
      if (id == "|") { brk = true; continue; }
      int i = findSensorIndex(id);
      if (i < 0) continue;
      order.push_back(i); breakBefore.push_back(brk); brk = false;
    }
  }
  for (size_t k = 0; k < order.size(); k++) {
    size_t i = order[k];
    if (i >= gSensorRt.size()) continue;
    const SensorConfig& c = gConfig.sensors[i];
    const SensorRuntime& r = gSensorRt[i];
    if (!c.enabled || c.oledStyle == OledStyle::OFF) continue;
    Item it;
    it.pageBreak = breakBefore[k];
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
      case ST_LINK_LOST: strcpy(it.value, "LNK"); break;
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
  String type; bool flip; uint8_t contrast; uint16_t khz;
  { StateLock lk; type = gConfig.sys.oledType; flip = gConfig.sys.oledFlip; contrast = gConfig.sys.oledContrast; khz = gConfig.sys.i2cKhz; }
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
        oled->setBusClock(khz * 1000UL);
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
  if (it.style == OledStyle::MARKER) {
    // Original look: threshold ticks inside the frame, a needle at the value.
    for (int k = 0; k < 4; k++) {
      if (isnan(it.ticks[k])) continue;
      int tx = bx + 1 + (int)((bw - 3) * it.ticks[k]);
      oled->drawVLine(tx, by + 2, barH - 4);
    }
    if (!isnan(it.frac)) {
      int nx = bx + 1 + (int)((bw - 5) * it.frac);
      oled->drawBox(nx + 1, by + 1, 2, barH - 2);           // stem
      oled->drawHLine(nx, by + 1, 4); oled->drawHLine(nx, by + barH - 2, 4);   // serifs: an "I"
    }
  } else {
    if (!isnan(it.frac)) oled->drawBox(bx + 1, by + 1, (int)((bw - 2) * it.frac), barH - 2);
    for (int k = 0; k < 4; k++) {
      if (isnan(it.ticks[k])) continue;
      int tx = bx + (int)((bw - 1) * it.ticks[k]);
      oled->drawVLine(tx, by - 2, 2);
      oled->drawVLine(tx, by + barH, 2);
    }
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

  // Turbo cool-down screen (unless an alarm needs the normal pages)
  if (s.turboPhase != TURBO_OFF && s.overall < LVL_WARN) {
    bool ok = s.turboPhase == TURBO_OK;
    oled->setCursor(0, large ? 14 : 11); oled->print(ok ? "TURBO: OK to stop" : "TURBO cooling");
    oled->setFont(large ? u8g2_font_logisoso32_tn : u8g2_font_logisoso22_tn);
    char b[12];
    if (isnan(s.turboEgt)) strcpy(b, "--"); else snprintf(b, sizeof(b), "%d", (int)s.turboEgt);
    oled->setCursor(0, large ? 60 : 42); oled->print(b);
    oled->setFont(large ? u8g2_font_7x13_tr : u8g2_font_6x10_tr);
    oled->print(" C");
    oled->setCursor(0, large ? 82 : 60);
    if (!ok && s.turboLeft > 0) { oled->print("wait "); oled->print(s.turboLeft); oled->print(" s"); }
    else if (!ok) { oled->print("until < "); oled->print((int)s.turboSafe); oled->print(" C"); }
    else oled->print("switch off now");
    oled->sendBuffer();
    return;
  }

  // Segments (layout page breaks), each: optional clock + text items, then bars.
  int textH = large ? 14 : 10, barH = large ? 17 : 11;
  size_t cols = s.columns >= 2 ? s.columns : (large ? 2 : 3);  // automatic: 2 roomy columns on the tall screen, 3 on 128x64
  std::vector<Item> texts, bars;
  std::vector<Row> rows;
  std::vector<size_t> forcedBreaks;   // row index where a new page must start
  auto flushSegment = [&](std::vector<Item>& t, std::vector<Item>& b) {
    if (!rows.empty()) forcedBreaks.push_back(rows.size());
    size_t t0 = texts.size(), b0 = bars.size();
    texts.insert(texts.end(), t.begin(), t.end()); bars.insert(bars.end(), b.begin(), b.end());
    for (size_t i = t0; i < texts.size(); i += cols) rows.push_back({false, (int)i, (int)min(texts.size(), i + cols), textH});
    for (size_t i = b0; i < bars.size(); i++) rows.push_back({true, (int)i, (int)i + 1, barH});
    t.clear(); b.clear();
  };
  std::vector<Item> segT, segB;
  if (s.clock && s.timeSynced) {
    Item c{}; time_t t = time(nullptr) + s.tzOffset * 60; struct tm tm; gmtime_r(&t, &tm);
    snprintf(c.value, sizeof(c.value), "%02d:%02d", tm.tm_hour, tm.tm_min);
    c.label[0] = 0; c.style = OledStyle::TEXT; c.level = LVL_OK; c.frac = NAN;
    segT.push_back(c);
  }
  for (auto& it : s.items) {
    if (it.pageBreak && (!segT.empty() || !segB.empty())) flushSegment(segT, segB);
    (it.style == OledStyle::BAR || it.style == OledStyle::MARKER ? segB : segT).push_back(it);
  }
  flushSegment(segT, segB);

  // Paginate
  std::vector<std::pair<size_t, size_t>> pages;
  size_t start = 0; int used = 0;
  for (size_t r = 0; r < rows.size(); r++) {
    bool forced = std::find(forcedBreaks.begin(), forcedBreaks.end(), r) != forcedBreaks.end();
    if ((forced || used + rows[r].h > H) && r > start) { pages.push_back({start, r}); start = r; used = 0; }
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
      int cw = W / (int)cols;
      // Narrow cells (3-4 per line) need a smaller font so label + value fit.
      const uint8_t* font = cols >= 4 ? u8g2_font_5x7_tr : (cols == 3 && large) ? u8g2_font_6x10_tr : (large ? u8g2_font_7x13_tr : u8g2_font_6x10_tr);
      oled->setFont(font);
      for (int k = row.a; k < row.b; k++) {
        const Item& it = texts[k];
        bool inv = it.level == LVL_WARN || (it.level == LVL_CRIT && blink);
        drawCellText((k - row.a) * cw, y, cw, row.h, it, inv);
      }
      oled->setFont(large ? u8g2_font_7x13_tr : u8g2_font_6x10_tr);
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
  Health::registerTask("oled");
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
