// Host-side test of the board-to-board exchange (firmware/CarMetrics/BoardNet.*):
// a main board and a sensor + relay board over a loopback link, simulated time.
#include "BoardNet.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

void check(bool ok, const char* what, const std::string& detail = "");
using namespace Proto;
using namespace BoardNet;

void boardsTest() {
  LoopbackLink mainLink, nodeLink;
  LoopbackLink::connect(mainLink, nodeLink);
  uint32_t now = 1000;

  // ---- the extra board
  Identity nodeId; nodeId.role = ROLE_SENSOR_RELAY; nodeId.firmware = "5.0.0-dev"; nodeId.name = "Engine bay";
  const uint8_t nodeMac[6] = {0x24, 0x6F, 0x28, 1, 2, 3}; memcpy(nodeId.mac, nodeMac, 6);
  uint8_t storedId = UNPAIRED; std::string applied; int applyCount = 0, relayActs = 0, linkDowns = 0, linkUps = 0;
  bool refuseNext = false; float coolant = 92.5f;
  NodeSession::Callbacks ncb;
  ncb.paired = [&](uint8_t id, const std::string&) { storedId = id; };
  ncb.config = [&](const std::vector<uint8_t>& j, uint32_t, std::string& msg) -> uint8_t {
    if (refuseNext) { msg = "relay on input-only GPIO34"; return CFG_INVALID; }
    applied.assign(j.begin(), j.end()); applyCount++; return CFG_OK;
  };
  ncb.command = [&](const Command& c) -> uint8_t { if (c.kind == CMD_RELAY) relayActs++; return 0; };
  ncb.link = [&](bool up) { (up ? linkUps : linkDowns)++; };
  ncb.points = [&](Point* out, uint8_t max) -> uint8_t {
    if (out && max >= 3) { out[0].value = coolant; out[1].value = 2.1f; out[2].value = NAN; out[2].status = 3; out[0].level = 1; }
    return 3;
  };
  ncb.level = [] { return (uint8_t)1; };
  NodeSession node(nodeLink, nodeId, storedId, 0, ncb);

  // ---- the main board
  Identity mainId; mainId.role = ROLE_MAIN;
  int onlineEvents = 0, offlineEvents = 0, configResults = 0, tableSaves = 0; uint8_t lastCfgStatus = 99; std::string lastCfgMsg;
  int cmdOk = 0, cmdNoAnswer = 0;
  MainSession::Callbacks mcb;
  mcb.online = [&](const BoardInfo&, bool on) { (on ? onlineEvents : offlineEvents)++; };
  mcb.configResult = [&](const BoardInfo& b) { configResults++; lastCfgStatus = b.configStatus; lastCfgMsg = b.configMessage; };
  mcb.commandResult = [&](uint8_t, uint16_t, uint8_t st) { if (st == 0) cmdOk++; else if (st == 255) cmdNoAnswer++; };
  mcb.boardsChanged = [&] { tableSaves++; };
  MainSession* mainB = new MainSession(mainLink, mainId, mcb);

  mainLink.onReceive([&](const uint8_t* f, size_t n, int8_t r) { mainB->receive(f, n, r, now); });
  nodeLink.onReceive([&](const uint8_t* f, size_t n, int8_t r) { node.receive(f, n, r, now); });
  auto run = [&](uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 50) { now += 50; node.update(now); mainB->update(now); nodeLink.poll(); mainLink.poll(); }
  };

  // 1. unpaired board shows up as "found"
  run(2500);
  check(mainB->found().size() == 1 && mainB->found()[0].name == "Engine bay" && mainB->found()[0].role == ROLE_SENSOR_RELAY && !node.paired(),
        "unpaired board listed as found");

  // 2. accept it
  int id = mainB->accept(nodeMac, "Engine bay");
  run(1500);
  const BoardInfo* b = mainB->board((uint8_t)id);
  check(id == 1 && node.paired() && storedId == 1 && b && b->online && node.linkUp() && mainB->found().empty() && tableSaves == 1,
        "accepted: board gets id 1, both sides online");

  // 3. configuration over a lossy link (1 in 4 frames lost both ways)
  std::string cfg = "{\"role\":\"sensor+relay\",\"sensors\":[";
  for (int i = 0; i < 25; i++) cfg += std::string(i ? "," : "") + "{\"id\":\"t" + std::to_string(i) + "\",\"type\":\"ds18b20\",\"warn\":100}";
  cfg += "]}";
  mainLink.dropPercent = 25; nodeLink.dropPercent = 25; nodeLink.rnd = 7;
  uint32_t rev = crc32((const uint8_t*)cfg.data(), cfg.size());
  mainB->setConfig(1, rev, std::vector<uint8_t>(cfg.begin(), cfg.end()));
  uint32_t t0 = now;
  while (now - t0 < 20000 && (b->configPending || node.configRev() != rev)) run(100);
  check(!b->configPending && b->ackedRev == rev && applied == cfg && node.configRev() == rev && applyCount == 1,
        "config delivered over a lossy link, applied once", std::to_string(cfg.size()) + " bytes in " + std::to_string((now - t0) / 1000.0).substr(0, 4) + " s");

  // 4. live values reach the main board
  run(2500);
  check(b->points.size() == 3 && b->points[0].value == 92.5f && b->points[0].level == 1 && std::isnan(b->points[2].value) && b->points[2].status == 3 && b->level == 1,
        "live values and alarm level at the main board");

  // 5. a relay command acts exactly once even with losses
  mainLink.dropPercent = nodeLink.dropPercent = 0;
  nodeLink.dropEvery = 2;   // every 2nd answer lost
  int okBefore = cmdOk;
  Command c; c.kind = CMD_RELAY; c.index = 0; c.on = 1;
  mainB->command(1, c);
  run(2000);
  check(relayActs == 1 && cmdOk == okBefore + 1 && cmdNoAnswer == 0, "relay command acts once, answered despite losses",
        "acted " + std::to_string(relayActs) + "x");
  nodeLink.dropEvery = 0;

  // 6. a 3 s gap (main board searching for Wi-Fi) is NOT a lost link
  int downs = linkDowns, offs = offlineEvents;
  mainLink.cut = nodeLink.cut = true; run(3000); mainLink.cut = nodeLink.cut = false; run(2000);
  check(linkDowns == downs && offlineEvents == offs && b->online && node.linkUp(), "3 s gap: no link-lost, relays stay as they are");

  // 7. a 7 s gap IS: relays to their safe state on the board, offline on the main board; both recover
  mainLink.cut = nodeLink.cut = true; run(7000);
  bool lostOk = linkDowns == downs + 1 && offlineEvents == offs + 1 && !b->online && !node.linkUp();
  int pointsBefore = (int)b->pointsAt;
  mainLink.cut = nodeLink.cut = false; run(3000);
  check(lostOk && b->online && node.linkUp() && linkUps >= 2 && (int)b->pointsAt > pointsBefore,
        "7 s gap: link lost on both sides (relay safe state), then recovers");

  // 8. refused configuration comes back with the reason
  refuseNext = true;
  std::string bad = "{\"relays\":[{\"pin\":34}]}";
  uint32_t rev2 = crc32((const uint8_t*)bad.data(), bad.size());
  int results = configResults;
  mainB->setConfig(1, rev2, std::vector<uint8_t>(bad.begin(), bad.end()));
  run(3000);
  check(configResults == results + 1 && lastCfgStatus == CFG_INVALID && lastCfgMsg == "relay on input-only GPIO34" && node.configRev() == rev && b->ackedRev == rev,
        "refused config reported with its reason; board keeps the last good one");
  refuseNext = false;

  // 9. main board restarts: paired table restored from storage, board comes back without re-pairing
  BoardInfo saved = *b;
  delete mainB;
  mainB = new MainSession(mainLink, mainId, mcb);
  mainB->restore(saved);
  run(3000);
  b = mainB->board(1);
  check(b && b->online && b->ackedRev == rev && node.paired() && node.boardId() == 1, "main board restart: board back online, no re-pairing");
  mainB->setConfig(1, rev, std::vector<uint8_t>(cfg.begin(), cfg.end()));   // same settings again
  run(3000);
  check(!b->configPending && applyCount == 1, "same settings after a restart are not applied twice");

  // 10. forget: the board goes back to unpaired and is offered again
  mainB->forget(1);
  run(5000);
  check(!node.paired() && storedId == UNPAIRED && mainB->boards().empty() && mainB->found().size() == 1, "forgotten board unpaired and offered again");

  // 11. commands to a board that is gone are reported as unanswered
  delete mainB;
  mainB = new MainSession(mainLink, mainId, mcb);
  saved.id = 2; mainB->restore(saved);
  int na = cmdNoAnswer;
  mainB->command(2, c);
  run(3000);
  check(cmdNoAnswer == na + 1, "command to an absent board: no answer reported");
  delete mainB;
}
