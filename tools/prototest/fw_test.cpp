// Host-side test of firmware updates over the board-to-board link (BoardNet fw*).
#include "BoardNet.h"
#include <cstring>
#include <string>
#include <vector>

void check(bool ok, const char* what, const std::string& detail = "");
using namespace Proto;
using namespace BoardNet;

namespace {
struct Rig {
  LoopbackLink mainLink, nodeLink;
  MainSession* m = nullptr;
  NodeSession* n = nullptr;
  uint32_t now = 1000;
  std::vector<uint8_t> written;
  bool switched = false, aborted = false, restarted = false;
  uint8_t chip = CHIP_ESP32;
  Rig() {
    LoopbackLink::connect(mainLink, nodeLink);
    Identity mi; mi.role = ROLE_MAIN;
    m = new MainSession(mainLink, mi, MainSession::Callbacks());
    Identity ni; ni.role = ROLE_SENSOR; const uint8_t mac[6] = {1, 2, 3, 4, 5, 6}; memcpy(ni.mac, mac, 6);
    NodeSession::Callbacks cb;
    cb.points = [](Point*, uint8_t) -> uint8_t { return 0; };
    cb.fwBegin = [this](uint32_t size, uint8_t c, const char*, std::string& msg) -> uint8_t {
      if (c != chip) { msg = "image is for another chip"; return FW_WRONG_CHIP; }
      if (size > 1700000) { msg = "too big"; return FW_TOO_BIG; }
      written.clear(); return FW_RECEIVING;
    };
    cb.fwWrite = [this](uint32_t off, const uint8_t* d, size_t len) { if (off != written.size()) return false; written.insert(written.end(), d, d + len); return true; };
    cb.fwEnd = [this](std::string&) -> uint8_t { switched = true; return FW_DONE; };
    cb.fwAbort = [this] { aborted = true; };
    cb.fwDone = [this] { restarted = true; };
    n = new NodeSession(nodeLink, ni, 1, 5, cb);   // already paired as board 1
    BoardInfo b; b.id = 1; memcpy(b.mac, mac, 6); b.name = "Bay";
    m->restore(b);
    mainLink.onReceive([this](const uint8_t* f, size_t l, int8_t r) { m->receive(f, l, r, now); });
    nodeLink.onReceive([this](const uint8_t* f, size_t l, int8_t r) { n->receive(f, l, r, now); });
  }
  ~Rig() { delete m; delete n; }
  void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 2) { now += 2; n->update(now); m->update(now); nodeLink.poll(); mainLink.poll(); } }
};
}

void fwTest() {
  std::vector<uint8_t> img(300000);
  uint32_t x = 12345; for (auto& b : img) { x = x * 1103515245u + 12345u; b = (uint8_t)(x >> 16); }
  img[0] = 0xE9;
  uint32_t crc = crc32(img.data(), img.size());
  auto reader = [&img](uint32_t off, uint8_t* buf, size_t len) { if (off + len > img.size()) return false; memcpy(buf, img.data() + off, len); return true; };

  {   // 300 KB over a link losing 15 % both ways
    Rig r; r.run(3000);
    r.mainLink.dropPercent = 15; r.nodeLink.dropPercent = 15; r.nodeLink.rnd = 99;
    r.m->startFirmware(1, img.size(), crc, CHIP_ESP32, "5.1.0-beta.9", reader);
    uint32_t t0 = r.now;
    while (r.now - t0 < 300000 && r.m->board(1)->fw.state != BoardInfo::Fw::DONE && r.m->board(1)->fw.state != BoardInfo::Fw::FAILED) r.run(100);
    const auto& fw = r.m->board(1)->fw;
    check(fw.state == BoardInfo::Fw::DONE && r.written == img && r.switched && r.restarted, "firmware image delivered intact over a lossy link",
          std::to_string(img.size() / 1024) + " KB in " + std::to_string((r.now - t0) / 1000) + " s (simulated), error: " + fw.error);
  }
  {   // wrong chip refused with the board's reason
    Rig r; r.chip = CHIP_ESP32S3; r.run(3000);
    r.m->startFirmware(1, img.size(), crc, CHIP_ESP32, "x", reader);
    r.run(5000);
    const auto& fw = r.m->board(1)->fw;
    check(fw.state == BoardInfo::Fw::FAILED && fw.error == "image is for another chip" && r.written.empty(), "image for another chip refused", fw.error);
  }
  {   // damaged image (CRC mismatch) is not switched to
    Rig r; r.run(3000);
    r.m->startFirmware(1, img.size(), crc ^ 1, CHIP_ESP32, "x", reader);
    uint32_t t0 = r.now;
    while (r.now - t0 < 120000 && r.m->board(1)->fw.state == BoardInfo::Fw::SENDING) r.run(100);
    r.run(2000);
    const auto& fw = r.m->board(1)->fw;
    check(fw.state == BoardInfo::Fw::FAILED && !r.switched && r.aborted, "damaged image refused, old firmware kept", fw.error);
  }
  {   // board out of reach: transfer gives up instead of hanging
    Rig r; r.run(3000);
    r.mainLink.cut = r.nodeLink.cut = true;
    r.m->startFirmware(1, img.size(), crc, CHIP_ESP32, "x", reader);
    r.run(25000);
    const auto& fw = r.m->board(1)->fw;
    check(fw.state == BoardInfo::Fw::FAILED, "unreachable board: update reported as failed", fw.error);
  }
}
