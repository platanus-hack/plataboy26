// ===================================================================================
// Arduboy compatibility layer for the CH32V003 game console (see arduboy.h)
// ===================================================================================
//
// Drawing functions follow the Arduboy library 1.1.1 / Adafruit GFX and OBONO's
// MyArduboy (MIT License, Copyright (c) 2016-2026 OBONO) closely, so the game draws
// exactly what it draws on an Arduboy.

#include "arduboy.h"
#include "driver.h"
#include "eeprom.h"
#include "sysx.h"                     // UP + DOWN -> menu, low battery (games/common)
#include "hopper_data.h"               // imgFont

#define FRAME_TICKS   (F_CPU / 60)
#define MS_TICKS      (F_CPU / 1000)
#define BUZ_TICKS     (BUZ_PULSE_US * (F_CPU / 1000000))   // buzzer pulse (games/common/buzzer.h)
#define SAVE_BASE     0x1F100UL        // this game's slot in the EEPROM save area
#define SAVE_AUDIO    48               // offset of the sound on/off byte
#define SAVE_SIZE     0xC0             // up to Just One Boss's slot (0x1F1C0)
_Static_assert(SAVE_AUDIO < SAVE_SIZE, "Hopper's sound byte is outside its save slot");

uint8_t AB_buffer[WIDTH * HEIGHT / 8];

static uint32_t nextFrameAt;
static uint8_t  frameCount;
static uint8_t  buttonsNow, buttonsLast;

// ---- display ----------------------------------------------------------------------
void AB_display(void) {
  OLED_setpos(0, 0);
  OLED_data_start();
  for(uint16_t i = 0; i < sizeof(AB_buffer); i++) I2C_write(AB_buffer[i]);
  I2C_stop();
}

void AB_clear(void) {
  for(uint16_t i = 0; i < sizeof(AB_buffer); i++) AB_buffer[i] = 0;
}

// ---- buttons ----------------------------------------------------------------------
// joypad state from SYS_poll (games/common/joycal.h: idle R U UR D DR UD L UL DL none)
static const uint8_t PAD[11] = { 0, RIGHT_BUTTON, UP_BUTTON, UP_BUTTON | RIGHT_BUTTON, DOWN_BUTTON, DOWN_BUTTON | RIGHT_BUTTON, 0, LEFT_BUTTON, UP_BUTTON | LEFT_BUTTON, DOWN_BUTTON | LEFT_BUTTON, 0 };

static uint8_t readButtons(void) {
  uint8_t b = PAD[SYS_poll(ADC_read())]; // joypad, calibrated, diagonals included
  if(JOY_act_pressed()) b |= A_BUTTON;
  else if(JOY_b_pressed()) b |= B_BUTTON;
  return b;
}

uint8_t AB_buttonDown(uint8_t buttons)    { return buttonsNow & ~buttonsLast & buttons; }
uint8_t AB_buttonPressed(uint8_t buttons) { return buttonsNow & buttons; }

// ---- frames -----------------------------------------------------------------------
// 60 updates per second like the Arduboy; returns 1 on every 2nd frame, when the main
// loop draws and sends the screen. A late frame is caught up by the next ones.
uint8_t AB_nextFrame(void) {
  while((int32_t)(STK->CNT - nextFrameAt) < 0);
  nextFrameAt += FRAME_TICKS;
  if((int32_t)(STK->CNT - nextFrameAt) > 4 * FRAME_TICKS) nextFrameAt = STK->CNT;
  buttonsLast = buttonsNow;
  buttonsNow = readButtons();
  return ++frameCount & 1;
}

