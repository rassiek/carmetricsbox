// CarMetrics Display on a PC: the real Ui / Model code drawing with the real
// Arduino_GFX drawing core into a memory frame buffer, saved as PPM pictures
// (turned into PNGs by tools/cm.sh dispsim), plus checks of the display logic.
#include "Model.h"
#include "Ui.h"
#include <Arduino_GFX.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Disp;
static int passed = 0, failed = 0;
static void check(bool ok, const char* what, const std::string& d = "") { (ok ? passed : failed)++; printf("%s %s %s\n", ok ? "PASS" : "FAIL", what, d.c_str()); }

// ---------------------------------------------------------------- a frame buffer GFX + surfaces
class MemGfx : public Arduino_GFX {
 public:
  MemGfx(int w, int h) : Arduino_GFX(w, h), fb(w * h, 0) {}
  bool begin(int32_t) override { return true; }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { fb[y * WIDTH + x] = c; }
  std::vector<uint16_t> fb;
  int W() const { return WIDTH; } int H() const { return HEIGHT; }
};
class WholeSurface : public Surface {   // JC3248W535 style: one canvas, regions drawn in place
 public:
  explicit WholeSurface(MemGfx& g) : g(g) {}
  int width() override { return g.width(); }
  int height() override { return g.height(); }
  Arduino_GFX* begin(int x, int y, int, int, int& ox, int& oy) override { ox = x; oy = y; regions++; return &g; }
  void end() override {}
  void present() override { frames++; }
  MemGfx& g; int regions = 0, frames = 0;
};
class TileSurface : public Surface {    // CYD style: each region in its own small canvas, then copied
 public:
  explicit TileSurface(MemGfx& g) : g(g) {}
  int width() override { return g.width(); }
  int height() override { return g.height(); }
  Arduino_GFX* begin(int x, int y, int w, int h, int& ox, int& oy) override { cur.reset(new MemGfx(w, h)); cx = x; cy = y; ox = oy = 0; regions++; return cur.get(); }
  void end() override { for (int j = 0; j < cur->H(); j++) for (int i = 0; i < cur->W(); i++) if (cx + i < g.W() && cy + j < g.H()) g.fb[(cy + j) * g.W() + cx + i] = cur->fb[j * cur->W() + i]; cur.reset(); }
  void present() override { frames++; }
  MemGfx& g; std::unique_ptr<MemGfx> cur; int cx = 0, cy = 0, regions = 0, frames = 0;
};
static void save(const MemGfx& g, const std::string& path) {
  FILE* f = fopen(path.c_str(), "wb");
  fprintf(f, "P6 %d %d 255\n", g.W(), g.H());
  for (uint16_t c : g.fb) { uint8_t p[3] = {(uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC), (uint8_t)((c << 3) & 0xF8)}; fwrite(p, 1, 3, f); }
  fclose(f);
}

// ---------------------------------------------------------------- a main board's catalog
static const char* CATALOG = R"({"v":1,"main":"carmetrics","ap":"CarMetrics","fw":"5.1.0-beta.11","s":[
 {"i":"coolant_temp","n":"Coolant","u":"°C","d":0,"l":"CLT","lo":40,"hi":130,"wh":100,"ch":108},
 {"i":"egt","n":"Exhaust gas","u":"°C","d":0,"l":"EGT","lo":0,"hi":900,"wh":700,"ch":780},
 {"i":"boost","n":"Boost","u":"bar","d":2,"l":"BST","lo":-0.8,"hi":2,"wh":1.4,"ch":1.6},
 {"i":"oil_press","n":"Oil pressure","u":"bar","d":1,"l":"OIL","lo":0,"hi":8,"wl":1,"cl":0.5},
 {"i":"intake_temp","n":"Intake","u":"°C","d":0,"l":"IT","lo":-10,"hi":80,"wh":55},
 {"i":"cabin_temp","n":"Cabin","u":"°C","d":1,"l":"CT","lo":-10,"hi":60},
 {"i":"bat_voltage","n":"Battery","u":"V","d":1,"lo":10,"hi":16,"wl":11.8,"cl":11.2,"wh":14.8},
 {"i":"gearbox_temp","n":"Gearbox","u":"°C","d":0,"l":"GBT","lo":20,"hi":150,"wh":110,"ch":125},
 {"i":"b1_coolant_temp","n":"Engine bay Coolant","u":"°C","d":0,"lo":40,"hi":130,"wh":100,"ch":108,"b":1}],
 "r":[{"b":0,"x":0,"n":"Fan"},{"b":0,"x":1,"n":"Spot lights"},{"b":1,"x":0,"n":"Engine bay Pump"}]})";

