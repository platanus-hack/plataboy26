// ===================================================================================
// Arduboy2 / ATMlib / EEPROM / Arduino compatibility for the CH32V003 game console
// ===================================================================================
//
// Display  1 KiB frame buffer (Arduboy layout). The game runs 60 frames per second
//          like on the Arduboy; display() sends every 2nd frame (~23 ms per full
//          frame at 400 kHz I2C), so the game keeps its speed at 30 fps on screen.
// Buttons  joypad (resistor ladder, PC4) with diagonals, A = fire button, B = B button
// Sound    ATMlib songs: the 4-channel play routine runs at the song's tick rate (from
//          the main loop, songs can be far data); the loudest tone channel is played on
//          the buzzer, a square wave from the SysTick compare interrupt
// EEPROM   the Arduboy's 1 KiB EEPROM = this game's slot in the AT24CM01 save area
// Far data read-only data moved out of flash into the EEPROM (far.h), read through a
//          small cache of 32-byte lines

#include "Arduboy2.h"
#include "ATMlib.h"

extern "C" {
#include "driver.h"
#include "at24.h"
#include "../../common/sysx.h"
}

#define SAVE_SLOT    0x1F200UL         // 1 KiB: Arduboy EEPROM addresses 0..1023
#define FAR_EEPROM   ARDU_FAR_EEPROM   // EEPROM address of far data (set by the build)
#define FRAME_TICKS  (F_CPU / 60)
#define MS_TICKS     (F_CPU / 1000)
#define AUDIO_SR     31250             // sound samples per second (SysTick rate)

SerialStub Serial;
EEPROMClass EEPROM;

// ---- debug output (simulator: USART1 data register writes are captured) --------------
#ifdef ARDU_DEBUG
static void dbg_hex(const char *tag, uint32_t v) {
  volatile uint32_t *dr = (volatile uint32_t *)0x40013804;
  while(*tag) *dr = *tag++;
  for(int i = 28; i >= 0; i -= 4) *dr = "0123456789abcdef"[(v >> i) & 15];
  *dr = '\n';
}
#else
#define dbg_hex(t, v)
#endif

// ---- far data cache ---------------------------------------------------------------------
// Fully associative, 4-byte lines, kept in most-recently-used order. Chosen from traces
// of the game's far reads: a frame touches ~15 scattered places (text index, words,
// map), which this holds (5193 -> ~110 EEPROM reads in 200 frames, vs 16-byte lines).
#ifndef FAR_LINES
#define FAR_LINES  24
#endif
static uint16_t farTag[FAR_LINES];        // line number + 1 (0 = empty)
static uint32_t farData[FAR_LINES];
uint32_t far_misses;

extern "C" __attribute__((noinline)) uint8_t far_read_byte(uint32_t addr) {
#ifdef ARDU_TRACE
  { volatile uint32_t *dr = (volatile uint32_t *)0x40013804; *dr = 'a';
    for(int i = 12; i >= 0; i -= 4) *dr = "0123456789abcdef"[(addr >> i) & 15]; }
#endif
  uint32_t off = addr - FAR_BASE;
  uint16_t line = (uint16_t)(off >> 2) + 1;
  uint8_t sh = (off & 3) * 8;
  if(farTag[0] == line) return farData[0] >> sh;
  uint8_t i = 1;
  while(i < FAR_LINES && farTag[i] != line) i++;
  uint32_t d;
  if(i == FAR_LINES) {                    // miss: the least recently used line goes
    i = FAR_LINES - 1;
    EE_read(FAR_EEPROM + (off & ~3UL), (uint8_t *)&d, 4);
    far_misses++;
  } else d = farData[i];
  for(; i; i--) { farTag[i] = farTag[i - 1]; farData[i] = farData[i - 1]; }
  farTag[0] = line; farData[0] = d;
  return d >> sh;
}

// ---- Arduino ------------------------------------------------------------------------
static uint32_t rngState = 0x2545F491;
static uint32_t rng() { uint32_t x = rngState; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rngState = x; }
long random(long howbig) { return howbig > 0 ? (long)((rng() >> 1) % (uint32_t)howbig) : 0; }
long random(long howsmall, long howbig) { return howbig > howsmall ? howsmall + random(howbig - howsmall) : howsmall; }
void randomSeed(unsigned long seed) { if(seed) rngState = seed; }
char *itoa(int value, char *s, int radix) {       // the game only uses radix 10
  (void)radix;
  char t[11], *p = t + 10, *q = s;
  unsigned u = value < 0 ? -value : value;
  *p = 0;
  do { *--p = '0' + u % 10; u /= 10; } while(u);
  if(value < 0) *q++ = '-';
  while((*q++ = *p++));
  return s;
}
static volatile uint32_t msCount;
unsigned long millis(void) { return msCount; }

// ---- EEPROM -------------------------------------------------------------------------
uint8_t EEPROMClass::read(int addr) { uint8_t v[4] = { 0xFF }; EE_read(SAVE_SLOT + (addr & 1023), v, 4); return v[0]; }
void EEPROMClass::write(int addr, uint8_t v) { EE_write(SAVE_SLOT + (addr & 1023), &v, 1); }
void EEPROMClass::readBlock(int addr, void *p, int n) { EE_read(SAVE_SLOT + (addr & 1023), (uint8_t *)p, n); }
void EEPROMClass::writeBlock(int addr, const void *p, int n) { EE_write(SAVE_SLOT + (addr & 1023), (const uint8_t *)p, n); }

// ---- audio on/off (Arduboy system EEPROM byte 1, bit 7) ------------------------------
static bool audioOn = true;
void Arduboy2Audio::begin() { uint8_t v = EEPROM.read(1); audioOn = (v == 0xFF) || (v & 0x80); }
void Arduboy2Audio::on()  { audioOn = true; }
bool Arduboy2Audio::enabled() { return audioOn; }

