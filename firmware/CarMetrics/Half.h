// CarMetrics - 16-bit floats (IEEE 754 half precision) for large in-RAM buffers.
//
// About 3 significant digits: 14.53 V -> 14.531, 486.3 °C -> 486.25,
// 1661 m -> 1661, 4123 rpm -> 4124. Plenty for charts, half the RAM of float.
// NaN (no value) and +-65504 range are kept. Plain C++, also tested on a PC.
#pragma once
#include <stdint.h>
#include <string.h>

namespace Half {

inline uint16_t fromFloat(float f) {
  uint32_t x; memcpy(&x, &f, 4);
  uint16_t sign = (x >> 16) & 0x8000;
  uint32_t exp = (x >> 23) & 0xFF, man = x & 0x7FFFFF;
  if (exp == 0xFF) return sign | 0x7C00 | (man ? 0x200 : 0);          // NaN / infinity
  int32_t e = (int32_t)exp - 127 + 15;
  if (e >= 0x1F) return sign | 0x7C00;                                  // too big -> infinity
  if (e <= 0) {                                                         // subnormal or zero
    if (e < -10) return sign;
    man |= 0x800000;
    uint32_t shift = 14 - e;
    uint32_t h = man >> shift;
    if ((man >> (shift - 1)) & 1) h++;                                  // round half up
    return sign | (uint16_t)h;
  }
  uint16_t h = sign | (uint16_t)(e << 10) | (uint16_t)(man >> 13);
  if (man & 0x1000) h++;                                                // round half up (may carry into the exponent: correct)
  return h;
}

inline float toFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF, x;
  if (exp == 0) {
    if (!man) x = sign;
    else {                                                              // subnormal: normalise
      exp = 127 - 15 + 1;
      while (!(man & 0x400)) { man <<= 1; exp--; }
      x = sign | (exp << 23) | ((man & 0x3FF) << 13);
    }
  } else if (exp == 0x1F) x = sign | 0x7F800000 | (man << 13);          // NaN / infinity
  else x = sign | ((exp - 15 + 127) << 23) | (man << 13);
  float f; memcpy(&f, &x, 4);
  return f;
}

constexpr uint16_t NaN = 0x7E00;
}  // namespace Half
