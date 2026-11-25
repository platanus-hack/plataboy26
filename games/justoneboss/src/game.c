// ===================================================================================
// Project:   Just One Boss for the CH32V003 Mini Game Console
// ===================================================================================
//
// Port of "Just One Boss" by ayla~nonsense (bridgs), PICO-8, 2018:
//   https://github.com/aylanonsense/just-one-boss
//
// Duel the magic mirror on a stage of 8 x 5 tiles: step from tile to tile (joypad),
// collect the magic tiles to fill the boss's bar, dodge his cards, lasers, flowers,
// coins and reflections. Title: right (or A) starts, left (or B) starts the hard mode
// once the game was beaten.
//
// The cart's Lua is in src/logic.py (Python syntax, compiled to bytecode by
// tools/jbc.py and run by src/vm.c): the PICO-8 game is far more code than the
// console's 15 KiB of flash hold as RISC-V code, so the logic is bytecode, and what runs
// rarely (the boss's choreography) stays in the EEPROM. This file is the engine:
// entities, moves, particles, flowers, hits, the main loop and the drawing.
// What changed for this console:
//   - the screen is 128x64 in 1 bit: the stage is drawn at 3/4 height (tiles 10 x 6
//     pixels), score, timer and hearts sit beside it; the sprites were converted and
//     redrawn (tools/sprites.txt); colours became blinking and dithering
//   - fixed point instead of PICO-8 numbers: positions in 1/16 pixel
//   - one voice of sound: the sound effects, no music
//   - fixed-size pools (entities, particles, moves, threads); when a pool is full, the
//     least important things (particles, poofs) are dropped

#include "game.h"
#include "at24.h"
#include "bytecode.h"

Ent E[NE + 1];
uint8_t order[NE], n_order;
Move MV[NM];
Part PT[NP];
Fx XF[NX];
Field FL[NF];
Glob G = { .boss = NONE, .brefl = NONE, .player = NONE, .prefl = NONE, .figment = NONE,
           .phealth = NONE, .title = NONE, .conjure = 1, .p_step_dir = NONE, .p_next = NONE };
uint16_t btn_held[4];

// ===================================================================================
// Helpers
// ===================================================================================
int ease(int e, int t) {        // t and the result in 1/256
  if(e == EO) return t * t >> 8;
  if(e == EI) return 256 - ((256 - t) * (256 - t) >> 8);
  if(e == EOI) return t < 128 ? t * t >> 7 : 256 - ((256 - t) * (256 - t) >> 7);
  return t;
}

int col_of(int x) { return x >= 0 ? x / 160 + 1 : -((-x + 159) / 160) + 1; }
int row_of(int y) { return y >= 0 ? y / 128 + 1 : -((-y + 127) / 128) + 1; }

// ===================================================================================
// Entities, moves, effects, particles, flowers
// ===================================================================================
void die(Ent *e) {
  if(e == &E[NE] || (e->fl & F_DEAD)) return;
  e->fl |= F_DEAD;
  vm_call(F_on_death, 1, e - E, 0);
}

static void free_ent(int i) {
  cancel_move(&E[i]);
  E[i].type = 0;
  for(Field *f = FL; f < FL + NF; f++) if(f->own == i) f->on = 0;
  uint8_t *r = &G.boss;                // the entity references in G
  for(int k = 0; k < 7; k++) if(r[k] == i) r[k] = NONE;
}

void effect(int type, int x, int y) {
  for(Fx *f = XF; f < XF + NX; f++)
    if(!f->type) { *f = (Fx){type, 0, x >> 4, y >> 4}; return; }
}

// the cart's sfx(n, ch): its note list from the EEPROM (sound effects 7..31, 28 bytes
// each, after the bytecode); a lower channel wins (a sound on channel 0 is not cut)
static uint8_t snd_buf[28];
void sfx(int n, int ch) {
  if(n < 7 || n > 31 || ch > snd_ch) return;
  sfx_stop();
  if(EE_read(JOB_COLD + ((BC_COLD_SIZE + 31) & ~31) + 28 * (n - 7), snd_buf, 28) == EE_OK)
    sfx_play(snd_buf, ch);
}

void freeze_shake(int f, int s) {
  if(f > G.freeze) G.freeze = f;
  if(s > G.shake) G.shake = s;
}

static Move *move_of(Ent *e, int free) {   // e's move (or else a free slot)
  Move *q = 0;
  for(Move *m = MV; m < MV + NM; m++) {
    if(m->dur && m->ent == e - E) return m;
    if(free && !m->dur) q = m;
  }
  return q;
}

