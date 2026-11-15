// CarMetrics - ESP-NOW link between boards (phase 3, the default link).
//
// Frames arrive in the Wi-Fi task; they are queued and handed to the protocol
// from poll() (main loop), so the protocol never runs in a radio callback.
// The link learns each board's MAC from the frames it sends (header byte 3 =
// sender's board id), so unicasts go to the right radio; everything else is
// broadcast. Not encrypted yet (planned for the pairing step).
//
// The main board uses its access-point interface (always up, follows the
// joined network's channel); extra boards use the station interface, never
// join a network, and switch channels to find the main board.
#pragma once
#include "Link.h"
#include <Arduino.h>

class EspNowLink : public Link {
 public:
  bool begin(bool mainBoard);             // after WiFi is started
  const char* name() const override { return "ESP-NOW"; }
  bool send(uint8_t dstBoard, const uint8_t* frame, size_t len) override;
  void poll() override;
  void forget(uint8_t board);             // drop a board's MAC and key (unpaired)
  void setPeer(uint8_t board, const uint8_t mac[6]);   // MAC known in advance (paired boards on the main board)
  void setKey(uint8_t board, const uint8_t* key);      // 16-byte link key; nullptr = unencrypted
  bool encrypted(uint8_t board) const { return board <= Proto::MAX_BOARD_ID && keyed[board]; }
  uint8_t channel() const;
  void setChannel(uint8_t ch);            // extra boards: channel search
  bool ok() const { return started; }
  uint32_t rxCount = 0, txCount = 0, txFail = 0, queueDrops = 0;
 private:
  bool ensurePeer(const uint8_t mac[6], int board = -1);
  void applyPeer(uint8_t board);          // (re)create the peer entry with the right encryption
  bool started = false, isMain = false;
  uint8_t macOf[Proto::MAX_BOARD_ID + 1][6] = {{0}};
  bool known[Proto::MAX_BOARD_ID + 1] = {false};
  uint8_t keyOf[Proto::MAX_BOARD_ID + 1][16] = {{0}};
  bool keyed[Proto::MAX_BOARD_ID + 1] = {false};
};
