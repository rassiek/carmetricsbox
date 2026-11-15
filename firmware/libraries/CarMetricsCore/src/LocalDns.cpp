#include "LocalDns.h"
#include "State.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>
#include <dhcpserver/dhcpserver_options.h>

namespace {
WiFiUDP udp;
bool started = false;
String host;            // lower case
IPAddress apIp;

// Offer the board as DNS server to the access point's DHCP clients.
void offerDns() {
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) return;
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = (uint32_t)apIp;
  esp_netif_dhcps_stop(ap);
  esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  dhcps_offer_t offer = OFFER_DNS;
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_dhcps_start(ap);
}
}  // namespace

void LocalDns::begin() {
  { StateLock lk; host = gConfig.sys.hostname; }
  host.toLowerCase();
  apIp = WiFi.softAPIP();
  if (!host.length() || (uint32_t)apIp == 0) return;
  offerDns();
  started = udp.begin(53);
  Serial.printf("[dns] %s / %s.local -> %s on the access point\n", host.c_str(), host.c_str(), apIp.toString().c_str());
}

void LocalDns::update() {
  if (!started) return;
  for (int k = 0; k < 4; k++) {
    int n = udp.parsePacket();
    if (n <= 0) return;
    uint8_t q[512];
    if (n > (int)sizeof(q) - 16) { udp.flush(); continue; }
    udp.read(q, n);
    if (n < 17 || (q[2] & 0x80) || ((q[2] >> 3) & 0x0F) != 0 || q[5] != 1) continue;   // a standard query with one question
    // First label of the name, and where the question ends
    size_t p = 12; String first; bool firstDone = false;
    while (p < (size_t)n && q[p]) {
      uint8_t l = q[p];
      if (l > 63 || p + 1 + l > (size_t)n) { p = n; break; }
      if (!firstDone) { first = String((const char*)q + p + 1).substring(0, l); first.toLowerCase(); firstDone = true; }
      p += 1 + l;
    }
    if (p + 5 > (size_t)n) continue;
    uint16_t qtype = (q[p + 1] << 8) | q[p + 2];
    size_t qend = p + 5;
    bool ours = first == host;
    q[2] = 0x84 | (q[2] & 0x01);                 // response, authoritative, keep RD
    q[3] = ours ? 0x80 : 0x83;                   // RA; NXDOMAIN for names that aren't ours
    q[6] = 0; q[7] = 0; q[8] = q[9] = q[10] = q[11] = 0;
    size_t len = qend;
    if (ours && qtype == 1) {                    // A record
      q[7] = 1;
      const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
      memcpy(q + len, ans, sizeof(ans)); len += sizeof(ans);
      uint32_t ip = (uint32_t)apIp;
      memcpy(q + len, &ip, 4); len += 4;         // IPAddress keeps network byte order
    }
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(q, len);
    udp.endPacket();
  }
}
