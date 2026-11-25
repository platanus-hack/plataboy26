// ===================================================================================
// Llegamos tarde a la hackathon - port for the CH32V003 game console
// ===================================================================================
//
// Original: Paolo (paoloose), Platanus Hack 25 arcade (github.com/paoloose/
// platanus-hack-25-arcade, game.js; Phaser, 320 x 240, color). A Crazy Climber: the
// hacker arrived late, the guard locked the doors, so up the glass building it goes,
// dodging the office objects that fall from above, with the guard climbing after.
//
// What is kept: the game's rules and numbers. The simulation runs in the original's
// world units (320 x 240 px, rows of 12 px, the building 200 px wide in 13 columns, the
// goal at row 150) at the original's 60 updates per second, with its timings, speeds,
// collision boxes, guard AI, obstacle spawning (giant balls at 50 % and 80 %), lives,
// the intro and the ending dialogs (Spanish, as in the original).
// The compromises:
//   - drawn at 1/3 scale on the 128 x 64 monochrome OLED (the building is 67 px wide, a
//     floor 4 px); the sprites at 1/2 (15 x 15), so they look a bit bigger than their
//     collision boxes; the view shows 192 of the original's 240 px of height
//   - one player (no 2-player mode)
//   - balance: a hit drops one row (game.js drops two, though it means one) and the guard
//     climbs at 0.923 x (1.51 rows/s against the player's 2.23): with game.js's numbers a
//     climbing burst under 600 ms (the dodges reset the cycle) gains one row instead of
//     two, and a player who dodges for ~0.4 s of each second could no longer outrun it
//   - sprites reduced to silhouettes (climbing) and 3-tone portraits (tools/gen_data.py)
//   - the falling emoji are 10 small icons; one-channel beeps instead of Web Audio
//   - A skips the current dialog of the intro / ending (the original only lets the
//     ending be skipped)
//   - the camera rests a bit lower (the ground stays in view at the start)
//   - objects fall at 0.8 x the original's speed: the view shows 192 / 240 = 0.8 of its
//     height, so they keep the same warning time on screen
//   - B in the character selection: back to the title; UP + DOWN 2 s: back to the menu
// Platanus Hack 26 edition: the hackers are the Platanus team (tools/team.py, drawn after
// their photos; one shared climbing silhouette) plus Condorito, Arturo Vidal and Bodoque;
// the title also credits the author (paoloose) and this edition.
// Build with -DLT_GODMODE for tests (no damage, the guard never catches).
// Nothing is saved (like the original).

#include "arduboy.h"
#include "game_data.h"

// ---- the original's constants (game.js, C) ------------------------------------------
#define BAC      13                     // columns players can be at (0..12)
#define ROWS     160
#define GOAL     150
#define STEP     6                      // 100 ms per animation state
#define OSR      10                     // obstacles from this row on
#define F_FALL1  36                     // fall one row: 600 ms
#define F_ENTER  48                     // entering the window: 800 ms
#define GRAV     36                     // 500 px/s^2, in Q8 px per frame^2
#define FPS      60

// Q8 world coordinates (px * 256)
#define colToX(c8) ((60L << 8) + (int32_t)(c8) * 200 / 12)
#define rowToY(r)  ((int32_t)(220 - (r) * 12) << 8)

// animation states
enum { A_I, A_R0, A_R1, A_LUP0, A_LUP1, A_F };
static const uint8_t CYCLE[9] = { A_I, A_R0, A_R1, A_R0, A_I, A_LUP0, A_LUP1, A_LUP0, A_I };
static const uint8_t HCYCLE[2] = { A_LUP0, A_R0 };

// player states
enum { P_OK, P_FALL1, P_FALL, P_DEAD, P_ENTER, P_WON, P_CAUGHT };

typedef struct {
  int32_t x, y, tx, ty;                 // Q8 world px, and the target they ease to
  int32_t vel;                          // falling: Q8 px per frame
  int16_t col;                          // Q8 columns
  int16_t row;
  uint16_t t;                           // state timer (frames)
  uint8_t anim, animIdx, animT;
  uint8_t climbing, movH, hIdx;
  uint8_t state;
  int8_t lives;
  uint8_t spr;                          // hacker 0..N_HACKERS-1, the guard GUARD_IDX
  uint8_t lostSt;
} Actor;

typedef struct { int16_t x; int32_t y; uint8_t spd, icon; } Obstacle;   // spd: Q8 px/frame
typedef struct { int16_t x, y; uint8_t delay, spd; } Pending;           // y: world px

#define MAX_OBS  16
#define MAX_PEND 12

static Actor pl, gd;
static Obstacle obs[MAX_OBS];
static Pending pend[MAX_PEND];
static uint8_t nObs, nPend;
static int32_t camY;                    // Q8 world px: the camera's center
static uint16_t obsT, obsInt, obsRounds;
static uint8_t obsPause, guardOn;
static uint32_t frames, startF, endF;   // 60 Hz frame counter, run start / end
static uint8_t hacker;                  // the chosen one

// (struct copies: no C library)
__attribute__((used)) void *memcpy(void *d, const void *s, unsigned n) {
  uint8_t *a = d; const uint8_t *b = s;
  while(n--) *a++ = *b++;
  return d;
}

