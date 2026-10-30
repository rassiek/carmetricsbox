#include "History.h"
#include "State.h"

namespace {
std::vector<float> buf;   // [sample][sensor]
size_t nSensors = 0, head = 0, count = 0;
uint32_t lastSample = 0;
}

void History::begin() {
  StateLock lk;
  nSensors = gConfig.sensors.size();
  buf.assign(nSensors * HISTORY_LEN, NAN);
  head = count = 0;
  lastSample = millis();
}

void History::update() {
  uint32_t now = millis();
  if (now - lastSample < gConfig.sys.historyIntervalSeconds * 1000UL) return;
  lastSample = now;
  StateLock lk;
  if (!nSensors) return;
  for (size_t i = 0; i < nSensors && i < gSensorRt.size(); i++) buf[head * nSensors + i] = gSensorRt[i].value;
  head = (head + 1) % HISTORY_LEN;
  if (count < HISTORY_LEN) count++;
}

// {"interval":5,"count":N,"ageMs":1234,"series":{"id":[v,...],...}}  oldest first
void History::writeJson(Print& out) {
  out.printf("{\"interval\":%u,\"count\":%u,\"ageMs\":%lu,\"series\":{",
             gConfig.sys.historyIntervalSeconds, (unsigned)count, (unsigned long)(millis() - lastSample));
  size_t start = (head + HISTORY_LEN - count) % HISTORY_LEN;
  for (size_t i = 0; i < nSensors && i < gConfig.sensors.size(); i++) {
    if (i) out.print(',');
    out.printf("\"%s\":[", gConfig.sensors[i].id.c_str());
    uint8_t dec = gConfig.sensors[i].decimals + 1;
    for (size_t k = 0; k < count; k++) {
      if (k) out.print(',');
      float v = buf[((start + k) % HISTORY_LEN) * nSensors + i];
      if (isnan(v)) out.print("null"); else out.print(v, dec);
    }
    out.print(']');
  }
  out.print("}}");
}
