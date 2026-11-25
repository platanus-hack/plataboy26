// ===================================================================================
// Project:   plataboy26 menu - game selection for the plataboy26 game library
// ===================================================================================
//
// Shows the programs stored compressed in the 128 KiB EEPROM as full-screen cards (each
// program's screenshot, the simulator's thumbnail) and scrolls between them sideways;
// the fire button installs the selected program into flash through the loader kernel
// (firmware/kernel/kernel.S). The menu is a normal program itself: entry 0 of the
// library, put back into flash by the kernel at every power-on (when a game is installed).
//
// Controls: Left/Right: previous/next card (its position, e.g. 3/15, in the bottom right
// corner), fire button (A): start (on release), B: the about box over the card ("by
// Platanus", the banana, "PLATABOY 26" and the firmware version); any button closes it.
// The last selection is kept in the EEPROM save area, so the menu opens on the game
// played last.
// Power-on (the kernel always brings the menu back then): the boot animation first
// (bootanim.h, made from art/boot by export_c.py: PH↗26 logo, Platanus banana, "PLATABOY
// 26"), ~1.5 s, any button skips it; then it dissolves into the card of the last game.
// Coming back from a game (UP + DOWN) goes straight to the card.
// Settings, the last card (A opens it): battery voltage, an estimate of the charge and
// the play time left, and the OLED contrast (Left/Right, 8 levels; A or B closes). The
// contrast is kept in the EEPROM save area, set at every start of the menu and handed to
// the kernel with each install, for the games (games/common/sysx.h SYSX_CONTRAST).
// Power: between frames and while waiting for a button the core sleeps (WFE until the
// next SysTick compare) instead of spinning.
// Low battery (VDD below SYSX_BATT_MV, games/common/sysx.h, measured against the internal
// 1.2 V reference at start and every ~5 s): "LOW BATTERY" over the top left corner and
// a double beep. In the games, games/common/sysx.c beeps; UP + DOWN held 2 s there
// brings this menu back.
//
// One 1 KiB buffer holds the card on screen, column by column (fb[x * 8 + page]),
// which is also how the OLED is filled (vertical addressing). Cards are LZ-compressed
// (tools/lz.py) in the same order, so a slide needs no second buffer:
//   next      the screen moves left one column at a time: the buffer shifts left by a
//             column and the next card's next column is decoded into the free end (LZ
//             back references are distances back from the write position, which the
//             shift keeps)
//   previous  the card slides right and uncovers the previous card, decoded in place
//             from the left

#define JOY_CAL_SET                // JOY_cal is set by cal_update() before any joypad reading
#include "driver.h"
#include "eeprom.h"
#include "library.h"
#include "font.h"
#include "sysx.h"
#ifdef EDITION_PLATANUS
#include "bootanim_platanus.h"     // the Platanus edition: the full Platanus logo (art/boot/platanus.py)
#else
#include "bootanim.h"              // the PH26 edition: PH-arrow-26 (art/boot/anim.py)
#endif

#define STEP   8                        // columns per animation frame

static uint8_t fb[128 * 8];             // column-major: fb[x * 8 + page]
static LibHeader hdr;
static uint8_t count;                   // listed entries (directory entries 1..count)

enum { K_NONE, K_LEFT, K_RIGHT, K_FIRE, K_B };
static uint8_t key(void);

// ---- sleep ------------------------------------------------------------------------
// The core sleeps until SysTick reaches t: the compare flag (CNTIF) is the wake-up event;
// its interrupt is enabled in SysTick but not in the PFIC, so it only wakes the core
// (SEVONPEND), and WFI is taken as WFE (WFITOWFE). sleep_init() sets that up.
static void sleep_init(void) {
  STK->CTLR |= STK_CTLR_STIE;
  PFIC->SCTLR = (PFIC->SCTLR & ~PFIC_SLEEPDEEP) | PFIC_SEVONPEND | PFIC_WFITOWFE;
}
static void wait_until(uint32_t t) {
  STK->CMP = t;
  while(1) {
    STK->SR = 0;                        // (a flag from before: it would wake at once)
    if((int32_t)(STK->CNT - t) >= 0) return;
    asm volatile ("wfi");               // a compare after the check above still wakes it
    PFIC->IPRR[0] = 1u << SysTicK_IRQn;
  }
}
static void nap(uint8_t ms) { wait_until(STK->CNT + ms * DLY_MS_TIME); }

