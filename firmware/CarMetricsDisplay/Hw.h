// CarMetrics Display - the screen hardware (one firmware build per board):
//
//  JC3248W535  ESP32-S3 (16 MB flash, 8 MB PSRAM), 3.5" 320x480 IPS, AXS15231B
//              over QSPI (CS 45, SCK 47, D0-D3 21/48/40/39), backlight GPIO 1,
//              capacitive touch in the same chip on I2C (SDA 4, SCL 8, 0x3B).
//              Whole screen drawn in a PSRAM canvas, then sent in one go.
//  CYD         "Cheap Yellow Display" ESP32-2432S028R: classic ESP32, 2.8"
//              320x240, ILI9341 (or ST7789 on the 2-USB version) on SPI
//              (DC 2, CS 15, SCK 14, MOSI 13, MISO 12), backlight GPIO 21,
//              resistive touch XPT2046 (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36).
//              No PSRAM: each changed tile is drawn in a small canvas.
#pragma once
#include "Ui.h"

namespace Hw {
struct TouchCal {                 // raw touch -> screen (CYD resistive panel; JC needs only the flips)
  int xMin = 300, xMax = 3800, yMin = 300, yMax = 3800;
  bool swapXY = false, flipX = false, flipY = false;
};
bool begin(uint8_t rotation, const char* panel, const TouchCal& cal);
Disp::Surface& surface();
void setBrightness(int pct);          // 5..100
void setRotation(uint8_t r);
uint8_t nextRotation(uint8_t r);      // JC: 0 portrait, 1 landscape, 2, 3; CYD: 1 landscape, 3 flipped
uint8_t defaultRotation();
bool readTouch(int& x, int& y);       // screen coordinates; false = not touched
bool readTouchRaw(int& x, int& y);    // for calibration (web page)
const char* profile();                // "JC3248W535" / "CYD ILI9341" / "CYD ST7789"
const char* chipName();
}