// ---- buttons ------------------------------------------------------------------------
// joypad state from SYS_poll (games/common/joycal.h: idle R U UR D DR UD L UL DL none)
static const uint8_t PAD[11] = { 0, RIGHT_BUTTON, UP_BUTTON, UP_BUTTON | RIGHT_BUTTON, DOWN_BUTTON, DOWN_BUTTON | RIGHT_BUTTON, 0, LEFT_BUTTON, UP_BUTTON | LEFT_BUTTON, DOWN_BUTTON | LEFT_BUTTON, 0 };

uint8_t Arduboy2Base::buttonsState() {
  uint16_t v = ADC_read();
  uint8_t b = PAD[SYS_poll(v)];          // calibrated; UP + DOWN held 2 s: back to the menu
#ifdef ARDU_BTNCHECK
  if(v > 10) dbg_hex("pad adc ", (uint32_t)msCount << 12 | v);
#endif
  // Arduventure confirms with the Arduboy's B and goes back with A: the console's main
  // (fire) button is B here, the second button (rev. 1.3) is A
  if(JOY_act_pressed()) b |= B_BUTTON;
  else if(JOY_b_pressed()) b |= A_BUTTON;
  return b;
}

// ---- frames and display -------------------------------------------------------------
static void atm_service(void);
static uint8_t dlOverflow;                // display list statistics (see below)
#ifdef ARDU_DEBUG
static uint16_t dlMax;
#define DL_MAX() if(dlLen > dlMax) dlMax = dlLen
#else
#define DL_MAX()                          // only printed by debug builds: saves flash
#endif
#ifdef DL_VERIFY
static uint32_t verifyPages, verifyBad;
#endif
static uint32_t nextFrameAt;

void Arduboy2Base::boot() {
  // JOY_init() without the buzzer pin (set up below for the PWM): saves flash
  PIN_input_AN(PIN_PAD);
  PIN_input_PU(PIN_ACT);
  OLED_init();
  ADC_init();
  ADC1->SAMPTR2 = 7 | 7 << 6;               // PA2 and PC4 (joypad): longest sample time (driver.h)
  ADC_input(PIN_PAD);
  EE_read(JOYCAL_EE, SYS_cal, JOYCAL_N);   // joypad thresholds, measured by the menu (sysx.h)
  // sound: TIM1 channel 2 PWM on PA1 (alternate function push-pull), 48 MHz / 256
  RCC->APB2PCENR |= RCC_TIM1EN;
  GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFu << 4)) | (0xBu << 4);
  TIM1->ATRLR = 255;
  TIM1->CH2CVR = 256;                       // pin high all the time: silent
  TIM1->CHCTLR1 = 0x6000;                   // OC2M: PWM mode 1
  TIM1->CCER = 1u << 4;                     // CC2E
  TIM1->BDTR = 0x8000;                      // MOE
  TIM1->CTLR1 = 1;                          // CEN
  PFIC->IENR[0] = 1u << 12;             // SysTick interrupt: sound and millis()
  STK->CMP = STK->CNT + F_CPU / AUDIO_SR;
  STK->CTLR |= STK_CTLR_STIE;
  asm volatile("csrsi mstatus, 0x8");
  nextFrameAt = STK->CNT;
  rngState ^= STK->CNT;
}

#ifdef ARDU_DEBUG
extern uint32_t _ebss[];
static void stack_paint(void) {
  uint32_t *sp; asm volatile("mv %0, sp" : "=r"(sp));
  for(uint32_t *p = _ebss; p < sp - 16; p++) *p = 0xA5A5A5A5;
}
static uint32_t stack_free(void) {
  uint32_t *p = _ebss; while(*p == 0xA5A5A5A5) p++;
  return (uint32_t)((uint8_t *)p - (uint8_t *)_ebss);
}
#endif

void Arduboy2Base::mainNoUSB() {
  extern void setup(); extern void loop();
#ifdef ARDU_DEBUG
  stack_paint();
#endif
  dbg_hex("setup ", 0);
  setup();
  dbg_hex("loop ", 0);
  while(1) loop();
}

bool Arduboy2Base::nextFrame() {
  while((int32_t)(STK->CNT - nextFrameAt) < 0);
  nextFrameAt += FRAME_TICKS;
  if((int32_t)(STK->CNT - nextFrameAt) > 4 * FRAME_TICKS) nextFrameAt = STK->CNT;
  frameCount++;
#if defined(ARDU_TRACE) || defined(ARDU_DLTRACE)
  { volatile uint32_t *dr = (volatile uint32_t *)0x40013804; *dr = 'F'; }
#endif
#ifdef ARDU_DEBUG
  if(!(frameCount & 31)) {
    dbg_hex("frame ", (uint32_t)frameCount << 16 | (far_misses & 0xFFFF));
    dbg_hex("stackfree ", stack_free());
    dbg_hex("dl bytes max/overflow ", (uint32_t)dlMax << 8 | dlOverflow);
#ifdef DL_VERIFY
    dbg_hex("verified pages/bad ", verifyPages << 16 | (verifyBad & 0xFFFF));
#endif
  }
#endif
  atm_service();
  return true;
}

// ---- display list ---------------------------------------------------------------------
// There is no 1 KiB frame buffer: draw calls are recorded (runs, see below) and replayed
// page by page (8 rows, 128 bytes) when the frame is sent, so the screen costs ~0.5 KiB
// less RAM. Every frame of the game starts with drawTiles() covering the whole screen
// (or fillScreen), so nothing relies on the previous frame's pixels: the list is
// cleared at the start of each frame. Pages that did not change are not sent again.
enum { M_OVERWRITE, M_ERASE, M_SELF, M_PLUSMASK, M_TILES, M_FILL };

// Entries are runs: sprites with the same image, mode and row drawn side by side (text,
// box borders) share one entry, 7 bytes + 1 byte per frame (or 1 frame for all):
//   [0] mode, bit 3: one frame per item, bit 4: run to the left
//   [1-2] x   [3] y   [4-5] image   [6] count
//   [7...] frame(s)
#ifndef DL_BYTES
#define DL_BYTES 640
#endif
static uint8_t dl[DL_BYTES];
static uint16_t dlLen, dlLast = 0xFFFF;   // bytes used, start of the last entry
static uint8_t page[WIDTH];
static uint32_t pageHash[8];              // what the OLED shows (0 = unknown)
static const uint8_t *tileMap;            // drawTiles' 9 x 5 tile IDs (see patch_upstream.py)
static const uint8_t *tileIndex;          // 4 block numbers per tile (dedupe_tiles)

