// CarMetrics Display - what the display knows: the main board's catalog (all
// sensors + relays it can show / switch), their live values, and the screen
// layout (pages of tiles) edited on the display's own web page.
//
// Plain C++ + ArduinoJson (no Arduino types): also built on a PC for the
// screen simulator (tools/cm.sh dispsim) and the tests.
#pragma once
#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace Disp {

// Same values as the main board (State.h)
enum Level : uint8_t { LVL_OK = 0, LVL_WARN = 1, LVL_FAULT = 2, LVL_CRIT = 3 };
enum Status : uint8_t { ST_DISABLED = 0, ST_WAITING, ST_OK, ST_UNASSIGNED, ST_NOT_FOUND, ST_FAULT, ST_LINK_LOST };
const char* statusShort(uint8_t st);       // "--", "WAIT", "FAULT", "LINK"...
int levelRank(uint8_t lvl);                // ok 0 < warn 1 < fault 2 < crit 3

struct SensorMeta {
  std::string id, name, label, unit;
  uint8_t decimals = 0;
  float lo = 0, hi = 100;                  // gauge range
  float wl = NAN, wh = NAN, cl = NAN, ch = NAN;   // warn / crit low / high (NaN = off)
  int board = 0;                           // 0 = on the main board
  std::string shortLabel() const;          // label, else made from the name (3-4 chars)
};
struct RelayMeta { uint8_t board = 0, index = 0; std::string name; };
struct Val { float v = NAN; uint8_t st = ST_WAITING, lvl = LVL_OK; };

enum Widget : uint8_t { W_NUMBER, W_BAR, W_DIAL, W_MARKER, W_RELAY };
const char* widgetName(Widget w);
Widget widgetFrom(const std::string& s);

struct Tile {
  std::string ref;                         // sensor id, or "r:<board>:<index>" for a relay
  Widget widget = W_NUMBER;
  std::string label;                       // overrides the catalog label ("" = catalog's)
  uint8_t cs = 1, rs = 1;                  // columns / rows spanned
};
struct Page { std::string name; uint8_t cols = 2, rows = 3; std::vector<Tile> tiles; };
struct Layout {
  std::vector<Page> pages;
  bool alarmPopup = true;                  // banner over the page when something goes critical
  bool relaysPage = true;                  // built-in page with every relay
};
struct Cell { int col, row, cs, rs; };     // where a tile landed in its page grid (cs = 0: didn't fit)
std::vector<Cell> placeTiles(const Page& p);   // left to right, top to bottom, respecting spans

std::string relayRef(uint8_t board, uint8_t index);

class Model {
 public:
  // Catalog JSON from the main board ({"s":[...],"r":[...],"main","ap","fw"}); rev = its CRC-32.
  bool setCatalog(const char* json, size_t len, uint32_t rev, std::string& err);
  uint32_t rev() const { return catRev; }
  bool hasCatalog() const { return catRev != 0; }
  const std::vector<SensorMeta>& sensors() const { return sens; }
  const std::vector<RelayMeta>& relays() const { return rels; }
  const std::string& mainName() const { return mainNm; }
  const std::string& mainAp() const { return apName; }
  const std::string& mainFirmware() const { return mainFw; }
  const std::string& catalogJson() const { return catJson; }

  // values: sensors first, then relays (as the main board sends them)
  void setValue(size_t index, float v, uint8_t st, uint8_t lvl, uint32_t now);
  void clearValues();                      // link lost: everything "waiting"
  const Val& sensorVal(int i) const { return vals[i]; }
  const Val& relayVal(int i) const { return vals[sens.size() + i]; }
  size_t valueCount() const { return vals.size(); }
  uint32_t valuesAt = 0;                   // when values last arrived
  uint8_t mainLevel = LVL_OK;
  bool mainMuted = false;
  int sensorIndex(const std::string& id) const;
  int relayIndex(const std::string& ref) const;
  uint8_t worstLevel() const;              // over all sensors

  // layout
  bool setLayout(JsonVariantConst j, std::string& err);   // validates
  void layoutToJson(JsonObject o) const;
  const Layout& layout() const { return lay; }
  bool layoutIsDefault() const { return layDefault; }
  // Pages made from the catalog when nothing was saved (portrait: 2x3 dials first).
  Layout defaultLayout(bool portrait) const;
  void useDefaultLayout(bool portrait) { lay = defaultLayout(portrait); layDefault = true; }

 private:
  uint32_t catRev = 0;
  std::string catJson, mainNm, apName, mainFw;
  std::vector<SensorMeta> sens;
  std::vector<RelayMeta> rels;
  std::vector<Val> vals;
  Layout lay;
  bool layDefault = true;
};

// Number formatting shared by the screen and the tests: value with the sensor's
// decimals ("-" sign, no exponent), or a short status word.
std::string formatValue(const SensorMeta& m, const Val& v);
// Step for the +/- buttons of the limit editor: 1, 2, 5 x 10^n, about range / 100.
float niceStep(float lo, float hi);

}  // namespace Disp