// ---- random -------------------------------------------------------------------------
static int16_t rnd(int16_t lo, int16_t hi) { return lo + (int16_t)AB_random(hi - lo + 1); }  // lo..hi

// ---- sound: one-channel scores (arduboy.c) ------------------------------------------
// 0x90 n: note n on, 0x80: off, 0xF0: end, 2 bytes: wait (ms). Priorities: lower wins.
static const uint8_t SND_SELECT[] = { 0x90, 69, 0, 80, 0x80, 0xF0 };
static const uint8_t SND_CONFIRM[] = { 0x90, 81, 0, 120, 0x80, 0xF0 };
static const uint8_t SND_TRANS[] = { 0x90, 86, 0, 100, 0x90, 74, 0, 200, 0x80, 0xF0 };
static const uint8_t SND_HIT[] = { 0x90, 50, 0, 30, 0x90, 40, 0, 100, 0x80, 0xF0 };
static const uint8_t SND_VICTORY[] = { 0x90, 72, 0, 150, 0x90, 76, 0, 150, 0x90, 79, 0, 150,
                                       0x90, 84, 1, 144, 0x80, 0xF0 };
static const uint8_t SND_LOSS[] = { 0x90, 69, 0, 200, 0x90, 67, 0, 200, 0x90, 65, 0, 200,
                                    0x90, 64, 1, 244, 0x80, 0xF0 };
static uint8_t sndBlip[6] = { 0x90, 0, 0, 0, 0x80, 0xF0 };

static void blip(uint8_t note, uint8_t ms, uint8_t prio) {   // step, move, voices
  sndBlip[1] = note; sndBlip[3] = ms;
  AB_playScore2(sndBlip, prio);
}

// ---- drawing helpers ----------------------------------------------------------------
static int16_t div3(int16_t v) { return v >= 0 ? v / 3 : -((2 - v) / 3); }
static int16_t sx(int32_t x8) { return (int16_t)(((x8 >> 8) + 32) / 3); }
static int16_t sy(int32_t y8) { return div3((int16_t)((y8 - camY) >> 8)) + 39; }

static uint8_t flip[30];
static const uint8_t *mirrored(const uint8_t *b) {          // a 15 x 15 sprite, mirrored
  for(uint8_t p = 0; p < 2; p++)
    for(uint8_t c = 0; c < 15; c++) flip[p * 15 + c] = b[p * 15 + 14 - c];
  return flip;
}

// a sprite with a dark outline, so it stays readable over the building's grid
static void outlined(int16_t x, int16_t y, const uint8_t *b, uint8_t w, uint8_t h) {
  AB_drawBitmap(x - 1, y, b, w, h, BLACK); AB_drawBitmap(x + 1, y, b, w, h, BLACK);
  AB_drawBitmap(x, y - 1, b, w, h, BLACK); AB_drawBitmap(x, y + 1, b, w, h, BLACK);
  AB_drawBitmap(x, y, b, w, h, WHITE);
}

static const uint8_t *front(uint8_t spr) {                 // the portrait, halved
  uint8_t *frontBuf = flip;
  const uint8_t *p = SPR_PORTRAIT + spr * 120;
  for(uint8_t i = 0; i < 30; i++) frontBuf[i] = 0;
  for(uint8_t y = 0; y < 15; y++)
    for(uint8_t x = 0; x < 15; x++) {
      uint8_t n = 0;
      for(uint8_t k = 0; k < 4; k++) {
        uint8_t yy = 2 * y + (k >> 1), xx = 2 * x + (k & 1);
        n += (p[(yy >> 3) * 30 + xx] >> (yy & 7)) & 1;
      }
      if(n >= 2) frontBuf[(y >> 3) * 15 + x] |= 1 << (y & 7);
    }
  return frontBuf;
}

static const uint8_t *frame(uint8_t spr, uint8_t anim) {
  const uint8_t *c = SPR_CLIMB + CLIMB_SET(spr) * 90;
  switch(anim) {
    case A_R0:   return c + 30;
    case A_R1:   return c + 60;
    case A_LUP0: return mirrored(c + 30);
    case A_LUP1: return mirrored(c + 60);
    case A_F:    return front(spr);
    default:     return c;
  }
}

// ---- text ---------------------------------------------------------------------------
static const uint8_t *glyph(uint8_t c) {
  return FONT + 5 * (c >= 0x80 ? 96 + (c - 0x80) : (c >= 32 && c < 128 ? c - 32 : 0));
}
static uint8_t slen(const char *s) { uint8_t n = 0; while(s[n]) n++; return n; }

static void text(int16_t x, int16_t y, const char *s, uint8_t n, uint8_t color) {
  for(uint8_t i = 0; i < n && s[i]; i++, x += 6) AB_drawBitmap(x, y, glyph((uint8_t)s[i]), 5, 8, color);
}
static void ctext(int16_t y, const char *s, uint8_t color) {    // centered
  uint8_t n = slen(s);
  text(64 - n * 3, y, s, n, color);
}
static void ctextBox(int16_t y, const char *s) {                // centered, on black
  uint8_t n = slen(s);
  AB_fillRect2(63 - n * 3, y - 1, n * 6 + 1, 10, BLACK);
  text(64 - n * 3, y, s, n, WHITE);
}