// ---- OLED, vertical addressing ----------------------------------------------------
static void oled_cmd(uint8_t a, uint8_t b, uint8_t c) {
  I2C_start(OLED_ADDR);
  I2C_write(OLED_CMD_MODE); I2C_write(a); I2C_write(b); I2C_write(c);
  I2C_stop();
}

// the position badge ("3/15", light digits on a dark box) in the bottom right corner: put
// over the card while sending, never into fb (the slides' LZ back references read fb)
static uint8_t badge[2 + 6 * 5], bw;
static uint8_t warn[2 + 6 * 11], ww;    // "LOW BATTERY" (dark on light) in page 0; ww = 0: none
#ifndef FW_VERSION
#define FW_VERSION "v0.4.4"
#endif

// dissolve: show() lights only the pixels whose 4 x 4 Bayer threshold is below `level`
// (16: all, 0: none)
static uint8_t level = 16;
static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static void show(void) {                // the whole buffer, column by column
  uint8_t mask[4];
  for(uint8_t c = 0; c < 4; c++) {      // per x & 3: the rows lit at this level
    uint8_t m = 0;
    for(uint8_t r = 0; r < 4; r++) if(BAYER[r * 4 + c] < level) m |= 0x11 << r;
    mask[c] = m;
  }
  oled_cmd(OLED_COLUMNS, 0, 127);
  oled_cmd(OLED_PAGES, 0, 7);
  OLED_data_start();
  for(uint16_t i = 0; i < sizeof(fb); i++) {
    uint8_t x = i >> 3, v = fb[i];
    if((i & 7) == 7 && x >= 128 - bw) v = badge[x - (128 - bw)];
    if((i & 7) == 0 && x < ww) v = warn[x];
    I2C_write(v & mask[x & 3]);
  }
  I2C_stop();
}

// ---- text (5 x 8 font) into the column-major buffer --------------------------------
static uint8_t text(uint8_t x, uint8_t page, const char *s, uint8_t inv) {
  for(; *s && x <= 122; s++, x += 6) {
    uint8_t c = (uint8_t)*s;
    const uint8_t *g = FONT[(c >= 32 && c < 128) ? c - 32 : '?' - 32];
    for(uint8_t i = 0; i < 6; i++) {
      uint8_t v = i < 5 ? g[i] : 0;
      fb[(x + i) * 8 + page] = inv ? (uint8_t)~v : v;
    }
  }
  return x;
}

static void badge_char(char c) {
  const uint8_t *g = FONT[c - 32];
  for(uint8_t i = 0; i < 5; i++) badge[bw++] = (uint8_t)(g[i] << 1);   // one dark row above
  badge[bw++] = 0;
}
static void badge_num(uint8_t n) {
  if(n >= 10) badge_char((char)('0' + n / 10));
  badge_char((char)('0' + n % 10));
}
static void position(uint8_t sel) {     // the badge for listed entry sel
  bw = 0; badge[bw++] = 0; badge[bw++] = 0;
  badge_num(sel + 1); badge_char('/'); badge_num(count + 1);   // + the settings
}

// ---- battery ----------------------------------------------------------------------
static uint16_t battery_mv(void) {      // VDD against the internal 1.2 V reference
  ADC1->CTLR2 |= ADC_TSVREFE;
  ADC1->SAMPTR2 |= 7UL << 24;           // channel 8: longest sample time
  ADC_input_VREF();
  ADC_read();                           // (the first one after switching: discarded)
  uint16_t raw = 0;
  for(uint8_t i = 0; i < 4; i++) raw += ADC_read();
  ADC_input(PIN_PAD);
  return (uint32_t)1200 * 1023 * 4 / raw;
}
static uint8_t battery_low(void) { return battery_mv() < SYSX_BATT_MV; }

static void battery_check(void) {
  uint8_t low = battery_low();
  if(low == (ww != 0)) return;
  ww = 0;
  if(low) {
    warn[ww++] = 0xFF;
    for(const char *c = "LOW BATTERY"; *c; c++) {
      const uint8_t *g = FONT[*c - 32];
      for(uint8_t i = 0; i < 5; i++) warn[ww++] = (uint8_t)~g[i];
      warn[ww++] = 0xFF;
    }
    warn[ww++] = 0xFF;
  }
  show();
  if(low) { JOY_sound(80, 40); DLY_ms(80); JOY_sound(80, 40); }
}

