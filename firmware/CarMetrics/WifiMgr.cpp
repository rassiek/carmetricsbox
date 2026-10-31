#include "WifiMgr.h"
#include "State.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <algorithm>

namespace {
enum Phase : uint8_t { IDLE, SCANNING, CONNECTING, RETRY_WAIT, CONNECTED };
enum Ev : uint8_t { EV_JOINED, EV_FAILED, EV_NONE_FOUND, EV_LOST, EV_SCAN_FAILED, EV_WAIT_AP };
const char* const EV_NAMES[] = {"joined", "failed", "no saved network in range", "connection lost", "scan failed", "waiting: access point in use"};

const uint8_t LOG_N = 20, SEEN_N = 16, MAX_CANDS = 3, TRIES_PER_ROUTER = 3;
const uint32_t ATTEMPT_MS = 15000, RETRY_GAP_MS = 1500, SCAN_TIMEOUT_MS = 15000;
const uint32_t BACKOFF_MIN = 10000, BACKOFF_MAX = 60000;

// A router to try: from our own scan (channel + BSSID known), the remembered
// network (fast rejoin, no scan) or a saved network the scan didn't show
// (hidden SSID: channel 0, the driver searches for it).
struct Cand {
  String ssid, pass;
  uint8_t bssid[6] = {0};
  uint8_t ch = 0, auth = WIFI_AUTH_WPA2_PSK, prio = 0;
  int8_t rssi = 0;
  const char* how = "";
};
struct SeenNet { char ssid[33]; int8_t rssi; uint8_t ch, auth; bool known; };
struct LogEntry {
  uint32_t t;            // uptime, s
  uint8_t ev, reason, ch, tries;
  int8_t rssi;
  uint16_t ms;           // attempt duration
  char ssid[33], bssid[18], note[48];
};

Phase phase = IDLE;
uint32_t nextTryMs = 0, fastAgainMs = 0, phaseSince = 0, backoffMs = BACKOFF_MIN, scanAtMs = 0;
std::vector<Cand> cands;
size_t candIdx = 0;
uint8_t tries = 0;
bool fastJoin = false, wantFast = true, listOnlyScan = false, waitLogged = false;
SeenNet seen[SEEN_N];
uint8_t seenCount = 0, seenTotal = 0;
LogEntry logBuf[LOG_N];
uint8_t logHead = 0, logCount = 0;
volatile bool searchReq = false;

// Written by the Wi-Fi event task, read by update().
portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t evDisc = 0;
volatile uint8_t evReason = 0;
volatile bool evAssoc = false;
uint32_t discSnap = 0;

// Last network joined: kept in NVS so the AP starts on its channel and the board
// rejoins it without scanning.
struct Hint { String ssid; uint8_t bssid[6] = {0}; uint8_t ch = 0; } hint;

String macStr(const uint8_t* b) {
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return s;
}

const char* authName(uint8_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
  }
}

void loadHint() {
  Preferences p;
  if (!p.begin("cmwifi", true)) return;
  hint.ssid = p.getString("ssid", "");
  hint.ch = p.getUChar("ch", 0);
  p.getBytes("bssid", hint.bssid, 6);
  p.end();
  if (hint.ch < 1 || hint.ch > 13) hint.ch = 0;
}

void saveHint(const String& ssid, const uint8_t* bssid, uint8_t ch) {
  if (ssid == hint.ssid && ch == hint.ch && !memcmp(bssid, hint.bssid, 6)) return;   // no flash write
  hint.ssid = ssid; hint.ch = ch; memcpy(hint.bssid, bssid, 6);
  Preferences p;
  if (!p.begin("cmwifi", false)) return;
  p.putString("ssid", ssid); p.putUChar("ch", ch); p.putBytes("bssid", bssid, 6);
  p.end();
}

const WifiNetwork* savedNet(const String& ssid) {
  for (auto& k : gConfig.sys.networks) if (k.ssid == ssid) return &k;
  return nullptr;
}

bool hintUsable() { return hint.ch && hint.ssid.length() && savedNet(hint.ssid); }

void setState(const char* st, const String& ssid = "") {
  StateLock lk;
  gSys.wifiState = st;
  gSys.wifiSsid = ssid;
}

