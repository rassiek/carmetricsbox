// CarMetrics Display - its own web page (layout editor with a live preview,
// settings, firmware update) at http://192.168.5.1/ on its access point, or at
// its address on the main board's network (shown on the main board's Boards page).
#pragma once
#include "DispConfig.h"
#include "Model.h"
#include <functional>

namespace WebUi {
struct Hooks {
  std::function<void()> layoutChanged;
  std::function<void()> settingsChanged;
  std::function<void()> identify;
  std::function<void(int& w, int& h)> screenSize;
};
void begin(DispSettings& s, Disp::Model& m, const Hooks& h);
bool rebootRequested();
}
