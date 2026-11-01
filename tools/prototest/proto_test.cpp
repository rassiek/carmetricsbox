// Host-side test of the board-to-board protocol (firmware/CarMetrics/Proto.*).
// Build + run: tools/cm.sh prototest
#include "Proto.h"
#include "Link.h"
#include "Half.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int passed = 0, failed = 0;
void boardsTest();
void check(bool ok, const char* what, const std::string& detail = "") {
  (ok ? passed : failed)++;
  printf("%s %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}

using namespace Proto;

int main() {
  uint8_t f[MAX_FRAME];
  Header h; h.src = 3; h.dst = MAIN_BOARD; h.seq = 4711;

  // hello round trip
  {
    Hello m; m.role = ROLE_SENSOR_RELAY; m.chip = CHIP_ESP32S3; m.channel = 6; m.points = 12; m.configRev = 99;
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6}; memcpy(m.mac, mac, 6);
    strcpy(m.firmware, "5.0.0-dev"); strcpy(m.name, "Engine bay");
    size_t n = encodeHello(f, h, m);
    Header h2; const uint8_t* p; size_t pn;
    const char* err = parse(f, n, h2, p, pn);
    Hello d;
    check(n && !err && h2.type == HELLO && h2.src == 3 && h2.seq == 4711 && decodeHello(p, pn, d) && d.role == ROLE_SENSOR_RELAY &&
          d.chip == CHIP_ESP32S3 && d.channel == 6 && d.configRev == 99 && !memcmp(d.mac, mac, 6) && !strcmp(d.name, "Engine bay") &&
          !strcmp(d.firmware, "5.0.0-dev"), "hello round trip", std::to_string(n) + " bytes");
    // long name is cut, never overflows
    memset(m.name, 'x', sizeof(m.name) - 1); m.name[sizeof(m.name) - 1] = 0;
    n = encodeHello(f, h, m);
    check(n && !parse(f, n, h2, p, pn) && decodeHello(p, pn, d) && strlen(d.name) == sizeof(d.name) - 1, "hello with a full-length name");
  }

  // points: a full frame, NaN kept, fits ESP-NOW v1
  {
    Points m; m.configRev = 0xBEEF; m.first = 5; m.count = MAX_POINTS_PER_FRAME;
    for (int i = 0; i < m.count; i++) { m.points[i].value = i * 1.5f; m.points[i].status = i % 4; m.points[i].level = i % 3; }
    m.points[7].value = NAN;
    size_t n = encodePoints(f, h, m);
    Header h2; const uint8_t* p; size_t pn; Points d;
    bool ok = n && n <= MAX_FRAME && !parse(f, n, h2, p, pn) && decodePoints(p, pn, d) && d.count == m.count && d.first == 5 && d.configRev == 0xBEEF;
    for (int i = 0; ok && i < m.count; i++)
      ok = (i == 7 ? std::isnan(d.points[i].value) : d.points[i].value == m.points[i].value) && d.points[i].status == m.points[i].status && d.points[i].level == m.points[i].level;
    check(ok, "points: 39 values in one frame", std::to_string(n) + " bytes");
    m.count = MAX_POINTS_PER_FRAME + 1;
    check(encodePoints(f, h, m) == 0, "points: too many for one frame refused");
  }

  // heartbeat, command, ack, config-ack
  {
    Heartbeat hb; hb.uptimeS = 123456; hb.configRev = 7; hb.level = 2; hb.flags = HB_RELAYS_SAFE | HB_MUTED; hb.rssi = -71; hb.points = 9;
    size_t n = encodeHeartbeat(f, h, hb);
    Header h2; const uint8_t* p; size_t pn; Heartbeat d;
    check(n && !parse(f, n, h2, p, pn) && decodeHeartbeat(p, pn, d) && d.uptimeS == 123456 && d.rssi == -71 && d.flags == (HB_RELAYS_SAFE | HB_MUTED), "heartbeat round trip");
    Command c; c.kind = CMD_RELAY; c.index = 2; c.on = 1; c.blinkMs = 500;
    n = encodeCommand(f, h, c); Command dc;
    check(n && !parse(f, n, h2, p, pn) && decodeCommand(p, pn, dc) && dc.kind == CMD_RELAY && dc.index == 2 && dc.on && dc.blinkMs == 500, "command round trip");
    Ack a; a.seq = 4711; a.status = 0; n = encodeAck(f, h, a); Ack da;
    check(n && !parse(f, n, h2, p, pn) && decodeAck(p, pn, da) && da.seq == 4711, "ack round trip");
    ConfigAck ca; ca.rev = 8; ca.status = CFG_INVALID; strcpy(ca.message, "relay on input-only GPIO34");
    n = encodeConfigAck(f, h, ca); ConfigAck dca;
    check(n && !parse(f, n, h2, p, pn) && decodeConfigAck(p, pn, dca) && dca.status == CFG_INVALID && !strcmp(dca.message, ca.message), "config-ack round trip");
  }

  // damaged frames are refused
  {
    Heartbeat hb; size_t n = encodeHeartbeat(f, h, hb);
    Header h2; const uint8_t* p; size_t pn;
    uint8_t g[MAX_FRAME];
    memcpy(g, f, n); g[10] ^= 0x01;
    check(parse(g, n, h2, p, pn) && !strcmp(parse(g, n, h2, p, pn), "bad CRC"), "flipped bit -> bad CRC");
    check(parse(f, n - 1, h2, p, pn) != nullptr, "truncated frame refused");
    memcpy(g, f, n); g[1] = VERSION + 1;
    check(parse(g, n, h2, p, pn) != nullptr, "other protocol version refused");
    memcpy(g, f, n); g[0] = 'X';
    check(parse(g, n, h2, p, pn) != nullptr, "foreign frame refused");
    Hello tmp; check(!decodeHello(f + 8, 3, tmp), "short payload refused");
  }

  // configuration: 2.5 KB over a loopback link, reversed order, a lost chunk resent
  {
    std::string cfg = "{\"role\":\"sensor\",\"sensors\":[";
    for (int i = 0; i < 40; i++) cfg += std::string(i ? "," : "") + "{\"id\":\"s" + std::to_string(i) + "\",\"type\":\"ds18b20\",\"warn\":100}";
    cfg += "]}";
    const uint8_t* all = (const uint8_t*)cfg.data();
    uint32_t crc = crc32(all, cfg.size());
    uint16_t chunks = chunkCount(cfg.size());

    LoopbackLink mainSide, board;
    LoopbackLink::connect(mainSide, board);
    board.reverse = true;          // arrive in reverse order
    mainSide.dropEvery = 4;        // every 4th lost
    ConfigAssembler as;
    int done = 0, errors = 0, again = 0;
    board.onReceive([&](const uint8_t* fr, size_t len, int8_t) {
      Header hh; const uint8_t* p; size_t pn; ConfigChunk c;
      if (parse(fr, len, hh, p, pn) || hh.type != CONFIG_CHUNK || !decodeConfigChunk(p, pn, c)) { errors++; return; }
      auto r = as.add(c);
      if (r == ConfigAssembler::DONE) done++; else if (r == ConfigAssembler::ERROR) errors++; else if (r == ConfigAssembler::ALREADY) again++;
    });
    uint16_t seq = 0;
    for (uint16_t i = 0; i < chunks; i++) {
      Header hh; hh.type = CONFIG_CHUNK; hh.seq = seq++;
      size_t n = encodeConfigChunk(f, hh, makeChunk(42, all, cfg.size(), crc, i));
      mainSide.send(1, f, n);
    }
    board.poll();
    uint16_t missingAfterFirst = as.missing();
    // resend everything once more (the main board resends until acknowledged); duplicates are fine
    mainSide.dropEvery = 0;
    for (uint16_t i = 0; i < chunks; i++) {
      Header hh; hh.type = CONFIG_CHUNK; hh.seq = seq++;
      size_t n = encodeConfigChunk(f, hh, makeChunk(42, all, cfg.size(), crc, i));
      mainSide.send(1, f, n);
    }
    board.poll();
    check(missingAfterFirst > 0 && done == 1 && again > 0 && errors == 0 && std::string(as.data().begin(), as.data().end()) == cfg && as.rev() == 42,
          "config reassembled after losses, reverse order and duplicates",
          std::to_string(cfg.size()) + " bytes, " + std::to_string(chunks) + " chunks, " + std::to_string(missingAfterFirst) + " missing after first pass");

    // wrong CRC -> refused with CFG_BAD_CRC
    ConfigAssembler bad; ConfigAssembler::Result r = ConfigAssembler::MORE;
    for (uint16_t i = 0; i < chunks; i++) r = bad.add(makeChunk(43, all, cfg.size(), crc ^ 1, i));
    check(r == ConfigAssembler::ERROR && bad.status() == CFG_BAD_CRC, "config with a wrong CRC refused");
    // a newer revision mid-transfer starts over
    ConfigAssembler mid;
    mid.add(makeChunk(50, all, cfg.size(), crc, 0));
    for (uint16_t i = 0; i < chunks; i++) r = mid.add(makeChunk(51, all, cfg.size(), crc, i));
    check(r == ConfigAssembler::DONE && mid.rev() == 51, "newer config revision replaces a partial one");
    ConfigChunk huge = makeChunk(1, all, cfg.size(), crc, 0); huge.total = 0;
    check(ConfigAssembler().add(huge) == ConfigAssembler::ERROR, "empty / oversized config refused");
  }

  // sequence numbers: duplicates dropped, gaps counted, wrap-around
  {
    SeqTracker t;
    bool a = t.fresh(65534), b = t.fresh(65535), c = t.fresh(65535), d = t.fresh(2);
    check(a && b && !c && d && t.lost() == 2 && t.received() == 3, "duplicates dropped, gaps counted across wrap-around",
          "lost " + std::to_string(t.lost()));
  }

  // half floats for the chart history: ~3 significant digits, NaN kept
  {
    struct { float v, tol; } cases[] = {{0, 0}, {14.53f, 0.008f}, {486.3f, 0.25f}, {1661, 0.5f}, {4123, 2}, {-40, 0}, {0.37f, 0.0002f}, {101.5f, 0.03f}, {65504, 0}, {-12.25f, 0}};
    bool ok = true; std::string worst;
    for (auto& c : cases) {
      float back = Half::toFloat(Half::fromFloat(c.v));
      if (std::fabs(back - c.v) > c.tol) { ok = false; worst += std::to_string(c.v) + "->" + std::to_string(back) + " "; }
    }
    check(ok, "half floats keep chart precision", worst);
    check(std::isnan(Half::toFloat(Half::fromFloat(NAN))) && std::isnan(Half::toFloat(Half::NaN)), "half floats keep NaN (no value)");
    check(std::isinf(Half::toFloat(Half::fromFloat(1e6f))), "half floats: out of range -> infinity, not garbage");
    bool exact = true;   // every half value survives a round trip through float
    for (uint32_t h = 0; h < 0x10000; h++) {
      float f = Half::toFloat((uint16_t)h);
      if (std::isnan(f)) continue;
      if (Half::fromFloat(f) != h) { exact = false; worst = std::to_string(h); break; }
    }
    check(exact, "all 65536 half values round-trip exactly", worst);
  }

  // pairing message: with and without a link key; an old-style frame (no key) still decodes
  {
    Assign a; const uint8_t mac[6] = {1, 2, 3, 4, 5, 6}; memcpy(a.mac, mac, 6); a.board = 2; strcpy(a.name, "Bay");
    a.hasKey = true; for (int i = 0; i < 16; i++) a.key[i] = (uint8_t)(i * 7);
    uint8_t f2[MAX_FRAME]; Header hh; size_t n = encodeAssign(f2, hh, a);
    Header h2; const uint8_t* p; size_t pn; Assign d;
    bool k = n && !parse(f2, n, h2, p, pn) && decodeAssign(p, pn, d) && d.hasKey && !memcmp(d.key, a.key, 16) && d.board == 2;
    a.hasKey = false; n = encodeAssign(f2, hh, a); Assign d2;
    bool nk = n && !parse(f2, n, h2, p, pn) && decodeAssign(p, pn, d2) && !d2.hasKey && !strcmp(d2.name, "Bay");
    check(k && nk, "pairing message with / without link key");
  }

  // known CRC values (so the ESP32 and a PC agree)
  check(crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
  check(crc16((const uint8_t*)"123456789", 9) == 0x29B1, "CRC-16/CCITT check value");

  boardsTest();
  printf("DONE: %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
