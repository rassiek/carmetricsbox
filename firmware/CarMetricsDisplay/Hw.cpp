#include "Hw.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define CM_JC3248W535 1
#else
#define CM_CYD 1
#endif

namespace {
Arduino_GFX* panelGfx = nullptr;     // the display itself
Arduino_Canvas* canvas = nullptr;    // JC: the whole screen
uint8_t rot = 0;
Hw::TouchCal cal;
String profileStr;

#if CM_JC3248W535
constexpr int BL_PIN = 1, NATIVE_W = 320, NATIVE_H = 480;
constexpr int TP_SDA = 4, TP_SCL = 8, TP_ADDR = 0x3B;

class CanvasSurface : public Disp::Surface {
 public:
  int width() override { return canvas->width(); }
  int height() override { return canvas->height(); }
  Arduino_GFX* begin(int x, int y, int, int, int& ox, int& oy) override { ox = x; oy = y; return canvas; }
  void end() override {}
  void present() override { canvas->flush(); }
} surf;

bool touchRaw(int& x, int& y) {
  static const uint8_t cmd[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  uint8_t b[8] = {0};
  Wire.beginTransmission(TP_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TP_ADDR, 8) != 8) return false;
  for (int i = 0; i < 8; i++) b[i] = Wire.read();
  uint8_t n = b[1];
  if (n == 0 || n > 5) return false;
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  return x < 4096 && y < 4096;
}
#else
constexpr int BL_PIN = 21, NATIVE_W = 240, NATIVE_H = 320;
constexpr int T_CLK = 25, T_MOSI = 32, T_MISO = 39, T_CS = 33, T_IRQ = 36;

// One small canvas per region (no PSRAM); big regions are drawn straight on the panel.
class TileSurface : public Disp::Surface {
 public:
  int width() override { return panelGfx->width(); }
  int height() override { return panelGfx->height(); }
  Arduino_GFX* begin(int x, int y, int w, int h, int& ox, int& oy) override {
    cur = nullptr;
    if ((size_t)w * h * 2 <= 60000 && ESP.getMaxAllocHeap() > (size_t)w * h * 2 + 30000) {
      cur = new Arduino_Canvas(w, h, panelGfx, x, y);
      if (cur->begin(GFX_SKIP_OUTPUT_BEGIN)) { ox = 0; oy = 0; return cur; }
      delete cur; cur = nullptr;
    }
    ox = x; oy = y;
    return panelGfx;
  }
  void end() override { if (cur) { cur->flush(); delete cur; cur = nullptr; } }
  void present() override {}
  Arduino_Canvas* cur = nullptr;
} surf;

uint16_t xpt(uint8_t cmd) {   // bit-banged SPI, 12-bit result
  digitalWrite(T_CLK, LOW);
  for (int i = 7; i >= 0; i--) { digitalWrite(T_MOSI, (cmd >> i) & 1); digitalWrite(T_CLK, HIGH); delayMicroseconds(1); digitalWrite(T_CLK, LOW); }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) { digitalWrite(T_CLK, HIGH); delayMicroseconds(1); digitalWrite(T_CLK, LOW); v = (v << 1) | digitalRead(T_MISO); }
  return v >> 4;
}
bool touchRaw(int& x, int& y) {
  if (digitalRead(T_IRQ) == HIGH) return false;
  digitalWrite(T_CS, LOW);
  uint16_t z1 = xpt(0xB1);
  long sx = 0, sy = 0;
  for (int i = 0; i < 4; i++) { sx += xpt(0xD1); sy += xpt(0x91); }
  xpt(0x90);   // power down
  digitalWrite(T_CS, HIGH);
  if (z1 < 100) return false;
  x = sx / 4; y = sy / 4;
  return true;
}
#endif
}  // namespace

