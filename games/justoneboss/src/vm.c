// ===================================================================================
// Just One Boss: the bytecode VM (the game logic, src/logic.py, tools/jbc.py)
// ===================================================================================
// A stack machine on 16-bit values. A call frame is [return pc][caller's fp][args]
// [locals] (fp = the first argument). The game calls functions of logic.py (vm_call);
// the boss's scripts are threads that wait() for frames (the cart's promise sequences).
// Functions below 0x8000 are in flash (BC_HOT), the others in the EEPROM (bin/cold.bin,
// read through a small cache, only from the update, never while a frame is sent).
#include "game.h"
#include "at24.h"
#include "bytecode.h"

enum { LDL0 = 0x40, STL0 = 0x50, PUSHK = 0x60 };   // and OP_* (bytecode.h)

// One work stack: whatever runs (a call from the game, a thread) runs on top of it.
// A thread that waits moves its frames (its stack as it is then, usually 10-20 values)
// to the heap and back when it goes on, so threads cost only what they hold.
#define WSTK VM_STACK                  // values (tools/jbc.py: the deepest call)
#define HEAP 56
typedef struct { uint16_t pc; uint8_t sp, fp, wait, own, off, len; } Thr;
static Thr TH[NT];
static int16_t work[WSTK], heap[HEAP];
static uint8_t top, heap_n;            // work stack top, heap used
static Thr *cur;                       // the thread running now (0: a call of the game)

// ---- fetching bytecode ---------------------------------------------------------------
// The cache of the EEPROM's bytecode is the two page buffers (8 lines of 32 bytes):
// free while the game updates, the only time that bytecode runs (vm_flush: drawn over).
static uint16_t ctag[8];
static uint8_t cnext;
void vm_flush(void) { for(int k = 0; k < 8; k++) ctag[k] = 0; }

static uint8_t fetch(unsigned pc) {
  if(pc < 0x8000) return BC_HOT[pc];
  unsigned line = pc & ~31u, k;
  for(k = 0; k < 8; k++) if(ctag[k] == line) return pages[0][32 * k + (pc & 31)];
  k = cnext = (cnext + 1) & 7;
  EE_read(JOB_COLD + line - 0x8000, pages[0] + 32 * k, 32);
  ctag[k] = line;
  return pages[0][32 * k + (pc & 31)];
}

