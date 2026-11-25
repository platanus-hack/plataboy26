// ===================================================================================
// Arduboy compatibility layer for the CH32V003 game console
// ===================================================================================
//
// The subset of the Arduboy library (1.1.1) and of OBONO's MyArduboy class that Hopper
// uses (taken from games/hopper for Llegamos tarde), on this console's hardware:
//   display    1 KiB frame buffer in the Arduboy/SSD1306 layout, sent over I2C
//   frames     60 updates per second; the screen is sent every 2nd frame (a full frame
//              takes ~23 ms at 400 kHz), so the game runs at its original speed
//   buttons    joypad (resistor ladder on PC4), A = fire button, B = B button (rev 1.3)
//   sound      Arduboy playtune scores, one channel, played from the SysTick compare
//              interrupt (the buzzer is toggled at the note frequency) - no busy waits
//   (no storage: the game saves nothing)

#pragma once
#include <stdint.h>
#include <stddef.h>

#define WIDTH         128
#define HEIGHT        64
#define BLACK         0
#define WHITE         1

#define UP_BUTTON     0x80
#define RIGHT_BUTTON  0x40
#define LEFT_BUTTON   0x20
#define DOWN_BUTTON   0x10
#define A_BUTTON      0x08
#define B_BUTTON      0x04

#define max(a, b)     (((a) > (b)) ? (a) : (b))
#define min(a, b)     (((a) < (b)) ? (a) : (b))

extern uint8_t AB_buffer[WIDTH * HEIGHT / 8];

void     AB_begin(void);
uint8_t  AB_nextFrame(void);           // waits for the next 1/60 s; 1 = time to draw
void     AB_display(void);
void     AB_clear(void);

uint8_t  AB_buttonDown(uint8_t buttons);
uint8_t  AB_buttonPressed(uint8_t buttons);

void     AB_drawPixel(int16_t x, int16_t y, uint8_t color);
void     AB_drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
void     AB_drawBitmap(int16_t x, int16_t y, const uint8_t *bmp, uint8_t w, uint8_t h, uint8_t color);
void     AB_drawFastVLine2(int16_t x, int16_t y, int8_t h, uint8_t color);
void     AB_drawFastHLine2(int16_t x, int16_t y, uint8_t w, uint8_t color);
void     AB_drawRect2(int16_t x, int16_t y, uint8_t w, int8_t h, uint8_t color);
void     AB_fillRect2(int16_t x, int16_t y, uint8_t w, int8_t h, uint8_t color);


uint8_t  AB_audioEnabled(void);
void     AB_audioOn(void);
void     AB_audioOff(void);
void     AB_playScore2(const uint8_t *score, uint8_t priority);
void     AB_stopScore(void);

uint16_t AB_rand(void);                // 0..32767, like avr-libc rand()
int32_t  AB_random(int32_t n);         // 0..n-1 (0 for n <= 0), like Arduino random(n)
int32_t  AB_random2(int32_t lo, int32_t hi);
void     AB_randomSeed(uint32_t seed);
uint32_t AB_micros(void);