static void fill(Model& m, bool crit) {
  float v[] = {92, 655, 1.32f, 3.4f, 41, 23.5f, 14.1f, 88, 95};
  for (int i = 0; i < 9; i++) m.setValue(i, v[i], ST_OK, LVL_OK, 1);
  m.setValue(1, 655, ST_OK, LVL_OK, 1);
  m.setValue(2, 1.47f, ST_OK, LVL_WARN, 1);
  if (crit) m.setValue(0, 111, ST_OK, LVL_CRIT, 1);
  m.setValue(8, NAN, ST_LINK_LOST, LVL_OK, 1);
  m.setValue(9, 1, ST_OK, 0, 1); m.setValue(10, 0, ST_OK, 0, 1); m.setValue(11, 0, ST_LINK_LOST, 0, 1);
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  // ---- model
  Model m;
  std::string err;
  check(m.setCatalog(CATALOG, strlen(CATALOG), 0x1234ABCD, err) && m.sensors().size() == 9 && m.relays().size() == 3, "catalog parsed: 9 values, 3 relays", err);
  check(m.sensors()[0].shortLabel() == "CLT" && m.sensors()[6].shortLabel() == "BATT" && m.sensors()[8].shortLabel() == "EBC", "short labels: given, else initials",
        m.sensors()[6].shortLabel() + " " + m.sensors()[8].shortLabel());
  check(m.relayIndex(relayRef(1, 0)) == 2 && m.sensorIndex("egt") == 1 && m.sensorIndex("nope") == -1, "relay and sensor references");
  Val v; v.v = -0.04f; v.st = ST_OK;
  check(formatValue(m.sensors()[0], v) == "0" && formatValue(m.sensors()[2], v) == "-0.04", "values formatted with the sensor's decimals, no \"-0\"");
  v.st = ST_LINK_LOST;
  check(formatValue(m.sensors()[0], v) == "LINK", "link lost shown as LINK");
  check(niceStep(0, 900) == 10 && niceStep(-0.8f, 2) < 0.05f && fabsf(niceStep(10, 16) - 0.05f) < 1e-6f, "limit editor steps 1/2/5", std::to_string(niceStep(10, 16)));
  {
    Page p; p.cols = 2; p.rows = 3;
    Tile big; big.ref = "a"; big.cs = 2; big.rs = 2;
    Tile one; one.ref = "b";
    p.tiles = {one, big, one, one, one};
    auto c = placeTiles(p);
    check(c[0].col == 0 && c[0].row == 0 && c[1].row == 1 && c[1].cs == 2 && c[2].col == 1 && c[2].row == 0 && c[4].cs == 0,
          "tile placement: spans respected, a tile that doesn't fit is left out");
  }
  {
    JsonDocument d; deserializeJson(d, R"({"pages":[{"name":"X","cols":5,"rows":3,"tiles":[]}]})");
    std::string e; check(!m.setLayout(d.as<JsonVariantConst>(), e) && e.find("1-4") != std::string::npos, "layout: bad grid refused", e);
    deserializeJson(d, R"({"pages":[{"name":"Mine","cols":2,"rows":2,"tiles":[{"ref":"egt","w":"dial","cs":2},{"ref":"r:0:1","w":"number"}]}],"alarmPopup":false})");
    bool ok = m.setLayout(d.as<JsonVariantConst>(), e);
    check(ok && m.layout().pages[0].tiles[1].widget == W_RELAY && !m.layout().alarmPopup && !m.layoutIsDefault(), "layout saved; relay tiles always drawn as relays", e);
    JsonDocument back; m.layoutToJson(back.to<JsonObject>());
    std::string s; serializeJson(back, s);
    check(s.find("\"cs\":2") != std::string::npos && s.find("\"w\":\"relay\"") != std::string::npos, "layout round trip", s);
  }
  Layout dp = m.defaultLayout(true), dl = m.defaultLayout(false);
  check(dp.pages.size() == 2 && dp.pages[0].tiles.size() == 6 && dp.pages[0].tiles[0].widget == W_DIAL && dp.pages[1].tiles.size() == 3 && dl.pages[0].cols == 3,
        "automatic layout: 6 dials, then bars (portrait 2x3, landscape 3x2)");

  // ---- screens: JC3248W535 portrait 320x480 and CYD landscape 320x240
  std::vector<std::string> acts;
  UiActions a;
  a.relay = [&](uint8_t b, uint8_t i, bool on) { acts.push_back("relay " + std::to_string(b) + ":" + std::to_string(i) + (on ? " on" : " off")); };
  a.mute = [&](uint16_t mins) { acts.push_back("mute " + std::to_string(mins)); };
  a.setLimit = [&](int s, uint8_t w, float val) { char b[64]; snprintf(b, sizeof b, "limit %d/%u=%g", s, w, val); acts.push_back(b); };
  UiInfo info; info.name = "Dash"; info.firmware = "5.1.0-beta.11"; info.profile = "JC3248W535"; info.link = 1; info.paired = true; info.boardId = 2; info.channel = 6;
  info.wifiSsid = "CarMetrics"; info.wifiIp = "192.168.4.3"; info.apName = "CarMetrics-Display-0A0B"; info.apIp = "192.168.5.1";

  for (int screen = 0; screen < 2; screen++) {
    bool jc = screen == 0;
    std::string tag = jc ? "jc3248w535" : "cyd";
    MemGfx g(jc ? 320 : 320, jc ? 480 : 240);
    std::unique_ptr<Surface> surf(jc ? (Surface*)new WholeSurface(g) : (Surface*)new TileSurface(g));
    Model mm; mm.setCatalog(CATALOG, strlen(CATALOG), 0x1234ABCD, err);
    mm.useDefaultLayout(jc);
    fill(mm, false);
    Ui ui(mm, a);
    info.profile = jc ? "JC3248W535" : "CYD ILI9341";
    ui.setInfo(info);
    uint32_t now = 1000;
    ui.draw(*surf, now);
    save(g, out + "/" + tag + "-1-main.ppm");
    int before = jc ? ((WholeSurface*)surf.get())->regions : ((TileSurface*)surf.get())->regions;
    ui.draw(*surf, now + 40);
    int after = jc ? ((WholeSurface*)surf.get())->regions : ((TileSurface*)surf.get())->regions;
    check(after == before, (tag + ": nothing changed = nothing drawn again").c_str(), std::to_string(after - before));
    mm.setValue(5, 24.0f, ST_OK, LVL_OK, 2);
    ui.draw(*surf, now + 80);
    int after2 = jc ? ((WholeSurface*)surf.get())->regions : ((TileSurface*)surf.get())->regions;
    check(after2 - after == (jc ? 1 : 1), (tag + ": one value changed = one tile drawn").c_str(), std::to_string(after2 - after));
    // page 2 by swipe
    ui.touch(true, 250, 200, now); ui.touch(true, 120, 205, now + 100); ui.touch(false, 120, 205, now + 150);
    ui.draw(*surf, now + 200);
    check(ui.currentPage() == 1, (tag + ": swipe left = next page").c_str());
    save(g, out + "/" + tag + "-2-page2.ppm");
    // relays page: tap the first relay
    ui.showPage(2); ui.draw(*surf, now + 300);
    save(g, out + "/" + tag + "-3-relays.ppm");
    acts.clear();
    ui.touch(true, 60, jc ? 80 : 60, now + 400); ui.touch(false, 60, jc ? 80 : 60, now + 450);
    check(!acts.empty() && acts[0] == "relay 0:0 off", (tag + ": tap a relay = command to the main board").c_str(), acts.empty() ? "" : acts[0]);
    ui.draw(*surf, now + 500);
    check(ui.relayPending(0), (tag + ": relay shows 'pending' until its value changes").c_str());
    mm.setValue(9, 0, ST_OK, 0, 3); ui.draw(*surf, now + 600);
    check(!ui.relayPending(0), (tag + ": pending cleared when the main board reports the new state").c_str());
    // setup page
    ui.showPage(3); ui.draw(*surf, now + 700);
    save(g, out + "/" + tag + "-4-setup.ppm");
    // critical: alarm banner, tap = mute
    ui.showPage(0); fill(mm, true); ui.draw(*surf, now + 1000);
    save(g, out + "/" + tag + "-5-alarm.ppm");
    acts.clear();
    int by = jc ? 36 + 10 : 28 + 8;
    ui.touch(true, 100, by, now + 1100); ui.touch(false, 100, by, now + 1150);
    check(!acts.empty() && acts[0] == "mute 15", (tag + ": tap the alarm banner = mute 15 min").c_str());
    // long press a dial: limit editor; + on "warn high", save
    ui.draw(*surf, now + 1200);
    int tx = 80, ty = jc ? 120 : 90;
    ui.touch(true, tx, ty, now + 2000); ui.touch(true, tx, ty, now + 2800); ui.touch(false, tx, ty, now + 2900);
    ui.draw(*surf, now + 2950);
    save(g, out + "/" + tag + "-6-limits.ppm");
    acts.clear();
    // rows: (h - 8) / 6 high, row 2 = warn high; "+" button is second from the right
    int ch = g.H() - (jc ? 36 : 28) - (jc ? 48 : 34), rh = (ch - 8) / 6, bw = std::min(64, g.W() / 7);
    int ry = (jc ? 36 : 28) + 4 + rh * 2 + rh / 2, px = g.W() - 4 - 2 * bw - 4 + bw / 2;
    ui.touch(true, px, ry, now + 3000); ui.touch(false, px, ry, now + 3050);
    int sy = (jc ? 36 : 28) + 4 + rh * 5 + rh / 2;
    ui.touch(true, g.W() * 3 / 4, sy, now + 3100); ui.touch(false, g.W() * 3 / 4, sy, now + 3150);
    check(acts.size() == 1 && acts[0] == "limit 0/1=101", (tag + ": limit editor sends the changed limit only").c_str(), acts.empty() ? "none" : acts[0]);
    // identify flash
    ui.identify(now + 4000); ui.draw(*surf, now + 4100);
    save(g, out + "/" + tag + "-7-identify.ppm");
    // no link
    info.link = 0; ui.setInfo(info); for (size_t i = 0; i < mm.valueCount(); i++) mm.setValue(i, NAN, ST_LINK_LOST, 0, 5);
    ui.draw(*surf, now + 6000);
    save(g, out + "/" + tag + "-8-nolink.ppm");
    info.link = 1;
  }
  printf("DONE: %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
