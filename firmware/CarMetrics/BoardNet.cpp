#include "BoardNet.h"
#include <string.h>
#include <algorithm>

namespace BoardNet {
using namespace Proto;

static void copyStr(char* dst, size_t n, const std::string& s) { strncpy(dst, s.c_str(), n - 1); dst[n - 1] = 0; }
static bool late(uint32_t now, uint32_t since, uint32_t ms) { return now - since >= ms; }

// ============================================================== extra board
NodeSession::NodeSession(Link& l, const Identity& m, uint8_t storedId, uint32_t storedRev, const Callbacks& c)
    : link(l), me(m), cb(c), id(storedId > MAX_BOARD_ID ? UNPAIRED : storedId), cfgRev(storedRev) {}

void NodeSession::send(const std::function<size_t(uint8_t*, const Header&)>& enc, uint8_t dst) {
  uint8_t f[MAX_FRAME];
  Header h; h.src = id; h.dst = dst; h.seq = seq++;
  size_t n = enc(f, h);
  if (n) link.send(dst, f, n);
}

void NodeSession::sendHello() {
  Hello m;
  m.role = me.role; m.chip = me.chip; memcpy(m.mac, me.mac, 6); m.configRev = cfgRev;
  m.points = cb.points ? cb.points(nullptr, 0) : 0;
  copyStr(m.firmware, sizeof(m.firmware), me.firmware); copyStr(m.name, sizeof(m.name), me.name);
  send([&](uint8_t* f, const Header& h) { return encodeHello(f, h, m); }, BROADCAST);
}

void NodeSession::sendConfigAck() {
  if (!haveAck) return;
  ConfigAck a = lastAck;
  send([&](uint8_t* f, const Header& h) { return encodeConfigAck(f, h, a); });
}

void NodeSession::update(uint32_t now) {
  if (!paired()) {
    if (!lastHello || late(now, lastHello, HELLO_UNPAIRED_MS)) { lastHello = now ? now : 1; sendHello(); }
    return;
  }
  if (up && late(now, lastHeard, LINK_LOST_MS)) {
    up = false;
    if (cb.link) cb.link(false);              // relays to their safe state; local alarms carry on
  }
  if (!up && (!lastHello || late(now, lastHello, HELLO_UNPAIRED_MS))) { lastHello = now ? now : 1; sendHello(); }   // "I'm still here"
  if (!lastHeartbeat || late(now, lastHeartbeat, HEARTBEAT_MS)) {
    lastHeartbeat = now ? now : 1;
    Heartbeat hb;
    hb.uptimeS = now / 1000; hb.configRev = cfgRev; hb.rssi = rssi;
    hb.level = cb.level ? cb.level() : 0;
    hb.points = cb.points ? cb.points(nullptr, 0) : 0;
    hb.flags = flags | (cfgRev ? 0 : HB_NEEDS_CONFIG) | (up ? 0 : HB_RELAYS_SAFE);
    send([&](uint8_t* f, const Header& h) { return encodeHeartbeat(f, h, hb); });
  }
  if (cfgRev && cb.points && (!lastPoints || late(now, lastPoints, pointsEveryMs))) {
    lastPoints = now ? now : 1;
    Point all[255];
    uint8_t n = cb.points(all, 255);
    for (uint8_t first = 0; first < n; first += MAX_POINTS_PER_FRAME) {
      Points m; m.configRev = (uint16_t)cfgRev; m.first = first;
      m.count = (uint8_t)std::min<int>(MAX_POINTS_PER_FRAME, n - first);
      for (uint8_t i = 0; i < m.count; i++) m.points[i] = all[first + i];
      send([&](uint8_t* f, const Header& h) { return encodePoints(f, h, m); });
    }
  }
}

void NodeSession::receive(const uint8_t* frame, size_t len, int8_t r, uint32_t now) {
  Header h; const uint8_t* p; size_t n;
  if (parse(frame, len, h, p, n) || h.src != MAIN_BOARD) return;
  heardMainAt = now ? now : 1;
  if (h.type == ASSIGN) {                     // addressed by MAC, works before and after pairing
    Assign a;
    if (!decodeAssign(p, n, a) || memcmp(a.mac, me.mac, 6)) return;
    id = a.board > MAX_BOARD_ID ? UNPAIRED : a.board;
    if (a.name[0]) me.name = a.name;
    if (cb.paired) cb.paired(id, me.name);
    fromMain.reset();
    lastHello = 0; up = false;
    if (paired()) { lastHeard = now; sendHello(); }
    return;
  }
  if (!paired() || (h.dst != id && h.dst != BROADCAST)) return;
  if (h.type == COMMAND && (int32_t)h.seq == lastCmdSeq) {   // our ACK was lost: answer again, don't act twice
    Ack a; a.seq = h.seq; a.status = lastCmdStatus;
    send([&](uint8_t* f, const Header& hh) { return encodeAck(f, hh, a); });
    return;
  }
  if (!up) fromMain.reset();                  // main board may have restarted: its numbers start again
  if (!fromMain.fresh(h.seq)) return;
  rssi = r; lastHeard = now;
  if (!up) { up = true; everHeard = true; if (cb.link) cb.link(true); }

  switch (h.type) {
    case CONFIG_CHUNK: {
      ConfigChunk c;
      if (!decodeConfigChunk(p, n, c)) return;
      if (c.rev == cfgRev) {                  // already running exactly these settings: just confirm
        lastAck = ConfigAck(); lastAck.rev = c.rev; lastAck.status = CFG_OK; haveAck = true;
        sendConfigAck();
        return;
      }
      ConfigAssembler::Result res = assembler.add(c);
      if (res == ConfigAssembler::MORE) return;
      if (res == ConfigAssembler::ALREADY) { sendConfigAck(); return; }
      ConfigAck a; a.rev = c.rev;
      if (res == ConfigAssembler::ERROR) a.status = assembler.status();
      else {
        std::string msg;
        a.status = cb.config ? cb.config(assembler.data(), c.rev, msg) : (uint8_t)CFG_OK;
        copyStr(a.message, sizeof(a.message), msg);
        if (a.status == CFG_OK) cfgRev = c.rev;
      }
      lastAck = a; haveAck = true;
      sendConfigAck();
      return;
    }
    case COMMAND: {
      Command c;
      if (!decodeCommand(p, n, c)) return;
      uint8_t st = cb.command ? cb.command(c) : 1;
      lastCmdSeq = h.seq; lastCmdStatus = st;
      Ack a; a.seq = h.seq; a.status = st;
      send([&](uint8_t* f, const Header& hh) { return encodeAck(f, hh, a); });
      return;
    }
    default: return;                          // heartbeat: lastHeard already updated
  }
}

// ============================================================== main board
MainSession::MainSession(Link& l, const Identity& m, const Callbacks& c) : link(l), me(m), cb(c) {}

BoardInfo* MainSession::find(uint8_t id) {
  for (auto& b : list) if (b.id == id) return &b;
  return nullptr;
}
const BoardInfo* MainSession::board(uint8_t id) const {
  for (auto& b : list) if (b.id == id) return &b;
  return nullptr;
}

uint16_t MainSession::send(uint8_t dst, const std::function<size_t(uint8_t*, const Header&)>& enc, int useSeq) {
  uint8_t f[MAX_FRAME];
  Header h; h.src = MAIN_BOARD; h.dst = dst; h.seq = useSeq >= 0 ? (uint16_t)useSeq : seq++;
  size_t n = enc(f, h);
  if (n) link.send(dst, f, n);
  return h.seq;
}

void MainSession::restore(const BoardInfo& b) {
  if (b.id > MAX_BOARD_ID || find(b.id) || list.size() >= MAX_BOARDS) return;
  BoardInfo c;
  c.id = b.id; memcpy(c.mac, b.mac, 6); c.name = b.name; c.role = b.role; c.chip = b.chip;
  list.push_back(c);
}

int MainSession::accept(const uint8_t mac[6], const std::string& name) {
  if (list.size() >= MAX_BOARDS) return -1;
  auto it = std::find_if(foundList.begin(), foundList.end(), [&](const FoundBoard& f) { return !memcmp(f.mac, mac, 6); });
  if (it == foundList.end()) return -1;
  uint8_t id = 1;
  while (find(id)) id++;
  BoardInfo b;
  b.id = id; memcpy(b.mac, mac, 6); b.name = name.empty() ? it->name : name;
  b.role = it->role; b.chip = it->chip; b.firmware = it->firmware;
  list.push_back(b);
  foundList.erase(it);
  Assign a; memcpy(a.mac, mac, 6); a.board = id; copyStr(a.name, sizeof(a.name), b.name);
  send(BROADCAST, [&](uint8_t* f, const Header& h) { return encodeAssign(f, h, a); });
  if (cb.boardsChanged) cb.boardsChanged();
  return id;
}

bool MainSession::forget(uint8_t id) {
  auto it = std::find_if(list.begin(), list.end(), [&](const BoardInfo& b) { return b.id == id; });
  if (it == list.end()) return false;
  Assign a; memcpy(a.mac, it->mac, 6); a.board = UNPAIRED;
  send(BROADCAST, [&](uint8_t* f, const Header& h) { return encodeAssign(f, h, a); });
  list.erase(it);
  pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PendingCmd& p) { return p.board == id; }), pending.end());
  if (cb.boardsChanged) cb.boardsChanged();
  return true;
}

