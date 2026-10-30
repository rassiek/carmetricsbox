// CarMetrics - OLED display. Runs in its own FreeRTOS task so slow I2C frame
// transfers (a 128x128 SSD1327 frame takes ~150 ms) never stall sensor reads.
#pragma once
#include <Arduino.h>

namespace Display {
void start();     // create the display task (once, from setup)
void reconfigure();  // ask the task to re-create the driver after a config change
#ifdef CM_EMU_SELFTEST
void dumpFrame();    // emulator self-test: print the next frame as PBM
#endif
}
