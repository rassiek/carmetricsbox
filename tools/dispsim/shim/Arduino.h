// Minimal Arduino API so Arduino_GFX's drawing core builds on a PC (screen simulator).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include "Print.h"
#define PROGMEM
#define F(s) ((const __FlashStringHelper*)(s))
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_dword(a) (*(const uint32_t*)(a))
#define pgm_read_ptr(a) (*(void* const*)(a))
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
typedef uint8_t byte;
typedef bool boolean;
inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
#ifndef min
using std::min; using std::max;
#endif
class String : public std::string {
 public:
  String() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
};