static uint8_t len(const char *s) { uint8_t n = 0; while(s[n]) n++; return n; }

// ---- LZ stream from the EEPROM (format: tools/lz.py) --------------------------------
typedef struct {
  uint32_t src;                         // EEPROM address of the next block
  uint8_t buf[16], pos;                 // block buffer
  uint8_t bits, nbits;                  // bit buffer, MSB first
  uint16_t mlen, moff;                  // match in progress
} LZ;

static uint8_t lz_byte(LZ *z) {
  if(z->pos == sizeof z->buf) { EE_read(z->src, z->buf, sizeof z->buf); z->src += sizeof z->buf; z->pos = 0; }
  return z->buf[z->pos++];
}
static uint8_t lz_bit(LZ *z) {
  if(!z->nbits) { z->bits = lz_byte(z); z->nbits = 8; }
  return (z->bits >> --z->nbits) & 1;
}
static uint16_t lz_bits(LZ *z, uint8_t n) { uint16_t v = 0; while(n--) v = (v << 1) | lz_bit(z); return v; }
static uint16_t lz_gamma(LZ *z) { uint8_t k = 0; while(!lz_bit(z)) k++; return (1u << k) | lz_bits(z, k); }

// the next n bytes of the stream to out; back references read out[-offset]
static void lz_out(LZ *z, uint8_t *out, uint16_t n) {
  while(n--) {
    if(!z->mlen) {
      if(!lz_bit(z)) { *out++ = (uint8_t)lz_bits(z, 8); continue; }
      z->mlen = lz_gamma(z) + 1;
      z->moff = (((lz_gamma(z) - 1) << 7) | lz_bits(z, 7)) + 1;
    }
    *out = out - z->moff >= fb ? out[-(int16_t)z->moff] : 0;   // (a bad stream: no fault)
    out++;
    z->mlen--;
  }
}

// ---- library and cards ------------------------------------------------------------
static void entry_name(uint8_t i, char *name) {   // i: listed index
  LibEntry e;
  EE_read(LIB_DIR + (uint32_t)(i + 1) * sizeof(LibEntry), (uint8_t *)&e, sizeof(LibEntry));
  for(uint8_t k = 0; k < 15; k++) name[k] = e.name[k];
  name[15] = 0;
}

// start the card of listed entry i; 0 if it has none (then a name card)
static uint8_t card_open(uint8_t i, LZ *z) {
  uint8_t t[6];
  if(i >= count) return 0;              // the settings
  EE_read(LIB_CARDS + (uint32_t)(i + 1) * 6, t, 6);
  uint16_t size = t[4] | t[5] << 8;
  if(size == 0 || size == 0xFFFF) return 0;
  z->src = t[0] | t[1] << 8 | (uint32_t)t[2] << 16;
  z->pos = sizeof z->buf; z->nbits = 0; z->mlen = 0;
  return 1;
}

// a card without an image: the name in the middle, dark background
static void ctext(uint8_t page, const char *s);
static void name_card(uint8_t *dst, uint8_t i) {
  char name[16];
  for(uint16_t k = 0; k < sizeof(fb); k++) dst[k] = 0;
  if(i >= count) {                      // the settings
    ctext(2, "SETTINGS");
    ctext(4, "battery");
    ctext(5, "contrast");
    return;
  }
  entry_name(i, name);
  text((uint8_t)(64 - len(name) * 3), 3, name, 0);
}

static void card_full(uint8_t i) {      // the whole card of entry i into fb
  LZ z;
  if(card_open(i, &z)) lz_out(&z, fb, sizeof(fb));
  else name_card(fb, i);
}

