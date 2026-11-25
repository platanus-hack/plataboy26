// ===================================================================================
// Project:   plataboy26 EEPROM writer - puts the game library into the AT24CM01
// ===================================================================================
//
// The library (build/eeprom.bin) is larger than the MCU flash, so it is written in
// chunks: tools/pack.py makes build/writer/writer_NN.bin, each this program plus up to
// 12 KiB of library data at 0x1000. Flash them one after the other (tools/flash.sh),
// then build/flash.bin. Each one writes its chunk, reads it back and shows the result:
//   "CHUNK 3/8 OK" + short beep, or "FAILED" + long beep.
//
// Chunk header at 0x1000: magic "PBWR", EEPROM address, length, chunk n of total,
// check (s = s * 33 ^ byte); the data follows at 0x1010.

#include "driver.h"
#include "eeprom.h"
#include "font.h"

#define CHUNK      ((const Chunk *)0x1000)
#define WR_MAGIC   0x52574250UL       // "PBWR"

typedef struct {
  uint32_t magic, addr;
  uint16_t len;
  uint8_t  n, total;
  uint32_t sum;
  uint8_t  data[];
} Chunk;

static uint8_t fb[128 * 8];

static void line(uint8_t row, const char *s) {
  uint8_t *p = fb + row * 128;
  for(uint8_t x = 4; *s && x <= 122; s++, x += 6) {
    uint8_t c = (uint8_t)*s;
    for(uint8_t i = 0; i < 5; i++) p[x + i] = FONT[(c >= 32 && c < 128) ? c - 32 : 0][i];
  }
}

static void show(void) {
  OLED_setpos(0, 0);
  OLED_data_start();
  for(uint16_t i = 0; i < sizeof(fb); i++) I2C_write(fb[i]);
  I2C_stop();
}

static char *u2s(char *p, uint32_t n) {   // decimal at p, returns the end
  char t[10]; uint8_t k = 0;
  do { t[k++] = '0' + n % 10; n /= 10; } while(n);
  while(k) *p++ = t[--k];
  *p = 0;
  return p;
}

int main(void) {
  JOY_init();
  DLY_ms(5);
  const Chunk *c = CHUNK;
  char s[22], *p;
  line(0, "PLATABOY.26 EEPROM");
  if(c->magic != WR_MAGIC) { line(3, "NO DATA"); show(); while(1); }
  p = u2s(s, c->n); *p++ = '/'; p = u2s(p, c->total);
  line(2, "CHUNK"); line(3, s);
  line(5, "WRITING..."); show();

  uint8_t ok = EE_probe() == EE_OK;
  if(ok) ok = EE_write(c->addr, c->data, c->len) == EE_OK;
  // read back and check
  uint32_t sum = 0;
  static uint8_t rd[256];
  for(uint32_t o = 0; ok && o < c->len; o += sizeof rd) {
    uint16_t n = c->len - o > sizeof rd ? sizeof rd : (uint16_t)(c->len - o);
    if(EE_read(c->addr + o, rd, n) != EE_OK) { ok = 0; break; }
    for(uint16_t i = 0; i < n; i++) {
      if(rd[i] != c->data[o + i]) ok = 0;
      sum = sum * 33 ^ rd[i];
    }
  }
  if(sum != c->sum) ok = 0;
  for(uint16_t i = 5 * 128; i < 6 * 128; i++) fb[i] = 0;
  p = u2s(s, c->len); for(const char *b = " BYTES"; *b; ) *p++ = *b++; *p = 0;
  line(4, s);
  line(5, ok ? "OK" : "FAILED");
  if(ok && c->n == c->total) line(6, "NOW FLASH flash.bin");
  show();
  if(ok) JOY_sound(200, 30); else JOY_sound(60, 250);
  while(1);
}