static void small(int16_t x, int16_t y, const char *s) {       // the 3 x 5 font
  for(; *s; s++, x += 4) {
    const char *k = SMALL_CHARS; uint8_t i = 0;
    while(k[i] && k[i] != *s) i++;
    if(!k[i]) continue;
    AB_drawBitmap(x, y, FONT_SMALL + i * 3, 3, 5, WHITE);
  }
}
static char *mmss(char *b, uint16_t s) {                       // "m:ss"
  char *p = b;
  if(s >= 600) *p++ = '0' + s / 600;
  *p++ = '0' + (s / 60) % 10; *p++ = ':';
  *p++ = '0' + (s % 60) / 10; *p++ = '0' + s % 10; *p = 0;
  return b;
}

// ---- dialog box: typewriter text with a voice beep, the speaker's portrait ------------
// (Dialog in game.js: 30 ms per character, a 50 ms beep for each one but spaces;
// the guard's voice 275-350 Hz, the hacker's 450-600 Hz; the hacker's portrait on the
// right, the guard's on the left)
#define DLG_W 15                        // characters per line beside the portrait
static char dlgText[80];
static uint8_t dlgOn, dlgSpk, dlgShown, dlgLen, dlgT, dlgDone;
static uint8_t dlgLine[7], dlgLines;

static void dlgShow(uint8_t speaker, const char *a, const char *b, const char *c) {
  uint8_t n = 0;
  for(const char *s = a; s && *s && n < sizeof dlgText - 1; ) dlgText[n++] = *s++;
  for(const char *s = b; s && *s && n < sizeof dlgText - 1; ) dlgText[n++] = *s++;
  for(const char *s = c; s && *s && n < sizeof dlgText - 1; ) dlgText[n++] = *s++;
  dlgText[n] = 0;
  dlgLen = n; dlgShown = 0; dlgT = 0; dlgDone = 0; dlgOn = 1; dlgSpk = speaker;
  // word wrap
  dlgLines = 0;
  uint8_t i = 0;
  while(i < n && dlgLines < sizeof dlgLine) {
    dlgLine[dlgLines++] = i;
    uint8_t end = i + DLG_W, brk = 0;
    if(end >= n) break;
    for(uint8_t k = i; k <= end; k++) if(dlgText[k] == ' ') brk = k;
    i = brk > i ? brk + 1 : end;
  }
}
static void dlgHide(void) { dlgOn = 0; }

static void dlgUpdate(void) {
  if(!dlgOn || dlgDone) return;
  if(++dlgT < 2) return;                // ~30 ms per character
  dlgT = 0;
  if(dlgShown < dlgLen) {
    char c = dlgText[dlgShown++];
    if(c != ' ') blip(dlgSpk == GUARD_IDX ? rnd(61, 65) : rnd(69, 74), 50, 3);
  } else dlgDone = 1;
}

static void dlgDraw(void) {
  if(!dlgOn) return;
  uint8_t h = dlgLines * 8 + 4;
  if(h < 34) h = 34;
  AB_fillRect2(0, 0, 128, h, BLACK);
  AB_drawRect2(0, 0, 128, h, WHITE);
  uint8_t left = dlgSpk == GUARD_IDX;                   // portrait side
  int16_t px = left ? 2 : 96, tx = left ? 35 : 4;
  AB_drawBitmap(px, 2, SPR_PORTRAIT + dlgSpk * 120, 30, 30, WHITE);
  for(uint8_t l = 0; l < dlgLines; l++) {
    uint8_t a = dlgLine[l], b = l + 1 < dlgLines ? dlgLine[l + 1] : dlgLen;
    if(a >= dlgShown) break;
    if(b > dlgShown) b = dlgShown;
    uint8_t n = b - a;
    if(n && dlgText[a + n - 1] == ' ') n--;
    text(tx, 2 + l * 8, dlgText + a, n, WHITE);
  }
}

// ---- background: Los Andes, the Santiago skyline, clouds (DynamicBackground) ---------
// Drawn on the screen at their parallax; cd = camera's offset from its first position.
static uint16_t hash(uint16_t v) { v ^= v << 7; v ^= v >> 9; v ^= v << 8; return v * 40503u; }
static uint16_t cloudX[6];

static void background(int16_t cd) {
  // mountains (tools/gen_data.py): their outline, 10 % of the camera's movement
  int16_t base = div3(200 - 120 - cd / 10) + 39;
  for(uint8_t x = 0; x < 128; x++) AB_drawPixel(x, base - MOUNTAINS[x], WHITE);
  // skyline beside the building: outlines and lit windows, 35 % of the movement
  int16_t gb = div3(240 - 120 - cd * 35 / 100) + 39;
  for(uint8_t i = 0; i < N_SKY; i++) {
    int16_t x = (int8_t)SKYLINE[i * 3]; uint8_t w = SKYLINE[i * 3 + 1], h = SKYLINE[i * 3 + 2];
    AB_fillRect2(x, gb - h, w, h + 1, BLACK);
    AB_drawRect2(x, gb - h, w, h + 1, WHITE);
    for(int16_t wy = gb - h + 3; wy < gb - 2; wy += 3)
      for(int16_t wx = x + 2; wx < x + w - 2; wx += 3)
        if((hash(wx * 31 + wy * 17) & 3) != 0) AB_drawPixel(wx, wy, WHITE);
  }
  // clouds: drift right; they wrap vertically so the sky keeps moving while climbing
  for(uint8_t i = 0; i < 6; i++) {
    int16_t cy = (int16_t)((i * 37u + (uint16_t)(-cd / 8 + 960)) % 96) - 16;
    int16_t cx = (int16_t)(cloudX[i] >> 6) % 160 - 16;
    for(int8_t k = -6; k <= 6; k += 2) {
      AB_drawPixel(cx + k, cy, WHITE);
      if(k > -5 && k < 5) AB_drawPixel(cx + k + 1, cy - 1, WHITE);
      if(k > -3 && k < 3) AB_drawPixel(cx + k, cy - 2, WHITE);
    }
  }
}
static void cloudsMove(void) { for(uint8_t i = 0; i < 6; i++) cloudX[i] += 1 + i % 3; }

