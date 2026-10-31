// CarMetrics - in-RAM history for charts (survives page reloads, not reboots)
#pragma once
#include <Arduino.h>

// Samples kept per sensor: 360 x 5 s = 30 min on boards without PSRAM,
// 2880 x 5 s = 4 h on boards with PSRAM (ESP32-S3 N8R8 / N16R8, WROVER).
#define HISTORY_LEN 360
#define HISTORY_LEN_PSRAM 2880

class Print;
namespace History {
void begin();             // clears and sizes for gConfig (call from loop)
void update();            // samples every historyIntervalSeconds
void writeJson(Print& out);  // call with StateLock held
size_t bytes();           // RAM used by the history buffer
size_t count();           // samples stored (call with StateLock held)

// The same JSON in pieces for a chunked HTTP reply, so it never sits in RAM
// as a whole (30 min x 14 sensors is ~30 KB of text; 4 h on PSRAM boards ~250 KB).
class Reader {
 public:
  Reader();                                // call with StateLock held
  size_t read(uint8_t* out, size_t max);   // takes the lock itself; 0 = finished
 private:
  bool next();                             // put the next piece of text in tok
  size_t start, count, sensors, pos = 0, sample = 0;
  uint32_t gen;
  uint8_t phase = 0;                       // 0 header, 1 series name, 2 values, 3 close series, 4 end, 5 done
  char tok[64]; size_t tokLen = 0, tokPos = 0;
};
}
