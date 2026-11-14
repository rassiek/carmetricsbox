#include "Model.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace Disp {

const char* statusShort(uint8_t st) {
  switch (st) {
    case ST_DISABLED: return "OFF";
    case ST_WAITING: return "--";
    case ST_OK: return "";
    case ST_UNASSIGNED: return "SETUP";
    case ST_NOT_FOUND: return "N/F";
    case ST_FAULT: return "FAULT";
    case ST_LINK_LOST: return "LINK";
    default: return "?";
  }
}
int levelRank(uint8_t lvl) { return lvl == LVL_CRIT ? 3 : lvl == LVL_FAULT ? 2 : lvl == LVL_WARN ? 1 : 0; }

std::string SensorMeta::shortLabel() const {
  if (!label.empty()) return label;
  std::string out;   // initials of the words, else the first letters
  bool start = true;
  for (char c : name) {
    if (c == ' ' || c == '_' || c == '-') { start = true; continue; }
    if (start && out.size() < 4) out += (char)toupper((unsigned char)c);
    start = false;
  }
  if (out.size() < 2) { out.clear(); for (char c : name) if (c != ' ' && out.size() < 4) out += (char)toupper((unsigned char)c); }
  return out.empty() ? id.substr(0, 4) : out;
}

const char* widgetName(Widget w) {
  static const char* const n[] = {"number", "bar", "dial", "marker", "relay"};
  return w <= W_RELAY ? n[w] : "number";
}
Widget widgetFrom(const std::string& s) {
  return s == "bar" ? W_BAR : s == "dial" ? W_DIAL : s == "marker" ? W_MARKER : s == "relay" ? W_RELAY : W_NUMBER;
}

std::string relayRef(uint8_t board, uint8_t index) { return "r:" + std::to_string(board) + ":" + std::to_string(index); }

std::vector<Cell> placeTiles(const Page& p) {
  int cols = std::max(1, (int)p.cols), rows = std::max(1, (int)p.rows);
  std::vector<std::vector<bool>> used(rows, std::vector<bool>(cols, false));
  std::vector<Cell> out;
  for (auto& t : p.tiles) {
    int cs = std::min<int>(std::max<int>(1, t.cs), cols), rs = std::min<int>(std::max<int>(1, t.rs), rows);
    Cell c{0, 0, 0, 0};
    for (int r = 0; r <= rows - rs && !c.cs; r++)
      for (int col = 0; col <= cols - cs && !c.cs; col++) {
        bool free = true;
        for (int y = r; y < r + rs && free; y++) for (int x = col; x < col + cs && free; x++) if (used[y][x]) free = false;
        if (!free) continue;
        for (int y = r; y < r + rs; y++) for (int x = col; x < col + cs; x++) used[y][x] = true;
        c = Cell{col, r, cs, rs};
      }
    out.push_back(c);
  }
  return out;
}

static float numOr(JsonVariantConst v, float d) { return v.is<float>() || v.is<int>() ? v.as<float>() : d; }

bool Model::setCatalog(const char* json, size_t len, uint32_t rev, std::string& err) {
  JsonDocument d;
  if (deserializeJson(d, json, len)) { err = "catalog is not valid JSON"; return false; }
  if (!d["s"].is<JsonArrayConst>()) { err = "catalog has no sensor list"; return false; }
  std::vector<SensorMeta> s;
  for (JsonObjectConst o : d["s"].as<JsonArrayConst>()) {
    SensorMeta m;
    m.id = o["i"] | ""; m.name = o["n"] | m.id.c_str(); m.label = o["l"] | ""; m.unit = o["u"] | "";
    m.decimals = o["d"] | 0; if (m.decimals > 3) m.decimals = 3;
    m.lo = numOr(o["lo"], 0); m.hi = numOr(o["hi"], 100); if (!(m.hi > m.lo)) m.hi = m.lo + 1;
    m.wl = numOr(o["wl"], NAN); m.wh = numOr(o["wh"], NAN); m.cl = numOr(o["cl"], NAN); m.ch = numOr(o["ch"], NAN);
    m.board = o["b"] | 0;
    if (m.id.empty()) continue;
    s.push_back(m);
  }
  std::vector<RelayMeta> r;
  for (JsonObjectConst o : d["r"].as<JsonArrayConst>()) { RelayMeta m; m.board = o["b"] | 0; m.index = o["x"] | 0; m.name = o["n"] | "relay"; r.push_back(m); }
  sens = s; rels = r;
  mainNm = d["main"] | "CarMetrics"; apName = d["ap"] | ""; mainFw = d["fw"] | "";
  catJson.assign(json, len);
  catRev = rev ? rev : 1;
  vals.assign(sens.size() + rels.size(), Val());
  return true;
}

void Model::setValue(size_t i, float v, uint8_t st, uint8_t lvl, uint32_t now) {
  if (i >= vals.size()) return;
  vals[i].v = v; vals[i].st = st; vals[i].lvl = lvl;
  valuesAt = now ? now : 1;
}
void Model::clearValues() { for (auto& v : vals) v = Val(); }

