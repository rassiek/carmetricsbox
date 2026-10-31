#include "History.h"
#include "State.h"
#include "Half.h"

namespace {
// [sample][sensor], half floats: half the RAM of float (charts need ~3 digits).
// In PSRAM when the board has it, so the internal RAM stays free for Wi-Fi / TLS.
uint16_t* buf = nullptr;
size_t len = HISTORY_LEN, nSensors = 0, head = 0, filled = 0;
uint32_t generation = 0;   // bumped by begin(): readers in progress stop cleanly
uint32_t lastSample = 0;
}

void History::begin() {
  StateLock lk;
  nSensors = gConfig.sensors.size();
  generation++;
  if (buf) { heap_caps_free(buf); buf = nullptr; }
  bool psram = ESP.getPsramSize() > 0;
  len = psram ? HISTORY_LEN_PSRAM : HISTORY_LEN;
  size_t n = nSensors * len;
  if (n) {
    if (psram) buf = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { len = HISTORY_LEN; n = nSensors * len; buf = (uint16_t*)malloc(n * 2); }
    if (!buf) { nSensors = 0; Serial.println("[history] no memory for chart history"); }
    else for (size_t i = 0; i < n; i++) buf[i] = Half::NaN;
  }
  head = filled = 0;
  lastSample = millis();
}

void History::update() {
  uint32_t now = millis();
  if (now - lastSample < gConfig.sys.historyIntervalSeconds * 1000UL) return;
  lastSample = now;
  StateLock lk;
  if (!nSensors) return;
  for (size_t i = 0; i < nSensors && i < gSensorRt.size(); i++) buf[head * nSensors + i] = Half::fromFloat(gSensorRt[i].value);
  head = (head + 1) % len;
  if (filled < len) filled++;
}

// {"interval":5,"len":360,"count":N,"ageMs":1234,"series":{"id":[v,...],...}}  oldest first
void History::writeJson(Print& out) {
  Reader rd;
  uint8_t b[256]; size_t n;
  while ((n = rd.read(b, sizeof(b))) > 0) out.write(b, n);
}

size_t History::count() { return filled; }
size_t History::bytes() { return buf ? nSensors * len * 2 : 0; }

History::Reader::Reader() {
  gen = generation; sensors = std::min(nSensors, gConfig.sensors.size());
  count = filled; start = (head + len - count) % len;
}

bool History::Reader::next() {   // lock held
  tokLen = tokPos = 0;
  if (gen != generation && phase < 4) phase = phase == 2 ? 3 : 4;   // settings changed mid-reply: close the JSON
  switch (phase) {
    case 0:
      tokLen = snprintf(tok, sizeof(tok), "{\"interval\":%u,\"len\":%u,\"count\":%u,\"ageMs\":%lu,\"series\":{",
                        gConfig.sys.historyIntervalSeconds, (unsigned)len, (unsigned)count, (unsigned long)(millis() - lastSample));
      phase = sensors ? 1 : 4;
      return true;
    case 1:
      tokLen = snprintf(tok, sizeof(tok), "%s\"%.40s\":[", pos ? "," : "", gConfig.sensors[pos].id.c_str());
      sample = 0; phase = count ? 2 : 3;
      return true;
    case 2: {
      float v = Half::toFloat(buf[((start + sample) % len) * nSensors + pos]);
      const char* sep = sample ? "," : "";
      if (isnan(v) || isinf(v)) tokLen = snprintf(tok, sizeof(tok), "%snull", sep);
      else tokLen = snprintf(tok, sizeof(tok), "%s%.*f", sep, (int)gConfig.sensors[pos].decimals + 1, v);
      if (++sample >= count) phase = 3;
      return true;
    }
    case 3:
      tok[0] = ']'; tokLen = 1;
      phase = ++pos < sensors && gen == generation ? 1 : 4;
      return true;
    case 4:
      tok[0] = '}'; tok[1] = '}'; tokLen = 2; phase = 5;
      return true;
    default:
      return false;
  }
}

size_t History::Reader::read(uint8_t* out, size_t max) {
  StateLock lk;
  size_t n = 0;
  while (n < max) {
    if (tokPos >= tokLen && !next()) break;
    size_t take = std::min(tokLen - tokPos, max - n);
    memcpy(out + n, tok + tokPos, take);
    n += take; tokPos += take;
  }
  return n;
}
