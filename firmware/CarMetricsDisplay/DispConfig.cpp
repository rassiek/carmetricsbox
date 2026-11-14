#include "DispConfig.h"
#include <Preferences.h>
#include <WiFi.h>

namespace {
const char* NS = "disp";
String defaultName() {
  uint8_t m[6]; WiFi.macAddress(m);
  char b[24]; snprintf(b, sizeof b, "Display %02X%02X", m[4], m[5]);
  return b;
}
}

namespace DispConfig {
void load(DispSettings& s) {
  s = DispSettings();
  s.name = defaultName();
  Preferences p;
  if (!p.begin(NS, true)) return;
  s.name = p.getString("name", s.name);
  s.brightness = p.getUChar("bright", s.brightness);
  s.rotation = p.getUChar("rot", s.rotation);
  s.dimAfterMin = p.getUShort("dim", s.dimAfterMin);
  s.panel = p.getString("panel", s.panel);
  s.touch.xMin = p.getShort("txmin", s.touch.xMin); s.touch.xMax = p.getShort("txmax", s.touch.xMax);
  s.touch.yMin = p.getShort("tymin", s.touch.yMin); s.touch.yMax = p.getShort("tymax", s.touch.yMax);
  s.touch.swapXY = p.getBool("tswap", false); s.touch.flipX = p.getBool("tfx", false); s.touch.flipY = p.getBool("tfy", false);
  s.wifi = p.getBool("wifi", s.wifi);
  s.mainSsid = p.getString("mssid", ""); s.mainPass = p.getString("mpass", s.mainPass); s.mainAdmin = p.getString("madmin", s.mainAdmin);
  s.apPass = p.getString("appass", s.apPass); s.adminPass = p.getString("admin", s.adminPass);
  p.end();
}

bool save(const DispSettings& s) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("name", s.name); p.putUChar("bright", s.brightness); p.putUChar("rot", s.rotation); p.putUShort("dim", s.dimAfterMin);
  p.putString("panel", s.panel);
  p.putShort("txmin", s.touch.xMin); p.putShort("txmax", s.touch.xMax); p.putShort("tymin", s.touch.yMin); p.putShort("tymax", s.touch.yMax);
  p.putBool("tswap", s.touch.swapXY); p.putBool("tfx", s.touch.flipX); p.putBool("tfy", s.touch.flipY);
  p.putBool("wifi", s.wifi); p.putString("mssid", s.mainSsid); p.putString("mpass", s.mainPass); p.putString("madmin", s.mainAdmin);
  p.putString("appass", s.apPass); p.putString("admin", s.adminPass);
  p.end();
  return true;
}

void toJson(const DispSettings& s, JsonObject o) {
  o["name"] = s.name; o["brightness"] = s.brightness; o["rotation"] = s.rotation; o["dimAfterMin"] = s.dimAfterMin; o["panel"] = s.panel;
  JsonObject t = o["touch"].to<JsonObject>();
  t["xMin"] = s.touch.xMin; t["xMax"] = s.touch.xMax; t["yMin"] = s.touch.yMin; t["yMax"] = s.touch.yMax;
  t["swapXY"] = s.touch.swapXY; t["flipX"] = s.touch.flipX; t["flipY"] = s.touch.flipY;
  o["wifi"] = s.wifi; o["mainSsid"] = s.mainSsid;
  o["mainPass"] = ""; o["mainAdmin"] = ""; o["apPass"] = ""; o["adminPass"] = "";   // write-only
}

