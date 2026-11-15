// CarMetrics - CAN link (ESP32 built-in CAN controller "TWAI" + a 3.3 V
// transceiver such as the SN65HVD230; 120 ohm at both ends of the bus).
// Same messages as ESP-NOW, split into 8-byte frames (CanFrag). A bus is
// shared: every board hears every message and keeps the ones for it.
#pragma once
#include "Link.h"
#include "CanFrag.h"
#include <Arduino.h>

class CanLink : public Link {
 public:
  bool begin(int txPin, int rxPin, uint16_t kbps, const uint8_t mac[6]);
  const char* name() const override { return "CAN"; }
  bool send(uint8_t dstBoard, const uint8_t* frame, size_t len) override;
  void poll() override;
  bool ok() const { return started; }
  const char* state() const;            // "running", "bus off (recovering)", ...
  uint32_t rxCount = 0, txCount = 0, txFail = 0, busErrors = 0;
 private:
  bool started = false;
  uint8_t sender = 0, tag = 0;
  CanFrag::Assembler asm_;
  uint32_t lastCheck = 0;
};

// Several links at once (main board with ESP-NOW and CAN): sends on all,
// receives from all.
class MultiLink : public Link {
 public:
  void add(Link* l) { links.push_back(l); l->onReceive([this](const uint8_t* f, size_t n, int8_t r) { if (rx) rx(f, n, r); }); }
  const char* name() const override { return links.size() > 1 ? "ESP-NOW + CAN" : links.empty() ? "none" : links[0]->name(); }
  bool send(uint8_t dst, const uint8_t* f, size_t n) override { bool any = false; for (auto* l : links) any |= l->send(dst, f, n); return any; }
  void poll() override { for (auto* l : links) l->poll(); }
 private:
  std::vector<Link*> links;
};
