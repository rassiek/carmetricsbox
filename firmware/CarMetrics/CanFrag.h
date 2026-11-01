// CarMetrics - board-to-board messages over CAN (option 2 next to ESP-NOW).
//
// A Proto frame (up to 250 bytes) is split into classic 8-byte CAN frames:
//   CAN id   0x100 + sender (0..255, from the board's MAC), 11-bit, so every
//            board's frames stay in order and never mix with another's
//   byte 0   message tag (high 4 bits, rolling per sender)
//   byte 1   fragment index (0..41)
//   index 0  bytes 2-3 total length, bytes 4-7 the first 4 bytes
//   others   bytes 2-7 the next 6 bytes (the last one may be shorter)
// At 500 kbit/s a full 250-byte message takes ~11 ms on the bus.
// Plain C++: tested on a PC (tools/cm.sh prototest).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace CanFrag {
constexpr uint32_t BASE_ID = 0x100;
struct Frame { uint32_t id = 0; uint8_t len = 0; uint8_t data[8] = {0}; };

std::vector<Frame> split(uint8_t sender, uint8_t tag, const uint8_t* msg, size_t len);

// Collects fragments from any number of senders; a message with a missing
// fragment is dropped (the protocol above resends what matters).
class Assembler {
 public:
  // true when f completed a message (copied to out)
  bool feed(const Frame& f, std::vector<uint8_t>& out);
  uint32_t dropped = 0;
 private:
  struct Slot { uint8_t sender = 0, tag = 0, next = 0; uint16_t total = 0; bool active = false; std::vector<uint8_t> buf; };
  std::vector<Slot> slots;
};
}  // namespace CanFrag