void addLog(Ev ev, const Cand* c, uint8_t reason, const String& note, uint32_t ms) {
  {
    StateLock lk;
    LogEntry& e = logBuf[logHead];
    memset(&e, 0, sizeof(e));
    e.t = millis() / 1000; e.ev = ev; e.reason = reason; e.ms = ms > 65535 ? 65535 : ms; e.tries = tries;
    if (c) {
      strlcpy(e.ssid, c->ssid.c_str(), sizeof(e.ssid));
      if (c->ch) strlcpy(e.bssid, macStr(c->bssid).c_str(), sizeof(e.bssid));
      e.ch = c->ch; e.rssi = c->rssi;
    }
    strlcpy(e.note, note.c_str(), sizeof(e.note));
    logHead = (logHead + 1) % LOG_N;
    if (logCount < LOG_N) logCount++;
  }
  Serial.printf("[wifi] %s%s%s", EV_NAMES[ev], c ? " '" : "", c ? c->ssid.c_str() : "");
  if (c && c->ch) Serial.printf(" ch %u %s %d dBm", c->ch, macStr(c->bssid).c_str(), c->rssi);
  if (reason) Serial.printf(", reason %u (%s)", reason, WifiMgr::reasonText(reason));
  if (note.length()) Serial.printf(", %s", note.c_str());
  if (ms) Serial.printf(", %u ms", (unsigned)ms);
  Serial.println();
}

// Wrong-password-like reasons: no point hammering the router.
bool passwordReason(uint8_t r) { return r == 14 || r == 15 || r == 23 || r == 202 || r == 204; }
// The router isn't there (or not with a security level we accept): try the next one.
bool notFoundReason(uint8_t r) { return r == 201 || r == 210 || r == 211 || r == 212; }

void schedule(uint32_t now, bool escalate = true) {
  nextTryMs = now + backoffMs;
  if (escalate) backoffMs = std::min<uint32_t>(backoffMs * 2, BACKOFF_MAX);
}

void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      portENTER_CRITICAL(&evMux);
      evReason = info.wifi_sta_disconnected.reason; evDisc++; evAssoc = false;
      portEXIT_CRITICAL(&evMux);
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      evAssoc = true;
      break;
    default: break;
  }
}

uint32_t discCount(uint8_t* reason) {
  portENTER_CRITICAL(&evMux);
  uint32_t n = evDisc;
  if (reason) *reason = evReason;
  portEXIT_CRITICAL(&evMux);
  return n;
}

void startAttempt(uint32_t now) {
  Cand& c = cands[candIdx];
  // Core default only accepts WPA2 or better; older routers (WPA/TKIP, WEP) need a lower bar.
  WiFi.setMinSecurity(c.auth == WIFI_AUTH_WPA_PSK || c.auth == WIFI_AUTH_WEP ? (wifi_auth_mode_t)c.auth : WIFI_AUTH_WPA2_PSK);
  discSnap = discCount(nullptr);
  evAssoc = false;
  tries++;
  Serial.printf("[wifi] joining '%s' (%s, try %u)\n", c.ssid.c_str(), c.how, tries);
  WiFi.begin(c.ssid.c_str(), c.pass.length() ? c.pass.c_str() : nullptr, c.ch, c.ch ? c.bssid : nullptr);
  phase = CONNECTING; phaseSince = now;
  setState("connecting", c.ssid);
}

void giveUp(uint32_t now) {
  bool tried = !cands.empty() && cands[0].ch;   // a visible router refused us
  cands.clear();
  WiFi.disconnect(false);
  phase = IDLE;
  if (fastJoin) {           // remembered network didn't answer: scan straight away
    fastJoin = false;
    fastAgainMs = now + backoffMs;   // and don't retry it blind before the back-off
    nextTryMs = now + RETRY_GAP_MS;
    setState("scanning");
    return;
  }
  schedule(now);
  setState(tried ? "join failed, retrying" : "no known network");
}

void attemptFailed(uint32_t now, uint8_t reason, const String& note) {
  Cand& c = cands[candIdx];
  addLog(EV_FAILED, &c, reason, note, now - phaseSince);
  WiFi.disconnect(false);   // stop the driver before the next try
  bool nextRouter = tries >= TRIES_PER_ROUTER || (passwordReason(reason) && tries >= 2) || notFoundReason(reason) || !c.ch;
  if (nextRouter) { candIdx++; tries = 0; }
  if (candIdx >= cands.size()) { giveUp(now); return; }
  phase = RETRY_WAIT; phaseSince = now;
}

