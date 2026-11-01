#include "EspNowLink.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {
struct RxItem { uint8_t mac[6]; int8_t rssi; uint8_t len; uint8_t data[Proto::MAX_FRAME]; };
QueueHandle_t rxQueue = nullptr;
volatile uint32_t drops = 0;
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void queueFrame(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  if (!rxQueue || len <= 0 || len > (int)Proto::MAX_FRAME || data[0] != 'C') return;   // not ours
  RxItem it;
  memcpy(it.mac, mac, 6); it.rssi = rssi; it.len = (uint8_t)len; memcpy(it.data, data, len);
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) drops++;
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  queueFrame(info->src_addr, data, len, info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0);
}
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) { queueFrame(mac, data, len, 0); }
#endif
}  // namespace

bool EspNowLink::begin(bool mainBoard) {
  isMain = mainBoard;
  if (!rxQueue) rxQueue = xQueueCreate(16, sizeof(RxItem));
  if (esp_now_init() != ESP_OK) { Serial.println("[espnow] init failed"); return false; }
  esp_now_register_recv_cb(onRecv);
  started = ensurePeer(BCAST);
  Serial.printf("[espnow] started on the %s interface, channel %u\n", isMain ? "access point" : "station", channel());
  return started;
}

bool EspNowLink::ensurePeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;                                 // = the radio's current channel
  p.ifidx = isMain ? WIFI_IF_AP : WIFI_IF_STA;
  p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

bool EspNowLink::send(uint8_t dst, const uint8_t* frame, size_t len) {
  if (!started) return false;
  const uint8_t* mac = BCAST;
  if (dst <= Proto::MAX_BOARD_ID && known[dst]) mac = macOf[dst];
  if (!ensurePeer(mac)) { txFail++; return false; }
  bool ok = esp_now_send(mac, frame, len) == ESP_OK;
  (ok ? txCount : txFail)++;
  return ok;
}

void EspNowLink::poll() {
  if (!rxQueue) return;
  RxItem it;
  while (xQueueReceive(rxQueue, &it, 0) == pdTRUE) {
    rxCount++;
    Proto::Header h; const uint8_t* p; size_t n;
    if (Proto::parse(it.data, it.len, h, p, n)) continue;     // damaged / foreign: ignore
    uint8_t src = h.src;
    if (src <= Proto::MAX_BOARD_ID && (!known[src] || memcmp(macOf[src], it.mac, 6))) {
      memcpy(macOf[src], it.mac, 6); known[src] = true;   // learn (or update) that board's radio address
    }
    if (rx) rx(it.data, it.len, it.rssi);
  }
  queueDrops = drops;
}

void EspNowLink::forget(uint8_t board) {
  if (board > Proto::MAX_BOARD_ID || !known[board]) return;
  esp_now_del_peer(macOf[board]);
  known[board] = false;
}

uint8_t EspNowLink::channel() const {
  uint8_t ch = 0; wifi_second_chan_t s;
  if (esp_wifi_get_channel(&ch, &s) != ESP_OK) return 0;
  return ch;
}

void EspNowLink::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}
