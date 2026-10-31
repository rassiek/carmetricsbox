// CarMetrics - board-to-board links (phase 3).
//
// A link carries Proto frames between boards: ESP-NOW (default), CAN (option)
// or, for tests, a loopback pair in memory. Delivery is best effort; the
// protocol above handles heartbeats, duplicates and resends.
#pragma once
#include "Proto.h"
#include <functional>
#include <deque>
#include <vector>

class Link {
 public:
  using RxFn = std::function<void(const uint8_t* frame, size_t len, int8_t rssi)>;
  virtual ~Link() {}
  virtual const char* name() const = 0;
  // Send one frame to a board (Proto::BROADCAST = everyone in range / on the bus).
  virtual bool send(uint8_t dstBoard, const uint8_t* frame, size_t len) = 0;
  virtual void poll() {}          // from the link task: deliver what has arrived
  void onReceive(RxFn fn) { rx = fn; }
 protected:
  RxFn rx;
};

// Two linked endpoints in memory. Frames are delivered on poll(); a test can
// drop every Nth frame or deliver them in reverse order.
class LoopbackLink : public Link {
 public:
  LoopbackLink() {}
  static void connect(LoopbackLink& a, LoopbackLink& b) { a.peer = &b; b.peer = &a; }
  const char* name() const override { return "loopback"; }
  bool send(uint8_t, const uint8_t* frame, size_t len) override {
    if (!peer || len > Proto::MAX_FRAME) return false;
    sent++;
    if (dropEvery && sent % dropEvery == 0) { dropped++; return true; }   // lost "in the air"
    peer->inbox.emplace_back(frame, frame + len);
    return true;
  }
  void poll() override {
    while (!inbox.empty()) {
      std::vector<uint8_t> f;
      if (reverse) { f = std::move(inbox.back()); inbox.pop_back(); }
      else { f = std::move(inbox.front()); inbox.pop_front(); }
      if (rx) rx(f.data(), f.size(), -50);
    }
  }
  uint32_t dropEvery = 0;   // 0 = never
  bool reverse = false;
  uint32_t sent = 0, dropped = 0;
 private:
  LoopbackLink* peer = nullptr;
  std::deque<std::vector<uint8_t>> inbox;
};