// ---- the building (drawBld): glass, columns, floors, the sign, the goal floor --------
static void building(void) {
  int16_t top = sy(rowToY(ROWS)), bot = sy(240L << 8);
  if(top < 0) top = 0;
  if(bot > 63) bot = 63;
  AB_fillRect2(31, top, 67, bot - top + 1, BLACK);
  for(uint8_t c = 0; c <= 6; c++) AB_drawFastVLine2(31 + c * 11, top, bot - top + 1, WHITE);
  // floors in view
  int16_t r0 = (int16_t)((220 - ((camY >> 8) + 125)) / 12) - 1, r1 = r0 + 18;
  if(r0 < 0) r0 = 0;
  if(r1 > ROWS - 1) r1 = ROWS - 1;
  for(int16_t r = r0; r <= r1; r++) {
    int16_t y = sy(rowToY(r));
    if(y < 0 || y > 63) continue;
    if(r == GOAL) {                                   // open windows, lit frames
      for(uint8_t c = 0; c < 6; c++) {
        AB_drawRect2(33 + c * 11, y - 4, 8, 4, WHITE);
      }
      AB_drawFastHLine2(31, y, 67, WHITE);
    } else
      for(int16_t x = 32 + (r & 1); x < 97; x += 2) AB_drawPixel(x, y, WHITE);
  }
  // "PLATANUS HACK 25" sign above the goal floor, lights blinking every 300 ms
  int16_t gy = sy(rowToY(GOAL)) - 15;
  if(gy > -12 && gy < 64) {
    AB_fillRect2(31, gy, 67, 9, WHITE);
    AB_fillRect2(32, gy + 1, 65, 7, BLACK);
    small(33, gy + 2, "PLATANUS HACK 26");
    uint8_t on = (frames / 18) & 1;
    for(uint8_t i = 0; i < 8; i++) {
      AB_drawPixel(33 + i * 9 + on, gy - 2, WHITE);
      AB_drawPixel(33 + i * 9 + !on, gy + 10, WHITE);
    }
  }
  // the ground: sidewalk
  int16_t g = sy(230L << 8);
  if(g < 64) {
    AB_fillRect2(0, g, 128, 64 - g, BLACK);
    AB_drawFastHLine2(0, g, 128, WHITE);
    for(int16_t y = g + 3; y < 64; y += 3)
      for(int16_t x = (y & 4) ? 0 : 6; x < 128; x += 13) AB_drawFastHLine2(x, y, 5, WHITE);
  }
}

// ---- actors -------------------------------------------------------------------------
static void actorInit(Actor *a, uint8_t spr, int16_t col8, int16_t row) {
  for(uint8_t *p = (uint8_t *)a; p < (uint8_t *)(a + 1); p++) *p = 0;
  a->spr = spr; a->col = col8; a->row = row;
  a->x = a->tx = colToX(col8); a->y = a->ty = rowToY(row);
  a->lives = 3; a->anim = A_F;
}

static void ease(Actor *a) {                          // Phaser.Math.Linear 0.2 / 0.15
  a->x += (a->tx - a->x) / 5;
  a->y += (a->ty - a->y) * 38 / 256;
}

// advClAnim / advCA: one step of the climbing cycle; LUP1 and R1 move a row
static void advance(Actor *a, uint8_t down, uint8_t player) {
  a->animIdx = (a->animIdx + 1) % 9;
  a->anim = CYCLE[a->animIdx];
  if(a->anim == A_LUP1 || a->anim == A_R1) {
    int16_t r = down ? a->row - 1 : a->row + 1;
    if(r >= 0 && r <= ROWS) {
      a->row = r; a->ty = rowToY(r);
      if(player) blip(rnd(57, 60), 80, 2);
    }
  }
  if(!player && a->anim == A_I && a->animIdx == 0) a->climbing = 0;
}

static uint8_t hits(const Actor *a, int32_t x8, int32_t y8, int16_t w, int16_t h) {
  int16_t ax = a->x >> 8, ay = a->y >> 8, bx = x8 >> 8, by = y8 >> 8;
  return !(ax + 7 < bx - w / 2 || ax - 7 > bx + w / 2 || ay + 13 < by - h / 2 || ay - 13 > by + h / 2);
}

static void die(Actor *a) {
  a->lives = 0; a->state = P_FALL; a->vel = -853;     // -200 px/s: a little jump first
  a->tx = a->x; a->ty = a->y; a->climbing = a->movH = 0; a->t = 0;
}

