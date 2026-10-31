// CarMetrics - board-to-board protocol (phase 3).
//
// One message format for every link (ESP-NOW by default, CAN as an option).
// Every frame fits ESP-NOW v1's 250-byte payload, so classic ESP32s on core
// 2.0.17 can take part. CAN splits frames into 8-byte pieces underneath.
//
// Frame:  'C' | version | type | src | dst | seq (u16) | len | payload[len] | crc16
//         little-endian; crc16 (CCITT) covers everything before it.
// Boards: 0 = main board, 1..14 = extra boards (given at pairing),
//         15 = not yet paired, 255 = everyone.
//
// Plain C++ (no Arduino types) so it also builds and tests on a PC:
// tools/cm.sh prototest.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace Proto {

constexpr uint8_t VERSION = 1;
constexpr size_t MAX_FRAME = 250;            // ESP-NOW v1 payload
constexpr size_t OVERHEAD = 10;              // 8 header + 2 CRC
constexpr size_t MAX_PAYLOAD = MAX_FRAME - OVERHEAD;
constexpr uint8_t MAIN_BOARD = 0, UNPAIRED = 15, BROADCAST = 255;
constexpr uint8_t MAX_BOARD_ID = 14;

enum Type : uint8_t {
  HELLO = 1,         // "I'm here": role, chip, firmware, config revision (sent on start, then every 5 s until paired)
  HEARTBEAT = 2,     // every ~1 s both ways; link lost after LINK_LOST_MS
  CONFIG_CHUNK = 3,  // main -> board: its share of the configuration (JSON), split into chunks
  CONFIG_ACK = 4,    // board -> main: configuration accepted / refused
  POINTS = 5,        // board -> main: live values, by index into the board's point list
  COMMAND = 6,       // main -> board: relay, mute, identify, reboot
  ACK = 7,           // reply to a COMMAND
};

enum Role : uint8_t { ROLE_MAIN = 0, ROLE_SENSOR = 1, ROLE_RELAY = 2, ROLE_SENSOR_RELAY = 3, ROLE_DISPLAY = 4 };
enum Chip : uint8_t { CHIP_ESP32 = 0, CHIP_ESP32S3 = 1, CHIP_OTHER = 255 };

constexpr uint32_t HEARTBEAT_MS = 1000;
constexpr uint32_t LINK_LOST_MS = 5000;      // > a 2-3 s Wi-Fi search on the main board
constexpr size_t CONFIG_CHUNK_DATA = 200;    // bytes of configuration per chunk
constexpr size_t MAX_CONFIG = 32768;
constexpr size_t POINT_BYTES = 6;            // value (float) + status + level
constexpr uint8_t MAX_POINTS_PER_FRAME = (MAX_PAYLOAD - 4) / POINT_BYTES;   // 39

struct Header {
  uint8_t type = 0, src = 0, dst = 0;
  uint16_t seq = 0;
};

struct Hello {
  uint8_t role = ROLE_SENSOR, chip = CHIP_ESP32, channel = 0, points = 0;
  uint8_t mac[6] = {0};
  uint32_t configRev = 0;
  char firmware[16] = "";
  char name[24] = "";
};

struct Heartbeat {
  uint32_t uptimeS = 0, configRev = 0;
  uint8_t level = 0;           // worst alarm level on the board (same values as the main board)
  uint8_t flags = 0;           // HB_* bits
  int8_t rssi = 0;             // signal of the last frame received from the peer
  uint8_t points = 0;
};
enum HeartbeatFlags : uint8_t { HB_SAFE_MODE = 1, HB_RELAYS_SAFE = 2, HB_NEEDS_CONFIG = 4, HB_MUTED = 8 };

struct ConfigChunk {
  uint32_t rev = 0, crc = 0;   // crc32 of the whole configuration
  uint16_t total = 0;          // whole configuration, bytes
  uint16_t index = 0;          // chunk number (offset = index * CONFIG_CHUNK_DATA)
  const uint8_t* data = nullptr;
  uint8_t len = 0;
};

struct ConfigAck {
  uint32_t rev = 0;
  uint8_t status = 0;          // CFG_*
  char message[48] = "";
};
enum ConfigStatus : uint8_t { CFG_OK = 0, CFG_BAD_CRC = 1, CFG_INVALID = 2, CFG_TOO_BIG = 3 };