// ---- sound: Arduboy playtune scores on one channel ----------------------------------
// 0x9t n: note n on, 0x8t: note off, 0xF0: end, 0xE0: from the start,
// two bytes < 0x80: wait (big endian, ms). The SysTick compare interrupt runs at the
// note's half period (or every 1 ms when silent), toggles the buzzer and counts the
// waits down.
static const uint32_t NOTE_HZ16[12] = {        // notes 116..127, Hz * 16
  106318, 112640, 119338, 126434, 133952, 141917, 150356, 159297, 168769, 178805, 189437, 200702
};
static const uint8_t *volatile scorePos;
static const uint8_t *scoreStart;
static volatile uint32_t noteHalf, notePhase; // ticks; noteHalf 0 = silent
static volatile int32_t  waitTicks;
static volatile uint8_t  playing;
static uint8_t audioOn = 1, audioSaved = 0xFF, playPriority;

static uint32_t note_half(uint8_t n) {
  uint8_t k = (uint8_t)((127 - n) / 12);
  uint8_t i = n - 116 + 12 * k;
  return ((uint32_t)F_CPU * 8 / NOTE_HZ16[i]) << k;
}

static void beep_off(void) { PIN_high(PIN_BEEP); }

// next commands of the score, up to the next wait (called from the interrupt)
static void score_step(void) {
  while(1) {
    uint8_t c = *scorePos++;
    if(c & 0x80) {
      switch(c & 0xF0) {
        case 0x90: noteHalf = note_half(*scorePos++ & 0x7F); notePhase = 0; break;
        case 0x80: noteHalf = 0; beep_off(); break;
        case 0xE0: scorePos = scoreStart; break;
        case 0xF0:
        default:   noteHalf = 0; beep_off(); playing = 0;
                   STK->CTLR &= ~STK_CTLR_STIE; return;
      }
    } else {
      waitTicks += (int32_t)(((uint16_t)c << 8) | *scorePos++) * MS_TICKS;
      if(waitTicks > 0) return;
    }
  }
}

void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void) {
  STK->SR = 0;
  uint32_t step = MS_TICKS;
  if(noteHalf) {
    // plataboy26: the pin low for BUZ_PULSE_US, high the rest of the period (same pitch,
    // far less current from the coin cell: games/common/buzzer.h). Every note's half
    // period is longer than that (the scores stay below 1.6 kHz).
    uint32_t left = ((GPIOA->OUTDR & 2) ? 2 * noteHalf - BUZ_TICKS : BUZ_TICKS) - notePhase;
    if((int32_t)left <= (int32_t)step) { step = left; notePhase = 0; PIN_toggle(PIN_BEEP); }
    else notePhase += step;
  }
  STK->CMP += step;
  waitTicks -= (int32_t)step;
  if(waitTicks <= 0 && playing) score_step();
}

void AB_stopScore(void) {
  STK->CTLR &= ~STK_CTLR_STIE;
  playing = 0; noteHalf = 0;
  beep_off();
}

void AB_playScore2(const uint8_t *score, uint8_t priority) {
  if(!audioOn) return;
  if(playing) {
    if(priority > playPriority) return;
    AB_stopScore();
  }
  playPriority = priority;
  scoreStart = score; scorePos = score;
  waitTicks = 0; noteHalf = 0; notePhase = 0;
  playing = 1;
  score_step();                         // up to the first wait
  if(!playing) return;
  STK->SR = 0;
  STK->CMP = STK->CNT + 64;
  STK->CTLR |= STK_CTLR_STIE;
}

uint8_t AB_audioEnabled(void) { return audioOn; }
void AB_audioOn(void)  { audioOn = 1; }
void AB_audioOff(void) { audioOn = 0; AB_stopScore(); }
void AB_audioSaveOnOff(void) {
  if(audioSaved != audioOn) { audioSaved = audioOn; AB_saveWrite(SAVE_AUDIO, &audioSaved, 1); }
}

// ---- random numbers ---------------------------------------------------------------
static uint32_t rngState = 0x2545F491;