static void takeDmg(Actor *a) {
  if(a->state == P_DEAD || a->state == P_FALL) return;
#ifdef LT_GODMODE                                     // tests only: no damage, no guard
  return;
#endif
  a->lives--;
  AB_playScore2(SND_HIT, 1);
  if(a->lives <= 0) die(a);
  else if(a->row > 0) {                               // fall one row (climbing down)
    a->state = P_FALL1; a->t = 0; a->climbing = 1;
    a->animIdx = 5; a->anim = A_LUP0; a->animT = 0;
  }
}

static void playerUpdate(Actor *a, uint8_t u, uint8_t d, uint8_t l, uint8_t r) {
  switch(a->state) {
    case P_CAUGHT:                                    // struggling: LUP0 / R0 every 200 ms
      if(++a->t >= 12) { a->t = 0; a->lostSt = !a->lostSt; }
      a->anim = a->lostSt ? A_R0 : A_LUP0;
      return;
    case P_FALL:                                      // Mario-like fall to the ground
      a->vel += GRAV; a->y += a->vel; a->tx = a->x; a->ty = a->y; a->t++;
      a->anim = (a->t / 6) & 1 ? A_R1 : A_LUP1;
      if(a->y >= (240L << 8)) { a->y = 240L << 8; a->state = P_DEAD; }
      return;
    case P_FALL1:
      a->t++;
      // only the first 3 steps (LUP1 moves a row): game.js's 600 ms also reach R1, a
      // second row, though it means to drop one
      if(++a->animT >= STEP && a->t <= 3 * STEP) { a->animT = 0; advance(a, 1, 1); }
      ease(a);
      if(a->t >= F_FALL1) { a->state = P_OK; a->t = 0; a->climbing = 0; }
      return;
    case P_DEAD: case P_WON:
      return;
    case P_ENTER:                                     // into the building, fading
      a->x += ((160L << 8) - a->x) / 50; a->tx = a->x;
      if(++a->t >= F_ENTER) a->state = P_WON;
      return;
  }
  if(l || r) {
    int8_t dir = l ? -1 : 1;
    if(a->climbing) { a->climbing = 0; a->animIdx = 0; }
    if(!a->movH) { a->movH = 1; a->hIdx = 0; a->animT = 0; a->animIdx = 0; }
    int16_t c = a->col + dir * 15;                    // 0.06 columns per frame
    if(c < 0) c = 0;
    if(c > (BAC - 1) * 256) c = (BAC - 1) * 256;
    a->col = c; a->tx = colToX(c);
    if(++a->animT >= STEP) { a->animT = 0; a->hIdx ^= 1; blip(rnd(54, 56), 60, 2); }
    a->anim = HCYCLE[a->hIdx];
  } else if(a->movH) { a->movH = 0; a->anim = A_I; a->hIdx = 0; }
  if(!a->movH) {
    if(u != d) {
      if(!a->climbing) { a->climbing = 1; a->animT = 0; }
      if(++a->animT >= STEP) { a->animT = 0; advance(a, d, 1); }
    } else a->climbing = 0;
  }
  ease(a);
}

// the guard: chases the player (nearest), climbing on a 530..670 ms rhythm (here 0.923 x
// that speed, see the header), slower
// (x 0.75) while the player is below row 10, sideways at 0.02 columns per frame.
// (game.js also meant to speed it up x 2.5 below the camera, but reads an undefined
// camera variable, so that never happens: kept that way)
static uint16_t gTmr, gCD;                            // Q8 frames
static uint16_t guardCD(void) { return (uint16_t)rnd(8820, 11149); }   // 574..726 ms: 530..670 / 0.923

static void guardUpdate(Actor *g, const Actor *p) {
  if(g->state == P_FALL) { g->vel += GRAV; g->y += g->vel; g->ty = g->y; return; }
  if(p->state == P_DEAD || p->state == P_FALL) { ease(g); return; }
  uint16_t sM = p->row < OSR ? 192 : 256;
  gTmr += sM;
  if(gTmr >= gCD) {
    gTmr = 0; gCD = guardCD();
    if(g->row < p->row) {
      if(!g->climbing) { g->climbing = 1; g->animT = 0; g->animIdx = 0; }
      advance(g, 0, 0);
    } else if(g->row > p->row) {
      if(!g->climbing) { g->climbing = 1; g->animT = 0; g->animIdx = 5; }
      advance(g, 1, 0);
    } else g->climbing = 0;
  }
  if(g->climbing) {
    g->animT += sM >> 6;                              // (Q2 frames: STEP * 4)
    if(g->animT >= 26) { g->animT = 0; advance(g, g->row > p->row, 0); }   // 108 ms: 100 / 0.923
  }
  int16_t dif = p->col - g->col;
  if(dif > 128 || dif < -128) {
    int16_t c = g->col + (dif > 0 ? 1 : -1) * (int16_t)((sM * 1311UL) >> 16);   // 0.02 * sM
    if(c < 0) c = 0;
    if(c > (BAC - 1) * 256) c = (BAC - 1) * 256;
    g->col = c; g->tx = colToX(c);
  }
  ease(g);
}

static void actorDraw(const Actor *a) {
  if(a->state == P_WON) return;
  if(a->state == P_ENTER && (a->t > F_ENTER / 2) && (frames & 1)) return;   // fading out
  int16_t x = sx(a->x) - 7, y = sy(a->y) - 7;
  if(y < -15 || y > 63) return;
  outlined(x, y, frame(a->spr, a->anim), 15, 15);
}

