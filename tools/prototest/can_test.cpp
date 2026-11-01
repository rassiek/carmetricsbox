// Host-side test of the CAN framing (firmware/CarMetrics/CanFrag.*).
#include "CanFrag.h"
#include <string>
#include <vector>
void check(bool ok, const char* what, const std::string& detail = "");

void canTest() {
  std::vector<uint8_t> a(250), b(37), c(1);
  for (size_t i = 0; i < a.size(); i++) a[i] = (uint8_t)(i * 3);
  for (size_t i = 0; i < b.size(); i++) b[i] = (uint8_t)(200 - i);
  c[0] = 'C';
  auto fa = CanFrag::split(0x12, 1, a.data(), a.size());
  auto fb = CanFrag::split(0x34, 7, b.data(), b.size());
  auto fc = CanFrag::split(0x56, 2, c.data(), c.size());
  bool sizes = fa.size() == 42 && fb.size() == 7 && fc.size() == 1;
  for (auto& f : fa) sizes = sizes && f.len <= 8 && f.id == 0x112;
  // interleave three senders on one bus, as CAN arbitration would
  std::vector<CanFrag::Frame> bus;
  for (size_t i = 0; i < 42; i++) { bus.push_back(fa[i]); if (i < fb.size()) bus.push_back(fb[i]); if (i == 3) bus.push_back(fc[0]); }
  CanFrag::Assembler as; std::vector<uint8_t> out; std::vector<std::vector<uint8_t>> got;
  for (auto& f : bus) if (as.feed(f, out)) got.push_back(out);
  bool ok = got.size() == 3 && got[0] == c && got[1] == b && got[2] == a;
  check(sizes && ok, "CAN: messages split into 8-byte frames and rebuilt, three boards interleaved", std::to_string(fa.size()) + " frames for 250 bytes");
  // a lost fragment drops only that message; the next one gets through
  CanFrag::Assembler as2; got.clear();
  auto fa2 = CanFrag::split(0x12, 2, a.data(), a.size());
  for (size_t i = 0; i < fa.size(); i++) if (i != 10 && as2.feed(fa[i], out)) got.push_back(out);
  for (auto& f : fa2) if (as2.feed(f, out)) got.push_back(out);
  check(got.size() == 1 && got[0] == a && as2.dropped >= 1, "CAN: a lost fragment drops that message only");
}
