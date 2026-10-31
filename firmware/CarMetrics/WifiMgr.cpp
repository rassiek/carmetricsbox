#include "WifiMgr.h"
#include "State.h"
#include <WiFi.h>
#include <esp_netif.h>

namespace {
enum Phase { IDLE, SCANNING, CONNECTING, CONNECTED };
Phase phase = IDLE;
uint32_t nextScanMs = 0, phaseSince = 0, backoffMs = 10000;
String target;

void setState(const char* st, const String& ssid = "") {
  StateLock lk;
  gSys.wifiState = st;
  gSys.wifiSsid = ssid;
}

void schedule(uint32_t now) {
  nextScanMs = now + backoffMs;
  backoffMs = min<uint32_t>(backoffMs * 2, 300000);
}

// Best saved network in the scan: highest priority, then strongest signal.
int pickBest(int n, String& pass) {
  int best = -1, bestPrio = -1, bestRssi = -1000;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    for (auto& k : gConfig.sys.networks) {
      if (k.ssid != ssid) continue;
      int rssi = WiFi.RSSI(i);
      if (k.priority > bestPrio || (k.priority == bestPrio && rssi > bestRssi)) {
        best = i; bestPrio = k.priority; bestRssi = rssi; pass = k.pass;
      }
    }
  }
  return best;
}
}  // namespace

void WifiMgr::begin() {
#ifdef CM_EMULATOR_TEST  // QEMU has no Wi-Fi radio
  esp_netif_init();       // the web server still needs the TCP/IP stack
  Serial.println("[wifi] skipped (emulator build)");
  return;
#endif
  const SystemConfig& s = gConfig.sys;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);   // station side stays idle when no networks are saved
  WiFi.setHostname(s.hostname.c_str());
  WiFi.setAutoReconnect(false);   // reconnection is handled here
  WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str());
  {
    StateLock lk;
    gSys.apIp = WiFi.softAPIP().toString();
  }
  Serial.printf("[wifi] AP '%s' at %s, %u saved network(s)\n", s.apSsid.c_str(), gSys.apIp.c_str(), (unsigned)s.networks.size());
  nextScanMs = millis() + 1000;
  setState(s.networks.empty() ? "off" : "scanning");
}

void WifiMgr::reconfigure() {
#ifdef CM_EMULATOR_TEST
  return;
#endif
  WiFi.disconnect(false);
  phase = IDLE; backoffMs = 10000; nextScanMs = millis() + 500;
  setState(gConfig.sys.networks.empty() ? "off" : "scanning");
}

void WifiMgr::update() {
#ifdef CM_EMULATOR_TEST
  return;
#endif
  uint32_t now = millis();
  const SystemConfig& s = gConfig.sys;
  if (s.networks.empty()) {
    if (phase != IDLE) { WiFi.disconnect(false); phase = IDLE; setState("off"); }
    return;
  }
  switch (phase) {
    case IDLE:
      if ((int32_t)(now - nextScanMs) < 0) return;
      if (!s.scanWhileApInUse && WiFi.softAPgetStationNum() > 0) { nextScanMs = now + 15000; setState("waiting (access point in use)"); return; }
      WiFi.scanNetworks(true, false, false, 200);   // async, ~2.5 s
      phase = SCANNING; phaseSince = now;
      setState("scanning");
      return;
    case SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && now - phaseSince < 15000) return;
      String pass;
      int best = n > 0 ? pickBest(n, pass) : -1;
      if (best >= 0) {
        target = WiFi.SSID(best);
        Serial.printf("[wifi] joining '%s' (%d dBm)\n", target.c_str(), WiFi.RSSI(best));
        WiFi.scanDelete();
        WiFi.begin(target.c_str(), pass.length() ? pass.c_str() : nullptr);
        phase = CONNECTING; phaseSince = now;
        setState("connecting", target);
      } else {
        WiFi.scanDelete();
        phase = IDLE; schedule(now);
        setState("no known network");
      }
      return;
    }
    case CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        phase = CONNECTED; backoffMs = 10000;
        Serial.printf("[wifi] connected to '%s', IP %s\n", target.c_str(), WiFi.localIP().toString().c_str());
        setState("connected", target);
      } else if (now - phaseSince > 20000) {
        Serial.printf("[wifi] '%s' did not connect\n", target.c_str());
        WiFi.disconnect(false);
        phase = IDLE; schedule(now);
        setState("no known network");
      }
      return;
    case CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] connection lost");
        WiFi.disconnect(false);
        phase = IDLE; backoffMs = 10000; nextScanMs = now + 3000;
        setState("scanning");
      }
      return;
  }
}