// ---- natives -------------------------------------------------------------------------
static int fork_(int16_t *a);
static void unheap(Thr *t);
#define EA(i) ENT(a[i])
static int16_t n_rnd(int16_t *a) { return a[0] > 0 ? rnd32() % a[0] : 0; }
static int16_t n_sfx(int16_t *a) { sfx(a[0], a[1]); return 0; }
static int16_t n_shake(int16_t *a) { freeze_shake(a[0], a[1]); return 0; }
static int16_t n_ent_new(int16_t *a) {  // entity a, all fields 0, joins the order
  Ent *e = EA(0);
  for(int j = 0; j < (int)sizeof(Ent); j++) ((uint8_t *)e)[j] = 0;
  order[n_order++] = a[0];
  return 0;
}
static int16_t n_die(int16_t *a) { die(EA(0)); return 0; }
static int16_t n_move4(int16_t *a) {
  if(EA(0) != &E[NE]) move_to(EA(0), a);
  return a[3] - 1;
}
static int16_t n_cancel_move(int16_t *a) { cancel_move(EA(0)); return 0; }
static int16_t n_effect(int16_t *a) { effect(a[0], a[1], a[2]); return 0; }
static int16_t n_particle(int16_t *a) { return particle(a[0], a[1], a[2], a[3], a[4], a[5]); }
static int16_t n_col(int16_t *a) { return col_of(a[0]); }
static int16_t n_row(int16_t *a) { return row_of(a[0]); }
static int16_t n_fork(int16_t *a) { return fork_(a); }
static int16_t n_cancel(int16_t *a) {  // the cart's cancel_promises: threads of entity a
  for(Thr *t = TH; t < TH + NT; t++)
    if(t->pc && t != cur && t->own == (uint8_t)a[0]) { if(t->len) unheap(t); t->pc = 0; }
  return 0;
}
static int16_t n_kill(int16_t *a) { vm_kill(); return 0; }
static int16_t n_btnp(int16_t *a) {    // PICO-8 btnp: press, then repeats after 15 frames
  int h = btn_held[a[0] & 3];
  return h == 1 || (h > 15 && (h - 16) % 4 == 0);
}
static int16_t n_btn_new(int16_t *a) { return btn_new; }
static int16_t n_spr(int16_t *a) { g_spr(a[0], a[1], a[2], a[3]); return 0; }
static int16_t n_spr_e(int16_t *a) {   // draw_sprite: (dx, dy) in the cart's pixels
  Ent *e = EA(0);
  g_spr(a[3], sx_of(e->x) - a[1], sy_of(e->y - FX(a[2])), a[4]);
  return 0;
}
static const char *str(int16_t a) { return (const char *)BC_HOT + (uint16_t)a; }
static int16_t n_text(int16_t *a) { return g_text(str(a[0]), a[1], a[2], a[3]); }
static int16_t n_ctext(int16_t *a) { g_ctext(str(a[0]), a[1], a[2], a[3]); return 0; }
static int16_t n_num(int16_t *a) { return g_num((uint16_t)a[0], a[1], a[2]); }
static int16_t n_pix(int16_t *a) { g_pix(a[0], a[1], a[2]); return 0; }
static int16_t n_fill(int16_t *a) { g_fill(a[0], a[1], a[2], a[3], a[4]); return 0; }
static int16_t n_save(int16_t *a) { save_write(G.rec); return 0; }
static int16_t n_peek(int16_t *a) { return ((uint8_t *)FL)[(uint8_t)a[0] % sizeof FL]; }
static int16_t n_poke(int16_t *a) { ((uint8_t *)FL)[(uint8_t)a[0] % sizeof FL] = a[1]; return 0; }
static int16_t n_sx(int16_t *a) { return sx_of(a[0]); }
static int16_t n_sy(int16_t *a) { return sy_of(a[0]); }
static int16_t n_ease(int16_t *a) { return ease(a[0], a[1]); }

static int16_t (*const NATS[])(int16_t *) = {
  n_rnd, n_sfx, n_shake, n_ent_new, n_die, n_move4, n_cancel_move, n_effect,
  n_particle, n_col, n_row, n_fork, n_cancel, n_kill, 0, n_btnp, n_btn_new, n_spr, n_spr_e,
  n_text, n_ctext, n_num, n_pix, n_fill, n_save, n_peek, n_poke, n_sx, n_sy, n_ease,
};
_Static_assert(sizeof NATS / sizeof NATS[0] == sizeof NAT_N, "natives");