static uint32_t rng(void) {                   // xorshift32
  uint32_t x = rngState;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return rngState = x;
}
uint16_t AB_rand(void) { return (uint16_t)(rng() >> 17); }
int32_t AB_random(int32_t n) { return n > 0 ? (int32_t)((rng() >> 1) % (uint32_t)n) : 0; }
int32_t AB_random2(int32_t lo, int32_t hi) { return hi > lo ? lo + AB_random(hi - lo) : lo; }
void AB_randomSeed(uint32_t seed) { if(seed) rngState = seed; }
uint32_t AB_micros(void) { return STK->CNT / (F_CPU / 1000000); }

// ---- storage ----------------------------------------------------------------------
uint8_t AB_saveRead(uint16_t off, void *buf, uint16_t len) {
  return EE_read(SAVE_BASE + off, (uint8_t *)buf, len);
}
uint8_t AB_saveWrite(uint16_t off, const void *buf, uint16_t len) {
  return EE_write(SAVE_BASE + off, (const uint8_t *)buf, len);
}

// ---- init -------------------------------------------------------------------------
void AB_begin(void) {
  JOY_init();
  EE_read(JOYCAL_EE, SYS_cal, JOYCAL_N);   // joypad thresholds, measured by the menu (sysx.h)
  uint8_t a;
  if(AB_saveRead(SAVE_AUDIO, &a, 1) == EE_OK && a <= 1) audioOn = audioSaved = a;
  PFIC->IENR[0] = 1u << 12;              // SysTick interrupt (used only while a score plays)
  asm volatile("csrsi mstatus, 0x8");
  nextFrameAt = STK->CNT;
  rngState ^= STK->CNT;
}

