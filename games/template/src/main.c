// ===================================================================================
// Project:   Catch - starter game for the CH32V003 Mini Game Console
// ===================================================================================
//
// A minimal but complete game to copy from: frame-timed main loop, full 1 KiB frame
// buffer, sprites, text, joypad, fire button and buzzer.
//
// Move the basket with Left/Right and catch the falling stars. Three misses and the
// game is over. Fire button starts a new game.
//
// Notes for real hardware:
// - 2 KiB of SRAM in total: the frame buffer takes 1 KiB, keep the rest small.
// - No hardware multiplier/divider (RV32EC): prefer shifts and small tables.
// - Sending a full frame over I2C @ 400 kHz takes ~23 ms, so 30 fps is the ceiling
//   for full-screen redraws (the simulator models this timing).
// - JOY_sound() busy-waits: long sounds stall the game, keep them short.

#include "driver.h"

#define FRAME_TIME  (33 * DLY_MS_TIME)        // ~30 frames per second
#define MAX_STARS   4

// -----------------------------------------------------------------------------------
// Frame buffer (128 x 64 pixels, 8 pages of 128 bytes, LSB = top pixel)
// -----------------------------------------------------------------------------------
static uint8_t fb[128 * 8];

static void FB_clear(void) {
  for(uint16_t i = 0; i < sizeof(fb); i++) fb[i] = 0;
}

static void FB_pixel(int16_t x, int16_t y) {
  if(x < 0 || x > 127 || y < 0 || y > 63) return;
  fb[(y >> 3) * 128 + x] |= 1 << (y & 7);
}

static void FB_rect(int16_t x, int16_t y, int16_t w, int16_t h) {
  for(int16_t j = 0; j < h; j++)
    for(int16_t i = 0; i < w; i++) FB_pixel(x + i, y + j);
}

// 8x8 sprite, column bytes, LSB = top
static void FB_sprite(int16_t x, int16_t y, const uint8_t *spr) {
  for(uint8_t i = 0; i < 8; i++)
    for(uint8_t j = 0; j < 8; j++)
      if(spr[i] & (1 << j)) FB_pixel(x + i, y + j);
}

static void FB_show(void) {
  OLED_setpos(0, 0);
  OLED_data_start();
  for(uint16_t i = 0; i < sizeof(fb); i++) I2C_write(fb[i]);
  I2C_stop();
}

// 3x5 font for digits and a few capitals (column bytes, LSB = top)
static const uint8_t FONT[][3] = {
  {0x1F,0x11,0x1F},{0x00,0x1F,0x00},{0x1D,0x15,0x17},{0x15,0x15,0x1F},{0x07,0x04,0x1F}, // 0-4
  {0x17,0x15,0x1D},{0x1F,0x15,0x1D},{0x01,0x01,0x1F},{0x1F,0x15,0x1F},{0x17,0x15,0x1F}, // 5-9
  {0x1E,0x05,0x1E},{0x1F,0x11,0x11},{0x1F,0x15,0x11},{0x1F,0x11,0x1D},{0x1F,0x02,0x1F}, // A C E G M
  {0x1F,0x11,0x1F},{0x1F,0x05,0x1A},{0x17,0x15,0x1D},{0x01,0x1F,0x01},{0x0F,0x10,0x0F}, // O R S T V
  {0x1F,0x10,0x10},{0x1F,0x05,0x07},{0x03,0x1C,0x03},{0x1F,0x04,0x1F},{0x1F,0x05,0x01}, // L P Y H F
  {0x11,0x1F,0x11},{0x00,0x00,0x00},                                                     // I space
};
static uint8_t FONT_index(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  switch(c) {
    case 'A': return 10; case 'C': return 11; case 'E': return 12; case 'G': return 13;
    case 'M': return 14; case 'O': return 15; case 'R': return 16; case 'S': return 17;
    case 'T': return 18; case 'V': return 19; case 'L': return 20; case 'P': return 21;
    case 'Y': return 22; case 'H': return 23; case 'F': return 24; case 'I': return 25;
    default:  return 26;
  }
}