// ---- the interpreter -------------------------------------------------------------------
// Runs from pc with the frame pointer fp, on the work stack from `top` (sp values in use)
// until the outermost function returns (result; t->pc = 0) or a thread waits.
#define FETCH(p) ((p) < 0x8000 ? BC_HOT[p] : fetch(p))
#ifdef DEBUG
void dbg_sp(void);
#endif
static int run(Thr *t, unsigned pc, unsigned spi, unsigned fpi) {
#ifdef DEBUG
  dbg_sp();
#endif
  Thr *outer = cur;
  unsigned base = top;
  int16_t *st = work + base, *s = st + spi, *f = st + fpi;
  int r = 0;
  cur = t;
  while(1) {
    int op = FETCH(pc), a = 0, x;
    pc++;
    if(op >= PUSHK) { *s++ = op - PUSHK - 8; continue; }
    if(op >= STL0) { f[op - STL0] = *--s; continue; }
    if(op >= LDL0) { *s++ = f[op - LDL0]; continue; }
    if(op >= OP_1) {                   // the operand
      a = FETCH(pc); pc++;
      if(op >= OP_2) { a |= FETCH(pc) << 8; pc++; }
    }
    switch(op) {
      case OP_RET: {
        int16_t val = s[-1];
        s = f - 2;
        pc = (uint16_t)s[0]; x = s[1];
        f = st + (x & 0xFF);
        if(!pc) { r = val; if(t) t->pc = 0; goto out; }
        if(!(x & 0x100)) *s++ = val;   // CALLV: no result
        break;
      }
      case OP_PUSHB: *s++ = (int8_t)a; break;
      case OP_PUSHX: *s++ = (int8_t)a * 16; break;
      case OP_PUSHW: *s++ = a; break;
      case OP_LDL: *s++ = f[a]; break;
      case OP_STL: f[a] = *--s; break;
      case OP_LDG: case OP_STG: {
        uint8_t *g = (uint8_t *)&G + a;
        if(op == OP_LDG) *s++ = G_WIDE(a) ? *(int16_t *)g : *g;
        else if(G_WIDE(a)) *(int16_t *)g = *--s; else *g = *--s;
        break;
      }
      case OP_LDLE: case OP_STLE:
        *s++ = f[a >> 5]; a &= 31; op -= OP_LDLE - OP_LDE;
        /* fall through */
      case OP_LDE: case OP_STE: {
        Ent *ent = ENT(s[-1]);
        uint8_t *e = (uint8_t *)ent + a;
        if(op == OP_LDE) { s[-1] = ENT_WIDE(a) ? *(int16_t *)e : ENT_SIGNED(a) ? *(int8_t *)e : *e; break; }
        s -= 2;
        if(ent == &E[NE]) break;       // writes to "no entity" are dropped
        if(ENT_WIDE(a)) *(int16_t *)e = *s; else *e = *s;
        break;
      }
      case OP_TAB: s[-1] = (int8_t)fetch(a + s[-1]); break;
      case OP_CALL: case OP_CALLV: {
        unsigned h = fetch(a), n = h & 15;
        for(int16_t *p = s; p > s - n; p--) p[1] = p[-1];
        s[-n] = pc; s[1 - n] = (f - st) | (op == OP_CALLV) << 8;
        f = s - n + 2; s = f + n;
        for(n = h >> 4; n; n--) *s++ = 0;
        pc = a + 2;
        break;
      }
      case OP_NAT: case OP_NATV: {
        int n = NAT_N[a];
        s -= n;
        if(a == N_WAIT) {              // a thread waits (on a call of the game: ignored)
          x = *s;
          if(op == OP_NAT) *s++ = 0;
          if(t && x > 0) {             // to the heap
            unsigned len = s - st;
            if(heap_n + len > HEAP) { t->pc = 0; goto out; }
            t->pc = pc; t->sp = len; t->fp = f - st; t->wait = x;
            t->off = heap_n; t->len = len;
            for(unsigned i = 0; i < len; i++) heap[heap_n++] = st[i];
            goto out;
          }
          break;
        }
        top = base + (s - st) + n;     // whatever the native runs goes above
        x = NATS[a](s);
        if(op == OP_NAT) *s++ = x;
        if(t && !t->pc) goto out;      // the thread was cancelled
        break;
      }
      case OP_JMP: pc += (int8_t)a; break;
      case OP_JZ: if(!*--s) pc += (int8_t)a; break;
      case OP_JNZ: if(*--s) pc += (int8_t)a; break;
      case OP_JMPW: pc += (int16_t)a; break;
      case OP_JZW: if(!*--s) pc += (int16_t)a; break;
      case OP_JNZW: if(*--s) pc += (int16_t)a; break;
      case OP_JNEK: if(*--s != (int8_t)a) pc += (int8_t)(a >> 8); break;
      case OP_JEQK: if(*--s == (int8_t)a) pc += (int8_t)(a >> 8); break;
      case OP_DUP: *s = s[-1]; s++; break;
      case OP_DROP: s--; break;
      case OP_NEG: s[-1] = -s[-1]; break;
      case OP_NOT: s[-1] = !s[-1]; break;
      case OP_INV: s[-1] = ~s[-1]; break;
      default: {                       // binary operators
        int b = *--s;
        a = s[-1];
        switch(op) {
          case OP_ADD: a += b; break;
          case OP_SUB: a -= b; break;
          case OP_MUL: a *= b; break;
          case OP_DIV: a = b ? a / b : 0; break;
          case OP_MOD: a = b ? a % b : 0; break;
          case OP_AND: a &= b; break;
          case OP_OR: a |= b; break;
          case OP_XOR: a ^= b; break;
          case OP_SHL: a <<= b; break;
          case OP_SHR: a >>= b; break;
          case OP_EQ: a = a == b; break;
          case OP_NE: a = a != b; break;
          case OP_LT: a = a < b; break;
          case OP_LE: a = a <= b; break;
          case OP_GT: a = a > b; break;
          case OP_GE: a = a >= b; break;
        }
        s[-1] = a;
      }
    }
  }
out:
  top = base;
  cur = outer;
  return r;
}