bool fromJson(JsonVariantConst j, DispSettings& s, String& err) {
  DispSettings n = s;
  if (j["name"].is<const char*>()) n.name = j["name"].as<const char*>();
  n.name.trim();
  if (n.name.isEmpty() || n.name.length() > 23) { err = "Name: 1 to 23 characters"; return false; }
  n.brightness = constrain((int)(j["brightness"] | (int)n.brightness), 5, 100);
  if (!j["rotation"].isNull()) n.rotation = j["rotation"].as<uint8_t>() > 3 ? 255 : j["rotation"].as<uint8_t>();
  n.dimAfterMin = constrain((int)(j["dimAfterMin"] | (int)n.dimAfterMin), 0, 240);
  if (j["panel"].is<const char*>()) { String p = j["panel"].as<const char*>(); if (p != "ili9341" && p != "st7789") { err = "Panel: ili9341 or st7789"; return false; } n.panel = p; }
  JsonVariantConst t = j["touch"];
  if (!t.isNull()) {
    n.touch.xMin = t["xMin"] | n.touch.xMin; n.touch.xMax = t["xMax"] | n.touch.xMax;
    n.touch.yMin = t["yMin"] | n.touch.yMin; n.touch.yMax = t["yMax"] | n.touch.yMax;
    n.touch.swapXY = t["swapXY"] | n.touch.swapXY; n.touch.flipX = t["flipX"] | n.touch.flipX; n.touch.flipY = t["flipY"] | n.touch.flipY;
    if (n.touch.xMax - n.touch.xMin < 100 || n.touch.yMax - n.touch.yMin < 100) { err = "Touch calibration: max must be well above min"; return false; }
  }
  n.wifi = j["wifi"] | n.wifi;
  if (j["mainSsid"].is<const char*>()) n.mainSsid = j["mainSsid"].as<const char*>();
  auto pass = [&](const char* k, String& dst, size_t minLen) -> bool {
    String v = j[k] | "";
    if (v.isEmpty()) return true;
    if (v.length() < minLen || v.length() > 63) { err = String(k) + ": " + minLen + " to 63 characters"; return false; }
    dst = v; return true;
  };
  if (!pass("mainPass", n.mainPass, 8) || !pass("mainAdmin", n.mainAdmin, 1) || !pass("apPass", n.apPass, 8) || !pass("adminPass", n.adminPass, 1)) return false;
  s = n;
  return true;
}

String loadLayout() {
  Preferences p;
  if (!p.begin(NS, true)) return "";
  size_t n = p.getBytesLength("layout");
  String out;
  if (n && n < 16384) { std::unique_ptr<char[]> b(new char[n + 1]); p.getBytes("layout", b.get(), n); b[n] = 0; out = b.get(); }
  p.end();
  return out;
}
bool saveLayout(const String& json) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = json.isEmpty() ? (p.remove("layout"), true) : p.putBytes("layout", json.c_str(), json.length()) == json.length();
  p.end();
  return ok;
}
bool loadCatalog(String& json, uint32_t& rev) {
  Preferences p;
  if (!p.begin(NS, true)) return false;
  size_t n = p.getBytesLength("cat");
  rev = p.getUInt("catrev", 0);
  bool ok = false;
  if (n && n < 32768 && rev) { std::unique_ptr<char[]> b(new char[n + 1]); p.getBytes("cat", b.get(), n); b[n] = 0; json = b.get(); ok = true; }
  p.end();
  return ok;
}
bool saveCatalog(const String& json, uint32_t rev) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.putBytes("cat", json.c_str(), json.length()) == json.length();
  p.putUInt("catrev", rev);
  p.end();
  return ok;
}
uint8_t loadNodeId() { Preferences p; uint8_t id = 15; if (p.begin(NS, true)) { id = p.getUChar("nodeid", 15); p.end(); } return id; }
void saveNodeId(uint8_t id, const uint8_t* key) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putUChar("nodeid", id);
  if (key) p.putBytes("nodekey", key, 16); else p.remove("nodekey");
  if (id == 15) p.putUInt("noderev", 0);
  p.end();
}
bool loadNodeKey(uint8_t key[16]) { Preferences p; bool ok = false; if (p.begin(NS, true)) { ok = p.getBytes("nodekey", key, 16) == 16; p.end(); } return ok; }
uint32_t loadNodeRev() { Preferences p; uint32_t r = 0; if (p.begin(NS, true)) { r = p.getUInt("noderev", 0); p.end(); } return r; }
void saveNodeRev(uint32_t rev) { Preferences p; if (p.begin(NS, false)) { p.putUInt("noderev", rev); p.end(); } }
}  // namespace DispConfig