void MainSession::setConfig(uint8_t id, uint32_t rev, const std::vector<uint8_t>& json) {
  BoardInfo* b = find(id);
  if (!b || json.empty() || json.size() > MAX_CONFIG) return;
  b->configRev = rev;
  b->configPending = rev != b->ackedRev;      // revision = CRC of the settings: same settings, nothing to send
  b->tx.json = json; b->tx.crc = crc32(json.data(), json.size()); b->tx.next = 0; b->tx.roundAt = 0;
}

int MainSession::command(uint8_t id, const Command& c) {
  BoardInfo* b = find(id);
  if (!b) return -1;
  PendingCmd p; p.board = id; p.c = c; p.tries = 1; p.lastSent = nowMs;
  p.seq = send(id, [&](uint8_t* f, const Header& h) { return encodeCommand(f, h, c); });
  pending.push_back(p);
  return p.seq;
}

void MainSession::update(uint32_t now) {
  nowMs = now;
  // online / offline
  for (auto& b : list)
    if (b.online && late(now, b.lastHeard, LINK_LOST_MS)) { b.online = false; if (cb.online) cb.online(b, false); }
  // forget "found" boards not heard for a while
  foundList.erase(std::remove_if(foundList.begin(), foundList.end(), [&](const FoundBoard& f) { return late(now, f.lastHeard, 4 * HELLO_UNPAIRED_MS); }), foundList.end());
  // heartbeat to every paired board, plus one broadcast "beacon" so boards can find our channel
  if (!lastHeartbeat || late(now, lastHeartbeat, HEARTBEAT_MS)) {
    lastHeartbeat = now ? now : 1;
    beacon(now);
    for (auto& b : list) {
      Heartbeat hb; hb.uptimeS = now / 1000; hb.configRev = b.configRev; hb.rssi = b.rssi;
      send(b.id, [&](uint8_t* f, const Header& h) { return encodeHeartbeat(f, h, hb); });
    }
  }
  // configuration: a few chunks per call, a new round every CONFIG_RESEND_MS until acknowledged
  for (auto& b : list) {
    if (!b.configPending || !b.online) continue;
    uint16_t total = chunkCount(b.tx.json.size());
    if (b.tx.next >= total) {
      if (!late(now, b.tx.roundAt, CONFIG_RESEND_MS)) continue;
      b.tx.next = 0;
    }
    if (b.tx.next == 0) b.tx.roundAt = now;
    for (uint8_t k = 0; k < CHUNKS_PER_UPDATE && b.tx.next < total; k++, b.tx.next++) {
      ConfigChunk c = makeChunk(b.configRev, b.tx.json.data(), b.tx.json.size(), b.tx.crc, b.tx.next);
      send(b.id, [&](uint8_t* f, const Header& h) { return encodeConfigChunk(f, h, c); });
    }
  }
  // commands: resend until answered
  for (auto it = pending.begin(); it != pending.end();) {
    if (!late(now, it->lastSent, COMMAND_RESEND_MS)) { ++it; continue; }
    if (it->tries >= COMMAND_TRIES) {
      if (cb.commandResult) cb.commandResult(it->board, it->seq, 255);
      it = pending.erase(it);
      continue;
    }
    it->tries++; it->lastSent = now;
    Command c = it->c;
    send(it->board, [&](uint8_t* f, const Header& h) { return encodeCommand(f, h, c); }, it->seq);
    ++it;
  }
}

