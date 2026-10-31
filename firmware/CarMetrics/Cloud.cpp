#include "Cloud.h"
#include "Health.h"
#include "State.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <deque>
#include <memory>
#include <sys/time.h>
#include <esp_random.h>

namespace {

// ============================================================== chunk format
// Header: "CMQ1" | bootId u32 | recSize u16 | nSensors u8 | idsLen u16 | ids (comma separated)
// Record: epochSec u32 (0 = clock unknown) | uptimeMs u32 | ms u16 | n x (avg,min,max) float32
const uint32_t CHUNK_MAX = 4096;
const size_t RAM_QUEUE_MAX = 6;
const char* QDIR = "/q";

struct Chunk {
  std::vector<uint8_t> buf;
  uint32_t records = 0;
  uint32_t createdMs = 0;
};

uint32_t bootId = 0;

template <typename T> void put(std::vector<uint8_t>& b, T v) { const uint8_t* p = (const uint8_t*)&v; b.insert(b.end(), p, p + sizeof(T)); }
template <typename T> T get(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

// ============================================================== RAM queue (loop -> net task)
portMUX_TYPE qMux = portMUX_INITIALIZER_UNLOCKED;
std::deque<Chunk*> ramQueue;

void enqueue(Chunk* c) {
  Chunk* dropped = nullptr;
  portENTER_CRITICAL(&qMux);
  if (ramQueue.size() >= RAM_QUEUE_MAX) { dropped = ramQueue.front(); ramQueue.pop_front(); }
  ramQueue.push_back(c);
  portEXIT_CRITICAL(&qMux);
  if (dropped) { StateLock lk; gNet.droppedRecords += dropped->records; delete dropped; }
}
Chunk* dequeue() {
  Chunk* c = nullptr;
  portENTER_CRITICAL(&qMux);
  if (!ramQueue.empty()) { c = ramQueue.front(); ramQueue.pop_front(); }
  portEXIT_CRITICAL(&qMux);
  return c;
}
size_t queueSize() { portENTER_CRITICAL(&qMux); size_t n = ramQueue.size(); portEXIT_CRITICAL(&qMux); return n; }

// ============================================================== recorder (loop task)
struct Acc { double sum = 0; float mn = INFINITY, mx = -INFINITY; uint32_t n = 0; };
std::vector<int> recIdx;          // sensor indexes recorded; -1-k = board metric Health::kSysIds[k]
std::vector<Acc> acc;
String idsCsv;
Chunk* cur = nullptr;
uint32_t periodStart = 0, lastTick = 0;
bool recEnabled = false;

void closeChunk() {
  if (cur && cur->records) enqueue(cur); else delete cur;
  cur = nullptr;
}

void newChunk() {
  cur = new Chunk();
  cur->createdMs = millis();
  cur->buf.reserve(CHUNK_MAX);
  const char m[4] = {'C', 'M', 'Q', '1'};
  cur->buf.insert(cur->buf.end(), m, m + 4);
  put<uint32_t>(cur->buf, bootId);
  put<uint16_t>(cur->buf, (uint16_t)(10 + recIdx.size() * 12));
  put<uint8_t>(cur->buf, (uint8_t)recIdx.size());
  put<uint16_t>(cur->buf, (uint16_t)idsCsv.length());
  cur->buf.insert(cur->buf.end(), idsCsv.begin(), idsCsv.end());
}

void emitRecord() {
  size_t recSize = 10 + recIdx.size() * 12;
  if (cur && cur->buf.size() + recSize > CHUNK_MAX) closeChunk();
  if (!cur) newChunk();
  struct timeval tv;
  bool timeOk;
  { StateLock lk; timeOk = gSys.timeSynced; }
  gettimeofday(&tv, nullptr);
  put<uint32_t>(cur->buf, timeOk ? (uint32_t)tv.tv_sec : 0);
  put<uint32_t>(cur->buf, millis());
  put<uint16_t>(cur->buf, timeOk ? (uint16_t)(tv.tv_usec / 1000) : 0);
  for (auto& a : acc) {
    float avg = a.n ? (float)(a.sum / a.n) : NAN;
    put<float>(cur->buf, avg);
    put<float>(cur->buf, a.n ? a.mn : NAN);
    put<float>(cur->buf, a.n ? a.mx : NAN);
    a = Acc();
  }
  cur->records++;
}

// ============================================================== net task state
volatile bool weatherRequested = true, flushRequested = false, clearRequested = false;
volatile bool sleepRequested = false, sleepReady = false, netRunning = false;
std::deque<std::pair<uint32_t, uint32_t>> backlog;   // (seq, bytes), oldest first
uint32_t nextSeq = 1;
bool ntpStarted = false;
uint32_t backoffMs = 0, lastFailMs = 0, lastWeatherTry = 0;

String fileName(uint32_t seq) { char b[24]; snprintf(b, sizeof(b), "%s/%08lu.bin", QDIR, (unsigned long)seq); return String(b); }

void scanBacklog() {
  backlog.clear();
  if (!LittleFS.exists(QDIR)) LittleFS.mkdir(QDIR);
  File d = LittleFS.open(QDIR);
  std::vector<std::pair<uint32_t, uint32_t>> v;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    uint32_t seq = strtoul(f.name(), nullptr, 10);   // name() is the bare file name
    if (seq) v.push_back({seq, (uint32_t)f.size()});
  }
  std::sort(v.begin(), v.end());
  for (auto& x : v) backlog.push_back(x);
  nextSeq = backlog.empty() ? 1 : backlog.back().first + 1;
}

uint32_t backlogBytes() { uint32_t t = 0; for (auto& x : backlog) t += x.second; return t; }

uint32_t countRecords(const std::vector<uint8_t>& b) {
  if (b.size() < 13) return 0;
  uint16_t recSize = get<uint16_t>(&b[8]);
  uint16_t idsLen = get<uint16_t>(&b[11]);
  size_t hdr = 13 + idsLen;
  return recSize && b.size() > hdr ? (b.size() - hdr) / recSize : 0;
}

void writeBacklog(Chunk* c, uint32_t capKB) {
  uint32_t t0 = millis();
  String fn = fileName(nextSeq);
  File f = LittleFS.open(fn, "w");
  if (!f) { StateLock lk; gNet.droppedRecords += c->records; gNet.lastError = "flash write failed"; return; }
  f.write(c->buf.data(), c->buf.size());
  Health::addFlashWrite(c->buf.size());
  f.close();
  backlog.push_back({nextSeq++, (uint32_t)c->buf.size()});
  // ring: drop oldest beyond the cap (and keep 64 KB of filesystem free)
  while (!backlog.empty() && (backlogBytes() > capKB * 1024UL || LittleFS.totalBytes() - LittleFS.usedBytes() < 65536)) {
    String old = fileName(backlog.front().first);
    File of = LittleFS.open(old, "r");
    std::vector<uint8_t> tmp(of ? of.size() : 0);
    if (of) { of.read(tmp.data(), tmp.size()); of.close(); }
    { StateLock lk; gNet.droppedRecords += countRecords(tmp); }
    LittleFS.remove(old);
    backlog.pop_front();
    vTaskDelay(1);
  }
#ifdef CM_EMULATOR_TEST
  Serial.printf("[net] chunk -> flash (%u bytes) in %lu ms\n", (unsigned)c->buf.size(), (unsigned long)(millis() - t0));
#endif
}

// ============================================================== decoding for upload
struct Rec { uint64_t tsMs; std::vector<float> v; };   // v = n*3 floats (copied: records are unaligned)
struct Decoded { std::vector<String> ids; std::vector<Rec> recs; uint32_t dropped = 0; };

bool decode(const std::vector<uint8_t>& b, Decoded& d) {
  if (b.size() < 13 || memcmp(b.data(), "CMQ1", 4)) return false;
  uint32_t boot = get<uint32_t>(&b[4]);
  uint16_t recSize = get<uint16_t>(&b[8]);
  uint8_t n = b[10];
  uint16_t idsLen = get<uint16_t>(&b[11]);
  if (13 + idsLen > b.size() || recSize != 10 + n * 12) return false;
  String ids((const char*)&b[13], idsLen);
  int start = 0;
  for (int i = 0; i <= (int)ids.length(); i++)
    if (i == (int)ids.length() || ids[i] == ',') { d.ids.push_back(ids.substring(start, i)); start = i + 1; }
  if (d.ids.size() != n) return false;
  struct timeval tv; gettimeofday(&tv, nullptr);
  uint64_t nowMs = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  for (size_t p = 13 + idsLen; p + recSize <= b.size(); p += recSize) {
    uint32_t sec = get<uint32_t>(&b[p]);
    uint32_t up = get<uint32_t>(&b[p + 4]);
    uint16_t ms = get<uint16_t>(&b[p + 8]);
    uint64_t ts;
    if (sec) ts = (uint64_t)sec * 1000 + ms;
    else if (boot == bootId) ts = nowMs - (millis() - up);   // recorded before the clock was set, same boot
    else { d.dropped++; continue; }                          // older boot, time unknown
    Rec r; r.tsMs = ts; r.v.resize(n * 3);
    memcpy(r.v.data(), &b[p + 10], n * 12);
    d.recs.push_back(std::move(r));
  }
  return true;
}

String fmtNum(float v) {
  char b[20];
  dtostrf(v, 1, fabsf(v) < 100 ? 3 : 1, b);
  // trim trailing zeros
  char* e = b + strlen(b) - 1;
  if (strchr(b, '.')) { while (*e == '0') *e-- = 0; if (*e == '.') *e = 0; }
  return String(b);
}

// ============================================================== Influx line protocol
String buildInflux(const CloudConfig& cc, const String& device, const Decoded& d) {
  String body;
  body.reserve(d.recs.size() * (40 + d.ids.size() * 40));
  String meas = cc.influxMeasurement, dev = device;
  dev.replace(" ", "\\ "); dev.replace(",", "\\,");
  for (auto& r : d.recs) {
    String fields;
    for (size_t i = 0; i < d.ids.size(); i++) {
      float a = r.v[i * 3], mn = r.v[i * 3 + 1], mx = r.v[i * 3 + 2];
      if (isnan(a)) continue;
      if (fields.length()) fields += ',';
      fields += d.ids[i] + "=" + fmtNum(a);
      if (!isnan(mn) && mn != mx) fields += "," + d.ids[i] + "_min=" + fmtNum(mn) + "," + d.ids[i] + "_max=" + fmtNum(mx);
    }
    if (!fields.length()) continue;
    char ts[24]; snprintf(ts, sizeof(ts), "%llu", (unsigned long long)r.tsMs);
    body += meas + ",device=" + dev + " " + fields + " " + ts + "\n";
  }
  return body;
}

bool sendInflux(const CloudConfig& cc, const String& device, const Decoded& d, String& err) {
  String body = buildInflux(cc, device, d);
  if (!body.length()) return true;

  String url = cc.influxUrl;
  if (url.indexOf("precision=") < 0) url += (url.indexOf('?') < 0 ? "?" : "&") + String("precision=ms");
  std::unique_ptr<WiFiClient> client;
  if (url.startsWith("https://")) { auto* s = new WiFiClientSecure(); s->setInsecure(); client.reset(s); }
  else client.reset(new WiFiClient());
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  if (!http.begin(*client, url)) { err = "bad Influx URL"; return false; }
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  if (cc.influxAuth.length()) http.addHeader("Authorization", cc.influxAuth);
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  String resp = code > 0 ? http.getString() : "";
  http.end();
  if (code >= 200 && code < 300) return true;
  err = code > 0 ? "Influx HTTP " + String(code) + " " + resp.substring(0, 80) : "Influx: " + HTTPClient::errorToString(code);
  return false;
}

// ============================================================== Zabbix trapper
String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o;
}

