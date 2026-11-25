// ===================================================================================
// Project:   Board Test - bring-up test for the rev. 1.3/1.4 game console
// ===================================================================================
//
// At power-on: EEPROM test (same checks as games/eeprom_test, user data restored).
// Then a live input monitor, 6 lines:
//   EEPROM PASS 43K       result of the EEPROM test, read speed in KB/s
//   PAD  197 U            joypad ADC (PC4) and the decoded direction(s)
//   AB   947 B            ACT/B ADC (PA2): ~1023 idle, ~930-950 B, ~0 A
//   fast 972 idle 1019    PA2 with the old 3-cycle sample time (reads low if the ADC does not
//                         settle), and the idle level JOY_b_pressed() has learned (B threshold
//                         = 0.969 idle)
//   cal   48 148 232     joypad thresholds from the EEPROM (measured by the menu at power-on,
//                         games/common/joycal.h): idle below the 1st, RIGHT from it, UP from
//                         the 3rd; higher than these on a board whose joypad idles above 0
//   keys ..R..B           U D L R A B, a letter while held
// Press every button once: each must light up alone, and the buzzer clicks on A or B.

#include "driver.h"
#include "eeprom.h"

#define TEST_ADDR  0x0FF00UL           // last page of bank 0 + first page of bank 1
#define TEST_LEN   512

static uint8_t saved[TEST_LEN];
static uint8_t buf[TEST_LEN];
static uint8_t rd[TEST_LEN];

static uint32_t ticks(void) { return STK->CNT; }

// EEPROM test: returns 1 on PASS, read speed in KB/s through *kbs
static uint8_t eeprom_test(uint32_t *kbs) {
  *kbs = 0;
  if(EE_probe() != EE_OK) return 0;
  if(EE_read(TEST_ADDR, saved, TEST_LEN)) return 0;
  for(uint16_t i = 0; i < TEST_LEN; i++) buf[i] = (uint8_t)(i * 7 + 3);
  uint8_t ok = EE_write(TEST_ADDR, buf, TEST_LEN) == EE_OK;
  ok &= EE_read(TEST_ADDR, rd, TEST_LEN) == EE_OK;               // across 0xFFFF -> 0x10000
  for(uint16_t i = 0; i < TEST_LEN; i++) ok &= rd[i] == buf[i];
  ok &= EE_read_safe(TEST_ADDR + 250, rd, 12) == EE_OK;          // WCH reference sequence
  for(uint16_t i = 0; i < 12; i++) ok &= rd[i] == buf[250 + i];
  for(uint8_t n = 1; n <= 3; n++) {                              // 1, 2, 3 byte receive paths
    ok &= EE_read(TEST_ADDR + 100, rd, n) == EE_OK;
    for(uint8_t i = 0; i < n; i++) ok &= rd[i] == buf[100 + i];
  }
  uint32_t t0 = ticks();
  for(uint32_t a = 0; a < 8192; a += TEST_LEN) ok &= EE_read(a, rd, TEST_LEN) == EE_OK;
  uint32_t us = (ticks() - t0) / (F_CPU / 1000000);
  *kbs = us ? 8192UL * 1000 / us : 0;
  ok &= EE_write(TEST_ADDR, saved, TEST_LEN) == EE_OK;           // restore user data
  ok &= EE_read(TEST_ADDR, rd, TEST_LEN) == EE_OK;
  for(uint16_t i = 0; i < TEST_LEN; i++) ok &= rd[i] == saved[i];
  return ok;
}

// redraw in place (no clear, no flicker): cursor home, and each line padded to the end
extern uint8_t line, column, scroll;
void OLED_setline(uint8_t line);
static void home(void) { line = 0; column = 0; OLED_setline(scroll & 7); }
static void eol(void) { uint8_t l = line; while(line == l) OLED_write(' '); }

static void print3(uint16_t v) {                                   // right-aligned 0..9999
  if(v < 1000) OLED_write(' ');
  if(v < 100) OLED_write(' ');
  if(v < 10) OLED_write(' ');
  OLED_printD(v);
}

// PA2 the way firmware before the fix read it: 3-cycle sample right after the joypad
static uint16_t actb_fast(void) {
  uint32_t smp = ADC1->SAMPTR2;
  ADC1->SAMPTR2 = smp & ~7UL;
  ADC_read();                                                      // joypad conversion first
  uint16_t v = JOY_actb_raw();
  ADC1->SAMPTR2 = smp;
  return v;
}

int main(void) {
  JOY_init();
  DLY_ms(10);
  uint32_t kbs;
  uint8_t pass = eeprom_test(&kbs);
  // the joypad thresholds the games use (measured by the menu at power-on, joycal.h); if
  // there are none, JOY_pad_read() computes them from its first reading
  if(EE_read(JOYCAL_EE, JOY_cal, JOYCAL_N) != EE_OK || JOY_cal[0] < 11 || JOY_cal[0] == 0xFF) JOY_cal[0] = 0;
  uint8_t prev = 0;
  OLED_clear();

  while(1) {
    uint16_t fast = actb_fast();
    uint16_t pad = ADC_read();
    uint16_t ab = JOY_actb_raw();
    uint8_t u = JOY_up_pressed(), d = JOY_down_pressed(), l = JOY_left_pressed(), r = JOY_right_pressed();
    uint8_t a = JOY_act_pressed(), b = JOY_b_pressed();
    uint8_t now = u | d << 1 | l << 2 | r << 3 | a << 4 | b << 5;

    home();
    OLED_print("EEPROM "); OLED_print(pass ? "PASS " : "FAIL "); OLED_printD(kbs); OLED_write('K'); eol();
    OLED_print("PAD "); print3(pad); OLED_write(' ');
    if(u) OLED_write('U');
    if(d) OLED_write('D');
    if(l) OLED_write('L');
    if(r) OLED_write('R');
    eol();
    OLED_print("AB  "); print3(ab); OLED_write(' ');
    if(a) OLED_write('A');
    if(b) OLED_write('B');
    eol();
    OLED_print("fast "); print3(fast); OLED_print(" idle "); print3(JOY_b_idle); eol();
    OLED_print("cal ");                                            // idle below, RIGHT from,
    print3(JOY_cal[0] * 4); print3(JOY_cal[1] * 4); print3(JOY_cal[2] * 4); eol();   // UP from
    OLED_print("keys ");
    OLED_write(u ? 'U' : '.'); OLED_write(d ? 'D' : '.'); OLED_write(l ? 'L' : '.');
    OLED_write(r ? 'R' : '.'); OLED_write(a ? 'A' : '.'); OLED_write(b ? 'B' : '.'); eol();

    if((now & 0x30) && !(prev & 0x30)) JOY_sound(200, 20);       // click on A or B
    prev = now;
    DLY_ms(40);
  }
}
