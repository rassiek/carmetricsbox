#include "BlackBox.h"
#include "State.h"
#include "Health.h"
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace {
const char* DIR_BB = "/bb";
std::vector<String> ids;                 // sensor ids in the ring
std::vector<float> ring;                 // (2*BB_SECONDS+1) rows x ids.size()
size_t rows = 2 * BB_SECONDS + 1, head = 0, filled = 0;
uint32_t lastSample = 0, lastTriggerMs = 0;
int32_t remaining = -1;                  // samples still to record after the event, -1 = idle
String reason; time_t eventEpoch = 0; uint32_t eventUptime = 0;
volatile bool pendingTrigger = false; String pendingReason;
portMUX_TYPE trigMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t fsMutex = nullptr;     // file list vs. web reads
uint32_t nextSeq = 1;

String fileFor(uint32_t seq) { char b[24]; snprintf(b, sizeof(b), "%s/%06lu.json", DIR_BB, (unsigned long)seq); return String(b); }

std::vector<uint32_t> listSeqs() {
  std::vector<uint32_t> v;
  File d = LittleFS.open(DIR_BB);
  if (!d) return v;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    uint32_t s = n.toInt();
    if (s) v.push_back(s);
  }
  std::sort(v.begin(), v.end());
  return v;
}

void save() {
  // Rows oldest -> newest
  String out;
  out.reserve(ids.size() * rows * 6 + 256);
  out += "{\"seq\":"; out += nextSeq;
  out += ",\"epoch\":"; out += (uint32_t)eventEpoch;
  out += ",\"uptimeS\":"; out += eventUptime / 1000;
  out += ",\"reason\":\""; out += reason; out += "\",\"interval\":1,\"before\":"; out += BB_SECONDS;
  out += ",\"ids\":[";
  for (size_t k = 0; k < ids.size(); k++) { if (k) out += ','; out += '"'; out += ids[k]; out += '"'; }
  out += "],\"rows\":[";
  size_t start = (head + rows - filled) % rows;
  for (size_t r = 0; r < filled; r++) {
    if (r) out += ',';
    out += '[';
    const float* row = &ring[((start + r) % rows) * ids.size()];
    for (size_t k = 0; k < ids.size(); k++) { if (k) out += ','; if (isnan(row[k])) out += "null"; else out += String(row[k], 2); }
    out += ']';
  }
  out += "]}";

  xSemaphoreTake(fsMutex, portMAX_DELAY);
  if (!LittleFS.exists(DIR_BB)) LittleFS.mkdir(DIR_BB);
  auto seqs = listSeqs();
  while (!seqs.empty() && seqs.size() >= gConfig.blackbox.keepEvents) { LittleFS.remove(fileFor(seqs.front())); seqs.erase(seqs.begin()); }
  File f = LittleFS.open(fileFor(nextSeq), "w");
  size_t n = 0;
  if (f) {   // in pieces, so other tasks get the CPU between flash writes
    for (size_t p = 0; p < out.length(); p += 512) { n += f.write((const uint8_t*)out.c_str() + p, min<size_t>(512, out.length() - p)); delay(2); }
    f.close();
  }
  xSemaphoreGive(fsMutex);
  Health::addFlashWrite(n);
  Serial.printf("[bb] event %u saved: %s (%u bytes)\n", (unsigned)nextSeq, reason.c_str(), (unsigned)n);
  nextSeq++;
}
}  // namespace

void BlackBox::begin() {
  if (!fsMutex) {
    fsMutex = xSemaphoreCreateMutex();
    auto s = listSeqs();
    if (!s.empty()) nextSeq = s.back() + 1;
  }
  std::vector<String> next;
  for (auto& c : gConfig.sensors) if (c.enabled) next.push_back(c.id);
  if (next == ids) return;   // same sensors: keep the history and any recording in progress
  ids = next;
  ring.assign(rows * ids.size(), NAN);
  head = filled = 0; remaining = -1;
}

void BlackBox::trigger(const String& why) {
  portENTER_CRITICAL(&trigMux);
  if (!pendingTrigger) { pendingTrigger = true; }
  portEXIT_CRITICAL(&trigMux);
  pendingReason = why;   // last writer wins; only read by loop() below
}

void BlackBox::update() {
  uint32_t now = millis();
  if (pendingTrigger) {
    pendingTrigger = false;
    bool ok = gConfig.blackbox.enabled && remaining < 0 && now > 20000 && (!lastTriggerMs || now - lastTriggerMs > 60000) && !ids.empty();
    if (ok) {
      lastTriggerMs = now; remaining = BB_SECONDS;
      reason = pendingReason; eventUptime = now;
      bool synced; { StateLock lk; synced = gSys.timeSynced; }
      eventEpoch = synced ? time(nullptr) : 0;
      Serial.printf("[bb] recording: %s\n", reason.c_str());
    }
  }
  if (now - lastSample < 1000 || ids.empty()) return;
  lastSample = now;
  float* row = &ring[head * ids.size()];
  {
    StateLock lk;
    for (size_t k = 0; k < ids.size(); k++) {
      int i = findSensorIndex(ids[k]);
      row[k] = (i >= 0 && gSensorRt[i].status == ST_OK) ? gSensorRt[i].value : NAN;
    }
  }
  head = (head + 1) % rows;
  if (filled < rows) filled++;
  if (remaining > 0) remaining--;
  if (remaining == 0) { save(); remaining = -1; }
}

void BlackBox::listToJson(JsonArray a) {
  if (!fsMutex) return;
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  auto seqs = listSeqs();
  for (auto it = seqs.rbegin(); it != seqs.rend(); ++it) {
    File f = LittleFS.open(fileFor(*it), "r");
    if (!f) continue;
    char head[200]; size_t n = f.readBytes(head, sizeof(head) - 1); head[n] = 0;   // seq, epoch, uptime, reason come first
    JsonObject o = a.add<JsonObject>();
    o["seq"] = *it; o["bytes"] = f.size();
    String h(head);
    auto num = [&](const char* k) { int p = h.indexOf(k); return p < 0 ? 0L : h.substring(p + strlen(k)).toInt(); };
    o["epoch"] = num("\"epoch\":"); o["uptimeS"] = num("\"uptimeS\":");
    int r = h.indexOf("\"reason\":\""); if (r >= 0) { int e = h.indexOf('"', r + 10); o["reason"] = h.substring(r + 10, e); }
    f.close();
  }
  xSemaphoreGive(fsMutex);
}

bool BlackBox::read(uint32_t seq, String& out) {
  if (!fsMutex) return false;
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File f = LittleFS.open(fileFor(seq), "r");
  bool ok = (bool)f;
  if (ok) { out = f.readString(); f.close(); }
  xSemaphoreGive(fsMutex);
  return ok;
}

void BlackBox::clear() {
  if (!fsMutex) return;
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  for (uint32_t s : listSeqs()) LittleFS.remove(fileFor(s));
  xSemaphoreGive(fsMutex);
}