// ---- drawing ----------------------------------------------------------------------
void AB_drawPixel(int16_t x, int16_t y, uint8_t color) {
  if(x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  uint8_t *p = &AB_buffer[(y >> 3) * WIDTH + x];
  if(color) *p |= 1 << (y & 7); else *p &= ~(1 << (y & 7));
}

void AB_drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
  int16_t steep = abs(y1 - y0) > abs(x1 - x0), t;
  if(steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
  if(x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  int16_t dx = x1 - x0, dy = abs(y1 - y0), err = dx / 2, ystep = y0 < y1 ? 1 : -1;
  for(; x0 <= x1; x0++) {
    if(steep) AB_drawPixel(y0, x0, color); else AB_drawPixel(x0, y0, color);
    err -= dy;
    if(err < 0) { y0 += ystep; err += dx; }
  }
}

void AB_drawBitmap(int16_t x, int16_t y, const uint8_t *bmp, uint8_t w, uint8_t h, uint8_t color) {
  if(x + w < 0 || x > WIDTH - 1 || y + h < 0 || y > HEIGHT - 1) return;
  int16_t yOffset = abs(y) % 8, sRow = y / 8;
  if(y < 0) { sRow--; yOffset = 8 - yOffset; }
  int16_t rows = h / 8 + (h % 8 != 0);
  for(int16_t a = 0; a < rows; a++) {
    int16_t bRow = sRow + a;
    if(bRow > HEIGHT / 8 - 1) break;
    if(bRow <= -2) continue;
    for(int16_t c = 0; c < w; c++) {
      if(c + x > WIDTH - 1) break;
      if(c + x < 0) continue;
      uint8_t d = bmp[a * w + c];
      if(bRow >= 0) {
        uint8_t *p = &AB_buffer[bRow * WIDTH + x + c];
        if(color) *p |= d << yOffset; else *p &= ~(uint8_t)(d << yOffset);
      }
      if(yOffset && bRow < HEIGHT / 8 - 1) {
        uint8_t *p = &AB_buffer[(bRow + 1) * WIDTH + x + c];
        if(color) *p |= d >> (8 - yOffset); else *p &= ~(uint8_t)(d >> (8 - yOffset));
      }
    }
  }
}

static void fillBelt(uint8_t *p, uint8_t d, uint8_t w, uint8_t color) {
  if(color) for(; w > 0; w--) *p++ |= d;
  else { d = ~d; for(; w > 0; w--) *p++ &= d; }
}

void AB_drawFastVLine2(int16_t x, int16_t y, int8_t h, uint8_t color) {
  if(y < 0) { if(h <= -y) return; h += y; y = 0; }
  if(h <= 0 || y >= HEIGHT || x < 0 || x >= WIDTH) return;
  if(y + h > HEIGHT) h = HEIGHT - y;
  uint8_t yOdd = y & 7, d = 0xFF << yOdd;
  y -= yOdd; h += yOdd;
  for(uint8_t *p = AB_buffer + x + (y / 8) * WIDTH; h > 0; h -= 8, p += WIDTH) {
    if(h < 8) d &= 0xFF >> (8 - h);
    if(color) *p |= d; else *p &= ~d;
    d = 0xFF;
  }
}

void AB_drawFastHLine2(int16_t x, int16_t y, uint8_t w, uint8_t color) {
  if(x < 0) { if(w <= -x) return; w += x; x = 0; }
  if(w == 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  if(x + w > WIDTH) w = WIDTH - x;
  fillBelt(AB_buffer + x + (y / 8) * WIDTH, 1 << (y & 7), w, color);
}

void AB_drawRect2(int16_t x, int16_t y, uint8_t w, int8_t h, uint8_t color) {
  AB_drawFastHLine2(x, y, w, color);
  AB_drawFastHLine2(x, y + h - 1, w, color);
  AB_drawFastVLine2(x, y + 1, h - 2, color);
  AB_drawFastVLine2(x + w - 1, y + 1, h - 2, color);
}

void AB_fillRect2(int16_t x, int16_t y, uint8_t w, int8_t h, uint8_t color) {
  if(x < 0) { if(w <= -x) return; w += x; x = 0; }
  if(y < 0) { if(h <= -y) return; h += y; y = 0; }
  if(w == 0 || x >= WIDTH || h <= 0 || y >= HEIGHT) return;
  if(x + w > WIDTH) w = WIDTH - x;
  if(y + h > HEIGHT) h = HEIGHT - y;
  uint8_t yOdd = y & 7, d = 0xFF << yOdd;
  y -= yOdd; h += yOdd;
  for(uint8_t *p = AB_buffer + x + (y / 8) * WIDTH; h > 0; h -= 8, p += WIDTH) {
    if(h < 8) d &= 0xFF >> (8 - h);
    fillBelt(p, d, w, color);
    d = 0xFF;
  }
}

// ---- text: OBONO's 6x6 font, characters ' '..'_', white on black --------------------
static int16_t cursorX, cursorY;

void AB_setCursor(int16_t x, int16_t y) { cursorX = x; cursorY = y; }

static void drawChar(int16_t x, int16_t y, uint8_t c) {
  if(x >= WIDTH || y >= HEIGHT || x + 5 < 0 || y + 6 < 0) return;
  uint32_t ptn = imgFont[c - ' '];
  for(int8_t i = 0; i < 6; i++)
    for(int8_t j = 0; j < 6; j++, ptn >>= 1) AB_drawPixel(x + i, y + j, ptn & 1);
}

static void write(uint8_t c) {
  if(c == '\n') { cursorY += 6; cursorX = 0; }
  else if(c >= ' ' && c <= '_') {
    drawChar(cursorX, cursorY, c);
    cursorX += 6;
    if(cursorX > WIDTH - 6) write('\n');
  }
}

size_t AB_print(const char *s) {
  size_t n = 0;
  for(; *s; s++, n++) write((uint8_t)*s);
  return n;
}

size_t AB_printU(uint32_t v) {
  char buf[11], *p = buf + 10;
  *p = 0;
  do { *--p = '0' + v % 10; v /= 10; } while(v);
  return AB_print(p);
}

size_t AB_printEx(int16_t x, int16_t y, const char *s) {
  AB_setCursor(x, y);
  return AB_print(s);
}
