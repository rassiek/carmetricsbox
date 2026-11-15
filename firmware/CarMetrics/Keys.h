// CarMetrics - API keys for integrations ("Authorization: Bearer cm_..."). Only a
// SHA-256 of each key is kept (NVS, not in the settings backup); the key itself
// is shown once when it is made.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum Scope : uint8_t { SCOPE_NONE = 0, SCOPE_READ = 1, SCOPE_CONTROL = 2, SCOPE_ADMIN = 3 };
const char* scopeStr(uint8_t s);
uint8_t scopeFromStr(const String& s);   // SCOPE_NONE if unknown

namespace Keys {
void begin();
uint8_t check(const String& key);        // scope of this key, SCOPE_NONE if unknown
String create(const String& name, uint8_t scope, String& key);   // "" = ok; key = the new key
bool remove(const String& id);
void toJson(JsonArray a);                // id, name, scope (never the key)
}
