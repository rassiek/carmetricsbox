// CarMetrics Display - the touch screen: status bar, pages of tiles (number,
// bar, dial, marker, relay), a relays page, a setup page, the alarm banner and
// the limit editor (long-press a value).
//
// Draws with Arduino_GFX through a Surface, so the same code runs on the
// JC3248W535 (whole-screen canvas in PSRAM), on the Cheap Yellow Display (one
// small canvas per tile, no PSRAM) and on a PC (tools/cm.sh dispsim, PNGs).
// Only regions whose content changed are drawn again.
#pragma once
#include "Model.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

class Arduino_GFX;

namespace Disp {

// Where drawing goes. begin() returns the GFX to draw a region on and the
// origin of the region in it; end() shows it; present() ends a frame.
class Surface {
 public:
  virtual ~Surface() {}
  virtual int width() = 0;
  virtual int height() = 0;
  virtual Arduino_GFX* begin(int x, int y, int w, int h, int& ox, int& oy) = 0;
  virtual void end() = 0;
  virtual void present() = 0;
};

struct UiActions {
  std::function<void(uint8_t board, uint8_t index, bool on)> relay;
  std::function<void(uint16_t minutes)> mute;                       // 0 = unmute
  std::function<void(int sensor, uint8_t which, float value)> setLimit;
  std::function<void(int delta)> brightness;
  std::function<void()> rotate;
  std::function<void()> restart;
};

struct UiInfo {                          // shown on the status bar and the setup page
  std::string name = "Display", firmware, profile;
  uint8_t link = 0;                      // 0 none, 1 ESP-NOW, 2 Wi-Fi (main board's access point)
  bool paired = false; int boardId = 0, channel = 0;
  std::string wifiSsid, wifiIp, apName, apIp;
  int brightness = 80;                   // %
  bool canRelays = true;                 // commands possible (link up)
};

enum class Hit : uint8_t { NONE, MUTE, TAB, PREV, NEXT, TILE, RELAY, BANNER, SET_BRIGHT_DOWN, SET_BRIGHT_UP, SET_ROTATE, SET_RESTART,
                           LIM_MINUS, LIM_PLUS, LIM_TOGGLE, LIM_SAVE, LIM_CANCEL };

class Ui {
 public:
  Ui(Model& m, const UiActions& a) : model(m), act(a) {}
  void setInfo(const UiInfo& i) { info = i; }
  void draw(Surface& s, uint32_t now);
  void invalidate() { drawn.clear(); }   // e.g. after rotation
  void touch(bool down, int x, int y, uint32_t now);
  void identify(uint32_t now) { flashUntil = now + 1500; invalidate(); }
  void toast(const std::string& msg, uint32_t now) { toastText = msg; toastUntil = now + 2500; }
  void layoutChanged() { if (page >= pageCount()) page = 0; editor.open = false; invalidate(); }
  void showPage(int p) { page = p; editor.open = false; invalidate(); }
  int currentPage() const { return page; }
  int pageCount() const;                 // user pages + relays page + setup page
  bool relayPending(int r) const;

 private:
  struct Region { int x, y, w, h; Hit kind; int a = 0, b = 0; };
  enum Special { PAGE_RELAYS = 1000, PAGE_SETUP = 1001 };
  int pageKind(int p) const;             // index of a user page, or PAGE_RELAYS / PAGE_SETUP
  std::string pageName(int p) const;
  std::vector<Region> regions(int W, int H) const;
  const Region* hitTest(int x, int y, int W, int H, std::vector<Region>& store) const;
  void tap(const Region& r, uint32_t now);
  void openEditor(int sensor);
  // drawing
  void drawRegion(Arduino_GFX* g, int ox, int oy, const Region& r, uint32_t now);
  std::string signature(const Region& r, uint32_t now) const;

  Model& model;
  UiActions act;
  UiInfo info;
  int page = 0;
  int W = 0, H = 0;
  std::map<std::string, size_t> drawn;   // region key -> hash of what is on screen
  // touch
  bool down = false, longFired = false, moved = false;
  int startX = 0, startY = 0, lastX = 0, lastY = 0;
  uint32_t downAt = 0;
  // overlays
  struct Editor { bool open = false; int sensor = -1; float v[4]; bool on[4]; float step = 1; } editor;
  bool bannerDismissed = false;
  uint32_t flashUntil = 0, toastUntil = 0;
  std::string toastText;
  std::map<int, uint32_t> pending;       // relay index -> when its command was sent
  std::map<int, float> pendingFrom;      // relay value when the command was sent
};

}  // namespace Disp