bool zabbixRequest(const CloudConfig& cc, const String& json, String& err) {
  std::unique_ptr<WiFiClient> client;
  if (cc.zbxPsk.length()) {
    auto* s = new WiFiClientSecure();
    s->setPreSharedKey(cc.zbxPskIdentity.c_str(), cc.zbxPsk.c_str());
    s->setHandshakeTimeout(10);
    client.reset(s);
  } else client.reset(new WiFiClient());
  if (!client->connect(cc.zbxServer.c_str(), cc.zbxPort, 8000)) { err = "Zabbix: can't connect to " + cc.zbxServer + ":" + cc.zbxPort; return false; }
  uint8_t hdr[13] = {'Z', 'B', 'X', 'D', 0x01};
  uint32_t len = json.length();
  memcpy(hdr + 5, &len, 4);   // little-endian data length, then 4 reserved bytes
  client->write(hdr, sizeof(hdr));
  client->write((const uint8_t*)json.c_str(), json.length());
  uint32_t t0 = millis();
  String resp;
  while (millis() - t0 < 10000) {
    while (client->available()) resp += (char)client->read();
    if (resp.length() >= 13 && resp.indexOf('}') > 0) break;
    if (!client->connected() && !client->available()) break;
    delay(20);
  }
  client->stop();
  int j = resp.indexOf('{');
  String body = j >= 0 ? resp.substring(j) : "";
  if (body.indexOf("\"success\"") >= 0) {
    int f = body.indexOf("failed: ");
    if (f >= 0 && body.substring(f + 8).toInt() > 0) {
      err = "Zabbix accepted the batch but rejected some values - check item keys/types (" + body.substring(f, f + 30) + ")";
    }
    return true;
  }
  err = body.length() ? "Zabbix: " + body.substring(0, 120) : "Zabbix: no response";
  return false;
}