struct Point {
  float value = 0;             // NaN = no value
  uint8_t status = 0;          // sensor status (ST_* on the main board)
  uint8_t level = 0;           // alarm level evaluated on the board itself
};
struct Points {
  uint16_t configRev = 0;      // low 16 bits: indexes only valid for this configuration
  uint8_t first = 0;           // index of points[0] in the board's point list
  uint8_t count = 0;
  Point points[MAX_POINTS_PER_FRAME];
};

struct Command {
  uint8_t kind = 0;            // CMD_*
  uint8_t index = 0;           // relay index on that board
  uint8_t on = 0;
  uint16_t blinkMs = 0, minutes = 0;
};
enum CommandKind : uint8_t { CMD_RELAY = 1, CMD_MUTE = 2, CMD_IDENTIFY = 3, CMD_REBOOT = 4 };

struct Ack {
  uint16_t seq = 0;            // seq of the COMMAND being answered
  uint8_t status = 0;          // 0 = done, else an error code
};

// ---- frames
// encode*: write a complete frame into out (MAX_FRAME bytes); return its length, 0 on error.
size_t encodeHello(uint8_t* out, const Header& h, const Hello& m);
size_t encodeHeartbeat(uint8_t* out, const Header& h, const Heartbeat& m);
size_t encodeConfigChunk(uint8_t* out, const Header& h, const ConfigChunk& m);
size_t encodeConfigAck(uint8_t* out, const Header& h, const ConfigAck& m);
size_t encodePoints(uint8_t* out, const Header& h, const Points& m);
size_t encodeCommand(uint8_t* out, const Header& h, const Command& m);
size_t encodeAck(uint8_t* out, const Header& h, const Ack& m);

// Check a received frame (marker, version, length, CRC) and split it.
// Returns nullptr when valid, else a short reason ("bad CRC"...).
const char* parse(const uint8_t* frame, size_t len, Header& h, const uint8_t*& payload, size_t& plen);

// decode*: payload -> message. false when the payload is too short or invalid.
// ConfigChunk.data points into the payload (valid while the frame is).
bool decodeHello(const uint8_t* p, size_t n, Hello& m);
bool decodeHeartbeat(const uint8_t* p, size_t n, Heartbeat& m);
bool decodeConfigChunk(const uint8_t* p, size_t n, ConfigChunk& m);
bool decodeConfigAck(const uint8_t* p, size_t n, ConfigAck& m);
bool decodePoints(const uint8_t* p, size_t n, Points& m);
bool decodeCommand(const uint8_t* p, size_t n, Command& m);
bool decodeAck(const uint8_t* p, size_t n, Ack& m);

// ---- configuration transfer
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);
uint16_t crc16(const uint8_t* data, size_t len);
inline uint16_t chunkCount(size_t total) { return (uint16_t)((total + CONFIG_CHUNK_DATA - 1) / CONFIG_CHUNK_DATA); }
// Fill a chunk of `all` (the whole configuration) for sending.
ConfigChunk makeChunk(uint32_t rev, const uint8_t* all, size_t total, uint32_t crc, uint16_t index);

// Collects chunks in any order (duplicates are fine) and checks the CRC.
class ConfigAssembler {
 public:
  // ALREADY: chunk of the revision just completed (a resend): data() is unchanged; ack again.
  enum Result { MORE, DONE, ERROR, ALREADY };
  Result add(const ConfigChunk& c);
  const std::vector<uint8_t>& data() const { return buf; }
  uint32_t rev() const { return curRev; }
  uint8_t status() const { return lastStatus; }   // CFG_* after ERROR
  uint16_t missing() const;                        // chunks still to come
  void reset();
 private:
  uint32_t curRev = 0, curCrc = 0;
  uint16_t curTotal = 0;
  bool active = false, complete = false;
  uint8_t lastStatus = CFG_OK;
  std::vector<uint8_t> buf;
  std::vector<bool> have;
};

// Sequence numbers per link: duplicates (same src + seq within the last few
// frames) are dropped; a jump is counted as lost frames for the link quality.
class SeqTracker {
 public:
  bool fresh(uint16_t seq);         // false = duplicate
  uint32_t received() const { return rx; }
  uint32_t lost() const { return lostCount; }
  void reset() { started = false; rx = 0; lostCount = 0; }
 private:
  bool started = false;
  uint16_t last = 0;
  uint32_t rx = 0, lostCount = 0;
  uint16_t recent[8] = {0};
  uint8_t recentPos = 0;
};

const char* typeName(uint8_t type);
const char* roleName(uint8_t role);
}  // namespace Proto