// slide to entry `to`: dir 1 = next (screen moves left), 0 = previous (uncover)
static void slide(uint8_t to, uint8_t dir) {
  LZ z;
  uint8_t has = card_open(to, &z);
  if(!has) {                                       // a name card (or the settings): no
    name_card(fb, to); show();                       // slide, but as long as one, so that a
    for(uint8_t i = 0; i < 18 && key(); i++) nap(20); // press doesn't run on past it
    return;
  }
  for(uint8_t s = 0; s < 128; s++) {
    if(dir) {                                        // shift left, decode at the end
      for(uint16_t k = 0; k < sizeof(fb) - 8; k++) fb[k] = fb[k + 8];
      lz_out(&z, fb + sizeof(fb) - 8, 8);
    } else {                                         // move the old card right by one
      for(uint16_t k = sizeof(fb) - 1; k >= (uint16_t)(s + 1) * 8; k--) fb[k] = fb[k - 8];
      lz_out(&z, fb + s * 8, 8);
    }
    if((s + 1) % STEP == 0) show();
  }
}

static void message(const char *l1, const char *l2) {
  for(uint8_t x = 8; x < 120; x++) for(uint8_t p = 2; p <= 5; p++) fb[x * 8 + p] = 0;
  text((uint8_t)(64 - len(l1) * 3), 3, l1, 0);
  if(l2) text((uint8_t)(64 - len(l2) * 3), 4, l2, 0);
  show();
}

// ---- boot animation (bootanim.h) ---------------------------------------------------
// sprite d (w columns, p pages, column-major) ORed into fb at (x, y), clipped; only the
// first n columns (the caption is revealed that way)
static void blit(const uint8_t *d, uint8_t w, uint8_t p, int16_t x, int16_t y, uint8_t n) {
  for(uint8_t c = 0; c < w && c < n; c++, d += p) {
    int16_t X = x + c;
    if(X < 0 || X > 127) continue;
    uint8_t *col = fb + X * 8;
    for(uint8_t k = 0; k < p; k++) {
      int16_t Y = y + k * 8;
      uint8_t v = d[k];
      if(!v || Y <= -8 || Y > 63) continue;
      if(Y < 0) { col[0] |= v >> -Y; continue; }
      uint8_t pg = Y >> 3, sh = Y & 7;
      col[pg] |= v << sh;
      if(sh && pg < 7) col[pg + 1] |= v >> (8 - sh);
    }
  }
}
#define BLIT(S, dx, dy, n) blit(S, S##_W, S##_P, S##_X + (dx), S##_Y + (dy), n)

static void frame_wait(uint32_t *t, uint8_t ms) {   // paces the frames on SysTick
  *t += ms * DLY_MS_TIME;
  wait_until(*t);
}


// ---- joypad calibration (games/common/joycal.h) ----------------------------------------
// The boot animation measures the joypad's idle level (nothing is held at power-on): the
// thresholds for it go to JOY_cal and to the EEPROM (JOYCAL_EE), where the games read them.
// The EEPROM is written only when they change.
static uint16_t cal_lo = 0xFFFF, cal_hi, cal_n;   // idle readings: lowest, highest, count
static uint32_t cal_sum;

static void cal_sample(void) {
  uint16_t v = ADC_read();                         // (key() leaves the ADC on the joypad)
  if(v < cal_lo) cal_lo = v;
  if(v > cal_hi) cal_hi = v;
  cal_sum += v; cal_n++;
}

// thresholds as joycal_thresholds() makes them: the first 12 (idle 0) .. 50 (idle 159), the
// last above it (a blank EEPROM reads 0xFF)
static uint8_t cal_valid(const uint8_t *t) {
  return t[0] >= 11 && t[0] <= 50 && t[JOYCAL_N - 1] > t[0] && t[JOYCAL_N - 1] != 0xFF;
}

// measured: the samples of the boot animation count (steady, and below any button: a
// button held through it, or an unsteady leak, keeps the thresholds as they were)
static void cal_update(uint8_t measured) {
  uint8_t ee[JOYCAL_N];
  if(EE_read(JOYCAL_EE, ee, JOYCAL_N) != EE_OK) ee[0] = 0;
  if(measured && cal_n >= 8 && cal_hi < JOYCAL_IDLE_MAX && cal_hi - cal_lo <= 16)
    joycal_thresholds(JOY_cal, cal_sum / cal_n);
  else if(cal_valid(ee)) for(uint8_t k = 0; k < JOYCAL_N; k++) JOY_cal[k] = ee[k];
  else joycal_thresholds(JOY_cal, 0);              // none yet: the nominal ones
  for(uint8_t k = 0; k < JOYCAL_N; k++)
    if(ee[k] != JOY_cal[k]) { EE_write(JOYCAL_EE, JOY_cal, JOYCAL_N); break; }
}

