#include "Proto.h"
#include <string.h>
#include <math.h>

namespace Proto {
namespace {

// Little-endian writer / reader with bounds checks (no packed structs: the
// layout is the same on every compiler and chip).
struct Writer {
  uint8_t* p; size_t cap, n = 0; bool ok = true;
  Writer(uint8_t* buf, size_t c) : p(buf), cap(c) {}
  void bytes(const void* d, size_t len) { if (n + len > cap) { ok = false; return; } memcpy(p + n, d, len); n += len; }
  void u8(uint8_t v) { bytes(&v, 1); }
  void u16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; bytes(b, 2); }
  void u32(uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; bytes(b, 4); }
  void f32(float v) { uint32_t u; memcpy(&u, &v, 4); u32(u); }
  void str(const char* s, size_t field) {   // length byte + text, at most field-1 chars
    size_t len = strnlen(s, field - 1);
    u8((uint8_t)len); bytes(s, len);
  }
};

struct Reader {
  const uint8_t* p; size_t n, i = 0; bool ok = true;
  Reader(const uint8_t* buf, size_t len) : p(buf), n(len) {}
  bool take(void* d, size_t len) { if (i + len > n) { ok = false; return false; } memcpy(d, p + i, len); i += len; return true; }
  uint8_t u8() { uint8_t v = 0; take(&v, 1); return v; }
  uint16_t u16() { uint8_t b[2] = {0}; take(b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
  uint32_t u32() { uint8_t b[4] = {0}; take(b, 4); return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
  float f32() { uint32_t u = u32(); float v; memcpy(&v, &u, 4); return v; }
  void str(char* out, size_t field) {
    uint8_t len = u8();
    if (len >= field || i + len > n) { ok = false; out[0] = 0; return; }
    take(out, len); out[len] = 0;
  }
};

// Header, then the payload written by fill(), then the CRC.
template <typename F>
size_t frame(uint8_t* out, const Header& h, uint8_t type, F fill) {
  uint8_t payload[MAX_PAYLOAD];
  Writer pw(payload, sizeof(payload));
  fill(pw);
  if (!pw.ok) return 0;
  Writer w(out, MAX_FRAME);
  w.u8('C'); w.u8(VERSION); w.u8(type); w.u8(h.src); w.u8(h.dst); w.u16(h.seq); w.u8((uint8_t)pw.n);
  w.bytes(payload, pw.n);
  if (!w.ok) return 0;
  w.u16(crc16(out, w.n));
  return w.ok ? w.n : 0;
}
}  // namespace

uint16_t crc16(const uint8_t* d, size_t len) {   // CRC-16/CCITT-FALSE
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

uint32_t crc32(const uint8_t* d, size_t len, uint32_t crc) {   // standard CRC-32 (zlib)
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

const char* parse(const uint8_t* f, size_t len, Header& h, const uint8_t*& payload, size_t& plen) {
  if (len < OVERHEAD) return "too short";
  if (len > MAX_FRAME) return "too long";
  if (f[0] != 'C') return "not a CarMetrics frame";
  if (f[1] != VERSION) return "other protocol version";
  size_t n = f[7];
  if (n + OVERHEAD != len) return "wrong length";
  uint16_t crc = (uint16_t)(f[len - 2] | (f[len - 1] << 8));
  if (crc != crc16(f, len - 2)) return "bad CRC";
  h.type = f[2]; h.src = f[3]; h.dst = f[4]; h.seq = (uint16_t)(f[5] | (f[6] << 8));
  payload = f + 8; plen = n;
  return nullptr;
}

// ---- messages
size_t encodeHello(uint8_t* out, const Header& h, const Hello& m) {
  return frame(out, h, HELLO, [&](Writer& w) {
    w.u8(m.role); w.u8(m.chip); w.u8(m.channel); w.u8(m.points); w.bytes(m.mac, 6); w.u32(m.configRev);
    w.str(m.firmware, sizeof(m.firmware)); w.str(m.name, sizeof(m.name));
  });
}
bool decodeHello(const uint8_t* p, size_t n, Hello& m) {
  Reader r(p, n);
  m.role = r.u8(); m.chip = r.u8(); m.channel = r.u8(); m.points = r.u8(); r.take(m.mac, 6); m.configRev = r.u32();
  r.str(m.firmware, sizeof(m.firmware)); r.str(m.name, sizeof(m.name));
  return r.ok;
}

size_t encodeHeartbeat(uint8_t* out, const Header& h, const Heartbeat& m) {
  return frame(out, h, HEARTBEAT, [&](Writer& w) {
    w.u32(m.uptimeS); w.u32(m.configRev); w.u8(m.level); w.u8(m.flags); w.u8((uint8_t)m.rssi); w.u8(m.points);
  });
}
bool decodeHeartbeat(const uint8_t* p, size_t n, Heartbeat& m) {
  Reader r(p, n);
  m.uptimeS = r.u32(); m.configRev = r.u32(); m.level = r.u8(); m.flags = r.u8(); m.rssi = (int8_t)r.u8(); m.points = r.u8();
  return r.ok;
}

size_t encodeConfigChunk(uint8_t* out, const Header& h, const ConfigChunk& m) {
  if (m.len > CONFIG_CHUNK_DATA || (m.len && !m.data)) return 0;
  return frame(out, h, CONFIG_CHUNK, [&](Writer& w) {
    w.u32(m.rev); w.u32(m.crc); w.u16(m.total); w.u16(m.index); w.u8(m.len); w.bytes(m.data, m.len);
  });
}
bool decodeConfigChunk(const uint8_t* p, size_t n, ConfigChunk& m) {
  Reader r(p, n);
  m.rev = r.u32(); m.crc = r.u32(); m.total = r.u16(); m.index = r.u16(); m.len = r.u8();
  if (!r.ok || m.len > CONFIG_CHUNK_DATA || r.i + m.len != n) return false;
  m.data = p + r.i;
  return true;
}

size_t encodeConfigAck(uint8_t* out, const Header& h, const ConfigAck& m) {
  return frame(out, h, CONFIG_ACK, [&](Writer& w) { w.u32(m.rev); w.u8(m.status); w.str(m.message, sizeof(m.message)); });
}
bool decodeConfigAck(const uint8_t* p, size_t n, ConfigAck& m) {
  Reader r(p, n);
  m.rev = r.u32(); m.status = r.u8(); r.str(m.message, sizeof(m.message));
  return r.ok;
}

size_t encodePoints(uint8_t* out, const Header& h, const Points& m) {
  if (m.count > MAX_POINTS_PER_FRAME) return 0;
  return frame(out, h, POINTS, [&](Writer& w) {
    w.u16(m.configRev); w.u8(m.first); w.u8(m.count);
    for (uint8_t i = 0; i < m.count; i++) { w.f32(m.points[i].value); w.u8(m.points[i].status); w.u8(m.points[i].level); }
  });
}
bool decodePoints(const uint8_t* p, size_t n, Points& m) {
  Reader r(p, n);
  m.configRev = r.u16(); m.first = r.u8(); m.count = r.u8();
  if (!r.ok || m.count > MAX_POINTS_PER_FRAME || r.i + (size_t)m.count * POINT_BYTES != n) return false;
  for (uint8_t i = 0; i < m.count; i++) { m.points[i].value = r.f32(); m.points[i].status = r.u8(); m.points[i].level = r.u8(); }
  return r.ok;
}

size_t encodeCommand(uint8_t* out, const Header& h, const Command& m) {
  return frame(out, h, COMMAND, [&](Writer& w) { w.u8(m.kind); w.u8(m.index); w.u8(m.on); w.u16(m.blinkMs); w.u16(m.minutes); });
}
bool decodeCommand(const uint8_t* p, size_t n, Command& m) {
  Reader r(p, n);
  m.kind = r.u8(); m.index = r.u8(); m.on = r.u8(); m.blinkMs = r.u16(); m.minutes = r.u16();
  return r.ok;
}

size_t encodeAck(uint8_t* out, const Header& h, const Ack& m) {
  return frame(out, h, ACK, [&](Writer& w) { w.u16(m.seq); w.u8(m.status); });
}
bool decodeAck(const uint8_t* p, size_t n, Ack& m) {
  Reader r(p, n);
  m.seq = r.u16(); m.status = r.u8();
  return r.ok;
}

size_t encodeAssign(uint8_t* out, const Header& h, const Assign& m) {
  return frame(out, h, ASSIGN, [&](Writer& w) {
    w.bytes(m.mac, 6); w.u8(m.board); w.str(m.name, sizeof(m.name));
    if (m.hasKey) w.bytes(m.key, 16);
  });
}
bool decodeAssign(const uint8_t* p, size_t n, Assign& m) {
  Reader r(p, n);
  r.take(m.mac, 6); m.board = r.u8(); r.str(m.name, sizeof(m.name));
  m.hasKey = r.ok && r.i + 16 <= n && r.take(m.key, 16);
  return r.ok;
}

// ---- configuration transfer
ConfigChunk makeChunk(uint32_t rev, const uint8_t* all, size_t total, uint32_t crc, uint16_t index) {
  ConfigChunk c;
  c.rev = rev; c.crc = crc; c.total = (uint16_t)total; c.index = index;
  size_t off = (size_t)index * CONFIG_CHUNK_DATA;
  if (off < total) {
    c.data = all + off;
    c.len = (uint8_t)(total - off < CONFIG_CHUNK_DATA ? total - off : CONFIG_CHUNK_DATA);
  }
  return c;
}

void ConfigAssembler::reset() { active = complete = false; buf.clear(); have.clear(); curRev = 0; }

uint16_t ConfigAssembler::missing() const {
  uint16_t m = 0;
  for (bool b : have) if (!b) m++;
  return m;
}

ConfigAssembler::Result ConfigAssembler::add(const ConfigChunk& c) {
  if (c.total == 0 || c.total > MAX_CONFIG) { lastStatus = CFG_TOO_BIG; return ERROR; }
  uint16_t chunks = chunkCount(c.total);
  if (c.index >= chunks) { lastStatus = CFG_INVALID; return ERROR; }
  size_t off = (size_t)c.index * CONFIG_CHUNK_DATA;
  size_t want = c.total - off < CONFIG_CHUNK_DATA ? c.total - off : CONFIG_CHUNK_DATA;
  if (c.len != want) { lastStatus = CFG_INVALID; return ERROR; }
  if (complete && c.rev == curRev && c.total == curTotal && c.crc == curCrc) return ALREADY;
  // A new revision (or a resend with a different size / CRC) starts over.
  if (!active || c.rev != curRev || c.total != curTotal || c.crc != curCrc) {
    active = true; complete = false; curRev = c.rev; curTotal = c.total; curCrc = c.crc;
    buf.assign(c.total, 0); have.assign(chunks, false);
  }
  memcpy(buf.data() + off, c.data, c.len);
  have[c.index] = true;
  if (missing()) return MORE;
  active = false;
  if (crc32(buf.data(), buf.size()) != curCrc) { lastStatus = CFG_BAD_CRC; return ERROR; }
  lastStatus = CFG_OK;
  complete = true;
  return DONE;
}

bool SeqTracker::fresh(uint16_t seq) {
  for (uint16_t r : recent) if (started && r == seq) return false;
  recent[recentPos] = seq; recentPos = (recentPos + 1) % 8;
  rx++;
  if (started) {
    int16_t ahead = (int16_t)(seq - last);
    if (ahead > 1) lostCount += ahead - 1;
    if (ahead > 0) last = seq;
  } else {
    started = true; last = seq;
    for (auto& r : recent) r = seq;   // only "seq" counts as seen so far
  }
  return true;
}

const char* typeName(uint8_t t) {
  switch (t) {
    case HELLO: return "hello"; case HEARTBEAT: return "heartbeat"; case CONFIG_CHUNK: return "config";
    case CONFIG_ACK: return "config-ack"; case POINTS: return "points"; case COMMAND: return "command"; case ACK: return "ack"; case ASSIGN: return "assign";
    default: return "unknown";
  }
}
const char* roleName(uint8_t r) {
  switch (r) {
    case ROLE_MAIN: return "main"; case ROLE_SENSOR: return "sensor"; case ROLE_RELAY: return "relay";
    case ROLE_SENSOR_RELAY: return "sensor + relay"; case ROLE_DISPLAY: return "display";
    default: return "unknown";
  }
}
}  // namespace Proto