static uint16_t dl_handle(const uint8_t *p) {
  uint32_t a = (uint32_t)p;
  return a >= FAR_BASE ? (uint16_t)(0x8000 | (a - FAR_BASE)) : (uint16_t)a;
}
static const uint8_t *dl_ptr(uint16_t h) {
  return (h & 0x8000) ? (const uint8_t *)(FAR_BASE + (h & 0x7FFF)) : (const uint8_t *)(uint32_t)h;
}

#ifdef DL_VERIFY
// verification build: every draw also goes straight into a full frame buffer, and each
// rendered page is compared with it (dev simulator with 4 KiB SRAM)
static uint8_t shadow[WIDTH * HEIGHT / 8];
static void shadow_draw(int16_t x, int16_t y, const uint8_t *bmp, uint8_t frame, uint8_t mode) {
  uint8_t w = pgm_read_byte(bmp), h = pgm_read_byte(bmp + 1);
  bmp += 2;
  uint8_t rows = (h + 7) >> 3;
  bmp += frame * (uint16_t)w * rows * (mode == M_PLUSMASK ? 2 : 1);
  if(x + w <= 0 || x >= WIDTH || y + h <= 0 || y >= HEIGHT) return;
  int16_t yOff = y & 7, sRow = y >> 3;
  for(uint8_t r = 0; r < rows; r++) {
    int16_t row = sRow + r;
    if(row >= HEIGHT / 8) break;
    if(row < -1) continue;
    for(uint8_t cc = 0; cc < w; cc++) {
      int16_t bx = x + cc;
      if(bx < 0) continue;
      if(bx >= WIDTH) break;
      uint16_t i = (uint16_t)r * w + cc;
      uint8_t d, m;
      if(mode == M_PLUSMASK) { d = pgm_read_byte(bmp + 2 * i); m = pgm_read_byte(bmp + 2 * i + 1); }
      else { d = pgm_read_byte(bmp + i); m = (mode == M_OVERWRITE) ? 0xFF : d; }
      uint16_t d16 = (uint16_t)d << yOff, m16 = (uint16_t)m << yOff;
      for(uint8_t k = 0; k < 2; k++, d16 >>= 8, m16 >>= 8) {
        int16_t rr = row + k;
        if(rr < 0 || rr >= HEIGHT / 8 || (k && !yOff)) continue;
        uint8_t *p = &shadow[rr * WIDTH + bx];
        uint8_t dd = (uint8_t)d16, mm = (uint8_t)m16;
        if(mode == M_SELF) *p |= dd; else if(mode == M_ERASE) *p &= ~dd; else *p = (*p & ~mm) | (dd & mm);
      }
    }
  }
}
#endif

static void dl_add(int16_t x, int16_t y, const uint8_t *bmp, uint8_t frame, uint8_t mode) {
#ifdef DL_VERIFY
  if(mode == M_FILL) memset(shadow, frame ? 0xFF : 0, sizeof shadow);
  else if(mode == M_TILES) {
    for(uint8_t j = 0; j < 5; j++) for(uint8_t i = 0; i < 9; i++) for(uint8_t q = 0; q < 4; q++)
      shadow_draw(x + 16 * i + 8 * (q & 1), y + 16 * j + 8 * (q >> 1), bmp, tileIndex[tileMap[j * 9 + i] * 4 + q], M_OVERWRITE);
  } else shadow_draw(x, y, bmp, frame, mode);
#endif
  if(mode < M_TILES && (x >= WIDTH || y >= HEIGHT || x < -128 || y < -64)) return;  // never visible
  uint16_t h = dl_handle(bmp);
  if(dlLast != 0xFFFF && mode < M_TILES) {          // continue the last run?
    uint8_t *r = &dl[dlLast];
    if((r[0] & 7) == mode && (int8_t)r[3] == y && (r[4] | r[5] << 8) == h && r[6] < 255) {
      int16_t x0 = (int16_t)(r[1] | r[2] << 8), w = pgm_read_byte(bmp) * r[6];
      uint8_t dir = 0;                                         // run to the right or left
      if(x == x0 + w && !(r[0] & 16)) dir = 1;
      else if(x == x0 - w && (r[6] == 1 || (r[0] & 16))) dir = 2;
      if(dir) {
        if(!(r[0] & 8) && r[7] == frame) {                     // same frame again
          if(dir == 2) r[0] |= 16;
          r[6]++; return;
        }
        if((r[0] & 8) || r[6] == 1) {                          // one frame per item
          if(dlLen == DL_BYTES) { dlOverflow = 1; return; }
          if(dir == 2) r[0] |= 16;
          r[0] |= 8; dl[dlLen++] = frame; r[6]++;
          DL_MAX();
          return;
        }
      }
    }
  }
  if(dlLen + 8 > DL_BYTES) { dlOverflow = 1; return; }
  uint8_t *r = &dl[dlLast = dlLen];
  r[0] = mode; r[1] = (uint8_t)x; r[2] = (uint8_t)(x >> 8); r[3] = (uint8_t)y;
  r[4] = (uint8_t)h; r[5] = (uint8_t)(h >> 8); r[6] = 1; r[7] = frame;
  dlLen += 8;
  DL_MAX();
}

static void dl_clear(void) { dlLen = 0; dlLast = 0xFFFF; }


void Arduboy2Base::fillScreen(uint8_t color) {   // everything drawn before is covered
  dl_clear();
  dl_add(0, 0, 0, color, M_FILL);
}
uint8_t *Arduboy2Base::getBuffer() { return page; }
void Arduboy2Base::exitToBootloader() { }

// the frame's tile map: 9 x 5 tiles of 16 x 16 from tileSheet, top-left at (x, y)
void dl_tiles(int16_t x, int16_t y, const uint8_t *blocks, const uint8_t *index, const uint8_t *ids) {
  dl_clear();
  tileMap = ids; tileIndex = index;
  dl_add(x, y, blocks, 0, M_TILES);
}