// returns 1 if A or B cut it short (not the joypad: it is being measured, and a board
// whose joypad reads off must not skip it)
static uint8_t anim_key(void) {
  if(JOY_act_pressed() || JOY_b_pressed()) return 1;
  cal_sample();
  return 0;
}

static uint8_t boot_anim(void) {
  uint32_t t = STK->CNT;
  for(uint8_t f = 0; f < BA_FRAMES; f++) {
    const int8_t *a = BA_TABLE[f];
    for(uint16_t i = 0; i < sizeof(fb); i++) fb[i] = 0;
#ifdef EDITION_PLATANUS
    BLIT(BA_S0, 0, a[0], 255); BLIT(BA_S1, 0, a[1], 255); BLIT(BA_S2, 0, a[2], 255);   // strokes
    BLIT(BA_WORD, 0, 0, (uint8_t)a[3]);              // the wordmark, revealed from the left
    if(a[3] && (uint8_t)a[3] < BA_WORD_W) {          // with a bright edge
      uint8_t *c = fb + (BA_WORD_X + a[3]) * 8;
      for(uint8_t y = BA_WORD_Y - 1; y <= BA_WORD_Y + BA_WORD_H; y++) c[y >> 3] |= 1 << (y & 7);
    }
    BLIT(BA_CAP, 0, 0, (uint8_t)(a[4] * BA_CHAR_W));
    if(a[5]) for(uint8_t x = (uint8_t)a[5]; x < a[5] + 5 && x < 128; x++)
      fb[x * 8 + BA_CAP_Y / 8] |= 0x7F << (BA_CAP_Y & 7); // cursor block, 5 x 7
#else
    BLIT(BA_G2, a[0], a[1], 255);                    // the arrow
    BLIT(BA_G0, a[2], 0, 255); BLIT(BA_G1, a[2], 0, 255);
    BLIT(BA_G3, a[3], 0, 255); BLIT(BA_G4, a[3], 0, 255);
    BLIT(BA_ISO, 0, a[4], 255);
    BLIT(BA_CAP, 0, 0, (uint8_t)(a[5] * BA_CHAR_W));
    if(a[6]) for(uint8_t x = (uint8_t)a[6]; x < a[6] + 5 && x < 128; x++)
      fb[x * 8 + BA_CAP_Y / 8] |= 0x7F << (BA_CAP_Y & 7); // cursor block, 5 x 7
#endif
    show();
    if(anim_key()) return 1;
    frame_wait(&t, 40);                              // 25 fps
  }
  for(uint8_t i = 0; i < 12; i++) { if(anim_key()) return 1; frame_wait(&t, 40); }   // hold ~0.5 s
  return 0;
}

static void fade(uint8_t in) {          // dissolve fb out (in = 0) or in, ~0.25 s
  uint32_t t = STK->CNT;
  for(uint8_t k = 1; k <= 8; k++) { level = in ? k * 2 : 16 - k * 2; show(); frame_wait(&t, 30); }
}

// ---- about box (B): over the card, until any button ---------------------------------
static void px_set(uint8_t x, uint8_t y, uint8_t on) {
  uint8_t *b = &fb[x * 8 + (y >> 3)], m = 1 << (y & 7);
  *b = on ? *b | m : *b & ~m;
}
static void ctext(uint8_t page, const char *s) { text((uint8_t)(64 - len(s) * 3), page, s, 0); }

static void about(void) {
  for(uint8_t x = 14; x <= 113; x++)                  // dark box, light border, y 3..52
    for(uint8_t y = 3; y <= 52; y++)
      px_set(x, y, x == 14 || x == 113 || y == 3 || y == 52);
  ctext(1, "by Platanus");
  blit(BA_ISO, BA_ISO_W, BA_ISO_P, 64 - BA_ISO_W / 2, 17, 255);   // the Platanus banana
  ctext(4, "PLATABOY 26");
  ctext(5, FW_VERSION);
  show();
  while(key());                                      // B still held
  while(!key()) nap(20);                             // any button closes it
  while(key()) nap(20);                              // (and is not taken as a command)
}

