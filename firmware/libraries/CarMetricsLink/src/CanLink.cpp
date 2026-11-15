#include "CanLink.h"
#include <driver/twai.h>

bool CanLink::begin(int txPin, int rxPin, uint16_t kbps, const uint8_t mac[6]) {
  if (txPin < 0 || rxPin < 0) return false;
  sender = mac[3] ^ mac[4] ^ mac[5];           // spreads boards over 256 CAN ids
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)txPin, (gpio_num_t)rxPin, TWAI_MODE_NORMAL);
  g.tx_queue_len = 64; g.rx_queue_len = 64;    // a full message is 42 frames
  twai_timing_config_t t = kbps >= 1000 ? (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS() :
                           kbps >= 500 ? (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS() :
                           kbps >= 250 ? (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS() : (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) { Serial.println("[can] driver install failed"); return false; }
  if (twai_start() != ESP_OK) { Serial.println("[can] start failed"); twai_driver_uninstall(); return false; }
  started = true;
  Serial.printf("[can] started: TX GPIO %d, RX GPIO %d, %u kbit/s, sender id 0x%03X\n", txPin, rxPin, kbps, (unsigned)(CanFrag::BASE_ID + sender));
  return true;
}

bool CanLink::send(uint8_t, const uint8_t* frame, size_t len) {
  if (!started) return false;
  auto frames = CanFrag::split(sender, tag++, frame, len);
  for (auto& f : frames) {
    twai_message_t m = {};
    m.identifier = f.id; m.data_length_code = f.len; memcpy(m.data, f.data, f.len);
    if (twai_transmit(&m, 0) != ESP_OK) { txFail++; return false; }   // queue full or bus off: the protocol resends
  }
  txCount++;
  return true;
}

void CanLink::poll() {
  if (!started) return;
  twai_message_t m;
  std::vector<uint8_t> msg;
  while (twai_receive(&m, 0) == ESP_OK) {
    if (m.extd || m.rtr) continue;
    CanFrag::Frame f; f.id = m.identifier; f.len = m.data_length_code > 8 ? 8 : m.data_length_code; memcpy(f.data, m.data, f.len);
    if (asm_.feed(f, msg)) { rxCount++; if (rx) rx(msg.data(), msg.size(), 0); }
  }
  uint32_t now = millis();
  if (now - lastCheck >= 500) {                // recover from bus-off (cut wire, no other board, no terminator)
    lastCheck = now;
    twai_status_info_t s;
    if (twai_get_status_info(&s) == ESP_OK) {
      busErrors = s.bus_error_count;
      if (s.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
      else if (s.state == TWAI_STATE_STOPPED) twai_start();
    }
  }
}

const char* CanLink::state() const {
  if (!started) return "off";
  twai_status_info_t s;
  if (twai_get_status_info(&s) != ESP_OK) return "unknown";
  switch (s.state) {
    case TWAI_STATE_RUNNING: return "running";
    case TWAI_STATE_BUS_OFF: return "bus off (recovering)";
    case TWAI_STATE_RECOVERING: return "recovering";
    default: return "stopped";
  }
}