std::vector<String> buildZabbix(const CloudConfig& cc, const Decoded& d) {
  std::vector<String> out;
  struct timeval tv; gettimeofday(&tv, nullptr);
  String host = jsonEsc(cc.zbxHost);
  String batch; int items = 0;
  auto flush = [&]() {
    if (!items) return;
    out.push_back("{\"request\":\"sender data\",\"data\":[" + batch + "],\"clock\":" + String((uint32_t)tv.tv_sec) + ",\"ns\":0}");
    batch = ""; items = 0;
  };
  for (auto& r : d.recs) {
    String clock = String((uint32_t)(r.tsMs / 1000)), ns = String((uint32_t)(r.tsMs % 1000) * 1000000UL);
    auto item = [&](const String& key, const String& val) {
      if (items) batch += ',';
      batch += "{\"host\":\"" + host + "\",\"key\":\"" + key + "\",\"value\":\"" + val + "\",\"clock\":" + clock + ",\"ns\":" + ns + "}";
      items++;
    };
    if (cc.zbxPacked) {
      // one item per sample: carmetrics.json = {"coolant_temp":[avg,min,max],...}; split with dependent items in Zabbix
      String v = "{";
      for (size_t i = 0; i < d.ids.size(); i++) {
        if (isnan(r.v[i * 3])) continue;
        if (v.length() > 1) v += ',';
        v += "\\\"" + d.ids[i] + "\\\":[" + fmtNum(r.v[i * 3]) + "," + fmtNum(r.v[i * 3 + 1]) + "," + fmtNum(r.v[i * 3 + 2]) + "]";
      }
      v += "}";
      item("carmetrics.json", v);
    } else {
      for (size_t i = 0; i < d.ids.size(); i++) {
        if (isnan(r.v[i * 3])) continue;
        item(d.ids[i] + ".avg", fmtNum(r.v[i * 3]));
        item(d.ids[i] + ".min", fmtNum(r.v[i * 3 + 1]));
        item(d.ids[i] + ".max", fmtNum(r.v[i * 3 + 2]));
      }
    }
    if (items >= 200 || batch.length() > 12000) flush();
  }
  flush();
  return out;
}