// ---- settings (the last card) -------------------------------------------------------
// The MCU runs straight from the cell, so VDD is the battery voltage. Up to 3.35 V the
// cell is taken as a CR2032 (rev. 1.3.x), above as a LiPo (rev. 1.4); above 4.4 V the
// board runs from USB (rev. 1.4). The play time comes from the bench measurement of the
// provider's board at 3.3 V and contrast 127 (menu 17 mA with 16 % of the pixels lit,
// Tiny Pacman 33 mA with 76 %): ~12 mA + ~8 mA per 127 of contrast for a typical game
// (~30 % lit); a CR2032 gives ~120 mAh at that drain, the LiPo 302030 ~130 mAh.
static const uint8_t CONTRAST[8] = { 4, 16, 40, 80, 127, 170, 212, 255 };
static uint8_t contrast = 4;            // level; CONTRAST[4] = 127, the SSD1306 default

static void contrast_set(void) { oled_cmd(OLED_CONTRAST, CONTRAST[contrast], 0xE3); }

// the voltage under load at 100, 80, 60, 40, 20 and 0 % charge
static const uint16_t CURVE[2][6] = {
  { 3000, 2920, 2850, 2790, 2720, 2600 },  // CR2032 at ~20 mA
  { 4150, 3980, 3850, 3770, 3690, 3450 },  // LiPo
};
static uint8_t charge(uint16_t mv, const uint16_t *c) {
  if(mv >= c[0]) return 100;
  for(uint8_t i = 0; i < 5; i++)
    if(mv >= c[i + 1]) return (uint8_t)(100u - 20u * i - 20u * (c[i] - mv) / (unsigned)(c[i] - c[i + 1]));
  return 0;
}

// v as decimal text at p with `point` digits after the decimal point, then the
// suffix; the end
static char *num(char *p, unsigned v, uint8_t point, const char *suffix) {
  char t[6]; uint8_t n = 0;
  do { t[n++] = (char)('0' + v % 10u); v /= 10u; if(n == point) t[n++] = '.'; } while(v || n <= point);
  while(n) *p++ = t[--n];
  while((*p = *suffix++)) p++;
  return p;
}
static void row(uint8_t page, const char *label, const char *val) {
  text(2, page, label, 0);
  text((uint8_t)(126 - len(val) * 6), page, val, 0);
}

__attribute__((noinline)) static void settings_draw(unsigned mv) {
  char v[12];
  for(uint16_t i = 0; i < sizeof(fb); i++) fb[i] = (i & 7) ? 0 : 0xFF;   // page 0: a light bar
  text(40, 0, "SETTINGS", 1);
  num(v, (mv + 5u) / 10u, 2, " V");                                   // "3.21 V"
  row(2, "Battery", v);
  if(mv > 4400) row(3, "Power", "USB");
  else {
    uint8_t lipo = mv > 3350, c = charge(mv, CURVE[lipo]);
    v[0] = '~'; num(v + 1, c, 0, "%");
    row(3, "Charge", v);
    unsigned ma10 = 120u + 80u * CONTRAST[contrast] / 127u;           // 0.1 mA
    num(v + 1, (lipo ? 130u : 120u) * c / ma10, 1, " h");
    row(4, "Play time", v);
  }
  row(6, "Contrast", "");
  text(70, 6, "<", 0);
  text(120, 6, ">", 0);
  for(uint8_t x = 0; x < 40; x++)                                     // 8 cells, 4 x 6
    fb[(78 + x) * 8 + 6] = x % 5u == 4 ? 0 : x / 5u <= contrast || x % 5u == 0 || x % 5u == 3 ? 0x7E : 0x42;
  ctext(7, "</> contrast  A: back");
}