// a[]: entity, x, y, frames, easing, 4 anchors (pixels; relative to start / end)
void move_to(Ent *e, int16_t *a) {
  Move *m = move_of(e, 1);
  if(!m) { e->x = a[1]; e->y = a[2]; return; }   // no slot: jump there
  m->ent = e - E; m->fr = 0; m->dur = a[3]; m->ease = a[4] & 3;
  m->x0 = e->x; m->y0 = e->y; m->x3 = a[1]; m->y3 = a[2];
  for(int i = 0; i < 4; i++) m->an[i] = a[5 + i];
}

void cancel_move(Ent *e) {
  Move *m = move_of(e, 0);
  if(m) m->dur = 0;
}

static void apply_moves(void) {        // the cart's apply_velocity with a movement
  for(Move *m = MV; m < MV + NM; m++) {
    if(!m->dur) continue;
    Ent *e = &E[m->ent];
    if(G.paused && e->layer < 13) continue;
    if(++m->fr >= m->dur) { e->x = m->x3; e->y = m->y3; m->dur = 0; continue; }
    int t = ease(m->ease, (m->fr << 8) / m->dur), u = 256 - t;
    int b1 = 3 * u * u * t >> 16, b2 = 3 * u * t * t >> 16, b23 = b2 + (t * t * t >> 16);
    e->x = m->x0 + ((b23 * (m->x3 - m->x0) + (b1 * m->an[0] + b2 * m->an[2]) * 16) >> 8);
    e->y = m->y0 + ((b23 * (m->y3 - m->y0) + (b1 * m->an[1] + b2 * m->an[3]) * 16) >> 8);
  }
}

// particles in 1/64 pixel; health particles push others out
int particle(int x, int y, int vx, int vy, int ttl, int kind) {
  Part *q = 0;
  for(Part *p = PT; p < PT + NP; p++) {
    if(!p->kind) { q = p; break; }
    if(kind >= 3 && p->kind < 3) q = p;
  }
  if(q) *q = (Part){x * 4, y * 4, vx, vy, ttl, kind};
  return q != 0;
}

#ifdef DEBUG                           // make debug: state on USART1 (the simulator prints it)
#include "ch32v003.h"
static void dbg_num(int v) {
  char b[8], *p = b + 7; unsigned u = v < 0 ? -v : v;
  *p = 0; do *--p = '0' + u % 10; while(u /= 10);
  if(v < 0) *--p = '-';
  while(*p) USART1->DATAR = *p++;
  USART1->DATAR = ' ';
}
#ifndef AUTO_UNTIL
#define AUTO_UNTIL 9                   // autoplay while the phase is below this
#endif
static uint32_t dbg_t[2];
extern uint32_t _ebss[];
static void dbg_fill(void) {           // the free stack, to find its low-water mark
  uint32_t *sp; asm volatile("mv %0, sp" : "=r"(sp));
  for(uint32_t *p = _ebss; p < sp - 16; p++) *p = 0xA5A5A5A5;
}
static int dbg_free(void) { uint32_t *p = _ebss; while(*p == 0xA5A5A5A5) p++; return (p - _ebss) * 4; }
uint32_t dbg_minsp = 0x20000800;
void dbg_sp(void) { uint32_t sp; asm volatile("mv %0, sp" : "=r"(sp)); if(sp < dbg_minsp) dbg_minsp = sp; }
static void dbg_frame(void) {
  static int n;
  Ent *p = &E[G.player];               // autoplay: invincible, onto every magic tile
  if(G.player < NE && p->type == T_PLAYER && G.phase < AUTO_UNTIL) {
    p->inv = 2;
    for(Ent *e = E; e < E + NE; e++)
      if(e->type == T_TILE && e->hit && !G.p_step && n % 20 == 0) { p->x = e->x; p->y = e->y; }
  }
  if(n++ % 60) return;
  dbg_num(n); dbg_num(n_order); dbg_num(G.phase); dbg_num(G.bh_health); dbg_num(G.score);
  dbg_num(G.hearts); dbg_num(G.boss); dbg_num(G.brefl); dbg_num(G.prefl);
  int th = 0, mv = 0, pt = 0;
  extern int vm_threads(void);
  th = vm_threads();
  for(Move *m = MV; m < MV + NM; m++) mv += m->dur != 0;
  for(Part *q = PT; q < PT + NP; q++) pt += q->kind != 0;
  dbg_num(th); dbg_num(mv); dbg_num(pt);
  dbg_num(dbg_t[0] / 300 / 48); dbg_num(dbg_t[1] / 300 / 48);  // us per frame: update, draw
  dbg_num(dbg_free()); dbg_num(dbg_minsp - (uint32_t)_ebss);
  dbg_t[0] = dbg_t[1] = 0;
  USART1->DATAR = '\n';
}
#endif

