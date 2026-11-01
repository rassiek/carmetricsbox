#include "CanFrag.h"
#include <string.h>

namespace CanFrag {

std::vector<Frame> split(uint8_t sender, uint8_t tag, const uint8_t* msg, size_t len) {
  std::vector<Frame> out;
  if (!len || len > 0xFFFF) return out;
  size_t pos = 0;
  for (uint8_t idx = 0; pos < len || idx == 0; idx++) {
    Frame f; f.id = BASE_ID + sender;
    f.data[0] = (uint8_t)((tag & 0x0F) << 4); f.data[1] = idx;
    size_t room, at;
    if (idx == 0) { f.data[2] = (uint8_t)len; f.data[3] = (uint8_t)(len >> 8); room = 4; at = 4; }
    else { room = 6; at = 2; }
    size_t n = len - pos < room ? len - pos : room;
    memcpy(f.data + at, msg + pos, n);
    f.len = (uint8_t)(at + n);
    pos += n;
    out.push_back(f);
  }
  return out;
}

bool Assembler::feed(const Frame& f, std::vector<uint8_t>& out) {
  if (f.id < BASE_ID || f.id > BASE_ID + 0xFF || f.len < 2) return false;
  uint8_t sender = (uint8_t)(f.id - BASE_ID), tag = f.data[0] >> 4, idx = f.data[1];
  Slot* s = nullptr;
  for (auto& x : slots) if (x.sender == sender) s = &x;
  if (!s) { slots.push_back(Slot()); s = &slots.back(); s->sender = sender; }
  if (idx == 0) {
    if (s->active) dropped++;                 // previous message never finished
    if (f.len < 4) { s->active = false; return false; }
    s->active = true; s->tag = tag; s->next = 1;
    s->total = (uint16_t)(f.data[2] | (f.data[3] << 8));
    s->buf.assign(f.data + 4, f.data + f.len);
  } else {
    if (!s->active || tag != s->tag || idx != s->next) { if (s->active) dropped++; s->active = false; return false; }
    s->buf.insert(s->buf.end(), f.data + 2, f.data + f.len);
    s->next++;
  }
  if (s->buf.size() > s->total) { s->active = false; dropped++; return false; }
  if (s->buf.size() < s->total) return false;
  s->active = false;
  out = s->buf;
  return true;
}
}  // namespace CanFrag