// a frame for fn(a0, a1) at the work stack's top that returns to pc 0 (the end)
static unsigned enter(uint16_t fn, int a0, int a1, unsigned n) {
  int16_t *st = work + top;
  st[0] = 0; st[1] = 0; st[2] = a0; st[3] = a1;
  unsigned sp = 2 + n;
  for(unsigned i = fetch(fn) >> 4; i; i--) st[sp++] = 0;
  return sp;
}

int vm_call(uint16_t fn, int n, int a0, int a1) {
  unsigned sp = enter(fn, a0, a1, n);
  return run(0, fn + 2, sp, 2);
}

// fork(entity, function, arg, frames to wait): a thread runs function(entity, arg)
static int fork_(int16_t *a) {
  Ent *e = ENT(a[0]);
  if(e == &E[NE] || !e->type) return 0;
  for(Thr *t = TH; t < TH + NT; t++) {
    if(t->pc) continue;
    unsigned sp = enter(a[1], a[0], a[2], 2);
    t->own = e - E;
    t->len = 0;
    if(a[3] <= 0) { t->pc = 1; run(t, (uint16_t)a[1] + 2, sp, 2); return 1; }
    if(heap_n + sp > HEAP) return 0;
    t->pc = (uint16_t)a[1] + 2; t->sp = sp; t->fp = 2; t->wait = a[3];
    t->off = heap_n; t->len = sp;
    for(unsigned i = 0; i < sp; i++) heap[heap_n++] = work[top + i];
    return 1;
  }
  return 0;
}

static void unheap(Thr *t) {           // removes t's frames from the heap
  unsigned o = t->off, n = t->len;
  for(unsigned i = o + n; i < heap_n; i++) heap[i - n] = heap[i];
  heap_n -= n;
  for(Thr *u = TH; u < TH + NT; u++) if(u->pc && u->off > o) u->off -= n;
  t->len = 0;
}

void vm_tick(void) {
  for(Thr *t = TH; t < TH + NT; t++) {
    if(!t->pc || --t->wait) continue;
    for(unsigned i = 0; i < t->len; i++) work[top + i] = heap[t->off + i];
    unheap(t);
    run(t, t->pc, t->sp, t->fp);
  }
}

#ifdef DEBUG
int vm_threads(void) { int n = 0; for(Thr *t = TH; t < TH + NT; t++) n += t->pc != 0; return n * 100 + heap_n; }
#endif

void vm_kill(void) {                   // the cart's promises = {}
  for(Thr *t = TH; t < TH + NT; t++)
    if(t != cur && t->pc) { if(t->len) unheap(t); t->pc = 0; }
}