#define ROW_PIECE  16                      // bytes of a sprite row read at once
static uint8_t piece[ROW_PIECE], pieceLen;  // the last piece of far data read (draw_page)
static const uint8_t *pieceAt;
// one sprite into the current page (row pg): Arduboy2 Sprites formats and modes
static void draw_page(int16_t x, int16_t y, const uint8_t *bmp, uint8_t frame, uint8_t mode, uint8_t pg) {
  uint8_t w = pgm_read_byte(bmp), h = pgm_read_byte(bmp + 1);
  if(y + h <= pg * 8 || y >= pg * 8 + 8 || x + w <= 0 || x >= WIDTH) return;
  bmp += 2;
  uint8_t rows = (h + 7) >> 3;
  bmp += frame * (uint16_t)w * rows * (mode == M_PLUSMASK ? 2 : 1);
  int16_t yOff = y & 7, sRow = y >> 3;              // floor for negative y
  // source rows that reach this page: r = pg - sRow (upper part) and pg - sRow - 1 (lower)
  for(int8_t k = 0; k < 2; k++) {
    int16_t r = pg - sRow - k;
    if(r < 0 || r >= rows || (k && !yOff)) continue;
    // the row's visible columns in pieces of ROW_PIECE bytes; a piece of far data comes in
    // one EEPROM read (vs one read per 4 bytes through the far cache, which sprites only
    // thrash: in the interiors the NPC and player sprites were ~1300 reads per second).
    // The last piece is kept: runs of one image (the door fade's 128 tiles, box borders)
    // read it once, not once per copy.
    uint8_t sz = mode == M_PLUSMASK ? 2 : 1;           // bytes per column
    int16_t c = x < 0 ? -x : 0, end = x + w > WIDTH ? WIDTH - x : w;
    while(c < end) {
      uint8_t n = end - c > ROW_PIECE / 2 ? ROW_PIECE / 2 : end - c;
      const uint8_t *q = bmp + ((uint16_t)r * w + c) * sz;
      if((uint32_t)q >= FAR_BASE) {
        uint8_t len = n * sz < 3 ? 3 : n * sz;          // EE_read: >= 3 bytes
        if(q != pieceAt || len > pieceLen) {
          EE_read(FAR_EEPROM + ((uint32_t)q - FAR_BASE), piece, len);
          pieceAt = q; pieceLen = len;
        }
        q = piece;
      }
      for(uint8_t j = 0; j < n; j++, c++) {
        uint8_t d = q[j * sz], m = sz == 2 ? q[j * 2 + 1] : (mode == M_OVERWRITE) ? 0xFF : d;
        if(mode == M_ERASE) d = 0;            // self masked / erase: the image is its mask
        uint8_t dd = k ? d >> (8 - yOff) : d << yOff, mm = k ? m >> (8 - yOff) : m << yOff;
        uint8_t *p = &page[x + c];
        *p = (*p & ~mm) | (dd & mm);        // all modes (erase: data 0, mask = image)
      }
    }
  }
}

static void render_page(uint8_t pg) {
  memset(page, 0, sizeof page);
  for(uint16_t o = 0; o < dlLen; ) {
    const uint8_t *r = &dl[o];
    uint8_t mode = r[0] & 7, n = r[6], var = r[0] & 8;
    int16_t x = (int16_t)(r[1] | r[2] << 8), y = (int8_t)r[3];
    const uint8_t *bmp = dl_ptr(r[4] | r[5] << 8);
    o += 7 + (var ? n : 1);
    if(mode == M_FILL) memset(page, r[7] ? 0xFF : 0x00, sizeof page);
    else if(mode == M_TILES) {
      for(uint8_t j = 0; j < 10; j++) {           // rows of 8 x 8 blocks
        int16_t ty = y + 8 * j;
        if(ty + 8 <= pg * 8 || ty >= pg * 8 + 8) continue;
        for(uint8_t i = 0; i < 18; i++)
          draw_page(x + 8 * i, ty, bmp, tileIndex[tileMap[(j >> 1) * 9 + (i >> 1)] * 4 + (j & 1) * 2 + (i & 1)], M_OVERWRITE, pg);
      }
    } else {
      int16_t w = pgm_read_byte(bmp);
      if(r[0] & 16) w = -w;                                    // a run to the left
      for(uint8_t k = 0; k < n; k++) draw_page(x + w * k, y, bmp, r[7 + (var ? k : 0)], mode, pg);
    }
  }
}

// three SSD1306 command bytes (0xE3 = no operation, as filler)
static void oled_cmd(uint8_t a, uint8_t b, uint8_t c) {
  I2C_start(OLED_ADDR);
  I2C_write(OLED_CMD_MODE); I2C_write(a); I2C_write(b); I2C_write(c);
  I2C_stop();
}

// horizontal addressing mode (oled_min.c): select the page through the address window
static void oled_window(uint8_t pg) {
  oled_cmd(OLED_COLUMNS, 0, 127);
  oled_cmd(OLED_PAGES, pg, 7);
}