bool sendZabbix(const CloudConfig& cc, const Decoded& d, String& err) {
  for (auto& json : buildZabbix(cc, d))
    if (!zabbixRequest(cc, json, err)) return false;
  return true;
}

bool sendChunk(const CloudConfig& cc, const String& device, const std::vector<uint8_t>& buf, String& err) {
  Decoded d;
  if (!decode(buf, d)) { err = ""; return true; }   // corrupt chunk: drop silently
  bool ok = cc.protocol == "influx" ? sendInflux(cc, device, d, err) : sendZabbix(cc, d, err);
  StateLock lk;
  if (ok) gNet.sentRecords += d.recs.size();
  gNet.droppedRecords += d.dropped;
  return ok;
}

// ============================================================== weather (Open-Meteo)
bool fetchWeather(const WeatherConfig& wc, String& err) {
  if (wc.towns.empty()) { err = ""; return true; }
  String lat, lon;
  for (auto& t : wc.towns) {
    if (lat.length()) { lat += ','; lon += ','; }
    lat += String(t.lat, 4); lon += String(t.lon, 4);
  }
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,weather_code,"
               "cloud_cover,pressure_msl,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
               "&timezone=Africa%2FNairobi";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  // HTTP/1.0: Open-Meteo answers HTTP/1.1 requests with a chunked body, and
  // parsing the raw stream then fails with "InvalidInput" (chunk sizes in the JSON).
  http.useHTTP10(true);
  if (!http.begin(client, url)) { err = "weather: bad URL"; return false; }
  int code = http.GET();
  if (code != 200) { err = "weather: " + (code > 0 ? "HTTP " + String(code) : HTTPClient::errorToString(code)); http.end(); return false; }
  // Keep only what's used ("current" + elevation): the reply also carries units,
  // time zone etc. per town, and this runs while TLS already holds ~40 KB of RAM.
  JsonDocument filter;
  JsonObject fo = wc.towns.size() > 1 ? filter.add<JsonObject>() : filter.to<JsonObject>();
  fo["current"] = true; fo["elevation"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) { err = String("weather: reply not readable (") + e.c_str() + ")"; return false; }
  std::vector<WeatherReading> out;
  auto one = [&](JsonObjectConst o, const WeatherTown& t) {
    WeatherReading w;
    w.name = t.name;
    JsonObjectConst c = o["current"];
    w.time = c["time"] | "";
    w.temp = c["temperature_2m"] | NAN; w.feels = c["apparent_temperature"] | NAN;
    w.rh = c["relative_humidity_2m"] | NAN; w.precip = c["precipitation"] | NAN;
    w.cloud = c["cloud_cover"] | NAN; w.code = c["weather_code"] | -1;
    w.msl = c["pressure_msl"] | NAN; w.sfc = c["surface_pressure"] | NAN;
    w.wind = c["wind_speed_10m"] | NAN; w.windDir = c["wind_direction_10m"] | NAN; w.gust = c["wind_gusts_10m"] | NAN;
    w.elevation = o["elevation"] | NAN;
    out.push_back(w);
  };
  if (doc.is<JsonArray>()) {
    JsonArrayConst a = doc.as<JsonArrayConst>();
    for (size_t i = 0; i < a.size() && i < wc.towns.size(); i++) one(a[i], wc.towns[i]);
  } else one(doc.as<JsonObjectConst>(), wc.towns[0]);

  StateLock lk;
  gWeather = out;
  gNet.weatherAtMs = millis();
  gNet.seaLevelAuto = NAN; gNet.seaLevelTown = "";
  for (auto& w : out)
    if (w.name == wc.refTown && !isnan(w.msl) && w.msl > 850 && w.msl < 1100) { gNet.seaLevelAuto = w.msl; gNet.seaLevelTown = w.name; }
  return true;
}