// ---- obstacles (spawnObs, spawnGiantBall) --------------------------------------------
static void obsAdd(int16_t x, int16_t y, uint8_t spd) {
  if(nObs >= MAX_OBS) return;
  Obstacle *o = &obs[nObs++];
  o->x = x; o->y = (int32_t)y << 8; o->spd = spd ? spd : (uint8_t)rnd(307, 375) - 256;  // 72..88 px/s: 90..110 x 0.8
  o->icon = (uint8_t)rnd(0, N_ICONS - 1);
}
static void pendAdd(int16_t x, int16_t y, uint8_t delay, uint8_t spd) {
  if(!delay) { obsAdd(x, y, spd); return; }
  if(nPend >= MAX_PEND) return;
  Pending *q = &pend[nPend++];
  q->x = x; q->y = y; q->delay = delay; q->spd = spd;
}
// offsets of the giant ball: 8 objects on a circle of radius 25
static const int8_t BALL[8][2] = { {25, 0}, {18, 18}, {0, 25}, {-18, 18}, {-25, 0}, {-18, -18}, {0, -25}, {18, -18} };
static void giantBall(int16_t cx, int16_t cy) {
  for(uint8_t i = 0; i < 8; i++) pendAdd(cx + BALL[i][0], cy + BALL[i][1], i * 3, 17);   // 64 px/s: 80 x 0.8
}

static void spawnObs(void) {
  if(pl.state != P_OK && pl.state != P_FALL1) return;
  if(pl.row < OSR) return;
  int16_t topY = (int16_t)(camY >> 8) - 120 - 50;
  uint8_t gb = obsRounds % 8 == 0;
  int16_t prog = pl.row * 100 / GOAL;
  if(gb && prog >= 80 && prog < 100) { giantBall(rnd(60, 260), topY); giantBall(rnd(60, 260), topY); return; }
  if(gb && prog >= 50 && prog < 80) { giantBall(rnd(60, 260), topY); return; }
  int16_t x = (int16_t)(pl.x >> 8) + rnd(-20, 20);
  if(x < 40) x = 40;
  if(x > 280) x = 280;
  pendAdd(x, topY, 0, 0);
  if(rnd(0, 99) < 20) pendAdd(rnd(60, 260), topY, 9, 0);   // + a random one, 150 ms later
}

static void obsUpdate(void) {
  for(uint8_t i = 0; i < nPend; ) {
    if(!--pend[i].delay) { obsAdd(pend[i].x, pend[i].y, pend[i].spd); pend[i] = pend[--nPend]; }
    else i++;
  }
  int16_t cy = camY >> 8;
  for(uint8_t i = 0; i < nObs; ) {
    Obstacle *o = &obs[i];
    o->y += o->spd + 256;
    int16_t y = o->y >> 8;
    if(y > cy + 220 || y < cy - 220) *o = obs[--nObs];
    else i++;
  }
}

static void obsDraw(void) {
  for(uint8_t i = 0; i < nObs; i++) {
    int16_t x = sx((int32_t)obs[i].x << 8) - 3, y = sy(obs[i].y) - 3;
    if(y > -7 && y < 64) outlined(x, y, ICONS + obs[i].icon * 7, 7, 7);
  }
}

// ---- scenes -------------------------------------------------------------------------
enum { SC_TITLE, SC_SELECT, SC_GAME, SC_END };
enum { G_CINE, G_PLAY, G_OVER, G_VICT };
static uint8_t scene, gstate, endWin, selOk;
static uint16_t st;                                   // frames in the current scene / state
static uint8_t cineIdx, vIdx, vWait;
static uint16_t evAt;                                 // next scheduled event (frames)

static void toScene(uint8_t s) {
  scene = s; st = 0; selOk = 0; dlgHide();
  if(s != SC_GAME) camY = 120L << 8;                // the title's view: the street
}

// title: falling objects in the background (DynamicBackground with obstacles)
static void titleUpdate(void) {
  cloudsMove();
  if(st % 60 == 0) obsAdd(rnd(40, 280), -50, 0);    // one every second, anywhere
  obsUpdate();
  if(AB_buttonDown(A_BUTTON) && st > 20) { AB_playScore2(SND_TRANS, 2); toScene(SC_SELECT); }
}
static void titleDraw(void) {
  background(0);
  obsDraw();
  // title bar, light with dark letters, with animated corner brackets
  AB_fillRect2(8, 5, 112, 20, WHITE);
  ctext(7, T_TITLE1, BLACK);
  ctext(16, T_TITLE2, BLACK);
  uint8_t k = 4 + ((st / 12) & 1);
  AB_drawFastHLine2(5 - k + 4, 2, k, WHITE); AB_drawFastVLine2(4, 2, k, WHITE);
  AB_drawFastHLine2(124 - k, 2, k, WHITE); AB_drawFastVLine2(123, 2, k, WHITE);
  AB_drawFastHLine2(5 - k + 4, 27, k, WHITE); AB_drawFastVLine2(4, 28 - k, k, WHITE);
  AB_drawFastHLine2(124 - k, 27, k, WHITE); AB_drawFastVLine2(123, 28 - k, k, WHITE);
  ctextBox(31, T_EDITION);
  // "Presiona A" (blinking), the author, again, this edition
  uint8_t ph = (st / 120) & 3;
  if(ph == 1) ctextBox(44, T_BY);
  else if(ph == 3) ctextBox(44, T_ED);
  else if((st / 20) % 3) ctextBox(44, T_PRESS);
}

