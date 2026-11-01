// CarMetrics - board-to-board exchange (phase 3, step 2): who sends what, when.
//
// MainSession runs on the main board, NodeSession on a sensor / relay board.
// Both sit on a Link (ESP-NOW, CAN or a loopback for tests) and are driven by
// update(nowMs) from one task; frames arriving on the link go to receive().
//
//  pairing   node broadcasts HELLO (unpaired) every 2 s -> main lists it as
//            "found" -> the user accepts -> main broadcasts ASSIGN (MAC -> id)
//            -> node stores the id and says HELLO again with it.
//  heartbeat both ways every 1 s. No frame for LINK_LOST_MS (5 s) = link lost:
//            the node puts its relays in their safe state (alarms carry on),
//            the main board marks the board offline.
//  config    main sends the board's settings in chunks, again every 2 s until
//            the board answers CONFIG_ACK (accepted or refused with a reason).
//  points    node sends all its values every pointsEveryMs; tagged with the
//            config revision so indexes never get mixed up after a change.
//  commands  main sends, node answers ACK; resent up to 3 times, duplicates
//            are ignored by sequence number, so a relay command acts once.
//
// Plain C++ (no Arduino types): tested on a PC with tools/cm.sh prototest.
#pragma once
#include "Proto.h"
#include "Link.h"
#include <functional>
#include <string>
#include <vector>

namespace BoardNet {

constexpr uint32_t HELLO_UNPAIRED_MS = 2000;
constexpr uint32_t CONFIG_RESEND_MS = 2000;
constexpr uint32_t COMMAND_RESEND_MS = 300;
constexpr uint8_t COMMAND_TRIES = 4;
constexpr uint8_t CHUNKS_PER_UPDATE = 4;      // don't flood the radio
constexpr size_t MAX_BOARDS = 3;              // extra boards in this phase

struct Identity {                             // this board, for HELLO
  uint8_t role = Proto::ROLE_SENSOR, chip = Proto::CHIP_ESP32;
  uint8_t mac[6] = {0};
  std::string firmware, name;
};

// ------------------------------------------------------------------ extra board
class NodeSession {
 public:
  struct Callbacks {
    // Store the id (NVS). key = link key for encrypted ESP-NOW, nullptr = unencrypted.
    std::function<void(uint8_t id, const std::string& name, const uint8_t* key)> paired;
    // New settings arrived complete: apply them; return CFG_OK or an error (+ message).
    std::function<uint8_t(const std::vector<uint8_t>& json, uint32_t rev, std::string& message)> config;
    std::function<uint8_t(const Proto::Command& c)> command;              // 0 = done
    std::function<void(bool up)> link;                                     // link lost / back
    std::function<uint8_t(Proto::Point* out, uint8_t max)> points;        // fill current values
    std::function<uint8_t()> level;                                        // worst local alarm level
  };
  NodeSession(Link& link, const Identity& me, uint8_t storedId, uint32_t storedConfigRev, const Callbacks& cb);
  void update(uint32_t now);
  void receive(const uint8_t* frame, size_t len, int8_t rssi, uint32_t now);
  bool paired() const { return id != Proto::UNPAIRED; }
  // Heard the main board (any frame, even one for another board) recently:
  // the radio is on the right channel. Used for the channel search on ESP-NOW.
  bool mainInRange(uint32_t now) const { return heardMainAt && now - heardMainAt < Proto::LINK_LOST_MS; }
  void sayHello() { sendHello(); }          // e.g. right after switching channel
  bool linkUp() const { return up; }
  uint8_t boardId() const { return id; }
  uint32_t configRev() const { return cfgRev; }
  uint32_t pointsEveryMs = 1000;
  uint8_t flags = 0;                          // extra HB_* flags (safe mode, muted...)