// ===================================================================================
// Main loop: updates (the cart's _update)
// ===================================================================================
// the cart's hit checks; the hurt entities are listed and hurt afterwards (the stack)
static uint8_t hurt_n, hurt[6];
static void hit(int i) { if(hurt_n < sizeof hurt) hurt[hurt_n++] = i; }

static void find_hits(void) {
  uint8_t tile[NE];                    // col | row << 4 of each entity
  for(int i = 0; i < NE; i++) tile[i] = (col_of(E[i].x) & 15) | row_of(E[i].y) << 4;
  for(Ent *a = E; a < E + NE; a++) {
    if(!a->type || !(a->hit & 15) || (a->fl & F_DEAD)) continue;
    int ta = tile[a - E], m = a->type == T_LASER ? 15 : 255;
    for(Ent *b = E; b < E + NE; b++)
      if(b != a && b->type && (a->hit & b->hit >> 4) && !b->inv && !(b->fl & F_DEAD) &&
         !((ta ^ tile[b - E]) & m)) hit(b - E);
  }
  for(Field *f = FL; f < FL + NF; f++) {   // blooming flowers hurt (hitbox 1)
    if(!f->on || !f->dc) continue;
    for(int w = 0; w < 2; w++) {
      int i = w ? G.prefl : G.player, c = tile[i] & 15, r = tile[i] >> 4;
      if(i >= NE || E[i].inv || c < 1 || c > 8 || r < 1 || r > 5) continue;
      if(f->mask[r - 1] >> (c - 1) & 1) hit(i);
    }
  }
}

static void hits(void) {
  hurt_n = 0;
  find_hits();
  for(int k = 0; k < hurt_n; k++) {
    Ent *b = &E[hurt[k]];
    if(!b->inv && !(b->fl & F_DEAD)) vm_call(F_hurt, 1, hurt[k], 0);
  }
}

static void particles(void) {
  if(G.owed && (G.scene & 1)) { G.owed--; vm_call(F_gain_health, 0, 0, 0); }
  for(Part *p = PT; p < PT + NP; p++) {
    int k = p->kind & 7;
    if(!k) continue;
    if(k == 4) {                       // to the bar: ease out over 8 frames
      int s = 2 * (9 - p->ttl) - 1;
      p->x += p->vx * s >> 6; p->y += p->vy * s >> 6;
      if(!--p->ttl) { p->kind = 0; vm_call(F_gain_health, 0, 0, 0); }
      continue;
    }
    int f = k == 2 ? 29 : 24;          // friction 0.9 (blooms) / 0.75, gravity 0.06 / 0.1
    p->vy += k == 2 ? 4 : 6;
    p->vx = p->vx * f >> 5; p->vy = p->vy * f >> 5;
    p->x += p->vx; p->y += p->vy;
    if(--p->ttl) continue;
    if(k == 3) {                       // start the move to the boss bar
      int h = G.bh_health + (p->kind >> 3) + 1;
      p->kind = 4; p->ttl = 8;
      p->vx = (8 + (h < 60 ? h : 60)) * 64 - p->x;
      p->vy = -44 * 64 - p->y;
    } else p->kind = 0;
  }
  for(Fx *x = XF; x < XF + NX; x++)
    if(x->type && ++x->t >= (x->type == X_POOF ? 12 : 3)) x->type = 0;
}

static void fields(void) {
  for(Field *f = FL; f < FL + NF; f++) {
    if(!f->on) continue;
    if(f->dc) f->dc--;
    if(f->ftd && !--f->ftd) f->on = 0;
  }
}

static int on_top(Ent *a, Ent *b) {
  return a->layer == b->layer ? a->y > b->y : a->layer > b->layer;
}

