// Arduboy2 library subset (Arduboy2Base, Sprites, Rect/Point) for the CH32V003 game
// console. Same buffer layout and sprite formats as the Arduboy2 library (BSD 3-Clause,
// Copyright (c) 2016-2018 Scott Allen, Chris Martinez and contributors).
#pragma once
#include "Arduino.h"
#include "EEPROM.h"

#define WIDTH   128
#define HEIGHT  64
#define BLACK   0
#define WHITE   1
#define INVERT  2

#define LEFT_BUTTON   0x20
#define RIGHT_BUTTON  0x40
#define UP_BUTTON     0x80
#define DOWN_BUTTON   0x10
#define A_BUTTON      0x08
#define B_BUTTON      0x04

struct Rect  { int16_t x, y; uint8_t width, height; };
struct Point { int16_t x, y; };

struct Arduboy2Audio {
  static void begin();
  static void on();
  static void off();
  static void toggle() { if(enabled()) off(); else on(); }
  // the Arduboy's system byte 1 (bit 7: sound on): here it holds nothing else, so it is
  // written whole when the game saves the setting (on a toggle)
  static void saveOnOff() { EEPROM.write(1, enabled() ? 0xFF : 0x7F); }
  static bool enabled();
};

struct Arduboy2Base {
  Arduboy2Audio audio;
  uint16_t frameCount;
  uint8_t currentButtonState, previousButtonState;

  void boot();
  void mainNoUSB();
  void setFrameRate(uint8_t rate) { (void)rate; }   // fixed: 60 updates per second
  bool nextFrame();
  void pollButtons() { previousButtonState = currentButtonState; currentButtonState = buttonsState(); }
  uint8_t buttonsState();
  bool pressed(uint8_t b)     { return (buttonsState() & b) == b; }
  bool justPressed(uint8_t b) { return !(previousButtonState & b) && (currentButtonState & b); }
  bool justReleased(uint8_t b){ return (previousButtonState & b) && !(currentButtonState & b); }
  bool everyXFrames(uint8_t n){ return frameCount % n == 0; }
  void fillScreen(uint8_t color);
  void clear() { fillScreen(BLACK); }
  void display();
  uint8_t *getBuffer();
  void exitToBootloader();
  static bool collide(Rect a, Rect b) {
    return !(b.x >= a.x + a.width || b.x + b.width <= a.x || b.y >= a.y + a.height || b.y + b.height <= a.y);
  }
  static bool collide(Point p, Rect r) {
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
  }
};

// the port's tile map draw (one display-list entry for drawTiles' 9 x 5 tiles)
void dl_tiles(int16_t x, int16_t y, const uint8_t *blocks, const uint8_t *index, const uint8_t *ids);

struct Sprites {
  static void drawOverwrite(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);
  static void drawErase(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);
  static void drawSelfMasked(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);
  static void drawPlusMask(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t frame);
};