int Model::sensorIndex(const std::string& id) const {
  for (size_t i = 0; i < sens.size(); i++) if (sens[i].id == id) return (int)i;
  return -1;
}
int Model::relayIndex(const std::string& ref) const {
  for (size_t i = 0; i < rels.size(); i++) if (relayRef(rels[i].board, rels[i].index) == ref) return (int)i;
  return -1;
}
uint8_t Model::worstLevel() const {
  uint8_t w = LVL_OK;
  for (size_t i = 0; i < sens.size() && i < vals.size(); i++) if (levelRank(vals[i].lvl) > levelRank(w)) w = vals[i].lvl;
  return w;
}

bool Model::setLayout(JsonVariantConst j, std::string& err) {
  Layout l;
  l.alarmPopup = j["alarmPopup"] | true; l.relaysPage = j["relaysPage"] | true;
  JsonArrayConst pa = j["pages"].as<JsonArrayConst>();
  if (pa.isNull()) { err = "no pages"; return false; }
  if (pa.size() > 8) { err = "at most 8 pages"; return false; }
  for (JsonObjectConst po : pa) {
    Page p;
    p.name = po["name"] | ""; if (p.name.size() > 12) p.name.resize(12);
    int cols = po["cols"] | 2, rows = po["rows"] | 3;
    if (cols < 1 || cols > 4 || rows < 1 || rows > 6) { err = "grid: 1-4 columns, 1-6 rows"; return false; }
    p.cols = cols; p.rows = rows;
    for (JsonObjectConst to : po["tiles"].as<JsonArrayConst>()) {
      Tile t;
      t.ref = to["ref"] | ""; t.widget = widgetFrom(to["w"] | "number"); t.label = to["l"] | "";
      if (t.label.size() > 5) t.label.resize(5);
      int cs = to["cs"] | 1, rs = to["rs"] | 1;
      t.cs = (uint8_t)std::max(1, std::min(cs, 4)); t.rs = (uint8_t)std::max(1, std::min(rs, 6));
      if (t.ref.empty()) { err = "a tile has no sensor or relay"; return false; }
      if (t.ref.rfind("r:", 0) == 0) t.widget = W_RELAY;
      else if (t.widget == W_RELAY) t.widget = W_NUMBER;
      if (p.tiles.size() >= 24) { err = "at most 24 tiles per page"; return false; }
      p.tiles.push_back(t);
    }
    l.pages.push_back(p);
  }
  lay = l; layDefault = false;
  return true;
}

void Model::layoutToJson(JsonObject o) const {
  o["alarmPopup"] = lay.alarmPopup; o["relaysPage"] = lay.relaysPage; o["default"] = layDefault;
  JsonArray pa = o["pages"].to<JsonArray>();
  for (auto& p : lay.pages) {
    JsonObject po = pa.add<JsonObject>();
    po["name"] = p.name; po["cols"] = p.cols; po["rows"] = p.rows;
    JsonArray ta = po["tiles"].to<JsonArray>();
    for (auto& t : p.tiles) {
      JsonObject to = ta.add<JsonObject>();
      to["ref"] = t.ref; to["w"] = widgetName(t.widget);
      if (!t.label.empty()) to["l"] = t.label;
      if (t.cs != 1) to["cs"] = t.cs;
      if (t.rs != 1) to["rs"] = t.rs;
    }
  }
}

Layout Model::defaultLayout(bool portrait) const {
  Layout l;
  size_t i = 0;
  // first page: the most important values as dials (portrait 2x3, landscape 3x2)
  Page p1; p1.name = "Main"; p1.cols = portrait ? 2 : 3; p1.rows = portrait ? 3 : 2;
  for (; i < sens.size() && p1.tiles.size() < 6; i++) { Tile t; t.ref = sens[i].id; t.widget = W_DIAL; p1.tiles.push_back(t); }
  if (!p1.tiles.empty()) l.pages.push_back(p1);
  // the rest as bars, a page at a time
  int pageNo = 2;
  while (i < sens.size() && l.pages.size() < 6) {
    Page p; p.name = "Page " + std::to_string(pageNo++); p.cols = portrait ? 1 : 2; p.rows = portrait ? 6 : 3;
    for (; i < sens.size() && p.tiles.size() < (size_t)(p.cols * p.rows); i++) { Tile t; t.ref = sens[i].id; t.widget = W_BAR; p.tiles.push_back(t); }
    l.pages.push_back(p);
  }
  if (l.pages.empty()) { Page p; p.name = "Main"; l.pages.push_back(p); }
  return l;
}

std::string formatValue(const SensorMeta& m, const Val& v) {
  if (v.st != ST_OK) return statusShort(v.st);
  if (isnan(v.v) || isinf(v.v)) return "--";
  char b[24];
  float x = v.v;
  if (fabsf(x) >= 1e6f) { snprintf(b, sizeof(b), "%.0e", x); return b; }
  snprintf(b, sizeof(b), "%.*f", (int)m.decimals, x);
  if (!strcmp(b, "-0") || !strcmp(b, "-0.0") || !strcmp(b, "-0.00") || !strcmp(b, "-0.000")) memmove(b, b + 1, strlen(b));
  return b;
}

float niceStep(float lo, float hi) {
  float r = fabsf(hi - lo) / 100.0f;
  if (!(r > 0)) return 1;
  float p = powf(10.0f, floorf(log10f(r)));
  float n = r / p;
  return (n < 1.5f ? 1 : n < 3.5f ? 2 : n < 7.5f ? 5 : 10) * p;
}

}  // namespace Disp
