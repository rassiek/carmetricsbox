#include "Keys.h"
#include "State.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>

const char* scopeStr(uint8_t s) { return s == SCOPE_ADMIN ? "admin" : s == SCOPE_CONTROL ? "control" : s == SCOPE_READ ? "read" : "none"; }
uint8_t scopeFromStr(const String& s) { return s == "admin" ? SCOPE_ADMIN : s == "control" ? SCOPE_CONTROL : s == "read" ? SCOPE_READ : SCOPE_NONE; }

namespace {
struct Key { String id, name, hash; uint8_t scope; };
std::vector<Key> keys;
const size_t MAX_KEYS = 8;

String sha(const String& s) {
  uint8_t h[32];
  mbedtls_sha256((const uint8_t*)s.c_str(), s.length(), h, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
  return hex;
}

void save() {
  Preferences p;
  if (!p.begin("cmkeys", false)) return;
  p.clear();
  p.putUChar("n", keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    String k = "k" + String(i);
    p.putString(k.c_str(), keys[i].id + "|" + String(keys[i].scope) + "|" + keys[i].hash + "|" + keys[i].name);
  }
  p.end();
}
}  // namespace

void Keys::begin() {
  Preferences p;
  if (!p.begin("cmkeys", true)) return;
  uint8_t n = p.getUChar("n", 0);
  for (uint8_t i = 0; i < n && i < MAX_KEYS; i++) {
    String v = p.getString(("k" + String(i)).c_str(), "");
    int a = v.indexOf('|'), b = v.indexOf('|', a + 1), c = v.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) continue;
    keys.push_back({v.substring(0, a), v.substring(c + 1), v.substring(b + 1, c), (uint8_t)v.substring(a + 1, b).toInt()});
  }
  p.end();
}

uint8_t Keys::check(const String& key) {
  if (!key.startsWith("cm_") || key.length() < 20) return SCOPE_NONE;
  String h = sha(key);
  uint8_t s = SCOPE_NONE;
  StateLock lk;
  for (auto& k : keys) if (k.hash == h) s = k.scope;
  return s;
}

String Keys::create(const String& name, uint8_t scope, String& key) {
  if (scope == SCOPE_NONE) return "scope: read, control or admin";
  if (!name.length() || name.length() > 32) return "Give the key a name (up to 32 characters)";
  StateLock lk0;
  if (keys.size() >= MAX_KEYS) return "At most 8 keys: delete one first";
  char buf[40];
  uint32_t r[4]; for (auto& x : r) x = esp_random();
  snprintf(buf, sizeof buf, "cm_%08x%08x%08x%08x", r[0], r[1], r[2], r[3]);
  key = buf;
  Key k; k.id = String(r[0] & 0xFFFF, HEX); k.name = name; k.hash = sha(key); k.scope = scope;
  keys.push_back(k);
  save();
  return "";
}

bool Keys::remove(const String& id) {
  bool found = false;
  StateLock lk;
  for (size_t i = 0; i < keys.size(); i++) if (keys[i].id == id) { keys.erase(keys.begin() + i); found = true; break; }
  if (found) save();
  return found;
}

void Keys::toJson(JsonArray a) {
  StateLock lk;
  for (auto& k : keys) { JsonObject o = a.add<JsonObject>(); o["id"] = k.id; o["name"] = k.name; o["scope"] = scopeStr(k.scope); }
}