// Turn the scan results into the "seen" list and the routers to try.
void takeScan(int n) {
  std::vector<Cand> found;
  {
    StateLock lk;
    seenCount = 0; seenTotal = n > 255 ? 255 : n; scanAtMs = millis();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      const WifiNetwork* k = ssid.length() ? savedNet(ssid) : nullptr;
      if (seenCount < SEEN_N) {
        SeenNet& s = seen[seenCount++];
        strlcpy(s.ssid, ssid.length() ? ssid.c_str() : "(hidden)", sizeof(s.ssid));
        s.rssi = WiFi.RSSI(i); s.ch = WiFi.channel(i); s.auth = WiFi.encryptionType(i); s.known = k;
      }
      if (!k) continue;
      Cand c;
      c.ssid = k->ssid; c.pass = k->pass; c.prio = k->priority;
      c.rssi = WiFi.RSSI(i); c.ch = WiFi.channel(i); c.auth = WiFi.encryptionType(i);
      memcpy(c.bssid, WiFi.BSSID(i), 6);
      c.how = "from scan";
      found.push_back(c);
    }
  }
  std::stable_sort(found.begin(), found.end(), [](const Cand& a, const Cand& b) {
    return a.prio != b.prio ? a.prio > b.prio : a.rssi > b.rssi;
  });
  if (found.size() > MAX_CANDS) found.resize(MAX_CANDS);
  if (found.empty()) {
    // Nothing visible: try the best saved network anyway (it may hide its name).
    const WifiNetwork* best = nullptr;
    for (auto& k : gConfig.sys.networks) if (!best || k.priority > best->priority) best = &k;
    if (best) {
      Cand c; c.ssid = best->ssid; c.pass = best->pass; c.prio = best->priority; c.how = "not in scan, hidden?";
      c.auth = best->pass.length() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
      found.push_back(c);
    }
  }
  cands = found;
  candIdx = 0; tries = 0;
}

bool startScan(uint32_t now) {
  // Active scan, 200 ms per channel: ~2.5 s with the AP paused between channels.
  int r = WiFi.scanNetworks(true, true, false, 200);
  if (r == WIFI_SCAN_FAILED) {
    addLog(EV_SCAN_FAILED, nullptr, 0, "driver refused the scan", 0);
    return false;
  }
  phase = SCANNING; phaseSince = now;
  return true;
}
}  // namespace

const char* WifiMgr::reasonText(uint8_t r) {
  switch (r) {
    case 0: return "";
    case 1: return "unspecified";
    case 2: return "authentication expired";
    case 3: return "router ended the session";
    case 4: return "association expired (inactivity)";
    case 5: return "router has too many clients";
    case 6: case 7: case 9: return "router lost track of the board";
    case 8: return "left the network";
    case 13: return "invalid element from router";
    case 14: return "message integrity failure (wrong password?)";
    case 15: return "4-way handshake timeout (wrong password?)";
    case 16: return "group key update timeout";
    case 17: return "handshake mismatch";
    case 18: case 19: case 20: case 24: return "unsupported encryption settings";
    case 23: return "802.1X authentication failed";
    case 200: return "beacon timeout (signal lost)";
    case 201: return "network not found";
    case 202: return "authentication failed (wrong password?)";
    case 203: return "association refused";
    case 204: return "handshake timeout (wrong password?)";
    case 205: return "connection failed";
    case 206: return "router clock reset";
    case 207: return "roaming";
    case 208: return "router asked to come back later";
    case 209: return "security query timeout";
    case 210: return "no router with compatible security";
    case 211: return "router security below the minimum";
    case 212: return "signal below the minimum";
    default: return "other";
  }
}

uint8_t WifiMgr::channel() {
#ifdef CM_EMULATOR_TEST
  return 0;
#else
  uint8_t primary = 0;
  wifi_second_chan_t second;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
  return primary;
#endif
}