void Arduboy2Base::display() {
  if(!(frameCount & 1)) {                 // every 2nd frame: render and send changed pages
    for(uint8_t pg = 0; pg < 8; pg++) {
      render_page(pg);
#ifdef DL_VERIFY
      verifyPages++;
      if(memcmp(page, &shadow[pg * WIDTH], WIDTH)) {
        verifyBad++; dbg_hex("VERIFY page differs ", (uint32_t)frameCount << 8 | pg);
        if(verifyBad == 1) {
          for(uint8_t i = 0; i < WIDTH; i++) if(page[i] != shadow[pg * WIDTH + i])
            dbg_hex(" col/dl/shadow ", (uint32_t)i << 16 | page[i] << 8 | shadow[pg * WIDTH + i]);
          for(uint16_t o = 0; o < dlLen; ) {
            const uint8_t *r = &dl[o];
            dbg_hex(" entry mode/n/x/y ", (uint32_t)r[0] << 24 | r[6] << 16 | (uint8_t)r[1] << 8 | (uint8_t)r[3]);
            o += 7 + ((r[0] & 8) ? r[6] : 1);
          }
        }
      }
#endif
      // djb2, 32 bits: no short period (a rotate-xor hash repeated every 16 bytes, the
      // width of a map tile, so scrolling over repeating tiles looked "unchanged")
      uint32_t hsh = 5381;
      for(uint8_t i = 0; i < WIDTH; i++) hsh = (hsh << 5) + hsh + page[i];
      if(!hsh) hsh = 1;
#ifdef DL_VERIFY
      static uint8_t oled[8][WIDTH];      // what the OLED shows
      if(hsh == pageHash[pg] && memcmp(oled[pg], page, WIDTH)) dbg_hex("STALE page kept ", (uint32_t)frameCount << 8 | pg);
      if(hsh != pageHash[pg]) memcpy(oled[pg], page, WIDTH);
#endif
      if(hsh == pageHash[pg]) continue;
      pageHash[pg] = hsh;
      atm_service();                      // keep the music queue filled while sending
      oled_window(pg);
      OLED_data_start();
      for(uint8_t i = 0; i < WIDTH; i++) I2C_write(page[i]);
      I2C_stop();
    }
  }
  dl_clear();                             // the next frame draws everything again
}

void Sprites::drawOverwrite(int16_t x, int16_t y, const uint8_t *b, uint8_t f)  { dl_add(x, y, b, f, M_OVERWRITE); }
void Sprites::drawErase(int16_t x, int16_t y, const uint8_t *b, uint8_t f)      { dl_add(x, y, b, f, M_ERASE); }
void Sprites::drawSelfMasked(int16_t x, int16_t y, const uint8_t *b, uint8_t f) { dl_add(x, y, b, f, M_SELF); }
void Sprites::drawPlusMask(int16_t x, int16_t y, const uint8_t *b, uint8_t f)   { dl_add(x, y, b, f, M_PLUSMASK); }

// ---- ATMlib play routine -------------------------------------------------------------
// The song data stays in flash (the routine runs in the interrupt and must not use the
// I2C bus). Channel state and commands as in ATMlib.cpp; only the output differs.
// ATMlib's note table (note 1 = C4 262 Hz ... 63 = D9 9397 Hz) from one octave: note n
// is C9..B9 (here) shifted down by its octave distance. At most 1 Hz off ATMlib's own
// rounding (inaudible on the buzzer); 100 bytes less flash than the 64-entry table.
static const uint16_t OCTAVE9[12] = { 8372, 8870, 9397, 9956, 10548, 11175, 11840, 12544,
                                      13290, 14080, 14917, 15804 };
static uint16_t noteFreq(uint8_t n) {     // 0 = silence
  if(!n) return 0;
  uint8_t s = n - 1;
  return OCTAVE9[s % 12] >> (5 - s / 12);
}

#define ATM_DEPTH 3
struct ch_t {
  const uint8_t *ptr;
  uint8_t note;
  uint16_t stackPointer[ATM_DEPTH];      // ATMlib: 7 levels; the songs here nest 1-2
  uint8_t stackCounter[ATM_DEPTH], stackTrack[ATM_DEPTH], stackIndex, repeatPoint;
  uint16_t delay;
  uint8_t counter, track;
  uint16_t freq;
  uint8_t vol;
  int8_t volFreSlide;
  uint8_t volFreConfig, volFreCount, arpNotes, arpTiming, arpCount, reConfig, reCount;
  int8_t transConfig;
  uint8_t treviDepth, treviConfig, treviCount;
  int8_t glisConfig;
  uint8_t glisCount;
};

static ch_t channel[4];
static uint8_t trackCount, tickRate = 25, chMute = 0xF0;
static const uint16_t *trackList;
static const uint8_t *trackBase;
static volatile uint8_t atmPlaying;
static uint8_t oscVol[4];
static uint16_t oscFreq[4];

static uint8_t rd(const uint8_t *p) { return pgm_read_byte(p); }
static uint16_t read_vle(const uint8_t **pp) {
  uint16_t q = 0; uint8_t d;
  do { q <<= 7; d = rd((*pp)++); q |= d & 0x7F; } while(d & 0x80);
  return q;
}
static const uint8_t *trackPtr(uint8_t t) {
  return trackBase + (rd((const uint8_t *)&trackList[t]) | (rd((const uint8_t *)&trackList[t] + 1) << 8));
}
static void setTempo(uint8_t r) { tickRate = r ? r : 1; }

