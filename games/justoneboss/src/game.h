// ===================================================================================
// Just One Boss: what game.c (the engine) and vm.c (the bytecode of src/logic.py) share
// ===================================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "plat.h"
#include "gfx.h"
#include "sprites.h"

#define FX(n)   ((n) * 16)             // positions: 1/16 pixel
#define NONE    0xFF
#define NE      29                     // entities (E[NE] is the "no entity" dummy)
#define NP      8                      // particles
#define NM      8                      // moves (bezier curves)
#define NT      7                      // script threads
#define NX      4                      // effects (poof, pain)
#define NF      3                      // flower fields
#define STAGE_X 24                     // stage x 0 -> screen x
#define STAGE_Y 34                     // stage y 0 -> screen y (3/4 scale)

enum {                                 // the cart's entity classes (logic.py)
  T_TOPHAT = 1, T_SPINHAT, T_BUNNY, T_TITLE = 6, T_CREDIT, T_VICTORY, T_GAMEOVER,
  T_FIGMENT, T_PLAYER, T_PHEALTH, T_TILESPAWN = 14, T_TILE, T_PREFL, T_CARD,
  T_COIN = 19, T_SLAM, T_MIRROR = 22, T_MREFL, T_HAND, T_LASER, T_HEART, T_POINTS = 29,
  T_COUNT
};
#define F_DEAD   1                     // Ent.fl (logic.py has them all)
#define F_IDLE   8
#define F_REFL   16
#define F_RIGHT  32
#define F_OCC    64
#define F_BOSS   128

typedef struct {
  uint8_t  type, fl, fl2, dc;          // class, flags, more flags, default_counter
  int16_t  x, y;
  uint16_t alive;                      // frames_alive
  uint8_t  ftd, layer, hit;            // frames_to_death, render layer, hitbox | hurtbox << 4
  uint8_t  idle;                       // mirror/hand: idle_mult * 20; coin: health
  uint8_t  a, b, c;                    // per class (logic.py)
  int8_t   u, v;                       // velocities (bunny, card)
  uint8_t  inv;                        // invincibility frames
} Ent;

typedef struct {
  uint8_t ent, fr, dur, ease;          // dur 0: free
  int16_t x0, y0, x3, y3;
  int8_t an[4];                        // anchors relative to start / end, pixels
} Move;
enum { LIN, EI, EO, EOI };

enum { X_POOF = 1, X_PAIN };
typedef struct { uint8_t type, t; int16_t x, y; } Fx;               // pixels, stage
typedef struct { int16_t x, y, vx, vy; uint8_t ttl, kind; } Part;   // 1/64 pixel
typedef struct { uint8_t own, dc, ftd, on, mask[5], pend[5]; } Field;   // 40 tiles

typedef struct {
  uint8_t boss, brefl, player, prefl, figment, phealth, title;   // entity references
  uint8_t hard, phase, paused, freeze, shake, mult, refl_color, conjure, starting,
          start, start_phase, score_idx;
  uint8_t hearts, ph_anim, ph_dc;      // player_health
  uint8_t bh_health, bh_rainbow, bh_dc, bh_vis;                  // boss_health
  uint8_t cur_open, cur_dc;            // curtains
  uint8_t p_facing, p_step_dir, p_next, p_step, p_teeter, p_stun, p_col, p_row;
  int8_t  shake_x;
  uint8_t spawn_n;                     // where spawn() looks for a free entity
  uint8_t owed;                        // boss bar points whose particle found no room
  uint16_t score, timer, scene;
  uint16_t rec[4];                     // cartdata: score, time, hard score, hard time
} Glob;

extern Ent E[NE + 1];
extern uint8_t order[NE], n_order;
extern Move MV[NM];
extern Part PT[NP];
extern Fx XF[NX];
extern Field FL[NF];
extern Glob G;
extern uint16_t btn_held[4];

// game.c: the engine, called by the natives of vm.c
int  col_of(int x);
int  row_of(int y);
void die(Ent *e);
int  ease(int e, int t);
void freeze_shake(int f, int s);
void sfx(int n, int ch);
void move_to(Ent *e, int16_t *a);
void cancel_move(Ent *e);
void effect(int type, int x, int y);
int  particle(int x, int y, int vx, int vy, int ttl, int kind);
int  sx_of(int x);
int  sy_of(int y);

// vm.c
#define ENT(i) (&E[(uint8_t)(i) < NE ? (uint8_t)(i) : NE])
#define ENT_WIDE(o)   ((unsigned)(o) - 4 < 6)          // x, y, alive (bytecode field widths)
#define ENT_SIGNED(o) ((unsigned)(o) - 17 < 2)         // u, v
#define G_WIDE(o)     ((o) >= 40)                      // score, timer, scene, rec
int  vm_call(uint16_t fn, int n, int a0, int a1);   // a function of logic.py (main stack)
void vm_tick(void);                    // the waiting threads
void vm_kill(void);
void vm_flush(void);                   // the page buffer was drawn over