void WifiMgr::begin() {
#ifdef CM_EMULATOR_TEST  // QEMU has no Wi-Fi radio
  esp_netif_init();       // the web server still needs the TCP/IP stack
  Serial.println("[wifi] skipped (emulator build)");
  return;
#endif
  const SystemConfig& s = gConfig.sys;
  loadHint();
  WiFi.persistent(false);
  WiFi.setHostname(s.hostname.c_str());   // must come before the interfaces start
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_AP_STA);   // station side stays idle when no networks are saved
  WiFi.setSleep(false);     // no modem sleep: steadier joins and AP
  WiFi.setAutoReconnect(false);   // reconnection is handled here
  // Start the AP on the remembered network's channel, so joining it later
  // doesn't move the AP (and drop phones / laptops connected to it).
  uint8_t apCh = hintUsable() ? hint.ch : 1;
  WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str(), apCh);
  {
    StateLock lk;
    gSys.apIp = WiFi.softAPIP().toString();
  }
  Serial.printf("[wifi] AP '%s' at %s ch %u, %u saved network(s), remembered '%s' ch %u\n", s.apSsid.c_str(), gSys.apIp.c_str(),
                apCh, (unsigned)s.networks.size(), hint.ssid.c_str(), hint.ch);
  wantFast = true;
  nextTryMs = millis() + 1000;
  setState(s.networks.empty() ? "off" : "scanning");
}

void WifiMgr::reconfigure() {
#ifdef CM_EMULATOR_TEST
  return;
#endif
  WiFi.disconnect(false);
  cands.clear();
  phase = IDLE; backoffMs = BACKOFF_MIN; nextTryMs = millis() + RETRY_GAP_MS;
  wantFast = true; fastJoin = false; waitLogged = false;
  setState(gConfig.sys.networks.empty() ? "off" : "scanning");
}

void WifiMgr::searchNow() { searchReq = true; }

void WifiMgr::update() {
#ifdef CM_EMULATOR_TEST
  return;
#endif
  uint32_t now = millis();
  const SystemConfig& s = gConfig.sys;
  if (s.networks.empty()) {   // nothing to join; "search now" still lists nearby networks
    if (phase == SCANNING) {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && now - phaseSince < SCAN_TIMEOUT_MS) return;
      if (n > 0) takeScan(n);
      WiFi.scanDelete();
      cands.clear(); listOnlyScan = false; phase = IDLE;
      return;
    }
    if (phase != IDLE) { WiFi.disconnect(false); cands.clear(); phase = IDLE; setState("off"); }
    if (searchReq) { searchReq = false; listOnlyScan = true; startScan(now); }
    return;
  }
  if (searchReq) {
    searchReq = false;
    if (phase == CONNECTED) { listOnlyScan = true; startScan(now); return; }   // refresh the list, stay joined
    if (phase == IDLE) { backoffMs = BACKOFF_MIN; nextTryMs = now; wantFast = false; waitLogged = false; }
  }

  switch (phase) {
    case IDLE: {
      if ((int32_t)(now - nextTryMs) < 0) return;
      bool apInUse = !s.scanWhileApInUse && WiFi.softAPgetStationNum() > 0;
      // Rejoin the remembered network directly: no scan. While the AP is in use,
      // only if that doesn't move the AP to another channel.
      bool fastOk = (int32_t)(now - fastAgainMs) >= 0;
      if (hintUsable() && (wantFast || (apInUse && fastOk && hint.ch == WifiMgr::channel()))) {
        wantFast = false;
        const WifiNetwork* k = savedNet(hint.ssid);
        Cand c; c.ssid = k->ssid; c.pass = k->pass; c.prio = k->priority; c.ch = hint.ch;
        memcpy(c.bssid, hint.bssid, 6); c.how = "remembered";
        c.auth = k->pass.length() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cands.assign(1, c); candIdx = 0; tries = 0; fastJoin = true;
        startAttempt(now);
        return;
      }
      wantFast = false;
      if (apInUse) {
        if (!waitLogged) { addLog(EV_WAIT_AP, nullptr, 0, "", 0); waitLogged = true; }
        nextTryMs = now + 15000;
        setState("waiting (access point in use)");
        return;
      }
      waitLogged = false;
      if (startScan(now)) setState("scanning");
      else { schedule(now); setState("no known network"); }
      return;
    }

    case SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && now - phaseSince < SCAN_TIMEOUT_MS) return;
      if (n < 0) {
        addLog(EV_SCAN_FAILED, nullptr, 0, n == WIFI_SCAN_RUNNING ? "timed out" : "no result", now - phaseSince);
        WiFi.scanDelete();
        if (listOnlyScan) { listOnlyScan = false; phase = CONNECTED; return; }
        phase = IDLE; schedule(now); setState("no known network");
        return;
      }
      takeScan(n);
      WiFi.scanDelete();
      if (listOnlyScan) { listOnlyScan = false; cands.clear(); phase = CONNECTED; return; }
      bool visible = !cands.empty() && cands[0].ch;
      if (!visible) addLog(EV_NONE_FOUND, nullptr, 0, String(n) + " network(s) seen", now - phaseSince);
      if (cands.empty()) { phase = IDLE; schedule(now); setState("no known network"); return; }
      fastJoin = false;
      startAttempt(now);
      return;
    }

    case CONNECTING: {
      uint8_t reason = 0;
      uint32_t d = discCount(&reason);
      if (WiFi.status() == WL_CONNECTED) {
        Cand c = cands[candIdx];
        c.ch = WifiMgr::channel(); c.rssi = WiFi.RSSI();
        memcpy(c.bssid, WiFi.BSSID(), 6);
        addLog(EV_JOINED, &c, 0, WiFi.localIP().toString(), now - phaseSince);
        saveHint(c.ssid, c.bssid, c.ch);
        cands.clear();
        phase = CONNECTED; backoffMs = BACKOFF_MIN; fastJoin = false;
        discSnap = d;
        setState("connected", c.ssid);
      } else if (d != discSnap) {
        attemptFailed(now, reason, "");
      } else if (now - phaseSince > ATTEMPT_MS) {
        attemptFailed(now, 0, evAssoc ? "joined but got no IP address (DHCP)" : "no answer");
      }
      return;
    }

    case RETRY_WAIT:
      if (now - phaseSince >= RETRY_GAP_MS) startAttempt(now);
      return;

    case CONNECTED: {
      uint8_t reason = 0;
      uint32_t d = discCount(&reason);
      if (WiFi.status() != WL_CONNECTED || d != discSnap) {
        Cand c; c.ssid = gSys.wifiSsid; c.ch = hint.ch; memcpy(c.bssid, hint.bssid, 6);
        addLog(EV_LOST, &c, d != discSnap ? reason : 0, "", 0);
        WiFi.disconnect(false);
        phase = IDLE; backoffMs = BACKOFF_MIN; nextTryMs = now + RETRY_GAP_MS;
        wantFast = true;   // try the same router again first
        setState("scanning");
      }
      return;
    }
  }
}