// ============================================================== net task
void netTask(void*) {
  Health::registerTask("net");
  bool fsOk = LittleFS.begin();
  if (fsOk) scanBacklog();
  Serial.printf("[net] task started, fs=%d backlog=%u files\n", fsOk, (unsigned)backlog.size());
  std::deque<Chunk*> pending;
  uint32_t lastSend = 0;
  netRunning = true;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(sleepRequested ? 20 : 500));
    if (sleepRequested) {   // going to deep sleep: everything in RAM goes to the flash ring
      while (Chunk* c = dequeue()) pending.push_back(c);
      CloudConfig cc; { StateLock lk; cc = gConfig.cloud; }
      for (auto* c : pending) { if (cc.enabled) writeBacklog(c, cc.bufferKB); delete c; }
      pending.clear();
      sleepReady = true;
      continue;
    }
    uint32_t now = millis();
    WeatherConfig wc; CloudConfig cc; String device; bool ota;
    { StateLock lk; wc = gConfig.weather; cc = gConfig.cloud; device = gConfig.sys.hostname; ota = gSys.otaProgress >= 0; }
    if (ota) continue;   // filesystem may be rewritten during an update

    bool sta = WiFi.status() == WL_CONNECTED;
    if (sta && !ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com"); ntpStarted = true; }
    bool ntpOk = time(nullptr) > 1700000000;
    {
      StateLock lk;
      gNet.sta = sta;
      if (!sta) gNet.internet = false;
      if (ntpOk && sta && !gNet.ntp) { gNet.ntp = true; gSys.timeSynced = true; gSys.timeSource = "internet"; }
    }

    // ---- weather
    if (wc.enabled && sta && (weatherRequested || !gNet.weatherAtMs ||
        now - gNet.weatherAtMs > wc.refreshMinutes * 60000UL) && (weatherRequested || now - lastWeatherTry > 120000)) {
      weatherRequested = false;
      lastWeatherTry = now;
      String err;
      bool ok = fetchWeather(wc, err);
      StateLock lk;
      gNet.weatherError = ok ? "" : err;
      if (ok) gNet.internet = true;
    }

    // ---- cloud
    if (clearRequested) {
      clearRequested = false;
      for (auto& x : backlog) LittleFS.remove(fileName(x.first));
      backlog.clear();
      for (auto* c : pending) delete c;
      pending.clear();
    }
    while (Chunk* c = dequeue()) pending.push_back(c);

    bool timeOk; { StateLock lk; timeOk = gSys.timeSynced; }
    bool canSend = cc.enabled && sta && timeOk && (flushRequested || now - lastFailMs >= backoffMs);
    if (canSend && (!pending.empty() || !backlog.empty()) && (flushRequested || now - lastSend >= 2000)) {
      flushRequested = false;
      lastSend = now;
      String err;
      bool ok = true;
      // fresh data first, then the oldest backlog (a few files per pass)
      while (ok && !pending.empty()) {
        ok = sendChunk(cc, device, pending.front()->buf, err);
        if (ok) { delete pending.front(); pending.pop_front(); }
      }
      for (int k = 0; ok && k < 3 && !backlog.empty(); k++) {
        String fn = fileName(backlog.front().first);
        File f = LittleFS.open(fn, "r");
        std::vector<uint8_t> buf(f ? f.size() : 0);
        if (f) { f.read(buf.data(), buf.size()); f.close(); }
        ok = sendChunk(cc, device, buf, err);
        if (ok) { LittleFS.remove(fn); backlog.pop_front(); }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      StateLock lk;
      gNet.lastTryMs = now;
      if (ok) { gNet.lastOkMs = now; gNet.lastError = err; gNet.internet = true; backoffMs = 0; }
      else { gNet.lastError = err; lastFailMs = now; backoffMs = backoffMs ? std::min<uint32_t>(backoffMs * 2, 300000) : 15000; }
    }
    // anything still pending and not sendable right now goes to the flash ring
    bool sendingSoon = cc.enabled && sta && timeOk && backoffMs == 0;
    while (!pending.empty() && (!sendingSoon || pending.size() > 2)) {
      Chunk* c = pending.front(); pending.pop_front();
      if (cc.enabled) writeBacklog(c, cc.bufferKB);
      delete c;
      vTaskDelay(pdMS_TO_TICKS(10));   // let IDLE0 run (task watchdog) between flash writes
    }

    StateLock lk;
    gNet.ramChunks = pending.size() + queueSize();
    gNet.backlogFiles = backlog.size();
    gNet.backlogBytes = backlogBytes();
    uint32_t pr = 0;
    for (auto* c : pending) pr += c->records;
    gNet.pendingRecords = pr + gNet.backlogBytes / max<uint32_t>(1, 10 + recIdx.size() * 12);
  }
}

}  // namespace