void MainSession::beacon(uint32_t now) {
  lastBeacon = now ? now : 1;
  Heartbeat hb; hb.uptimeS = now / 1000;
  send(BROADCAST, [&](uint8_t* f, const Header& h) { return encodeHeartbeat(f, h, hb); });
}

void MainSession::receive(const uint8_t* frame, size_t len, int8_t rssi, uint32_t now) {
  Header h; const uint8_t* p; size_t n;
  if (parse(frame, len, h, p, n) || h.src == MAIN_BOARD) return;
  if (h.dst != MAIN_BOARD && h.dst != BROADCAST) return;

  if (h.type == HELLO) {
    Hello m;
    if (!decodeHello(p, n, m)) return;
    if (!lastBeacon || late(now, lastBeacon, 200)) beacon(now);   // answer at once: a searching board only waits briefly per channel
    BoardInfo* b = nullptr;
    for (auto& x : list) if (!memcmp(x.mac, m.mac, 6)) b = &x;
    if (!b) {                                 // not paired here: list it as found
      // (a board that thinks it is paired to another main board is offered here too)
      auto it = std::find_if(foundList.begin(), foundList.end(), [&](const FoundBoard& f) { return !memcmp(f.mac, m.mac, 6); });
      if (it == foundList.end()) { foundList.push_back(FoundBoard()); it = foundList.end() - 1; }
      memcpy(it->mac, m.mac, 6); it->name = m.name; it->firmware = m.firmware; it->role = m.role; it->chip = m.chip;
      it->rssi = rssi; it->lastHeard = now;
      return;
    }
    if (h.src != b->id) {                     // paired here but it lost / never got its id: tell it again
      Assign a; memcpy(a.mac, b->mac, 6); a.board = b->id; copyStr(a.name, sizeof(a.name), b->name);
      send(BROADCAST, [&](uint8_t* f, const Header& hh) { return encodeAssign(f, hh, a); });
      return;
    }
    b->firmware = m.firmware; b->role = m.role; b->chip = m.chip;
    if (m.configRev != b->ackedRev) b->ackedRev = m.configRev;
  }

  BoardInfo* b = find(h.src);
  if (!b) return;
  if (!b->online) b->seqs.reset();            // the board may have restarted
  if (!b->seqs.fresh(h.seq)) return;
  b->lastHeard = now; b->rssi = rssi;
  if (!b->online) { b->online = true; if (cb.online) cb.online(*b, true); }

  switch (h.type) {
    case HEARTBEAT: {
      Heartbeat hb;
      if (!decodeHeartbeat(p, n, hb)) return;
      b->uptimeS = hb.uptimeS; b->level = hb.level; b->flags = hb.flags; b->rssiAtBoard = hb.rssi;
      if (hb.configRev && hb.configRev == b->configRev && b->configPending) {   // it has it already (ack lost)
        b->configPending = false; b->ackedRev = hb.configRev; b->configStatus = CFG_OK;
      }
      if (!b->ackedRev) b->ackedRev = hb.configRev;
      return;
    }
    case CONFIG_ACK: {
      ConfigAck a;
      if (!decodeConfigAck(p, n, a) || a.rev != b->configRev) return;
      bool wasPending = b->configPending;
      b->configPending = false;
      b->configStatus = a.status; b->configMessage = a.message;
      if (a.status == CFG_OK) b->ackedRev = a.rev;
      if (wasPending && cb.configResult) cb.configResult(*b);
      return;
    }
    case POINTS: {
      Points m;
      if (!decodePoints(p, n, m) || m.configRev != (uint16_t)b->ackedRev) return;   // values for another layout
      size_t need = (size_t)m.first + m.count;
      if (b->points.size() < need) b->points.resize(need);
      for (uint8_t i = 0; i < m.count; i++) b->points[m.first + i] = m.points[i];
      b->pointsAt = now;
      return;
    }
    case ACK: {
      Ack a;
      if (!decodeAck(p, n, a)) return;
      for (auto it = pending.begin(); it != pending.end(); ++it)
        if (it->board == b->id && it->seq == a.seq) {
          if (cb.commandResult) cb.commandResult(b->id, a.seq, a.status);
          pending.erase(it);
          break;
        }
      return;
    }
    default: return;
  }
}
}  // namespace BoardNet
