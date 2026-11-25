// ===================================================================================
// Just One Boss: drawing into the current 8-row page (see gfx.h)
// ===================================================================================
#include "gfx.h"
#include "plat.h"
#include "sprites.h"

int gy;

// 3x5 font, ' '..'Z' (lower case is drawn as upper case; '<' '>' are arrows):
// column-major, bit 0 = top of the left column
static const uint16_t FONT[59] = {
  0x0000,0x02E0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x022E,0x3A20,0x0000,0x0000,
  0x0000,0x1084,0x0200,0x0C98,0x7E3F,0x43F1,0x5EBD,0x7EB1,0x7C87,0x76B7,0x729F,0x7C21,
  0x7EBF,0x7CA7,0x0140,0x0000,0x7DC4,0x0000,0x11DF,0x0000,0x0000,0x7CBE,0x2ABF,0x462E,
  0x3A3F,0x46BF,0x04BF,0x762E,0x7C9F,0x47F1,0x07F1,0x6C9F,0x421F,0x7C7F,0x783F,0x3E3E,
  0x1CBF,0x5B2E,0x6CBF,0x36B6,0x07E1,0x7E0F,0x3F0F,0x7F1F,0x6C9B,0x1F87,0x4EB9
};

// one column of bits (bit 0 = row y) into the page: dark bits cleared, lit bits set
#ifdef DEBUG
void dbg_sp(void);
#endif
static void col(int x, int y, uint32_t lit, uint32_t mask) {
#ifdef DEBUG
  dbg_sp();
#endif
  if((unsigned)x > 127) return;
  int s = y - gy;
  if(s >= 0) { if(s > 7) return; lit <<= s; mask <<= s; }
  else { if(s < -31) return; lit >>= -s; mask >>= -s; }
  pg[x] = (pg[x] & ~(uint8_t)mask) | (uint8_t)lit;
}

static uint32_t bits(uint32_t o, uint32_t n) {    // n bits of the sprite stream at o
  const uint8_t *p = SPR_DATA + (o >> 3);
  uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
  return v >> (o & 7) & ((1u << n) - 1);
}

void g_spr(int id, int x, int y, int fl) {
  uint32_t e = SPR[id][0] | SPR[id][1] << 8 | SPR[id][2] << 16;
  uint32_t o = (e & 0x7FF) * 8, w = (e >> 11) & 31, h = (e >> 16) & 31;
  if(y > gy + 7 || y + (int)h <= gy) return;
  for(uint32_t c = 0; c < w; c++, o += h) {
    uint32_t lit = bits(o, h), dk = (e >> 21) ? bits(o + w * h, h) : 0;
    int sx = (fl & G_FLIPX) ? x + (int)(w - 1 - c) : x + (int)c;
    uint32_t m = lit | dk;
    if(fl & G_DITHER) lit &= ((sx ^ y) & 1) ? 0xAAAAAAAA : 0x55555555;
    col(sx, y, lit, m);
  }
}

void g_pix(int x, int y, int c) {
  if(y >= gy && y < gy + 8) col(x, y, c, 1);
}

void g_fill(int x0, int y0, int x1, int y1, int c) {
  if(y1 < y0 || y0 > gy + 7 || y1 < gy) return;
  if(y0 < gy - 8) y0 = gy - 8;
  if(y1 > gy + 7) y1 = gy + 7;
  uint32_t m = (2u << (y1 - y0)) - 1;
  for(int x = x0; x <= x1; x++) col(x, y0, c ? m : 0, m);
}

// a line of text (up to '\n' or the end); returns the x after it
int g_text(const char *s, int x, int y, int scale) {
  for(; *s && *s != '\n'; s++, x += 4 * scale) {
    unsigned ch = *s;
    if(ch >= 'a') ch -= 32;
    uint32_t g = ch >= 32 && ch <= 'Z' ? FONT[ch - 32] : 0;
    if(y > gy + 7 || y + 5 * scale <= gy) continue;
    for(int i = 0; i < 3 * scale; i++) {
      uint32_t b = g >> (i / scale * 5) & 31, m = 0;
      if(scale > 1) {                  // double every row
        for(int r = 0; r < 5; r++) if(b >> r & 1) m |= 3u << (2 * r);
        b = m;
      }
      col(x + i, y, b, b);
    }
  }
  return x;
}

int g_num(unsigned n, int x, int y) {
  char buf[6], *p = buf + 5;
  *p = 0;
  do { *--p = '0' + n % 10; n /= 10; } while(n);
  return g_text(p, x, y, 1);
}

// lines centred on cx, 6 * scale + 1 rows apart
void g_ctext(const char *s, int cx, int y, int scale) {
  while(1) {
    int n = 0;
    while(s[n] && s[n] != '\n') n++;
    g_text(s, cx - 2 * scale * n, y, scale);
    if(!s[n]) return;
    s += n + 1; y += 6 * scale + 1;
  }
}