 private:
  void send(const std::function<size_t(uint8_t*, const Proto::Header&)>& enc, uint8_t dst = Proto::MAIN_BOARD);
  void sendHello();
  void sendConfigAck();
  Link& link; Identity me; Callbacks cb;
  uint8_t id;
  uint32_t cfgRev;
  bool up = false, everHeard = false;
  uint32_t lastHeard = 0, lastHello = 0, lastHeartbeat = 0, lastPoints = 0, heardMainAt = 0;
  uint16_t seq = 0;
  int8_t rssi = 0;
  Proto::SeqTracker fromMain;
  Proto::ConfigAssembler assembler;
  Proto::ConfigAck lastAck;
  bool haveAck = false;
  int32_t lastCmdSeq = -1;                    // a resent command is answered again, not run twice
  uint8_t lastCmdStatus = 0;
};

// ------------------------------------------------------------------ main board
struct BoardInfo {
  uint8_t id = Proto::UNPAIRED;
  uint8_t mac[6] = {0};
  std::string name, firmware;
  uint8_t role = 0, chip = 0;
  bool hasKey = false;                        // encrypted link (key given at pairing)
  uint8_t key[16] = {0};
  bool online = false;
  uint32_t lastHeard = 0, uptimeS = 0;
  int8_t rssi = 0, rssiAtBoard = 0;           // as heard here / as heard by the board
  uint8_t level = 0, flags = 0;
  // configuration
  uint32_t configRev = 0;                     // revision being / last sent
  uint32_t ackedRev = 0;                      // revision the board accepted (0 = none)
  uint8_t configStatus = Proto::CFG_OK;
  std::string configMessage;
  bool configPending = false;
  // live values (index = the board's point list for ackedRev)
  std::vector<Proto::Point> points;
  uint32_t pointsAt = 0;
  Proto::SeqTracker seqs;
  // internal: configuration being sent
  struct Tx { std::vector<uint8_t> json; uint32_t crc = 0; uint16_t next = 0; uint32_t roundAt = 0; } tx;
};

struct FoundBoard {                           // unpaired board heard recently
  uint8_t mac[6] = {0};
  std::string name, firmware;
  uint8_t role = 0, chip = 0;
  int8_t rssi = 0;
  uint32_t lastHeard = 0;
};

class MainSession {
 public:
  struct Callbacks {
    std::function<void(const BoardInfo& b, bool online)> online;           // went online / offline
    std::function<void(const BoardInfo& b)> configResult;                  // accepted / refused
    std::function<void(uint8_t board, uint16_t cmdSeq, uint8_t status)> commandResult;   // status 0 ok, 255 no answer
    std::function<void()> boardsChanged;                                    // paired / forgotten: store the table
  };
  MainSession(Link& link, const Identity& me, const Callbacks& cb);
  void update(uint32_t now);
  void receive(const uint8_t* frame, size_t len, int8_t rssi, uint32_t now);

  // pairing
  const std::vector<FoundBoard>& found() const { return foundList; }
  // -> new id, or -1 (full / unknown). key: link key for encryption (nullptr = none), sent with the pairing.
  int accept(const uint8_t mac[6], const std::string& name, const uint8_t* key = nullptr);
  bool forget(uint8_t id);
  void restore(const BoardInfo& b);                             // from storage at boot

  // configuration and commands
  void setConfig(uint8_t id, uint32_t rev, const std::vector<uint8_t>& json);
  int command(uint8_t id, const Proto::Command& c);             // -> command seq, or -1

  const std::vector<BoardInfo>& boards() const { return list; }
  const BoardInfo* board(uint8_t id) const;
  uint32_t pointsEveryMs = 1000;

 private:
  struct PendingCmd { uint8_t board; uint16_t seq; Proto::Command c; uint32_t lastSent; uint8_t tries; };
  BoardInfo* find(uint8_t id);
  uint16_t send(uint8_t dst, const std::function<size_t(uint8_t*, const Proto::Header&)>& enc, int useSeq = -1);
  Link& link; Identity me; Callbacks cb;
  std::vector<BoardInfo> list;
  std::vector<FoundBoard> foundList;
  std::vector<PendingCmd> pending;
  uint16_t seq = 0;
  uint32_t lastHeartbeat = 0, nowMs = 0, lastBeacon = 0;
  void beacon(uint32_t now);                  // broadcast heartbeat: lets boards find our channel
};

}  // namespace BoardNet