namespace Hw {
const char* chipName() {
#if CM_JC3248W535
  return "ESP32-S3";
#else
  return "ESP32";
#endif
}
const char* profile() { return profileStr.c_str(); }
uint8_t defaultRotation() {
#if CM_JC3248W535
  return 0;
#else
  return 1;
#endif
}
uint8_t nextRotation(uint8_t r) {
#if CM_JC3248W535
  return (r + 1) % 4;
#else
  return r == 1 ? 3 : 1;
#endif
}

bool begin(uint8_t rotation, const char* panel, const TouchCal& c) {
  cal = c;
  rot = rotation;
#if CM_JC3248W535
  (void)panel;
  profileStr = "JC3248W535";
  Arduino_DataBus* bus = new Arduino_ESP32QSPI(45, 47, 21, 48, 40, 39);
  panelGfx = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, NATIVE_W, NATIVE_H, 0, 0, 0, 0,
                                   axs15231b_320480_type1_init_operations, sizeof(axs15231b_320480_type1_init_operations));
  canvas = new Arduino_Canvas(NATIVE_W, NATIVE_H, panelGfx, 0, 0, 0);
  if (!canvas->begin()) { Serial.println("[hw] display init failed (no PSRAM?)"); return false; }
  canvas->setRotation(rot);
  canvas->fillScreen(0);
  canvas->flush();
  Wire.begin(TP_SDA, TP_SCL, 400000);
#else
  bool st = panel && String(panel) == "st7789";
  profileStr = st ? "CYD ST7789" : "CYD ILI9341";
  Arduino_DataBus* bus = new Arduino_ESP32SPI(2, 15, 14, 13, 12);
  if (st) panelGfx = new Arduino_ST7789(bus, GFX_NOT_DEFINED, rot, true, 240, 320);
  else panelGfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, rot);
  if (!panelGfx->begin()) { Serial.println("[hw] display init failed"); return false; }
  panelGfx->fillScreen(0);
  pinMode(T_CLK, OUTPUT); pinMode(T_MOSI, OUTPUT); pinMode(T_CS, OUTPUT); pinMode(T_MISO, INPUT); pinMode(T_IRQ, INPUT);
  digitalWrite(T_CS, HIGH);
#endif
  ledcAttach(BL_PIN, 5000, 8);
  setBrightness(80);
  return true;
}

Disp::Surface& surface() { return surf; }

void setBrightness(int pct) {
  pct = constrain(pct, 5, 100);
  ledcWrite(BL_PIN, pct * 255 / 100);
}

void setRotation(uint8_t r) {
  rot = r;
#if CM_JC3248W535
  canvas->setRotation(r);
  canvas->fillScreen(0);
#else
  panelGfx->setRotation(r);
  panelGfx->fillScreen(0);
#endif
}

bool readTouchRaw(int& x, int& y) { return touchRaw(x, y); }

bool readTouch(int& x, int& y) {
  int rx, ry;
  if (!touchRaw(rx, ry)) return false;
  if (cal.swapXY) std::swap(rx, ry);
#if CM_JC3248W535
  // raw = native portrait pixels
  int nx = cal.flipX ? NATIVE_W - 1 - rx : rx, ny = cal.flipY ? NATIVE_H - 1 - ry : ry;
  switch (rot & 3) {
    case 0: x = nx; y = ny; break;
    case 1: x = ny; y = NATIVE_W - 1 - nx; break;
    case 2: x = NATIVE_W - 1 - nx; y = NATIVE_H - 1 - ny; break;
    default: x = NATIVE_H - 1 - ny; y = nx; break;
  }
#else
  // calibrated for landscape (rotation 1); rotation 3 is the same turned round
  int W = panelGfx->width(), H = panelGfx->height();
  long sx = (long)(rx - cal.xMin) * W / std::max(1, cal.xMax - cal.xMin);
  long sy = (long)(ry - cal.yMin) * H / std::max(1, cal.yMax - cal.yMin);
  if (cal.flipX) sx = W - 1 - sx;
  if (cal.flipY) sy = H - 1 - sy;
  if (rot == 3) { sx = W - 1 - sx; sy = H - 1 - sy; }
  x = constrain(sx, 0, W - 1); y = constrain(sy, 0, H - 1);
#endif
  return true;
}
}  // namespace Hw
