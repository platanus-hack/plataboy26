// ===================================================================================
// CH32V003 Game Console Simulator - headless runner
// ===================================================================================
//
// Usage: chsim <firmware.bin|.hex> [options]
//   -t <ms>              emulated run time (default 3000)
//   -p <btn>@<from>-<to> press a button between two times in ms (repeatable)
//                        btn: up down left right act
//   -s <ms>:<file.pgm>   save a screenshot at the given time (repeatable)
//   -a <file.raw>        dump audio (s16le mono 48 kHz)
//   -e <file.bin>        EEPROM image to start with (AT24CM01, up to 128 KiB; default erased)
//   -E <file.bin>        write the EEPROM contents to this file at the end
//   -r <11|12|13>        board revision (default 13): 12 adds the EEPROM, 13 the B button
//   -n                   same as -r 11 (original board, no EEPROM)
//   -F <file.bin>        write the flash contents (16 KiB) to this file at the end
//   -P <ms>              power cycle at this time (flash and EEPROM keep their contents; repeatable)
//   -S <ms>              software reset (PFIC RESETSYS, SFTRSTF) at this time (repeatable)
//   -i <file>            more button presses, one "btn from to" (ms) per line
//   -R <file.bin>        fill SRAM with 0xA5 at power-up and write it (2 KiB) to this file at
//                        the end: bytes still 0xA5 above .bss were never used (stack depth)
//   -v <mV>              supply voltage (default 3300): the battery, as the ADC's Vref reads it
//   -q                   quiet (no stats)
//
// Prints serial output, simulator warnings and run statistics.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "emu.h"

uint32_t emu_state_size(void);

typedef struct { uint32_t mask, from, to; } Press;
typedef struct { uint32_t at; const char *file; } Shot;

static int load_hex(const char *path, uint8_t *out, uint32_t *len) {
  FILE *f = fopen(path, "r");
  if(!f) return 0;
  char line[600]; uint32_t base = 0, max = 0;
  while(fgets(line, sizeof line, f)) {
    if(line[0] != ':') continue;
    unsigned n, addr, type;
    sscanf(line + 1, "%2x%4x%2x", &n, &addr, &type);
    if(type == 0) {
      for(unsigned i = 0; i < n; i++) {
        unsigned b; sscanf(line + 9 + i * 2, "%2x", &b);
        uint32_t a = (base + addr + i) & 0x07FFFFFF;
        if(a < FLASH_SIZE) { out[a] = (uint8_t)b; if(a + 1 > max) max = a + 1; }
      }
    } else if(type == 4) { unsigned hi; sscanf(line + 9, "%4x", &hi); base = hi << 16; }
    else if(type == 2) { unsigned hi; sscanf(line + 9, "%4x", &hi); base = hi << 4; }
  }
  fclose(f);
  *len = max;
  return 1;
}

static uint32_t btn_mask(const char *s) {
  if(!strncmp(s, "up", 2)) return BTN_UP;
  if(!strncmp(s, "down", 4)) return BTN_DOWN;
  if(!strncmp(s, "left", 4)) return BTN_LEFT;
  if(!strncmp(s, "right", 5)) return BTN_RIGHT;
  if(!strncmp(s, "act", 3)) return BTN_ACT;
  if(!strncmp(s, "b@", 2)) return BTN_B;
  return 0;
}

static void save_pgm(const char *file) {
  uint8_t *fb = emu_framebuffer();
  FILE *f = fopen(file, "wb");
  if(!f) { perror(file); return; }
  fprintf(f, "P5\n128 64\n255\n");
  fwrite(fb, 1, 128 * 64, f);
  fclose(f);
}