static void update(void) {
  for(int d = 0; d < 4; d++)
    btn_held[d] = (btn_now >> d & 1) ? btn_held[d] + (btn_held[d] < 1000) : 0;
  if(G.freeze) {
    G.freeze--;
    vm_call(F_frozen, 0, 0, 0);
    return;
  }
  vm_call(F_tick, 0, 0, 0);
  vm_tick();
  if(!G.paused) fields();
  int n = n_order;
  for(int i = 0; i < n; i++) {
    Ent *e = &E[order[i]];
    if((e->fl & F_DEAD) || (G.paused && e->layer < 13)) continue;   // 13+: pause immune
    vm_call(F_update, 2, e - E, e->dc && !--e->dc);
    if(e->inv) e->inv--;
    e->alive++;
    if(e->ftd && !--e->ftd) die(e);
  }
  apply_moves();
  if(!G.paused) { hits(); particles(); }
  // remove the dead, sort for drawing
  int m = 0;
  for(int i = 0; i < n_order; i++) {
    int k = order[i];
    if(E[k].fl & F_DEAD) free_ent(k); else order[m++] = k;
  }
  n_order = m;
  for(int i = 1; i < m; i++)
    for(int j = i; j > 0 && on_top(&E[order[j - 1]], &E[order[j]]); j--) {
      int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
  if(G.start && !--G.start) vm_call(F_start_game_now, 0, 0, 0);
#ifdef DEBUG
  dbg_frame();
#endif
}

// ===================================================================================
// Drawing (the cart's _draw), one page at a time
// ===================================================================================
int sx_of(int x) { return STAGE_X + ((x + 8) >> 4) + G.shake_x; }
int sy_of(int y) { return STAGE_Y + ((y * 3 + 32) >> 6); }

static void draw_floor(void) {         // tile corners, a mark in each tile
  if(gy < 32) return;
  for(int r = 0; r < 6; r++)
    for(int c = 0; c < 9; c++) {
      int x = STAGE_X + G.shake_x + 10 * c, y = STAGE_Y + 6 * r - (r == 5);
      g_pix(x, y, 1);
      if(c < 8 && r < 5) g_fill(x + 5 - ((c + r) & 1), y + 3, x + 5 + ((c + r) & 1), y + 3, 1);
    }
}

// draws the entities of layers below `below`; a stage entity only on the pages near it
// (its sprites reach 24 rows up and 16 down; lasers go to the bottom)
static int draw_layers(int i, int below) {
  for(; i < n_order; i++) {
    Ent *e = &E[order[i]];
    if(e->layer >= below) break;
    int y = sy_of(e->y);
    if(e->layer < 13 && e->type != T_LASER && (y + 16 < gy || y - 24 > gy + 7)) continue;
    vm_call(F_draw, 1, order[i], 0);
  }
  return i;
}

static void draw(void) {
  G.shake_x = 0;
  if(!G.freeze && G.shake) G.shake_x = (G.phase == 5 ? 1 : (G.shake + 2) / 3) * (G.scene & 1 ? 1 : -1);
  lcd_begin();
  for(gy = 0; gy < 64; gy += 8) {
    for(int i = 0; i < 128; i++) pg[i] = 0;
    draw_floor();
    for(Field *f = FL; f < FL + NF; f++) {   // flowers (layer 4)
      if(!f->on) continue;
      int s = f->dc ? 2 : f->ftd ? 1 : 0, fl = (E[f->own].fl & F_REFL) && G.hard ? G_DITHER : 0;
      for(int b = 0; b < 40; b++)
        if(f->mask[b >> 3] >> (b & 7) & 1)
          g_spr(SPR_FLOWER + s, STAGE_X + G.shake_x + (b & 7) * 10 + 1, sy_of(FX(8 * (b >> 3) + 4)) - 3, fl);
    }
    int i = draw_layers(0, 11);
    for(Part *p = PT; p < PT + NP; p++) {    // particles (layer 11)
      if(!p->kind) continue;
      g_pix(sx_of(p->x >> 2), sy_of(p->y >> 2), 1);
    }
    for(Fx *x = XF; x < XF + NX; x++) {      // poofs (layer 9) and pain (12)
      int sx = STAGE_X + x->x + G.shake_x, sy = sy_of(FX(x->y));
      if(x->type == X_POOF) g_spr(SPR_POOF + x->t / 4, sx - 8, sy - 6, 0);
      else if(x->type == X_PAIN) g_spr(SPR_PAIN, sx - 11, sy - 12, 0);
    }
    i = draw_layers(i, 14);            // cards, lasers, ..., player health (13)
    vm_call(F_hud, 0, 0, 0);
    vm_call(F_curtains, 0, 0, 0);      // layer 14
    draw_layers(i, 255);               // figment, screens
    lcd_page();
  }
  lcd_end();
  vm_flush();
}

// ===================================================================================
// Main
// ===================================================================================
int main(void) {
#ifdef DEBUG
  dbg_fill();
#endif
  plat_init();
  save_read(G.rec);
  vm_call(F_init, 0, 0, 0);
  while(1) {
    plat_frame();
#ifdef DEBUG
    uint32_t t0 = STK->CNT;
    update();
    uint32_t t1 = STK->CNT;
    draw();
    dbg_t[0] += t1 - t0; dbg_t[1] += STK->CNT - t1;
#else
    update();
    draw();
#endif
  }
}