// Commands the game's songs never use are compiled out (ATM_FULL keeps them): measured
// with tools/songs.py over every song - glissando (18/19), retrigger (9/10), advanced
// and frequency slides (2, 4, 5, 6), arpeggio setup (7/8; note cut 20 uses the
// arpeggio engine and stays), relative jump (255).
static void atm_tick(void) {
  for(uint8_t n = 0; n < 4; n++) {
    ch_t *ch = &channel[n];
#ifdef ATM_FULL
    if(ch->reConfig) {
      if(ch->reCount >= (ch->reConfig & 0x03)) { oscFreq[n] = noteFreq(ch->reConfig >> 2); ch->reCount = 0; }
      else ch->reCount++;
    }
    if(ch->glisConfig) {
      if(ch->glisCount >= (ch->glisConfig & 0x7F)) {
        if(ch->glisConfig & 0x80) ch->note -= 1; else ch->note += 1;
        if(ch->note < 1) ch->note = 1; else if(ch->note > 63) ch->note = 63;
        ch->freq = noteFreq(ch->note);
        ch->glisCount = 0;
      } else ch->glisCount++;
    }
#endif
    if(ch->volFreSlide) {                 // volume slide (command 1; the frequency and
      int16_t vf = ch->vol + ch->volFreSlide;   // "advanced" slides are never used: their
      ch->vol = vf < 0 ? 0 : vf > 63 ? 63 : vf; // config is 0, i.e. every tick, volume)
    }
    if(ch->arpNotes && ch->note) {
      if((ch->arpCount & 0x1F) < (ch->arpTiming & 0x1F)) ch->arpCount++;
      else {
        if((ch->arpCount & 0xE0) == 0x00) ch->arpCount = 0x20;
        else if((ch->arpCount & 0xE0) == 0x20 && !(ch->arpTiming & 0x40) && ch->arpNotes != 0xFF) ch->arpCount = 0x40;
        else ch->arpCount = 0x00;
        uint8_t arpNote = ch->note;
        if((ch->arpCount & 0xE0) != 0x00) {
          if(ch->arpNotes == 0xFF) arpNote = 0; else arpNote += ch->arpNotes >> 4;
        }
        if((ch->arpCount & 0xE0) == 0x40) arpNote += ch->arpNotes & 0x0F;
        ch->freq = noteFreq((uint8_t)(arpNote + ch->transConfig) & 63);
      }
    }
    if(ch->treviDepth) {
      int16_t vt = (ch->treviConfig & 0x40) ? ch->freq : ch->vol;
      vt = (ch->treviCount & 0x80) ? vt + ch->treviDepth : vt - ch->treviDepth;
      if(vt < 0) vt = 0;
      else if(ch->treviConfig & 0x40) { if(vt > 9397) vt = 9397; }
      else if(vt > 63) vt = 63;
      if(ch->treviConfig & 0x40) ch->freq = vt; else ch->vol = vt;
      if((ch->treviCount & 0x1F) < (ch->treviConfig & 0x1F)) ch->treviCount++;
      else ch->treviCount = (ch->treviCount & 0x80) ? 0 : 0x80;
    }
    if(ch->delay) {
      if(ch->delay != 0xFFFF) ch->delay--;
    } else {
      do {
        uint8_t cmd = rd(ch->ptr++);
        if(cmd < 64) {
          if((ch->note = cmd)) ch->note += ch->transConfig;
          ch->freq = noteFreq(ch->note & 63);
          if(!ch->volFreConfig) ch->vol = ch->reCount;
          if(ch->arpTiming & 0x20) ch->arpCount = 0;
        } else if(cmd < 160) {
          switch(cmd - 64) {
            case 0: ch->vol = rd(ch->ptr++); ch->reCount = ch->vol; break;
            case 1: case 4: ch->volFreSlide = rd(ch->ptr++); ch->volFreConfig = (cmd - 64) == 1 ? 0x00 : 0x40; break;
            case 3: case 6: ch->volFreSlide = 0; break;
#ifdef ATM_FULL
            case 2: case 5: ch->volFreSlide = rd(ch->ptr++); ch->volFreConfig = rd(ch->ptr++); break;
            case 7: ch->arpNotes = rd(ch->ptr++); ch->arpTiming = rd(ch->ptr++); break;
            case 8: ch->arpNotes = 0; break;
            case 9: ch->reConfig = rd(ch->ptr++); break;
            case 10: ch->reConfig = 0; break;
#endif
            case 11: ch->transConfig += (int8_t)rd(ch->ptr++); break;
            case 12: ch->transConfig = rd(ch->ptr++); break;
            case 13: ch->transConfig = 0; break;
            case 14: case 16:     // ATMlib reads these with pgm_read_word but advances by one byte
              ch->treviDepth = rd(ch->ptr); ch->ptr++;
              ch->treviConfig = rd(ch->ptr) + ((cmd - 64) == 14 ? 0x00 : 0x40); ch->ptr++;
              break;
            case 15: case 17: ch->treviDepth = 0; break;
#ifdef ATM_FULL
            case 18: ch->glisConfig = rd(ch->ptr++); break;
            case 19: ch->glisConfig = 0; break;
#endif
            case 20: ch->arpNotes = 0xFF; ch->arpTiming = rd(ch->ptr++); break;
            case 21: ch->arpNotes = 0; break;
            case 92: setTempo(tickRate + rd(ch->ptr++)); break;
            case 93: setTempo(rd(ch->ptr++)); break;
            case 94: for(uint8_t i = 0; i < 4; i++) channel[i].repeatPoint = rd(ch->ptr++); break;
            case 95: chMute ^= 1 << (n + 4); ch->vol = 0; ch->delay = 0xFFFF; break;
          }
        } else if(cmd < 224) {
          ch->delay = cmd - 159;
        } else if(cmd == 224) {
          ch->delay = read_vle(&ch->ptr) + 65;
        } else if(cmd < 252) {
        } else if(cmd == 252 || cmd == 253) {
          uint8_t newCounter = cmd == 252 ? 0 : rd(ch->ptr++);
          uint8_t newTrack = rd(ch->ptr++);
          if(newTrack != ch->track && ch->stackIndex < ATM_DEPTH) {
            ch->stackCounter[ch->stackIndex] = ch->counter;
            ch->stackTrack[ch->stackIndex] = ch->track;
            ch->stackPointer[ch->stackIndex] = ch->ptr - trackBase;
            ch->stackIndex++;
            ch->track = newTrack;
          }
          ch->counter = newCounter;
          ch->ptr = trackPtr(ch->track);
        } else if(cmd == 254) {
          if(ch->counter > 0 || ch->stackIndex == 0) {
            if(ch->counter) ch->counter--;
            ch->ptr = trackPtr(ch->track);
          } else {
            ch->stackIndex--;
            ch->ptr = ch->stackPointer[ch->stackIndex] + trackBase;
            ch->counter = ch->stackCounter[ch->stackIndex];
            ch->track = ch->stackTrack[ch->stackIndex];
          }
        }
#ifdef ATM_FULL
        else if(cmd == 255) ch->ptr += read_vle(&ch->ptr);
#endif
      } while(ch->delay == 0);
      if(ch->delay != 0xFFFF) ch->delay--;
    }
    if(!(chMute & (1 << n))) {
      if(n != 3) oscFreq[n] = ch->freq;
      oscVol[n] = n == 3 ? ch->vol >> 1 : ch->vol;
    }
    if(!(chMute & 0xF0)) {
      uint8_t repeatSong = 0;
      for(uint8_t j = 0; j < 4; j++) repeatSong += channel[j].repeatPoint;
      if(repeatSong) {
        for(uint8_t k = 0; k < 4; k++) { channel[k].ptr = trackPtr(channel[k].repeatPoint); channel[k].delay = 0; }
        chMute = 0xF0;
      } else {
        memset(channel, 0, sizeof(channel));
        atmPlaying = 0;
      }
    }
  }
}

