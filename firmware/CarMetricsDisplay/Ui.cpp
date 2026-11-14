#include "Ui.h"
#include <Arduino_GFX.h>
#include <U8g2lib.h>
#include <stdio.h>
#include <algorithm>
#include <functional>

namespace Disp {
namespace {
// ---------------------------------------------------------------- look
constexpr uint16_t C_BG = RGB565(7, 10, 16), C_PANEL = RGB565(20, 27, 40), C_PANEL2 = RGB565(38, 48, 66), C_LINE = RGB565(56, 68, 88);
constexpr uint16_t C_TEXT = RGB565(232, 238, 244), C_MUTED = RGB565(140, 150, 162), C_ACCENT = RGB565(56, 189, 248);
constexpr uint16_t C_OK = RGB565(34, 197, 94), C_WARN = RGB565(245, 158, 11), C_CRIT = RGB565(239, 68, 68), C_FAULT = RGB565(168, 85, 247);
constexpr uint16_t C_WARN_BG = RGB565(64, 42, 6), C_CRIT_BG = RGB565(78, 16, 16), C_FAULT_BG = RGB565(48, 22, 70), C_ON = RGB565(14, 116, 144);

uint16_t levelColor(uint8_t l) { return l == LVL_CRIT ? C_CRIT : l == LVL_FAULT ? C_FAULT : l == LVL_WARN ? C_WARN : C_TEXT; }
uint16_t levelBg(uint8_t l) { return l == LVL_CRIT ? C_CRIT_BG : l == LVL_FAULT ? C_FAULT_BG : l == LVL_WARN ? C_WARN_BG : C_PANEL; }
const char* levelWord(uint8_t l) { return l == LVL_CRIT ? "CRIT" : l == LVL_FAULT ? "FAULT" : l == LVL_WARN ? "WARN" : "OK"; }

const uint8_t* const NUM_FONTS[] = {u8g2_font_logisoso58_tn, u8g2_font_logisoso46_tn, u8g2_font_logisoso38_tn, u8g2_font_logisoso32_tn,
                                    u8g2_font_logisoso26_tn, u8g2_font_logisoso20_tn, u8g2_font_logisoso16_tn};
const uint8_t* const TXT_FONTS[] = {u8g2_font_helvB24_tf, u8g2_font_helvB18_tf, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf,
                                    u8g2_font_helvB10_tf, u8g2_font_helvB08_tf};
const uint8_t* const F_SMALL = u8g2_font_helvR08_tf;
const uint8_t* const F_LABEL = u8g2_font_helvB10_tf;
const uint8_t* const F_LABEL_L = u8g2_font_helvB12_tf;

struct Box { int16_t x1, y1; uint16_t w, h; };
Box measure(Arduino_GFX* g, const uint8_t* font, const std::string& s) {
  g->setFont(font);
  Box b{0, 0, 0, 0};
  g->getTextBounds(s.c_str(), 0, 0, &b.x1, &b.y1, &b.w, &b.h);
  return b;
}
// align: 0 left, 1 centre, 2 right (within x..x+w); text vertically centred in y..y+h
void text(Arduino_GFX* g, const uint8_t* font, const std::string& s, int x, int y, int w, int h, uint16_t color, int align = 0) {
  if (s.empty()) return;
  Box b = measure(g, font, s);
  int tx = align == 1 ? x + (w - (int)b.w) / 2 : align == 2 ? x + w - (int)b.w : x;
  int ty = y + (h - (int)b.h) / 2;
  g->setTextColor(color);
  g->setCursor(tx - b.x1, ty - b.y1);
  g->print(s.c_str());
}
bool numeric(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  return s != "-" && s != "--";
}
// Biggest font of the list that fits; returns it (and its box).
const uint8_t* fit(Arduino_GFX* g, const std::string& s, int w, int h, Box* out = nullptr) {
  bool num = numeric(s);
  const uint8_t* const* list = num ? NUM_FONTS : TXT_FONTS;
  size_t n = num ? sizeof(NUM_FONTS) / sizeof(*NUM_FONTS) : sizeof(TXT_FONTS) / sizeof(*TXT_FONTS);
  for (size_t i = 0; i < n; i++) {
    Box b = measure(g, list[i], s);
    if ((int)b.w <= w && (int)b.h <= h) { if (out) *out = b; return list[i]; }
  }
  if (out) *out = measure(g, list[n - 1], s);
  return list[n - 1];
}
void bigText(Arduino_GFX* g, const std::string& s, int x, int y, int w, int h, uint16_t color, int align = 1) {
  const uint8_t* f = fit(g, s, w, h);
  text(g, f, s, x, y, w, h, color, align);
}

float frac(const SensorMeta& m, float v) {
  if (isnan(v)) return 0;
  float f = (v - m.lo) / (m.hi - m.lo);
  return f < 0 ? 0 : f > 1 ? 1 : f;
}
struct Zone { float from, to; uint16_t color; };
std::vector<Zone> zones(const SensorMeta& m) {   // coloured parts of the scale (warn / crit)
  std::vector<Zone> z;
  auto add = [&](float a, float b, uint16_t c) { if (!isnan(a) && !isnan(b) && b > a) z.push_back({frac(m, a), frac(m, b), c}); };
  float lo = m.lo, hi = m.hi;
  if (!isnan(m.cl)) add(lo, m.cl, C_CRIT);
  if (!isnan(m.wl)) add(isnan(m.cl) ? lo : m.cl, m.wl, C_WARN);
  if (!isnan(m.wh)) add(m.wh, isnan(m.ch) ? hi : m.ch, C_WARN);
  if (!isnan(m.ch)) add(m.ch, hi, C_CRIT);
  return z;
}
uint16_t valueColor(const Val& v) { return v.st != ST_OK ? C_MUTED : v.lvl == LVL_OK ? C_TEXT : levelColor(v.lvl); }
uint16_t fillColor(const Val& v) { return v.lvl == LVL_OK ? C_ACCENT : levelColor(v.lvl); }

void panel(Arduino_GFX* g, int x, int y, int w, int h, uint16_t c) { g->fillRoundRect(x, y, w, h, std::min(8, h / 4), c); }

size_t hashOf(const std::string& s) { return std::hash<std::string>()(s); }
std::string fmtLimit(float v, int dec) { if (isnan(v)) return "off"; char b[20]; snprintf(b, sizeof b, "%.*f", dec, v); return b; }
}  // namespace

// ---------------------------------------------------------------- pages
int Ui::pageCount() const {
  return (int)model.layout().pages.size() + (model.layout().relaysPage && !model.relays().empty() ? 1 : 0) + 1;
}
int Ui::pageKind(int p) const {
  int n = (int)model.layout().pages.size();
  if (p < n) return p;
  if (p == n && model.layout().relaysPage && !model.relays().empty()) return PAGE_RELAYS;
  return PAGE_SETUP;
}
std::string Ui::pageName(int p) const {
  int k = pageKind(p);
  if (k == PAGE_RELAYS) return "Relays";
  if (k == PAGE_SETUP) return "Setup";
  const std::string& n = model.layout().pages[k].name;
  return n.empty() ? "Page " + std::to_string(k + 1) : n;
}
bool Ui::relayPending(int r) const { return pending.count(r) > 0; }

// ---------------------------------------------------------------- geometry
std::vector<Ui::Region> Ui::regions(int w, int h) const {
  std::vector<Region> v;
  bool small = h <= 240 || w <= 240;
  int SH = small ? 28 : 36, NH = small ? 34 : 48;
  if (flashUntil) { v.push_back({0, 0, w, h, Hit::NONE, 9}); return v; }
  v.push_back({0, 0, w, SH, Hit::NONE, 0});                         // status bar
  int mw = small ? 58 : 76;
  v.push_back({w - mw - 4, 3, mw, SH - 6, Hit::MUTE, -1});           // (hit only, a = -1)
  // navigation
  int tabs = pageCount();
  v.push_back({0, h - NH, w, NH, Hit::TAB, -2});                     // nav bar (drawn)
  if (tabs * (small ? 56 : 64) <= w) {
    int tw = w / tabs;
    for (int i = 0; i < tabs; i++) v.push_back({i * tw, h - NH, i == tabs - 1 ? w - i * tw : tw, NH, Hit::TAB, -1, i});
  } else {
    v.push_back({0, h - NH, 56, NH, Hit::PREV, -1});
    v.push_back({w - 56, h - NH, 56, NH, Hit::NEXT, -1});
  }
  int cy = SH, ch = h - SH - NH;
  // alarm banner (pushes the page down)
  uint8_t worst = model.worstLevel();
  if (model.layout().alarmPopup && worst == LVL_CRIT && !model.mainMuted && !bannerDismissed && !editor.open) {
    int bh = small ? 34 : 48;
    v.push_back({0, cy, w, bh, Hit::BANNER, 0});
    cy += bh; ch -= bh;
  }
  const int pad = 4, gap = 4;
  if (editor.open) {
    v.push_back({0, cy, w, ch, Hit::NONE, 1});                       // the editor panel
    int rh = (ch - 2 * pad) / 6;
    int bw = std::min(64, w / 7);
    for (int i = 0; i < 4; i++) {
      int ry = cy + pad + rh * (i + 1);
      v.push_back({w - pad - 3 * bw - 2 * gap, ry + 2, bw, rh - 4, Hit::LIM_MINUS, -1, i});
      v.push_back({w - pad - 2 * bw - gap, ry + 2, bw, rh - 4, Hit::LIM_PLUS, -1, i});
      v.push_back({w - pad - bw, ry + 2, bw, rh - 4, Hit::LIM_TOGGLE, -1, i});
    }
    int by = cy + pad + rh * 5 + 2;
    v.push_back({pad, by, w / 2 - pad - gap / 2, rh - 4, Hit::LIM_CANCEL, -1});
    v.push_back({w / 2 + gap / 2, by, w / 2 - pad - gap / 2, rh - 4, Hit::LIM_SAVE, -1});
    return v;
  }
  int k = pageKind(page);
  if (k == PAGE_SETUP) {
    v.push_back({0, cy, w, ch, Hit::NONE, 3});
    int bh = small ? 30 : 40, by = cy + ch - bh - pad, bw = (w - 2 * pad - 3 * gap) / 4;
    v.push_back({pad, by, bw, bh, Hit::SET_BRIGHT_DOWN, -1});
    v.push_back({pad + (bw + gap), by, bw, bh, Hit::SET_BRIGHT_UP, -1});
    v.push_back({pad + 2 * (bw + gap), by, bw, bh, Hit::SET_ROTATE, -1});
    v.push_back({pad + 3 * (bw + gap), by, bw, bh, Hit::SET_RESTART, -1});
    return v;
  }
  v.push_back({0, cy, w, ch, Hit::NONE, 2});                         // page background
  if (k == PAGE_RELAYS) {
    int n = (int)model.relays().size();
    int cols = w > h ? 3 : 2, maxRows = std::max(1, ch / (small ? 52 : 64));
    int rows = std::min(maxRows, (n + cols - 1) / cols);
    int cw = (w - 2 * pad - gap * (cols - 1)) / cols, rh = (ch - 2 * pad - gap * (rows - 1)) / std::max(1, rows);
    rh = std::min(rh, small ? 70 : 96);
    for (int i = 0; i < n && i < cols * rows; i++)
      v.push_back({pad + (i % cols) * (cw + gap), cy + pad + (i / cols) * (rh + gap), cw, rh, Hit::RELAY, i, -1});
    return v;
  }
  const Page& p = model.layout().pages[k];
  std::vector<Cell> cells = placeTiles(p);
  int cw = (w - 2 * pad - gap * (p.cols - 1)) / p.cols, rh = (ch - 2 * pad - gap * (p.rows - 1)) / p.rows;
  for (size_t i = 0; i < p.tiles.size(); i++) {
    const Cell& c = cells[i];
    if (!c.cs) continue;
    int x = pad + c.col * (cw + gap), y = cy + pad + c.row * (rh + gap);
    int tw = c.cs * cw + (c.cs - 1) * gap, th = c.rs * rh + (c.rs - 1) * gap;
    if (p.tiles[i].widget == W_RELAY) v.push_back({x, y, tw, th, Hit::RELAY, model.relayIndex(p.tiles[i].ref), (int)i});
    else v.push_back({x, y, tw, th, Hit::TILE, model.sensorIndex(p.tiles[i].ref), (int)i});
  }
  return v;
}

// ---------------------------------------------------------------- what a region shows (to skip unchanged ones)
std::string Ui::signature(const Region& r, uint32_t now) const {
  char geo[48]; snprintf(geo, sizeof geo, "%d,%d,%d,%d,%d,%d,%d|", r.x, r.y, r.w, r.h, (int)r.kind, r.a, r.b);
  std::string s = geo;
  if (r.kind == Hit::NONE && r.a == 9) return s + std::to_string((now / 250) % 2);
  if (r.kind == Hit::NONE && r.a == 0) {
    uint8_t lvl = std::max<int>(levelRank(model.worstLevel()), levelRank(model.mainLevel)) == levelRank(model.mainLevel) ? model.mainLevel : model.worstLevel();
    return s + model.mainName() + "|" + info.name + "|" + std::to_string(info.link) + std::to_string(lvl) + (model.mainMuted ? "M" : "") +
           (lvl == LVL_CRIT && !model.mainMuted ? std::to_string((now / 500) % 2) : "");
  }
  if (r.kind == Hit::TAB) {
    s += std::to_string(page) + "/" + std::to_string(pageCount());
    if (toastUntil && now < toastUntil) s += "T" + toastText;
    for (int i = 0; i < pageCount(); i++) s += pageName(i) + ",";
    return s;
  }
  if (r.kind == Hit::BANNER) {
    for (size_t i = 0; i < model.sensors().size(); i++)
      if (model.sensorVal((int)i).lvl == LVL_CRIT) s += model.sensors()[i].shortLabel() + formatValue(model.sensors()[i], model.sensorVal((int)i)) + ",";
    return s + std::to_string((now / 500) % 2);
  }
  if (r.kind == Hit::NONE && r.a == 1) {
    s += std::to_string(editor.sensor);
    for (int i = 0; i < 4; i++) s += fmtLimit(editor.on[i] ? editor.v[i] : NAN, 3) + ",";
    if (editor.sensor >= 0 && editor.sensor < (int)model.sensors().size()) s += formatValue(model.sensors()[editor.sensor], model.sensorVal(editor.sensor));
    return s;
  }
  if (r.kind == Hit::NONE && r.a == 3)
    return s + info.name + info.firmware + info.profile + std::to_string(info.link) + std::to_string(info.paired) + std::to_string(info.boardId) +
           std::to_string(info.channel) + info.wifiSsid + info.wifiIp + info.apName + info.apIp + std::to_string(info.brightness) + model.mainName() + model.mainFirmware() +
           std::to_string(model.sensors().size()) + "/" + std::to_string(model.relays().size());
  if (r.kind == Hit::NONE && r.a == 2) return s + std::to_string(page);
  if (r.kind == Hit::TILE) {
    if (r.a < 0) return s + "missing";
    const SensorMeta& m = model.sensors()[r.a];
    const Val& v = model.sensorVal(r.a);
    const Tile& t = model.layout().pages[pageKind(page)].tiles[r.b];
    char q[16]; snprintf(q, sizeof q, "%d", (int)(frac(m, v.v) * 400));
    return s + (t.label.empty() ? m.shortLabel() : t.label) + widgetName(t.widget) + formatValue(m, v) + q + std::to_string(v.lvl) + m.unit +
           fmtLimit(m.wl, 3) + fmtLimit(m.wh, 3) + fmtLimit(m.cl, 3) + fmtLimit(m.ch, 3);
  }
  if (r.kind == Hit::RELAY) {
    if (r.a < 0) return s + "missing";
    const Val& v = model.relayVal(r.a);
    return s + model.relays()[r.a].name + std::to_string(v.st) + (v.v > 0.5f ? "1" : "0") + (relayPending(r.a) ? "P" : "") + (info.canRelays ? "" : "x");
  }
  return s;
}

// ---------------------------------------------------------------- drawing
void Ui::draw(Surface& s, uint32_t now) {
  if (s.width() != W || s.height() != H) { W = s.width(); H = s.height(); drawn.clear(); }
  if (flashUntil && now >= flashUntil) { flashUntil = 0; drawn.clear(); }
  if (toastUntil && now >= toastUntil) toastUntil = 0;
  for (auto it = pending.begin(); it != pending.end();) {   // a relay shows "pending" until its value changes (or 4 s)
    bool changed = it->first < (int)model.relays().size() && (model.relayVal(it->first).v > 0.5f) != (pendingFrom[it->first] > 0.5f);
    if (changed || now - it->second > 4000) { pendingFrom.erase(it->first); it = pending.erase(it); } else ++it;
  }
  if (model.worstLevel() != LVL_CRIT) bannerDismissed = false;
  bool any = false;
  std::vector<Region> rs = regions(W, H);
  std::map<std::string, size_t> next;
  for (auto& r : rs) {
    if (r.a == -1 && r.kind != Hit::TILE && r.kind != Hit::RELAY) continue;   // hit-only
    std::string sig = signature(r, now);
    size_t h = hashOf(sig);
    std::string key = std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string((int)r.kind) + "," + std::to_string(r.a) + "," + std::to_string(r.b);
    next[key] = h;
    auto it = drawn.find(key);
    if (it != drawn.end() && it->second == h) continue;
    // a page background drawn again means everything on it must be drawn again
    if (r.kind == Hit::NONE && (r.a == 2 || r.a == 9)) { drawn.clear(); }
    int ox, oy;
    Arduino_GFX* g = s.begin(r.x, r.y, r.w, r.h, ox, oy);
    if (!g) continue;
    drawRegion(g, ox, oy, r, now);
    s.end();
    any = true;
    drawn[key] = h;
  }
  for (auto& kv : next) drawn[kv.first] = kv.second;
  if (any) s.present();
}

void Ui::drawRegion(Arduino_GFX* g, int x, int y, const Region& r, uint32_t now) {
  int w = r.w, h = r.h;
  bool small = H <= 240 || W <= 240;
  g->setUTF8Print(true);
  g->setTextWrap(false);
  // identify: flashing screen
  if (r.kind == Hit::NONE && r.a == 9) {
    bool on = (now / 250) % 2 == 0;
    g->fillRect(x, y, w, h, on ? C_TEXT : C_BG);
    text(g, TXT_FONTS[0], info.name, x, y, w, h, on ? C_BG : C_TEXT, 1);
    return;
  }
  // ---- status bar
  if (r.kind == Hit::NONE && r.a == 0) {
    g->fillRect(x, y, w, h, C_BG);
    g->drawFastHLine(x, y + h - 1, w, C_LINE);
    uint8_t lvl = levelRank(model.mainLevel) >= levelRank(model.worstLevel()) ? model.mainLevel : model.worstLevel();
    bool blink = lvl == LVL_CRIT && !model.mainMuted && (now / 500) % 2;
    if (blink) g->fillRect(x, y, w, h - 1, C_CRIT_BG);
    const char* linkTxt = info.link == 1 ? "ESP-NOW" : info.link == 2 ? "Wi-Fi" : "NO LINK";
    uint16_t linkCol = info.link == 1 ? C_OK : info.link == 2 ? C_WARN : C_CRIT;
    int mw = small ? 58 : 76;
    std::string name = model.hasCatalog() ? model.mainName() : info.name;
    Box lb = measure(g, F_SMALL, linkTxt);
    int pillW = lb.w + 12;
    text(g, small ? F_LABEL : F_LABEL_L, name, x + 8, y, w - mw - pillW * 2 - 30, h, C_TEXT, 0);
    // level pill + link pill
    int px = x + w - mw - 8 - pillW;
    g->fillRoundRect(px, y + h / 2 - 9, pillW, 18, 9, C_PANEL2);
    text(g, F_SMALL, linkTxt, px, y + h / 2 - 9, pillW, 18, linkCol, 1);
    const char* lw = levelWord(lvl);
    Box wb = measure(g, F_SMALL, lw);
    int lwW = wb.w + 12; int lx = px - 6 - lwW;
    g->fillRoundRect(lx, y + h / 2 - 9, lwW, 18, 9, lvl == LVL_OK ? C_PANEL2 : levelBg(lvl));
    text(g, F_SMALL, lw, lx, y + h / 2 - 9, lwW, 18, lvl == LVL_OK ? C_OK : levelColor(lvl), 1);
    // mute button
    int bx = x + w - mw - 4;
    g->fillRoundRect(bx, y + 3, mw, h - 6, 6, model.mainMuted ? C_WARN_BG : C_PANEL2);
    text(g, F_LABEL, model.mainMuted ? "MUTED" : "MUTE", bx, y + 3, mw, h - 6, model.mainMuted ? C_WARN : C_TEXT, 1);
    return;
  }
  // ---- navigation bar (or a short message)
  if (r.kind == Hit::TAB) {
    g->fillRect(x, y, w, h, C_PANEL);
    g->drawFastHLine(x, y, w, C_LINE);
    if (toastUntil && now < toastUntil) { text(g, F_LABEL_L, toastText, x + 8, y, w - 16, h, C_ACCENT, 1); return; }
    int tabs = pageCount();
    if (tabs * (small ? 56 : 64) <= w) {
      int tw = w / tabs;
      for (int i = 0; i < tabs; i++) {
        int tx = x + i * tw, ww = i == tabs - 1 ? w - i * tw : tw;
        if (i == page) { g->fillRoundRect(tx + 3, y + 5, ww - 6, h - 10, 8, C_PANEL2); g->fillRect(tx + 10, y + h - 6, ww - 20, 3, C_ACCENT); }
        text(g, small ? F_SMALL : F_LABEL, pageName(i), tx + 4, y, ww - 8, h, i == page ? C_TEXT : C_MUTED, 1);
      }
    } else {
      text(g, F_LABEL_L, "<", x, y, 56, h, C_TEXT, 1);
      text(g, F_LABEL_L, ">", x + w - 56, y, 56, h, C_TEXT, 1);
      text(g, F_LABEL_L, pageName(page), x + 56, y, w - 112, h - 10, C_TEXT, 1);
      int dots = tabs, dw = 10, dx = x + (w - dots * dw) / 2;
      for (int i = 0; i < dots; i++) g->fillCircle(dx + i * dw + dw / 2, y + h - 8, 3, i == page ? C_ACCENT : C_LINE);
    }
    return;
  }
  // ---- alarm banner
  if (r.kind == Hit::BANNER) {
    bool on = (now / 500) % 2 == 0;
    g->fillRect(x, y, w, h, on ? C_CRIT : C_CRIT_BG);
    std::string s;
    for (size_t i = 0; i < model.sensors().size() && s.size() < 40; i++)
      if (model.sensorVal((int)i).lvl == LVL_CRIT) {
        const SensorMeta& m = model.sensors()[i];
        s += (s.empty() ? "" : "  ") + m.shortLabel() + " " + formatValue(m, model.sensorVal((int)i)) + m.unit;
      }
    text(g, small ? F_LABEL : F_LABEL_L, s, x + 8, y, w - 16 - (small ? 70 : 96), h, C_TEXT, 0);
    text(g, F_SMALL, "tap: mute", x + w - (small ? 70 : 96), y, (small ? 66 : 90), h, C_TEXT, 2);
    return;
  }
  // ---- page background
  if (r.kind == Hit::NONE && r.a == 2) {
    g->fillRect(x, y, w, h, C_BG);
    if (pageKind(page) != PAGE_RELAYS && pageKind(page) < (int)model.layout().pages.size() && model.layout().pages[pageKind(page)].tiles.empty())
      text(g, F_LABEL, model.hasCatalog() ? "Empty page: add tiles in the layout editor" : "Waiting for the main board…", x + 8, y, w - 16, h, C_MUTED, 1);
    return;
  }
  // ---- setup page
  if (r.kind == Hit::NONE && r.a == 3) {
    g->fillRect(x, y, w, h, C_BG);
    int rh = small ? 17 : 26, ty = y + 6;
    auto row = [&](const std::string& k, const std::string& v) {
      text(g, F_SMALL, k, x + 8, ty, 90, rh, C_MUTED, 0);
      text(g, small ? F_SMALL : F_LABEL, v, x + (small ? 86 : 104), ty, w - (small ? 92 : 112), rh, C_TEXT, 0);
      ty += rh;
    };
    row("Display", info.name + "  " + info.firmware);
    row("Screen", info.profile + ", brightness " + std::to_string(info.brightness) + "%");
    row("ESP-NOW", info.paired ? "board " + std::to_string(info.boardId) + (info.link == 1 ? ", linked" : ", no link") + ", ch " + std::to_string(info.channel)
                               : "not paired: Accept it on the main board's Boards page");
    row("Wi-Fi", info.wifiIp.empty() ? "not joined to " + (model.mainAp().empty() ? std::string("the main board") : model.mainAp()) : info.wifiSsid + "  " + info.wifiIp);
    row("Main board", model.hasCatalog() ? model.mainName() + " " + model.mainFirmware() + ", " + std::to_string(model.sensors().size()) + " values, " +
                                               std::to_string(model.relays().size()) + " relays" : "nothing received yet");
    row("Edit layout", "http://" + (info.wifiIp.empty() ? info.apIp : info.wifiIp) + "/");
    row("", info.wifiIp.empty() ? "on Wi-Fi " + info.apName : "or on Wi-Fi " + info.apName);
    if (!info.wifiIp.empty()) row("", "at http://" + info.apIp + "/");
    // buttons
    int bh = small ? 30 : 40, by = y + h - bh - 4, bw = (w - 8 - 12) / 4;
    const char* labels[] = {"Dimmer", "Brighter", "Rotate", "Restart"};
    for (int i = 0; i < 4; i++) {
      int bx = x + 4 + i * (bw + 4);
      g->fillRoundRect(bx, by, bw, bh, 8, C_PANEL2);
      text(g, small ? F_SMALL : F_LABEL, labels[i], bx, by, bw, bh, C_TEXT, 1);
    }
    return;
  }
  // ---- limit editor
  if (r.kind == Hit::NONE && r.a == 1) {
    g->fillRect(x, y, w, h, C_BG);
    if (editor.sensor < 0 || editor.sensor >= (int)model.sensors().size()) return;
    const SensorMeta& m = model.sensors()[editor.sensor];
    const int pad = 4, gap = 4;
    int rh = (h - 2 * pad) / 6, bw = std::min(64, w / 7);
    text(g, small ? F_LABEL : F_LABEL_L, m.name + "  " + formatValue(m, model.sensorVal(editor.sensor)) + " " + m.unit, x + 8, y + pad, w - 16, rh, C_TEXT, 0);
    const char* names[] = {"Warn low", "Warn high", "Crit low", "Crit high"};
    for (int i = 0; i < 4; i++) {
      int ry = y + pad + rh * (i + 1);
      uint16_t c = i >= 2 ? C_CRIT : C_WARN;
      text(g, F_LABEL, names[i], x + 8, ry, w / 3, rh, c, 0);
      text(g, small ? F_LABEL : F_LABEL_L, editor.on[i] ? fmtLimit(editor.v[i], m.decimals + (editor.step < 1 ? 1 : 0)) + " " + m.unit : "off",
           x + w / 3, ry, w - w / 3 - 3 * bw - 2 * gap - 8, rh, editor.on[i] ? C_TEXT : C_MUTED, 0);
      const char* bl[] = {"-", "+", editor.on[i] ? "off" : "on"};
      for (int k = 0; k < 3; k++) {
        int bx = x + w - pad - (3 - k) * bw - (2 - k) * gap;
        g->fillRoundRect(bx, ry + 2, bw, rh - 4, 8, C_PANEL2);
        text(g, k < 2 ? F_LABEL_L : F_LABEL, bl[k], bx, ry + 2, bw, rh - 4, C_TEXT, 1);
      }
    }
    int by = y + pad + rh * 5 + 2, hw = w / 2 - pad - gap / 2;
    g->fillRoundRect(x + pad, by, hw, rh - 4, 8, C_PANEL2);
    text(g, F_LABEL_L, "Cancel", x + pad, by, hw, rh - 4, C_TEXT, 1);
    g->fillRoundRect(x + w / 2 + gap / 2, by, hw, rh - 4, 8, C_ON);
    text(g, F_LABEL_L, "Save", x + w / 2 + gap / 2, by, hw, rh - 4, C_TEXT, 1);
    return;
  }
  // ---- relay tile
  if (r.kind == Hit::RELAY) {
    g->fillRect(x, y, w, h, C_BG);
    if (r.a < 0) { panel(g, x, y, w, h, C_PANEL); text(g, F_SMALL, "relay not found", x, y, w, h, C_MUTED, 1); return; }
    const Val& v = model.relayVal(r.a);
    bool on = v.st == ST_OK && v.v > 0.5f;
    panel(g, x, y, w, h, on ? C_ON : C_PANEL);
    if (relayPending(r.a)) { g->drawRoundRect(x, y, w, h, std::min(8, h / 4), C_ACCENT); g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, std::min(8, h / 4), C_ACCENT); }
    int lh = std::min(h / 3, small ? 16 : 22);
    text(g, small ? F_SMALL : F_LABEL, model.relays()[r.a].name, x + 8, y + 4, w - 16, lh, on ? C_TEXT : C_MUTED, 0);
    std::string st = v.st != ST_OK ? statusShort(v.st) : on ? "ON" : "OFF";
    const uint8_t* f = fit(g, st, w - 16, h - lh - 10);
    text(g, f, st, x + 8, y + lh + 4, w - 16, h - lh - 10, v.st != ST_OK ? C_MUTED : on ? C_TEXT : C_MUTED, 1);
    return;
  }
  // ---- sensor tile
  if (r.kind != Hit::TILE) return;
  g->fillRect(x, y, w, h, C_BG);
  if (r.a < 0) { panel(g, x, y, w, h, C_PANEL); text(g, F_SMALL, "not on the main board", x, y, w, h, C_MUTED, 1); return; }
  const SensorMeta& m = model.sensors()[r.a];
  const Val& v = model.sensorVal(r.a);
  const Tile& t = model.layout().pages[pageKind(page)].tiles[r.b];
  std::string label = t.label.empty() ? m.shortLabel() : t.label;
  std::string val = formatValue(m, v);
  uint16_t vc = valueColor(v);
  int lh = std::max(14, std::min(h / 5, small ? 16 : 22));
  const uint8_t* lf = lh >= 20 ? F_LABEL_L : lh >= 16 ? F_LABEL : F_SMALL;
  switch (t.widget) {
    case W_DIAL: {
      panel(g, x, y, w, h, v.lvl == LVL_OK ? C_PANEL : levelBg(v.lvl));
      int r0 = std::min(w / 2 - 6, (h - lh - 6) / 2 + (h - lh) / 10);
      int cx = x + w / 2, cy = y + 4 + r0 + 2;
      if (r0 < 16) { bigText(g, val, x + 4, y, w - 8, h, vc); return; }
      int th = std::max(6, r0 / 5);
      g->fillArc(cx, cy, r0, r0 - th, 135, 405, C_PANEL2);
      for (auto& z : zones(m)) g->fillArc(cx, cy, r0, r0 - 3, 135 + 270 * z.from, 135 + 270 * z.to, z.color);
      if (v.st == ST_OK && !isnan(v.v)) {
        float f = frac(m, v.v);
        if (f > 0.004f) g->fillArc(cx, cy, r0 - 4, r0 - th, 135, 135 + 270 * f, fillColor(v));
        float a = (135 + 270 * f) * DEGTORAD;
        g->drawLine(cx + (int)((r0 - th - 2) * cosf(a)), cy + (int)((r0 - th - 2) * sinf(a)), cx + (int)((r0 + 1) * cosf(a)), cy + (int)((r0 + 1) * sinf(a)), C_TEXT);
      }
      int iw = (int)((r0 - th) * 1.25f), ih = (int)((r0 - th) * 0.8f);
      bigText(g, val, cx - iw / 2, cy - ih / 2 - ih / 8, iw, ih, vc);
      text(g, F_SMALL, m.unit, cx - iw / 2, cy + ih / 2 - ih / 8, iw, std::max(12, ih / 3), C_MUTED, 1);
      text(g, lf, label, x + 4, y + h - lh - 2, w - 8, lh, v.lvl == LVL_OK ? C_MUTED : levelColor(v.lvl), 1);
      return;
    }
    case W_BAR:
    case W_MARKER: {
      panel(g, x, y, w, h, v.lvl == LVL_OK ? C_PANEL : levelBg(v.lvl));
      int barH = t.widget == W_BAR ? std::max(6, std::min(h / 5, 16)) : std::max(10, std::min(h / 4, 20));
      int by = y + h - barH - 8, bx = x + 10, bw = w - 20;
      int top = y + 4, avail = by - top - 4;
      Box ub = measure(g, F_SMALL, m.unit);
      int lw = std::max(30, (int)measure(g, lf, label).w + 6);
      text(g, lf, label, x + 10, top, lw, std::min(avail, lh + 4), v.lvl == LVL_OK ? C_MUTED : levelColor(v.lvl), 0);
      int vx = x + 10 + lw, vw = w - 20 - lw - (int)ub.w - 4;
      bigText(g, val, vx, top, vw, avail, vc, 2);
      text(g, F_SMALL, m.unit, x + w - 10 - ub.w, top, ub.w, avail, C_MUTED, 2);
      float f = frac(m, v.v);
      bool have = v.st == ST_OK && !isnan(v.v);
      if (t.widget == W_BAR) {
        g->fillRoundRect(bx, by, bw, barH, barH / 2, C_PANEL2);
        if (have && f > 0) g->fillRoundRect(bx, by, std::max(barH, (int)(bw * f)), barH, barH / 2, fillColor(v));
        for (auto& z : zones(m)) g->fillRect(bx + (int)(bw * z.from), by + barH + 2, std::max(1, (int)(bw * (z.to - z.from))), 3, z.color);
      } else {   // marker: scale with threshold ticks and a needle (the original OLED look)
        int ly = by + barH / 2;
        g->drawFastHLine(bx, ly, bw, C_LINE);
        for (auto& z : zones(m)) g->fillRect(bx + (int)(bw * z.from), ly - 1, std::max(1, (int)(bw * (z.to - z.from))), 3, z.color);
        float lims[] = {m.wl, m.wh, m.cl, m.ch};
        for (int i = 0; i < 4; i++) if (!isnan(lims[i])) { int tx = bx + (int)(bw * frac(m, lims[i])); g->fillRect(tx - 1, by, 3, barH, i >= 2 ? C_CRIT : C_WARN); }
        if (have) { int nx = bx + (int)(bw * f); g->fillTriangle(nx - barH / 2, by - 2, nx + barH / 2, by - 2, nx, by + barH - 2, fillColor(v)); g->drawFastVLine(nx, by, barH + 4, C_TEXT); }
      }
      return;
    }
    default: {   // number
      panel(g, x, y, w, h, v.lvl == LVL_OK ? C_PANEL : levelBg(v.lvl));
      text(g, lf, label, x + 8, y + 4, w / 2, lh, v.lvl == LVL_OK ? C_MUTED : levelColor(v.lvl), 0);
      text(g, F_SMALL, m.unit, x + w / 2, y + 4, w / 2 - 8, lh, C_MUTED, 2);
      bigText(g, val, x + 6, y + lh + 6, w - 12, h - lh - 12, vc);
      return;
    }
  }
}