static void selectUpdate(void) {
  if(AB_buttonDown(LEFT_BUTTON))  { hacker = hacker ? hacker - 1 : N_HACKERS - 1; AB_playScore2(SND_SELECT, 2); }
  if(AB_buttonDown(RIGHT_BUTTON)) { hacker = (hacker + 1) % N_HACKERS; AB_playScore2(SND_SELECT, 2); }
  if(AB_buttonDown(B_BUTTON)) { toScene(SC_TITLE); return; }
  if(AB_buttonDown(A_BUTTON) && !selOk) { selOk = 1; st = 0; AB_playScore2(SND_CONFIRM, 2); }
  if(selOk && st >= 30) {                             // "LISTO!" shown for 0.5 s
    toScene(SC_GAME);
    // GameScene.create
    actorInit(&pl, hacker, 16 * 256, 0);              // walks in from the right
    actorInit(&gd, GUARD_IDX, 3 * 256 + 128, 0);
    gstate = G_CINE; cineIdx = 0; evAt = 120;
    nObs = nPend = 0; obsT = 0; obsInt = 60; obsRounds = 0; obsPause = 0; guardOn = 0;
    camY = 120L << 8; gTmr = 0; gCD = guardCD();
    AB_playScore2(SND_TRANS, 2);
  }
}
static void selectDraw(void) {
  ctext(0, T_CHOOSE, WHITE);
  AB_drawBitmap(49, 11, SPR_PORTRAIT + hacker * 120, 30, 30, WHITE);
  if((st / 15) & 1) { text(36, 22, "<", 1, WHITE); text(87, 22, ">", 1, WHITE); }
  ctext(44, NAMES[hacker], WHITE);
  // the team member's company (T_ED ends in "Platanus")
  if(hacker < N_TEAM) ctext(54, hacker < N_PLATANUS ? T_ED + 8 : "Buk", WHITE);
  if(selOk) { AB_fillRect2(40, 53, 48, 11, WHITE); ctext(55, T_READY, BLACK); }
}

// the intro (CinematicController): walk in, dialogs, the first 5 rows, the guard's
// shout; A skips to the next step
static void cineUpdate(void) {
  uint8_t skip = AB_buttonDown(A_BUTTON) && st < evAt;
  if(skip) st = evAt;
  if(cineIdx == 0) {                                  // walking in: col 16 -> 9 in 2 s
    uint16_t t = st < 120 ? st : 120;
    pl.col = 16 * 256 - (int32_t)7 * 256 * t / 120;
    pl.x = pl.tx = colToX(pl.col);
    pl.y = pl.ty = rowToY(0) - ((st < 120 && ((st >> 2) & 1)) ? 3 * 256 : 0);
  }
  if(st < evAt) return;
  switch(cineIdx++) {
    case 0: dlgShow(hacker, D_HELLO, 0, 0); evAt = 330; break;
    case 1: dlgShow(GUARD_IDX, D_SORRY, 0, 0); evAt = 600; break;
    case 2: dlgShow(hacker, D_WHAT, 0, 0); evAt = 780; break;
    case 3: case 4: case 5: case 6: case 7:          // climb 5 rows, 300 ms apart
      dlgHide();
      pl.row++; pl.ty = rowToY(pl.row); pl.anim = (cineIdx & 1) ? A_LUP1 : A_R1;
      evAt = cineIdx < 8 ? st + 18 : st + 30;
      break;
    case 8: dlgShow(GUARD_IDX, D_WHERE, 0, 0); evAt = st + 120; break;
    case 9:
      dlgHide(); gstate = G_PLAY; startF = frames;
      pl.anim = A_I; pl.animIdx = 0;
      break;
  }
}

static void endGame(uint8_t win) {
  toScene(SC_END); endWin = win;
}

