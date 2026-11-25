// ===================================================================================
// Project:   EEPROM Test - bring-up test for the AT24CM01 on the game console
// ===================================================================================
//
// Checks the 128 KiB I2C EEPROM next to the OLED and prints the results:
//   page 1: detected?, write cycle time (acknowledge polling), page write + verify
//           across the 64 KiB boundary (device address bit P0), read speed
//   page 2: summary PASS / FAIL
// The two test pages (0x0FF00..0x100FF) are saved first and restored at the end, so
// an EEPROM full of games survives the test. Fire button toggles the pages.

#include "driver.h"
#include "eeprom.h"

#define TEST_ADDR  0x0FF00UL           // last page of bank 0 + first page of bank 1
#define TEST_LEN   512

static uint8_t saved[TEST_LEN];
static uint8_t buf[TEST_LEN];

static uint32_t ticks(void) { return STK->CNT; }
static uint32_t us_since(uint32_t t0) { return (ticks() - t0) / (F_CPU / 1000000); }

static void print_err(uint8_t r) {
  OLED_print(r == EE_OK ? "ok" : r == EE_NACK ? "NACK" : "TIMEOUT");
}

int main(void) {
  JOY_init();
  DLY_ms(10);

  uint8_t found, ok_rw = 0, ok_seq = 0, ok_restore = 0;
  uint32_t twr_us = 0, read_kbs = 0;

  found = EE_probe();
  if(found == EE_OK) {
    EE_read(TEST_ADDR, saved, TEST_LEN);                        // keep the user data

    // one page write, timed: STOP -> acknowledge polling
    for(uint16_t i = 0; i < EE_PAGE; i++) buf[i] = (uint8_t)(i * 7 + 3);
    uint32_t t0 = ticks();
    EE_write(TEST_ADDR, buf, EE_PAGE);
    twr_us = us_since(t0);

    // second page in the upper bank (device address 0x51)
    for(uint16_t i = 0; i < EE_PAGE; i++) buf[EE_PAGE + i] = (uint8_t)(i ^ 0xA5);
    EE_write(TEST_ADDR + EE_PAGE, buf + EE_PAGE, EE_PAGE);

    // verify: one sequential read across the 64 KiB boundary
    static uint8_t rd[TEST_LEN];
    ok_rw = EE_read(TEST_ADDR, rd, EE_PAGE) == EE_OK;
    for(uint16_t i = 0; i < EE_PAGE; i++) if(rd[i] != buf[i]) ok_rw = 0;
    ok_seq = EE_read(TEST_ADDR + 128, rd, 256) == EE_OK;
    for(uint16_t i = 0; i < 256; i++) if(rd[i] != buf[128 + i]) ok_seq = 0;
    // the WCH reference sequence must return the same bytes
    if(EE_read_safe(TEST_ADDR + 250, rd, 12)) ok_seq = 0;
    for(uint16_t i = 0; i < 12; i++) if(rd[i] != buf[250 + i]) ok_seq = 0;
    // short reads use different receive sequences (1, 2, 3 bytes)
    for(uint8_t n = 1; n <= 3; n++) {
      uint8_t s[3];
      if(EE_read(TEST_ADDR + 250, s, n)) ok_seq = 0;
      for(uint8_t i = 0; i < n; i++) if(s[i] != buf[250 + i]) ok_seq = 0;
    }

    // read throughput: 8 KiB in 512-byte blocks
    t0 = ticks();
    for(uint32_t a = 0; a < 8192; a += TEST_LEN) EE_read(a, rd, TEST_LEN);
    uint32_t us = us_since(t0);
    read_kbs = us ? 8192UL * 1000 / us : 0;                     // bytes/ms = KB/s

    // restore the user data
    EE_write(TEST_ADDR, saved, TEST_LEN);
    ok_restore = EE_read(TEST_ADDR, rd, TEST_LEN) == EE_OK;
    for(uint16_t i = 0; i < TEST_LEN; i++) if(rd[i] != saved[i]) ok_restore = 0;
  }
  uint8_t pass = found == EE_OK && ok_rw && ok_seq && ok_restore;

  uint8_t page = 0;
  while(1) {                                       // terminal font: 4 lines per page
    OLED_clear();
    if(!page) {
      OLED_print("EEPROM 0x50: "); print_err(found); OLED_write('\n');
      OLED_print("page write "); OLED_printD(twr_us); OLED_print("us\n");
      OLED_print("wr "); OLED_print(ok_rw ? "ok" : "BAD");
      OLED_print(" seq "); OLED_print(ok_seq ? "ok" : "BAD"); OLED_write('\n');
      OLED_print("read "); OLED_printD(read_kbs); OLED_print(" KB/s");
    } else {
      OLED_println(pass ? "EEPROM PASS" : "EEPROM FAIL");
      OLED_print("restore "); OLED_print(ok_restore ? "ok" : "BAD"); OLED_write('\n');
      OLED_print("128 KiB AT24CM01");
    }
    while(JOY_act_released());
    while(JOY_act_pressed());
    page ^= 1;
  }
}