// ---- sound: ATMlib's synthesizer ------------------------------------------------------
// The sample interrupt is ATMlib's (ATMlib.h, TIMER4 ISR) in C, bit for bit: 31250 Hz,
// each oscillator adds its table value to a 16-bit phase per sample (the table holds
// phase steps, not hertz: 440 sounds at 440 * 31250 / 65536 Hz), channel 0 is a 25 %
// pulse, 1 a square, 2 a triangle, 3 a 16-bit LFSR noise, each +-volume; the sum around
// 128 (8-bit, wrapping like the AVR's) is the PWM duty. The play routine runs tickRate
// times per second (see atm_service). Here the duty goes to TIM1's PWM on
// the buzzer pin (PA1 = TIM1 channel 2, 187.5 kHz), inverted: the buzzer is active low.
// The play routine runs in the main loop (song data may be far data, read over I2C)
// ahead of time; each tick's oscillator settings are queued with its length in samples.
// The same interrupt counts millis().
#define ARDU_REST 80                      // PWM duty of the wave's rest level (was 128)
struct ATick { uint16_t freq[3]; uint8_t vol[4]; uint16_t len; };  // len: samples
#define AQ        4                       // queued ticks (power of 2): 3 ahead
static volatile ATick aq[AQ];
static volatile uint8_t aqHead, aqTail;   // main loop writes aq[aqHead], ISR reads aq[aqTail]
static volatile uint8_t synthOn;          // 0: buzzer off (pin high)
#ifdef ARDU_MUSICSTAT
uint16_t aqUnderruns;                     // ms the queue was empty while playing (tools/music_check.py)
#endif

extern "C" void SysTick_body(void);
// The startup code enables the QingKe hardware prologue/epilogue (INTSYSCR.HPE): the
// core itself saves the caller-saved registers on entry and restores them on mret, so
// the vector is a naked call to an ordinary function, without the software register
// save of GCC's interrupt attribute (which would only repeat it).
extern "C" __attribute__((naked)) void SysTick_Handler(void) {
  asm volatile("call SysTick_body \n mret");
}

extern "C" __attribute__((used)) void SysTick_body(void) {
  static ATick cur;
  static uint16_t phase[3], lfsr = 1;
  static uint8_t msAcc;
  STK->SR = 0;
  STK->CMP += F_CPU / AUDIO_SR;
  if((int32_t)(STK->CMP - STK->CNT) <= 0) STK->CMP = STK->CNT + F_CPU / AUDIO_SR;  // after a stall
  if((msAcc += 8) >= 250) { msAcc -= 250; msCount++; }     // 31.25 samples per ms
  if(!cur.len) {                                           // next tick from the queue
    if(aqTail != aqHead) {
      cur = *(ATick *)&aq[aqTail]; aqTail = (aqTail + 1) & (AQ - 1);
    }
#ifdef ARDU_MUSICSTAT
    else if(synthOn) { aqUnderruns++; cur.len = AUDIO_SR / 1000; }   // late: the last tick goes on
#endif
  }
  {
  }
  if(cur.len) cur.len--;
  uint8_t h;
  h = (phase[2] += cur.freq[2]) >> 8;                       // triangle
  uint8_t t = (h & 0x80) ? ~h : h;
  int8_t mix = (int8_t)(((int16_t)(int8_t)(uint8_t)((t << 1) - 128) * cur.vol[2]) >> 8) << 1;
  h = (phase[0] += cur.freq[0]) >> 8;                       // 25 % pulse
  mix += ((h << 1) & h & 0x80) ? -cur.vol[0] : cur.vol[0];
  h = (phase[1] += cur.freq[1]) >> 8;                       // square
  mix += (h & 0x80) ? -cur.vol[1] : cur.vol[1];
  lfsr <<= 1;                                               // noise
  if(lfsr & 0x8000) lfsr ^= 1;
  if(lfsr & 0x4000) lfsr ^= 1;
  mix += (lfsr & 0x8000) ? -(cur.vol[3]) : cur.vol[3];
  // plataboy26: the wave rests on ARDU_REST instead of 128: the same wave, but the buzzer
  // no longer draws ~50 % of its current as DC (wasted, a load on the coin cell:
  // games/common/buzzer.h). The game's music swings 72 below the rest level at most
  // (4 min in the simulator); anything lower is clipped at 0 (pin high).
  int16_t d = mix + ARDU_REST;
  uint8_t duty = d < 0 ? 0 : (uint8_t)d;
  TIM1->CH2CVR = (synthOn && audioOn) ? 256 - duty : 256;  // low time = duty (active low)
}

// ahead of the queue: up to AQ - 1 ticks (~40 ms at the fastest tempo)
static void atm_service(void) {
  while(atmPlaying && ((aqHead + 1) & (AQ - 1)) != aqTail) {
    // ticks per second = the song's tempo (its note lengths are ordinary tempos then, and
    // that is how it sounds right); ATMlib's code reads as twice that (cia = 15625 /
    // tempo counted at the 31250 Hz sample rate) - the shipped build's timer clock or
    // library revision evidently differed. Set before the play routine, as cia.
    uint16_t len = AUDIO_SR / tickRate;
    atm_tick();
    volatile ATick *t = &aq[aqHead];
    for(uint8_t n = 0; n < 4; n++) {
      t->vol[n] = atmPlaying ? oscVol[n] : 0;
      if(n < 3) t->freq[n] = oscFreq[n];
    }
    t->len = len;
    aqHead = (aqHead + 1) & (AQ - 1);
    synthOn = 1;
  }
}

static void audio_flush(void) { aqTail = aqHead; synthOn = 0; }

