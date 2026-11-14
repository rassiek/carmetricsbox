// Host-side test of the display role: the main board sends a display its catalog
// (as settings) and all values (POINTS), the display sends commands back.
#include "BoardNet.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

void check(bool ok, const char* what, const std::string& detail = "");
using namespace Proto;
using namespace BoardNet;

void displayTest() {
  // ---- message format: new trailing fields, and old 7-byte commands still decode
  {
    uint8_t f[MAX_FRAME]; Header h; h.src = 2; h.dst = 0; h.seq = 9;
    Command c; c.kind = CMD_SET_LIMIT; c.index = 4; c.which = LIMIT_CRIT_HIGH; c.value = 105.5f; c.rev = 0xBEEF; c.board = 3;
    size_t n = encodeCommand(f, h, c);
    Header h2; const uint8_t* p; size_t pl; Command d;
    bool ok = n && !parse(f, n, h2, p, pl) && decodeCommand(p, pl, d);
    check(ok && d.kind == CMD_SET_LIMIT && d.index == 4 && d.which == LIMIT_CRIT_HIGH && d.value == 105.5f && d.rev == 0xBEEF && d.board == 3,
          "display command: limit, board and revision fields round-trip");
    const uint8_t old[7] = {CMD_RELAY, 1, 1, 0, 0, 0, 0};
    Command o;
    check(decodeCommand(old, 7, o) && o.kind == CMD_RELAY && o.index == 1 && o.on == 1 && o.board == 0 && o.value == 0,
          "command from an older board (no trailing fields) still decodes");
  }

  LoopbackLink mainLink, dispLink;
  LoopbackLink::connect(mainLink, dispLink);
  uint32_t now = 1000;
  // main board
  int cmdsRun = 0; Command lastCmd; uint8_t mainLevel = 2, mainFlags = HB_MUTED;
  MainSession::Callbacks mcb;
  mcb.boardCommand = [&](const BoardInfo& b, const Command& c) -> uint8_t {
    if (b.role != ROLE_DISPLAY) return ACK_NOT_ALLOWED;
    cmdsRun++; lastCmd = c; return ACK_OK;
  };
  mcb.status = [&](uint8_t& l, uint8_t& fl) { l = mainLevel; fl = mainFlags; };
  Identity mi; mi.role = ROLE_MAIN;
  MainSession m(mainLink, mi, mcb);
  // display
  std::vector<uint8_t> catalog; uint32_t catalogRev = 0;
  std::vector<float> values(60, NAN); uint16_t valuesRev = 0; int frames = 0;
  uint8_t gotLevel = 0, gotFlags = 0; int results = 0; uint8_t lastResult = 99;
  NodeSession::Callbacks dcb;
  dcb.config = [&](const std::vector<uint8_t>& j, uint32_t rev, std::string&) -> uint8_t { catalog = j; catalogRev = rev; return CFG_OK; };
  dcb.mainPoints = [&](const Points& p) {
    frames++; valuesRev = p.configRev;
    for (uint8_t i = 0; i < p.count && p.first + i < (int)values.size(); i++) values[p.first + i] = p.points[i].value;
  };
  dcb.mainStatus = [&](uint8_t l, uint8_t fl) { gotLevel = l; gotFlags = fl; };
  dcb.commandResult = [&](uint16_t, uint8_t st) { results++; lastResult = st; };
  Identity di; di.role = ROLE_DISPLAY; di.chip = CHIP_ESP32S3; di.name = "Dash";
  const uint8_t mac[6] = {9, 8, 7, 6, 5, 4}; memcpy(di.mac, mac, 6);
  NodeSession d(dispLink, di, UNPAIRED, 0, dcb);
  mainLink.onReceive([&](const uint8_t* f, size_t l, int8_t r) { m.receive(f, l, r, now); });
  dispLink.onReceive([&](const uint8_t* f, size_t l, int8_t r) { d.receive(f, l, r, now); });
  auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 20) { now += 20; d.update(now); m.update(now); dispLink.poll(); mainLink.poll(); } };

  run(2500);
  check(m.found().size() == 1 && m.found()[0].role == ROLE_DISPLAY, "display says hello and is listed as found, role display");
  int id = m.accept(mac, "Dash");
  run(1500);
  const BoardInfo* b = m.board((uint8_t)id);
  check(id == 1 && d.paired() && b && b->online && b->role == ROLE_DISPLAY, "display paired");

  // catalog = the display's settings, over a lossy link
  mainLink.dropPercent = 20; dispLink.dropPercent = 20; dispLink.rnd = 3;
  std::string cat = "{\"v\":1,\"s\":[";
  for (int i = 0; i < 50; i++) cat += std::string(i ? "," : "") + "{\"i\":\"s" + std::to_string(i) + "\",\"l\":\"S" + std::to_string(i) + "\"}";
  cat += "],\"r\":[]}";
  uint32_t rev = crc32((const uint8_t*)cat.data(), cat.size());
  m.setConfig(1, rev, std::vector<uint8_t>(cat.begin(), cat.end()));
  uint32_t t0 = now;
  while (now - t0 < 20000 && b->configPending) run(100);
  check(!b->configPending && catalogRev == rev && std::string(catalog.begin(), catalog.end()) == cat, "catalog (2.5 KB) delivered to the display with 20 % loss");

  // values: 50 points = 2 frames per second
  mainLink.dropPercent = 0; dispLink.dropPercent = 0;
  std::vector<Point> pts(50);
  for (int i = 0; i < 50; i++) { pts[i].value = i * 1.5f; pts[i].status = 0; pts[i].level = i == 7 ? 2 : 0; }
  bool sent = m.sendPoints(1, (uint16_t)rev, pts);
  run(200);
  check(sent && valuesRev == (uint16_t)rev && values[0] == 0 && values[49] == 49 * 1.5f && frames == 2, "50 values reach the display in 2 frames, tagged with the catalog revision");
  run(1500);
  check(gotLevel == 2 && (gotFlags & HB_MUTED), "display gets the main board's alarm level and mute state from its heartbeat");

  // command display -> main, answers lost: acts once
  mainLink.dropPercent = 50; mainLink.rnd = 11;   // main's answers get lost half the time
  int seq = d.sendCommand([] { Command c; c.kind = CMD_RELAY; c.board = 2; c.index = 1; c.on = 1; return c; }());
  t0 = now;
  while (now - t0 < 5000 && results == 0) run(50);
  check(seq >= 0 && cmdsRun == 1 && lastCmd.kind == CMD_RELAY && lastCmd.board == 2 && lastCmd.index == 1 && lastCmd.on == 1 && results == 1 && (lastResult == ACK_OK || lastResult == ACK_NO_ANSWER),
        "relay command from the display runs exactly once on the main board", "runs=" + std::to_string(cmdsRun) + " result=" + std::to_string(lastResult));
  mainLink.dropPercent = 0;
  int before = cmdsRun;
  results = 0;
  d.sendCommand([] { Command c; c.kind = CMD_SET_LIMIT; c.index = 3; c.which = LIMIT_WARN_HIGH; c.value = 98; c.rev = 7; return c; }());
  run(500);
  check(cmdsRun == before + 1 && lastCmd.kind == CMD_SET_LIMIT && lastCmd.value == 98 && results == 1 && lastResult == ACK_OK && b->commandsIn >= 2,
        "limit change from the display is answered OK");

  // link cut: commands not sent; no answer reported
  mainLink.cut = true; dispLink.cut = true;
  run(7000);
  check(!d.linkUp() && d.sendCommand(Command()) < 0 && !b->online, "link lost: display stops sending commands, main marks it offline");
  mainLink.cut = false; dispLink.cut = false;
  run(3000);
  check(d.linkUp() && b->online, "display link back by itself");

  // Real firmware: update(now) runs with a time taken just BEFORE the frames are
  // handled with a fresh millis(), so lastHeard can be a few ms ahead of "now".
  // That must not count as "silent for 5 s" (boards flapped online/offline on 5.2.0-beta.2).
  {
    int flaps = 0;
    MainSession::Callbacks c2; c2.online = [&](const BoardInfo&, bool on) { if (!on) flaps++; };
    LoopbackLink a1, a2; LoopbackLink::connect(a1, a2);
    Identity mi2; mi2.role = ROLE_MAIN;
    MainSession m2(a1, mi2, c2);
    BoardInfo bi; bi.id = 1; memcpy(bi.mac, mac, 6); bi.name = "Bay"; m2.restore(bi);
    NodeSession::Callbacks nc; int nodeDrops = 0; nc.link = [&](bool up) { if (!up) nodeDrops++; };
    Identity ni; ni.role = ROLE_SENSOR; memcpy(ni.mac, mac, 6);
    NodeSession n2(a2, ni, 1, 0, nc);
    uint32_t t = 5000;
    a1.onReceive([&](const uint8_t* f, size_t l, int8_t r) { m2.receive(f, l, r, t + 3); });   // handled "3 ms later"
    a2.onReceive([&](const uint8_t* f, size_t l, int8_t r) { n2.receive(f, l, r, t + 3); });
    for (int i = 0; i < 3000; i++) { t += 10; a2.poll(); a1.poll(); n2.update(t); m2.update(t); }   // firmware order: poll, then update(now taken before)
    check(flaps == 0 && nodeDrops == 0 && m2.board(1)->online && n2.linkUp(), "link stays up when frames are timestamped a few ms after update()",
          "main offline " + std::to_string(flaps) + "x, board " + std::to_string(nodeDrops) + "x");
  }
}