__attribute__((noinline)) static void settings(void) {
  uint8_t old = contrast, saved_bw = bw;
  uint16_t mv = 0;
  bw = 0;                               // no position badge over the footer
  while(key()) nap(20);                 // A still held
  for(uint8_t t = 0;; t++) {
    uint8_t k = key();
    if(k == K_FIRE || k == K_B) break;
    uint8_t c = contrast;
    if(k == K_LEFT && c) c--;
    if(k == K_RIGHT && c < 7) c++;
    if(c != contrast) { contrast = c; contrast_set(); JOY_sound(200, 3); }
    if(!(t & 15) || k) {                // the battery every ~0.3 s, smoothed
      uint16_t m = battery_mv();
      mv = mv ? (uint16_t)((mv * 3u + m) / 4) : m;
      settings_draw(mv); show();
    }
    nap(k ? 150 : 20);
  }
  while(key()) nap(20);
  bw = saved_bw;
  if(contrast != old) EE_write(LIB_SAVE_CONTRAST, &contrast, 1);
}

// ---- input ------------------------------------------------------------------------

static uint8_t key(void) {
  if(JOY_act_pressed())   return K_FIRE;
  if(JOY_b_pressed())     return K_B;     // (also keeps learning PA2's idle level)
  if(JOY_left_pressed())  return K_LEFT;
  if(JOY_right_pressed()) return K_RIGHT;
  return K_NONE;
}

int main(void) {
  JOY_init();
  sleep_init();
  OLED_send_command(OLED_DISPLAY_OFF);             // (OLED_init turned it on) until the RAM,
  DLY_ms(5);                                       // random at power-on, is cleared
  oled_cmd(OLED_MEMORYMODE, 0x01, 0xE3);          // vertical addressing (0xE3: no-op)
  uint8_t c = 0xFF;                                // the contrast of the settings (always
  EE_read(LIB_SAVE_CONTRAST, &c, 1);               // set: a game may have left another)
  if(c < sizeof CONTRAST) contrast = c;
  contrast_set();
  show();                                          // black: fb is still empty
  OLED_send_command(OLED_DISPLAY_ON);
  if(EE_read(LIB_HDR, (uint8_t *)&hdr, sizeof hdr) != EE_OK || hdr.magic != LIB_MAGIC || hdr.count < 2) {
    for(uint16_t i = 0; i < sizeof(fb); i++) fb[i] = 0;
    message("NO GAME LIBRARY", "IN THE EEPROM");
    while(1);
  }
  count = hdr.count - 1;
  if(count > LIB_DIR_MAX - 1) count = LIB_DIR_MAX - 1;

  // power-on (or a reset pin): the boot animation; after an exit from a game (a software
  // reset by the kernel's install) straight to the card. The flags are cleared here, so
  // the reset after the next install reads as software only (the kernel starts the game).
  uint8_t boot = (RCC->RSTSCKR & (RCC_PORRSTF | RCC_PINRSTF)) != 0;
  RCC->RSTSCKR |= RCC_RMVF;

  uint8_t sel = 0, last = 0xFF;
  EE_read(LIB_SAVE_MENU, &last, 1);
  if(last < count) sel = last;
  if(boot) boot_anim();
  cal_update(boot);                      // the joypad thresholds, for the menu and the games
  if(boot) {
    while(key());                        // a button that skipped it: wait for its release
    fade(0);
    level = 0;
  }
  card_full(sel);
  position(sel);
  if(boot) fade(1); else show();
  battery_check();

  while(JOY_act_pressed());              // still held from the power-on: wait
  uint8_t prev = K_NONE, tick = 0;
  while(1) {
    if(!++tick) battery_check();         // every 256 x ~20 ms
    uint8_t k = key();
    if(k == K_RIGHT) { sel = sel < count ? sel + 1 : 0; position(sel); JOY_sound(200, 3); slide(sel, 1); }
    if(k == K_LEFT)  { sel = sel ? sel - 1 : count;    position(sel); JOY_sound(200, 3); slide(sel, 0); }
    if(k == K_B && prev != K_B) { JOY_sound(160, 3); about(); card_full(sel); show(); k = K_NONE; }
    if(prev == K_FIRE && k != K_FIRE && sel == count) {   // released on the settings
      JOY_sound(160, 3); settings(); card_full(sel); show(); k = K_NONE;
    }
    else if(prev == K_FIRE && k != K_FIRE) {   // released: start the selection
      char name[16];
      entry_name(sel, name);
      message("LOADING", name);
      if(last != sel) EE_write(LIB_SAVE_MENU, &sel, 1);
      JOY_sound(120, 20);
      LIB_install(sel + 1, CONTRAST[contrast]);
    }
    prev = k;
    nap(20);
  }
}