// ============================================================== public
void Cloud::begin() {
  if (!bootId) bootId = esp_random() | 1;
  weatherRequested = true;   // towns may have changed
  closeChunk();
  recIdx.clear();
  idsCsv = "";
  for (size_t i = 0; i < gConfig.sensors.size(); i++) {
    if (!gConfig.sensors[i].enabled) continue;
    recIdx.push_back(i);
    if (idsCsv.length()) idsCsv += ',';
    idsCsv += gConfig.sensors[i].id;
  }
  if (gConfig.cloud.sendSystem)
    for (uint8_t k = 0; k < Health::kSysCount; k++) { recIdx.push_back(-1 - k); idsCsv += ','; idsCsv += Health::kSysIds[k]; }
  acc.assign(recIdx.size(), Acc());
  periodStart = lastTick = millis();
  recEnabled = gConfig.cloud.enabled && !recIdx.empty();
}

void Cloud::update() {
  const CloudConfig& cc = gConfig.cloud;
  uint32_t now = millis();
  bool engine; { StateLock lk; engine = gSys.engineRunning; }
  bool active = recEnabled && (!cc.onlyEngineRunning || engine);
  { StateLock lk; gNet.recording = active; }
  if (!active) {
    if (cur && cur->records) closeChunk();   // hand over what we have when the engine stops
    periodStart = now;
    for (auto& a : acc) a = Acc();
    return;
  }
  uint32_t tick = cc.intervalMs < 250 ? cc.intervalMs : 250;
  if (now - lastTick >= tick) {
    lastTick = now;
    StateLock lk;
    for (size_t k = 0; k < recIdx.size(); k++) {
      float v;
      if (recIdx[k] < 0) v = Health::sysMetric(-1 - recIdx[k]);
      else {
        const SensorRuntime& r = gSensorRt[recIdx[k]];
        if (r.status != ST_OK) continue;
        v = r.value;
      }
      if (isnan(v)) continue;
      Acc& a = acc[k];
      a.sum += v; a.n++;
      if (v < a.mn) a.mn = v;
      if (v > a.mx) a.mx = v;
    }
  }
  if (now - periodStart >= cc.intervalMs) {
    periodStart += cc.intervalMs;
    if (now - periodStart > cc.intervalMs) periodStart = now;   // don't try to catch up after a stall
    emitRecord();
  }
  // Online: hand over every uploadEverySec. Offline: only full 4 KB chunks go to flash (fewer erases).
  bool online; { StateLock lk; online = gNet.sta && gSys.timeSynced; }
  if (online && cur && cur->records && now - cur->createdMs >= cc.uploadEverySec * 1000UL) closeChunk();
}