// ---------------------------------------------------------------- touch
const Ui::Region* Ui::hitTest(int x, int y, int w, int h, std::vector<Region>& store) const {
  store = regions(w, h);
  const Region* best = nullptr;
  for (auto& r : store) {
    if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) continue;
    if (r.kind == Hit::NONE) continue;
    if (r.kind == Hit::TAB && r.a == -2) continue;   // nav bar background
    best = &r;                                       // later (smaller) regions win
  }
  return best;
}

void Ui::openEditor(int sensor) {
  if (sensor < 0 || sensor >= (int)model.sensors().size()) return;
  const SensorMeta& m = model.sensors()[sensor];
  editor.open = true; editor.sensor = sensor;
  float lims[] = {m.wl, m.wh, m.cl, m.ch};
  for (int i = 0; i < 4; i++) { editor.on[i] = !isnan(lims[i]); editor.v[i] = editor.on[i] ? lims[i] : (i % 2 ? m.hi : m.lo); }
  editor.step = niceStep(m.lo, m.hi);
  if (m.decimals == 0 && editor.step < 1) editor.step = 1;
  invalidate();
}

void Ui::touch(bool isDown, int x, int y, uint32_t now) {
  if (W == 0) return;
  if (isDown && !down) { down = true; longFired = false; moved = false; startX = lastX = x; startY = lastY = y; downAt = now ? now : 1; return; }
  if (isDown) {
    lastX = x; lastY = y;
    if (abs(x - startX) > 12 || abs(y - startY) > 12) moved = true;
    if (!longFired && !moved && now - downAt >= 700) {   // long press on a value: limit editor
      std::vector<Region> st;
      const Region* r = hitTest(startX, startY, W, H, st);
      if (r && r->kind == Hit::TILE && r->a >= 0) { longFired = true; openEditor(r->a); }
    }
    return;
  }
  if (!down) return;
  down = false;
  if (longFired) return;
  if (flashUntil) { flashUntil = 0; invalidate(); return; }
  int dx = lastX - startX, dy = lastY - startY;
  if (!editor.open && abs(dx) > 60 && abs(dx) > 2 * abs(dy)) {   // swipe: next / previous page
    int n = pageCount();
    page = (page + (dx < 0 ? 1 : n - 1)) % n;
    invalidate();
    return;
  }
  if (moved) return;
  std::vector<Region> st;
  const Region* r = hitTest(startX, startY, W, H, st);
  if (r) tap(*r, now);
}

