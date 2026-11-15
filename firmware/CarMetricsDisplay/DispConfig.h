// CarMetrics Display - its own settings (NVS namespace "disp").
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Hw.h"

#define DISP_VERSION "5.4.0-beta.2"

struct DispSettings {
  String name;                       // shown on the main board's Boards page ("Dash")
  uint8_t brightness = 80;           // %
  uint8_t rotation = 255;            // 255 = board default
  uint16_t dimAfterMin = 0;          // dim to 10 % after this long without a touch (0 = never)
  String panel = "ili9341";          // CYD only: ili9341 | st7789
  Hw::TouchCal touch;
  // Wi-Fi fallback: the main board's access point
  bool wifi = true;                  // join it (also makes this page reachable from the main board's network)
  String mainSsid;                   // "" = the name the main board sends (default CarMetrics)
  String mainPass = "carmetrics";
  String mainAdmin = "carmetrics";   // main board's admin password: relays and limits over Wi-Fi
  // this display
  String apPass = "carmetrics";      // its own access point CarMetrics-Display-xxxx (>= 8 chars)
  String adminPass = "carmetrics";   // its web page (user "admin")
};

namespace DispConfig {
void load(DispSettings& s);
bool save(const DispSettings& s);
void toJson(const DispSettings& s, JsonObject o);            // without passwords
bool fromJson(JsonVariantConst j, DispSettings& s, String& err);   // empty passwords keep the old ones
// big blobs
String loadLayout();
bool saveLayout(const String& json);
bool loadCatalog(String& json, uint32_t& rev);
bool saveCatalog(const String& json, uint32_t rev);
uint8_t loadNodeId(); void saveNodeId(uint8_t id, const uint8_t* key);
bool loadNodeKey(uint8_t key[16]);
uint32_t loadNodeRev(); void saveNodeRev(uint32_t rev);
}