static void FB_text(int16_t x, int16_t y, const char *s, uint8_t scale) {
  for(; *s; s++, x += 4 * scale) {
    const uint8_t *g = FONT[FONT_index(*s)];
    for(uint8_t i = 0; i < 3; i++)
      for(uint8_t j = 0; j < 5; j++)
        if(g[i] & (1 << j)) FB_rect(x + i * scale, y + j * scale, scale, scale);
  }
}

static void FB_number(int16_t x, int16_t y, uint16_t n) {
  char buf[6]; int8_t i = 5;
  buf[5] = 0;
  do { buf[--i] = '0' + n % 10; n /= 10; } while(n && i);
  FB_text(x, y, buf + i, 1);
}

// -----------------------------------------------------------------------------------
// Game
// -----------------------------------------------------------------------------------
static const uint8_t SPR_STAR[8]   = {0x08,0x18,0x7C,0x3F,0x7C,0x18,0x08,0x00};
static const uint8_t SPR_BASKET[8] = {0x07,0x0F,0x1C,0x18,0x18,0x1C,0x0F,0x07};

typedef struct { int16_t x, y; uint8_t active, speed; } Star;

static Star    stars[MAX_STARS];
static int16_t player;            // basket x position (pixels)
static uint16_t score;
static uint8_t misses, spawn;

static void Game_init(void) {
  for(uint8_t i = 0; i < MAX_STARS; i++) stars[i].active = 0;
  player = 60; score = 0; misses = 0; spawn = 20;
}

static void Game_update(void) {
  if(JOY_left_pressed()  && player > 0)   player -= 3;
  if(JOY_right_pressed() && player < 120) player += 3;

  if(!--spawn) {                                    // spawn a new star
    spawn = 25 - (score > 40 ? 20 : score >> 1);
    for(uint8_t i = 0; i < MAX_STARS; i++) if(!stars[i].active) {
      stars[i].active = 1;
      stars[i].x = JOY_random() % 120;
      stars[i].y = -8;
      stars[i].speed = 1 + (score >> 4) + (JOY_random() & 1);
      break;
    }
  }

  for(uint8_t i = 0; i < MAX_STARS; i++) {
    Star *s = &stars[i];
    if(!s->active) continue;
    s->y += s->speed;
    if(s->y > 48 && s->y < 58 && s->x > player - 7 && s->x < player + 7) {
      s->active = 0; score++;
      JOY_sound(200, 8);                            // catch: short high beep
    } else if(s->y > 63) {
      s->active = 0; misses++;
      JOY_sound(40, 20);                            // miss: low buzz
    }
  }
}

static void Game_draw(void) {
  FB_clear();
  FB_number(1, 1, score);
  for(uint8_t i = 0; i < 3; i++) if(i >= misses) FB_rect(116 + i * 4, 1, 3, 3);
  for(uint8_t i = 0; i < MAX_STARS; i++) if(stars[i].active) FB_sprite(stars[i].x, stars[i].y, SPR_STAR);
  FB_sprite(player, 54, SPR_BASKET);
  FB_rect(0, 63, 128, 1);
  FB_show();
}

static void WaitFrame(uint32_t start) {
  while((int32_t)(STK->CNT - start) < FRAME_TIME);
}

int main(void) {
  JOY_init();
  while(1) {
    // title screen: wait for fire button
    FB_clear();
    FB_text(28, 14, "CATCH", 3);
    FB_text(30, 46, "PRESS FIRE", 1);
    FB_show();
    while(JOY_act_released()) JOY_random();          // seed RNG with the press time
    while(JOY_act_pressed());

    Game_init();
    while(misses < 3) {
      uint32_t start = STK->CNT;
      Game_update();
      Game_draw();
      WaitFrame(start);
    }

    FB_clear();
    FB_text(28, 18, "GAME OVER", 2);
    FB_text(46, 40, "SCORE", 1);
    FB_number(70, 40, score);
    FB_show();
    JOY_sound(80, 60); JOY_sound(60, 80);
    DLY_ms(1000);
  }
}