int main(int argc, char **argv) {
  if(argc < 2) { fprintf(stderr, "usage: %s firmware.bin [-t ms] [-p btn@a-b] [-s ms:file.pgm] [-a audio.raw] [-e in.bin] [-E out.bin] [-F flash.bin] [-P ms] [-S ms] [-R ram.bin] [-r 11|12|13] [-v mV] [-L leak ohms] [-q]\n", argv[0]); return 1; }
  static uint8_t img[FLASH_SIZE];
  memset(img, 0xFF, sizeof img);
  uint32_t len = 0;
  const char *path = argv[1];
  size_t pl = strlen(path);
  if(pl > 4 && !strcmp(path + pl - 4, ".hex")) {
    if(!load_hex(path, img, &len)) { perror(path); return 1; }
  } else {
    FILE *f = fopen(path, "rb");
    if(!f) { perror(path); return 1; }
    len = (uint32_t)fread(img, 1, FLASH_SIZE, f);
    fclose(f);
  }

  uint32_t total_ms = 3000, np = 0, ns = 0, quiet = 0;
  static Press press[8192]; Shot shot[64];
  const char *audio_file = 0, *ee_in = 0, *ee_out = 0, *flash_out = 0, *ram_out = 0;
  uint32_t power[16], npow = 0, soft[16], nsoft = 0;
  int board = 13;
  uint32_t vcc = 0, leak = 0;
  for(int i = 2; i < argc; i++) {
    if(!strcmp(argv[i], "-t") && i + 1 < argc) total_ms = (uint32_t)atoi(argv[++i]);
    else if(!strcmp(argv[i], "-i") && i + 1 < argc) {
      FILE *f = fopen(argv[++i], "r");
      if(!f) { perror(argv[i]); return 1; }
      char b[16]; unsigned a, z;
      while(np < 8192 && fscanf(f, "%15s %u %u", b, &a, &z) == 3) {
        char t[20]; snprintf(t, sizeof t, "%s@", b);
        press[np].mask = btn_mask(t); press[np].from = a; press[np].to = z; np++;
      }
      fclose(f);
    }
    else if(!strcmp(argv[i], "-p") && i + 1 < argc && np < 8192) {
      char *s = argv[++i], *at = strchr(s, '@');
      if(!at) continue;
      press[np].mask = btn_mask(s);
      press[np].from = (uint32_t)atoi(at + 1);
      char *dash = strchr(at, '-');
      press[np].to = dash ? (uint32_t)atoi(dash + 1) : press[np].from + 100;
      np++;
    } else if(!strcmp(argv[i], "-s") && i + 1 < argc && ns < 64) {
      char *s = argv[++i], *c = strchr(s, ':');
      if(!c) continue;
      shot[ns].at = (uint32_t)atoi(s); shot[ns].file = c + 1; ns++;
    } else if(!strcmp(argv[i], "-a") && i + 1 < argc) audio_file = argv[++i];
    else if(!strcmp(argv[i], "-e") && i + 1 < argc) ee_in = argv[++i];
    else if(!strcmp(argv[i], "-E") && i + 1 < argc) ee_out = argv[++i];
    else if(!strcmp(argv[i], "-F") && i + 1 < argc) flash_out = argv[++i];
    else if(!strcmp(argv[i], "-P") && i + 1 < argc && npow < 16) power[npow++] = (uint32_t)atoi(argv[++i]);
    else if(!strcmp(argv[i], "-S") && i + 1 < argc && nsoft < 16) soft[nsoft++] = (uint32_t)atoi(argv[++i]);
    else if(!strcmp(argv[i], "-n")) board = 11;
    else if(!strcmp(argv[i], "-r") && i + 1 < argc) board = atoi(argv[++i]);
    else if(!strcmp(argv[i], "-R") && i + 1 < argc) ram_out = argv[++i];
    else if(!strcmp(argv[i], "-v") && i + 1 < argc) vcc = (uint32_t)atoi(argv[++i]);
    else if(!strcmp(argv[i], "-L") && i + 1 < argc) leak = (uint32_t)atoi(argv[++i]);   // joypad leak, ohms
    else if(!strcmp(argv[i], "-q")) quiet = 1;
  }

  emu_init();
  emu_set_board((uint32_t)board);
  if(vcc) E.vcc_mv = vcc;
  E.pad_leak = leak;
  if(ee_in) {
    FILE *f = fopen(ee_in, "rb");
    if(!f) { perror(ee_in); return 1; }
    fread(eeprom_mem, 1, EEPROM_SIZE, f);
    fclose(f);
  }
  emu_load(img, len);
  if(ram_out) memset(E.sram, 0xA5, SRAM_SIZE);
  FILE *af = audio_file ? fopen(audio_file, "wb") : 0;

  clock_t t0 = clock();
  uint32_t status = 0, frames0 = 0;
  for(uint32_t ms = 0; ms < total_ms; ms++) {
    uint32_t mask = 0;
    for(uint32_t k = 0; k < np; k++) if(ms >= press[k].from && ms < press[k].to) mask |= press[k].mask;
    emu_set_buttons(mask);
    for(uint32_t k = 0; k < npow; k++) if(power[k] == ms && ms) emu_reset();
    for(uint32_t k = 0; k < nsoft; k++) if(soft[k] == ms && ms) emu_soft_reset();
    for(uint32_t k = 0; k < ns; k++) if(shot[k].at == ms) save_pgm(shot[k].file);
    status = emu_run_us(1000);
    if(af) { fwrite(E.audio, 2, E.audio_len, af); }
    E.audio_len = 0;
    if(status) { fprintf(stderr, "stopped at %u ms (status %u)\n", ms, status); break; }
  }
  double host = (double)(clock() - t0) / CLOCKS_PER_SEC;
  for(uint32_t k = 0; k < ns; k++) if(shot[k].at >= total_ms) save_pgm(shot[k].file);
  if(af) fclose(af);
  if(ee_out) {
    FILE *f = fopen(ee_out, "wb");
    if(!f) { perror(ee_out); return 1; }
    fwrite(eeprom_mem, 1, EEPROM_SIZE, f);
    fclose(f);
  }

  if(ram_out) {
    FILE *f = fopen(ram_out, "wb");
    if(!f) { perror(ram_out); return 1; }
    fwrite(E.sram, 1, SRAM_SIZE, f);
    fclose(f);
  }

  if(flash_out) {
    FILE *f = fopen(flash_out, "wb");
    if(!f) { perror(flash_out); return 1; }
    fwrite(E.flash, 1, FLASH_SIZE, f);
    fclose(f);
  }

  if(E.uart_len) { printf("--- serial ---\n%.*s\n", (int)E.uart_len, E.uart); }
  if(E.log_len) { printf("--- simulator ---\n%.*s", (int)E.log_len, E.log); }
  if(!quiet) {
    printf("firmware: %u bytes, HCLK %u Hz, emulated %.3f s, %llu instr, %llu cycles\n",
           len, E.hclk, (double)E.cycles / E.hclk, (unsigned long long)E.instret, (unsigned long long)E.cycles);
    printf("host time %.3f s (%.1fx realtime, %.1f MIPS)  oled data bytes %u, frames %u, pc=0x%08x\n",
           host, ((double)E.cycles / E.hclk) / (host > 0 ? host : 1e-9),
           E.instret / (host > 0 ? host : 1e-9) / 1e6, E.oled.data_bytes, E.oled.frames - frames0, E.pc);
    printf("state size %u bytes, eeprom page writes %u, flash page erase/program %u\n",
           emu_state_size(), E.eeprom.writes, E.sys.flash_ops);
    uint32_t mx = 0, at = 0, n = 0;
    for(uint32_t i = 0; i < FLASH_SIZE / 64; i++) {
      if(flash_page_erases[i] > mx) { mx = flash_page_erases[i]; at = i * 64; }
      n += flash_page_erases[i] != 0;
    }
    printf("flash wear: most erased page 0x%04x: %u erases, %u pages erased at least once\n", at, mx, n);
    if(getenv("CHSIM_WEAR")) for(uint32_t i = 0; i < FLASH_SIZE / 64; i++)
      if(flash_page_erases[i]) printf("wear 0x%04x %u\n", i * 64, flash_page_erases[i]);
  }
  return status == 1 ? 2 : 0;
}
