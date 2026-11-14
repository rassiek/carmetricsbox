// CarMetrics Display - getting data from the main board.
//
//  1. ESP-NOW (default): the display pairs like any other board (role display,
//     Accept on the main board's Boards page). The main board sends it the
//     catalog (as its settings) and every value once a second; relay, mute and
//     limit commands go back the same way. Channel search as on extra boards.
//  2. Wi-Fi (fallback): the display also joins the main board's access point
//     (CarMetrics / carmetrics unless changed) and, whenever ESP-NOW is silent,
//     reads /api/v2/display/catalog + /values once a second and sends commands
//     to /api/v2/display/command (admin password of the main board).
//
// The display always keeps its own access point CarMetrics-Display-xxxx
// (192.168.5.1) for its web page (layout editor, settings, firmware).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "DispConfig.h"
#include "Model.h"

namespace Net {
struct Hooks {
  std::function<void()> catalogChanged;        // new catalog (loop task, model locked)
  std::function<void(const String& msg)> notice;   // command result etc. for the screen
  std::function<void()> identify;
};
void begin(DispSettings& s, Disp::Model& m, const Hooks& h);
void update(uint32_t now);                     // loop()
uint8_t linkKind();                            // 0 none, 1 ESP-NOW, 2 Wi-Fi
bool canCommand();
void relay(uint8_t board, uint8_t index, bool on);
void mute(uint16_t minutes);
void setLimit(int sensor, uint8_t which, float value);
void statusJson(JsonObject o);
bool paired(); int boardId(); int channel();
String wifiSsid(); String wifiIp(); String apName(); String apIp();
void applyWifiSettings();                      // after the settings changed

// Model lock: the HTTP task and the web server change / read the model too.
void lock(); void unlock();
struct Lock { Lock() { lock(); } ~Lock() { unlock(); } };
}