void WifiMgr::toJson(JsonObject o) {
  uint32_t now = millis();
  o["uptimeS"] = now / 1000;
  o["state"] = gSys.wifiState;
  o["ssid"] = gSys.wifiSsid;
  o["channel"] = channel();
  o["saved"] = gConfig.sys.networks.size();
#ifndef CM_EMULATOR_TEST
  o["apClients"] = WiFi.softAPgetStationNum();
  if (WiFi.status() == WL_CONNECTED) { o["rssi"] = WiFi.RSSI(); o["ip"] = WiFi.localIP().toString(); }
#endif
  if (phase == IDLE && (int32_t)(nextTryMs - now) > 0) o["nextTryS"] = (nextTryMs - now + 999) / 1000;
  JsonObject h = o["remembered"].to<JsonObject>();
  h["ssid"] = hint.ssid; h["channel"] = hint.ch;
  if (hint.ch) h["bssid"] = macStr(hint.bssid);
  o["scanAgeS"] = scanAtMs ? (int32_t)((now - scanAtMs) / 1000) : -1;
  o["scanTotal"] = seenTotal;
  JsonArray sa = o["scan"].to<JsonArray>();
  for (uint8_t i = 0; i < seenCount; i++) {
    JsonObject e = sa.add<JsonObject>();
    e["ssid"] = seen[i].ssid; e["rssi"] = seen[i].rssi; e["ch"] = seen[i].ch;
    e["security"] = authName(seen[i].auth); e["saved"] = seen[i].known;
  }
  JsonArray la = o["log"].to<JsonArray>();   // newest first
  for (uint8_t i = 0; i < logCount; i++) {
    const LogEntry& e = logBuf[(logHead + LOG_N - 1 - i) % LOG_N];
    JsonObject j = la.add<JsonObject>();
    j["t"] = e.t; j["event"] = EV_NAMES[e.ev];
    if (e.ssid[0]) j["ssid"] = e.ssid;
    if (e.bssid[0]) j["bssid"] = e.bssid;
    if (e.ch) j["ch"] = e.ch;
    if (e.rssi) j["rssi"] = e.rssi;
    if (e.tries) j["try"] = e.tries;
    if (e.reason) { j["reason"] = e.reason; j["reasonText"] = reasonText(e.reason); }
    if (e.ms) j["ms"] = e.ms;
    if (e.note[0]) j["note"] = e.note;
  }
}