static void gameUpdate(void) {
  uint8_t u = 0, d = 0, l = 0, r = 0;
  cloudsMove();
  dlgUpdate();
  if(gstate == G_CINE) cineUpdate();
  else if(gstate == G_PLAY) {
    u = AB_buttonPressed(UP_BUTTON) != 0; d = AB_buttonPressed(DOWN_BUTTON) != 0;
    l = AB_buttonPressed(LEFT_BUTTON) != 0; r = AB_buttonPressed(RIGHT_BUTTON) != 0;
    guardUpdate(&gd, &pl);
  } else if(gstate == G_VICT) guardUpdate(&gd, &pl);
  if(gstate == G_CINE) ease(&pl); else playerUpdate(&pl, u, d, l, r);
  if(gstate == G_CINE) ease(&gd);
  obsUpdate();

  if(gstate == G_PLAY) {
    if(!obsPause && ++obsT >= obsInt) {
      obsT = 0; obsRounds++;
      obsInt = (uint16_t)rnd(54, 66);                 // 1000 +- 100 ms
      spawnObs();
    }
    // collisions: the guard, the falling objects
    if(pl.state == P_OK || pl.state == P_FALL1) {
#ifndef LT_GODMODE
      if(hits(&pl, gd.x, gd.y, 14, 26)) {             // caught (one player: game over)
        gstate = G_OVER; st = 0; obsPause = 1;
        dlgShow(hacker, D_LETGO, 0, 0);
        pl.state = P_CAUGHT; pl.t = 0; pl.lostSt = 0;
        AB_playScore2(SND_LOSS, 0);
      }
#endif
    }
    if(pl.state == P_OK) {
      for(uint8_t i = 0; i < nObs; i++)
        if(hits(&pl, (int32_t)obs[i].x << 8, obs[i].y, 14, 14)) { takeDmg(&pl); obs[i] = obs[--nObs]; break; }
    }
    // checkWin
    if(pl.state == P_OK && pl.row >= GOAL) { pl.state = P_ENTER; pl.t = 0; }
    if(pl.state == P_WON) {                           // trigVict
      gstate = G_VICT; st = 0; endF = frames; vIdx = 0; vWait = 0;
      AB_playScore2(SND_VICTORY, 0);
      gd.state = P_FALL; gd.vel = 0;
    } else if(pl.state == P_DEAD) {                   // trigLoss
      gstate = G_OVER; st = 0;
      AB_playScore2(SND_LOSS, 0);
    }
  } else if(gstate == G_OVER) {
    if(st >= (pl.state == P_CAUGHT ? 150 : 90)) endGame(0);
  } else if(gstate == G_VICT) {
    // the roll call: each dialog 2 s after its text is complete, or A
    uint8_t next = 0;
    if(st == 120) next = 1;
    else if(st > 120 && dlgOn && ((dlgDone && ++vWait >= 120) || AB_buttonDown(A_BUTTON))) next = 1;
    if(next) {
      vWait = 0;
      switch(vIdx++) {
        case 0: dlgShow(GUARD_IDX, D_MORNING, 0, 0); break;
        case 1: dlgShow(GUARD_IDX, D_ROLL, 0, 0); break;
        case 2: dlgShow(GUARD_IDX, D_TEAM1, NAMES[hacker], D_TEAM2); break;
        case 3: dlgShow(hacker, D_HERE, 0, 0); break;
        default: endGame(1); break;
      }
    }
  }

  // camera: follows the player while alive (CS = 0.08, CO = 20)
  if(pl.state != P_DEAD && pl.state != P_FALL) {
    int32_t t = pl.y - (20L << 8);
    if(t > (170L << 8)) t = 170L << 8;
    camY += (t - camY) * 20 / 256;
  }
}

static void gameDraw(void) {
  int16_t cd = (int16_t)(camY >> 8) - 120;
  background(cd);
  building();
  actorDraw(&gd);
  actorDraw(&pl);
  obsDraw();
  // HUD: lives, time, progress
  for(int8_t i = 0; i < pl.lives; i++) {
    AB_fillRect2(i * 6, 0, 6, 6, BLACK);
    AB_drawBitmap(i * 6, 0, ICON_HEART, 5, 5, WHITE);
  }
  if(gstate != G_CINE) {
    char b[8];
    uint16_t s = (uint16_t)(((gstate == G_VICT ? endF : frames) - startF) / FPS);
    mmss(b, s);
    AB_fillRect2(110, 0, 18, 6, BLACK);
    small(128 - slen(b) * 4, 0, b);
    int16_t py = 62 - (int16_t)(pl.row > GOAL ? GOAL : pl.row) * 52 / GOAL;
    int16_t gy2 = 62 - (int16_t)(gd.row > GOAL ? GOAL : (gd.row < 0 ? 0 : gd.row)) * 52 / GOAL;
    AB_drawFastVLine2(127, 10, 53, WHITE);
    AB_fillRect2(124, py - 1, 3, 3, WHITE);
    AB_drawPixel(125, gy2, WHITE);
  }
  dlgDraw();
}

static void endDraw(void) {
  if(endWin) {
    ctext(2, E_WIN1, WHITE); ctext(12, E_WIN2, WHITE); ctext(21, E_WIN3, WHITE);
    char b[8], line[20]; uint8_t n = 0;
    for(const char *s = E_TIME; *s; ) line[n++] = *s++;
    for(const char *s = mmss(b, (uint16_t)((endF - startF) / FPS)); *s; ) line[n++] = *s++;
    line[n] = 0;
    ctext(36, line, WHITE);
  } else {
    ctext(6, E_LOSE1, WHITE); ctext(16, E_LOSE2, WHITE); ctext(26, E_LOSE3, WHITE);
  }
  ctext(55, E_BACK, WHITE);
}

int main(void) {
  AB_begin();
  camY = 120L << 8;
  for(uint8_t i = 0; i < 6; i++) cloudX[i] = hash(i + 9) % 10240;
  while(1) {
    uint8_t draw = AB_nextFrame();
    frames++; st++;
    switch(scene) {
      case SC_TITLE:  titleUpdate(); break;
      case SC_SELECT: selectUpdate(); break;
      case SC_GAME:   gameUpdate(); break;
      case SC_END:
        if(st > 30 && (AB_buttonDown(A_BUTTON) || AB_buttonDown(B_BUTTON))) { nObs = nPend = 0; toScene(SC_TITLE); }
        break;
    }
    if(!draw) continue;
    AB_clear();
    switch(scene) {
      case SC_TITLE:  titleDraw(); break;
      case SC_SELECT: selectDraw(); break;
      case SC_GAME:   gameDraw(); break;
      case SC_END:    endDraw(); break;
    }
    AB_display();
  }
}