void ATMsynth::play(const uint8_t *song) {
  atmPlaying = 0;
  memset(channel, 0, sizeof(channel));
  memset(oscVol, 0, sizeof oscVol);
  chMute = 0xF0;
  setTempo(25);
  trackCount = rd(song++);
  trackList = (const uint16_t *)song;
  trackBase = (song += trackCount << 1) + 4;
  for(uint8_t n = 0; n < 4; n++) channel[n].ptr = trackPtr(rd(song++));
  audio_flush();
  atmPlaying = 1;
}

void ATMsynth::stop() {
  atmPlaying = 0;
  memset(channel, 0, sizeof(channel));
  chMute = 0xF0;
  audio_flush();
}

void ATMsynth::playPause() { atmPlaying = !atmPlaying; if(!atmPlaying) audio_flush(); }

void Arduboy2Audio::off() { audioOn = false; }

// ---- code overlays -----------------------------------------------------------------------
// The game's code groups (title, overworld, menus, battle; tools/patch_upstream.py) share
// one flash window. ov_ensure(g) before running a group's code: if the window holds
// another group, the image is copied from the EEPROM (header at FAR_EEPROM - 32:
// u32 address + u16 size per group). Only 64-byte pages that differ are rewritten
// (flash wear), and the page with the group number (the image's first word) goes last,
// so an interrupted load is simply redone. Fast page erase/program as in the kernel.
#ifdef ARDU_OVERLAYS
extern "C" char __ovl_base[];
uint16_t ov_loads, ov_pages;              // statistics: loads, pages rewritten

static void fl_wait(void) { while(FLASH->STATR & 1); }
static void fl_page(uint32_t a, const uint32_t *w) {   // a = 0x0800xxxx
  FLASH->CTLR = 1u << 17; FLASH->ADDR = a; FLASH->CTLR = (1u << 17) | (1u << 6); fl_wait();
  FLASH->CTLR = 1u << 16; FLASH->CTLR = (1u << 16) | (1u << 19); fl_wait();
  for(uint8_t i = 0; i < 16; i++) {
    ((volatile uint32_t *)a)[i] = w[i];
    FLASH->CTLR = (1u << 16) | (1u << 18); fl_wait();
  }
  FLASH->ADDR = a; FLASH->CTLR = (1u << 16) | (1u << 6); fl_wait();
  FLASH->CTLR = 0;
}

extern "C" uint8_t ov_loaded();
static void ov_load(uint8_t g) {
  uint8_t dim = g == 3 || ov_loaded() == 3;  // into or out of the menus/map: only dim
  uint8_t hdr[6];
  EE_read(FAR_EEPROM - 32 + 4 + 6 * (g - 1), hdr, 6);
  uint32_t src = hdr[0] | hdr[1] << 8 | (uint32_t)hdr[2] << 16;
  uint16_t size = hdr[4] | hdr[5] << 8;
  uint32_t base = (uint32_t)__ovl_base;
  uint8_t pages = (size + 63) >> 6;
  FLASH->KEYR = 0x45670123; FLASH->KEYR = 0xCDEF89AB;
  FLASH->MODEKEYR = 0x45670123; FLASH->MODEKEYR = 0xCDEF89AB;
  uint8_t shown = 0xFF;
  for(uint8_t k = pages; k--; ) {          // the last page first, page 0 (number) last
    // mask the load: the OLED fades out over the first 8 pages and back in over the
    // last 8 (contrast), switched off (black) in between
    uint8_t done = pages - 1 - k, e = done < k ? done : k;
    uint8_t full = SYSX_CONTRAST;          // the menu's setting (the kernel's config page)
    uint8_t level = e < 8 ? full - (full >> 3) * e : 0;
    if(dim && level < full >> 2) level = full >> 2;   // menus and map open over the game: only dim it
#ifndef ARDU_MUSICSTAT                       // (the measuring build needs the room)
    if(level != shown) oled_cmd(OLED_CONTRAST, shown = level, level ? 0xAF : 0xAE);  // 0: off
#endif
    atm_service();                         // the music goes on during the load
    uint32_t buf[16];
    EE_read(src + 64 * k, (uint8_t *)buf, 64);
    const uint32_t *cur = (const uint32_t *)(base + 64 * k);
    uint8_t i = 0;
    while(i < 16 && buf[i] == cur[i]) i++;
    if(i == 16) continue;                  // already there: no flash write
    fl_page(0x08000000 + base + 64 * k, buf);
    ov_pages++;
  }
  FLASH->CTLR = (1u << 15) | (1u << 7);    // lock
  oled_cmd(OLED_CONTRAST, SYSX_CONTRAST, 0xAF);   // full again (the menu's setting), display on
  ov_loads++;
  dbg_hex("overlay ", (uint32_t)g << 16 | ov_pages);
}

extern "C" char OVL_TAG[];               // linker symbol: this build's ID << 16
extern "C" uint8_t ov_loaded() {          // 0 = none (erased, another program, older build)
  uint32_t w = *(volatile uint32_t *)__ovl_base, g = w - (uint32_t)OVL_TAG;
  return g >= 1 && g <= 4 ? (uint8_t)g : 0;
}

void ov_ensure(uint8_t group) {
  if(group && ov_loaded() != group) ov_load(group);
}
#else
void ov_ensure(uint8_t group) { (void)group; }   // development build: everything resident
extern "C" uint8_t ov_loaded() { return 2; }     // (any group with a tile refill)
#endif

// ---- debug: game state changes (simulator serial log) --------------------------------
#ifdef ARDU_DEBUG
void dbg_state(uint8_t st) {
  static uint8_t last = 0xFF;
  if(st != last) { last = st; dbg_hex("state ", (uint32_t)msCount << 8 | st); }
}
void dbg_tile(uint8_t i, uint8_t j) { dbg_hex("STALE tile ", (uint32_t)msCount << 8 | j << 4 | i); }
#else
void dbg_state(uint8_t st) { (void)st; }
void dbg_tile(uint8_t i, uint8_t j) { (void)i; (void)j; }
#endif

// ---- entry ---------------------------------------------------------------------------
int arduventure_main();
int main(void) { return arduventure_main(); }

// plataboy26 framework: UP + DOWN -> menu (games/common/sysx.c; no battery check: no flash)
#define SYSX_NO_BATTERY
#include "../../common/sysx.c"