#ifdef CM_EMU_SELFTEST
// Build a chunk from fake readings, decode it, and render both wire formats.
void Cloud::selftestEncode(String& influx, String& zabbix, uint32_t& records) {
  closeChunk();
  for (int k = 0; k < 2; k++) {
    for (auto& a : acc) { a.sum = 90 + k; a.n = 1; a.mn = 89 + k; a.mx = 91.5f + k; }
    if (!acc.empty()) { acc.back() = Acc(); }   // last sensor has no data -> skipped
    emitRecord();
  }
  Decoded d;
  bool ok = cur && decode(cur->buf, d);
  records = ok ? d.recs.size() : 0;
  CloudConfig cc; cc.influxMeasurement = "carmetrics"; cc.zbxHost = "my car";
  influx = ok ? buildInflux(cc, "carmetrics", d) : "";
  auto z = ok ? buildZabbix(cc, d) : std::vector<String>();
  zabbix = z.empty() ? "" : z[0];
  closeChunk();
}
#endif

void Cloud::startNetTask() {
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void Cloud::requestWeather() { weatherRequested = true; }

void Cloud::prepareSleep(uint32_t timeoutMs) {
  closeChunk();
  if (!netRunning) return;
  sleepReady = false; sleepRequested = true;
  uint32_t t0 = millis();
  while (!sleepReady && millis() - t0 < timeoutMs) delay(20);
}
void Cloud::requestFlush() { flushRequested = true; }
void Cloud::requestClearBacklog() { clearRequested = true; }

static void putF(JsonObject o, const char* k, float v) { if (isnan(v)) o[k] = nullptr; else o[k] = v; }

void Cloud::weatherToJson(JsonObject o) {
  o["enabled"] = gConfig.weather.enabled;
  o["sta"] = gNet.sta;
  o["ageS"] = gNet.weatherAtMs ? (int32_t)((millis() - gNet.weatherAtMs) / 1000) : -1;
  o["error"] = gNet.weatherError;
  JsonObject sl = o["seaLevel"].to<JsonObject>();
  putF(sl, "hpa", effectiveSeaLevelHpa());
  sl["source"] = !isnan(gNet.seaLevelAuto) && gConfig.weather.autoSeaLevel ? "weather" : "manual";
  sl["town"] = gNet.seaLevelTown;
  JsonArray a = o["towns"].to<JsonArray>();
  for (auto& w : gWeather) {
    JsonObject x = a.add<JsonObject>();
    x["name"] = w.name; x["time"] = w.time; x["code"] = w.code;
    putF(x, "temp", w.temp); putF(x, "feels", w.feels); putF(x, "rh", w.rh); putF(x, "precip", w.precip);
    putF(x, "cloud", w.cloud); putF(x, "msl", w.msl); putF(x, "sfc", w.sfc); putF(x, "wind", w.wind);
    putF(x, "windDir", w.windDir); putF(x, "gust", w.gust); putF(x, "elevation", w.elevation);
  }
}

void Cloud::statusToJson(JsonObject o) {
  o["enabled"] = gConfig.cloud.enabled;
  o["sta"] = gNet.sta; o["internet"] = gNet.internet; o["ntp"] = gNet.ntp; o["timeSynced"] = gSys.timeSynced;
  o["recording"] = gNet.recording;
  o["ramChunks"] = gNet.ramChunks; o["backlogFiles"] = gNet.backlogFiles; o["backlogBytes"] = gNet.backlogBytes;
  o["pendingRecords"] = gNet.pendingRecords; o["sentRecords"] = gNet.sentRecords; o["droppedRecords"] = gNet.droppedRecords;
  o["lastOkAgeS"] = gNet.lastOkMs ? (int32_t)((millis() - gNet.lastOkMs) / 1000) : -1;
  o["lastTryAgeS"] = gNet.lastTryMs ? (int32_t)((millis() - gNet.lastTryMs) / 1000) : -1;
  o["lastError"] = gNet.lastError;
  o["recordBytes"] = 10 + recIdx.size() * 12;
  o["fsFree"] = LittleFS.totalBytes() - LittleFS.usedBytes();
}