void Ui::tap(const Region& r, uint32_t now) {
  switch (r.kind) {
    case Hit::MUTE: if (act.mute) act.mute(model.mainMuted ? 0 : 15); toast(model.mainMuted ? "Unmuting" : "Muting for 15 min", now); return;
    case Hit::BANNER: if (act.mute) act.mute(15); bannerDismissed = true; invalidate(); return;
    case Hit::TAB: editor.open = false; page = r.b; invalidate(); return;
    case Hit::PREV: editor.open = false; page = (page + pageCount() - 1) % pageCount(); invalidate(); return;
    case Hit::NEXT: editor.open = false; page = (page + 1) % pageCount(); invalidate(); return;
    case Hit::RELAY: {
      if (r.a < 0) return;
      if (!info.canRelays) { toast("No link to the main board", now); return; }
      const Val& v = model.relayVal(r.a);
      if (v.st != ST_OK) { toast("That relay's board is out of reach", now); return; }
      bool on = !(v.v > 0.5f);
      pending[r.a] = now ? now : 1; pendingFrom[r.a] = v.v;
      if (act.relay) act.relay(model.relays()[r.a].board, model.relays()[r.a].index, on);
      return;
    }
    case Hit::TILE: return;   // long press opens the limit editor
    case Hit::SET_BRIGHT_DOWN: if (act.brightness) act.brightness(-10); return;
    case Hit::SET_BRIGHT_UP: if (act.brightness) act.brightness(+10); return;
    case Hit::SET_ROTATE: if (act.rotate) act.rotate(); return;
    case Hit::SET_RESTART: if (act.restart) act.restart(); return;
    case Hit::LIM_MINUS: case Hit::LIM_PLUS: {
      int i = r.b;
      if (!editor.on[i]) { editor.on[i] = true; }
      else editor.v[i] += (r.kind == Hit::LIM_PLUS ? 1 : -1) * editor.step;
      editor.v[i] = roundf(editor.v[i] / editor.step) * editor.step;
      return;
    }
    case Hit::LIM_TOGGLE: editor.on[r.b] = !editor.on[r.b]; return;
    case Hit::LIM_CANCEL: editor.open = false; invalidate(); return;
    case Hit::LIM_SAVE: {
      if (editor.sensor >= 0 && editor.sensor < (int)model.sensors().size()) {
        const SensorMeta& m = model.sensors()[editor.sensor];
        float old[] = {m.wl, m.wh, m.cl, m.ch};
        int sent = 0;
        for (int i = 0; i < 4; i++) {
          float nv = editor.on[i] ? editor.v[i] : NAN;
          bool same = (isnan(nv) && isnan(old[i])) || (!isnan(nv) && !isnan(old[i]) && fabsf(nv - old[i]) < 1e-4f);
          if (!same && act.setLimit) { act.setLimit(editor.sensor, (uint8_t)i, nv); sent++; }
        }
        toast(sent ? "Limits sent to the main board" : "No change", now);
      }
      editor.open = false; invalidate();
      return;
    }
    default: return;
  }
}

}  // namespace Disp
