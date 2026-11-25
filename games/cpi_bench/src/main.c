// ===================================================================================
// Project:   CPI Bench - instruction timing calibration for the CH32V003
// ===================================================================================
//
// Measures the cycles per instruction of a few instruction classes with SysTick
// (HCLK) and prints them on the OLED as cycles x100. Run it on the real console and
// in the simulator: if the numbers differ, adjust the simulator's timing model
// (Hardware tab -> CPU timing model) until they match.
//
//   make                     -> 12 MHz build (0 flash wait states)
//   make F_CPU=48000000      -> 48 MHz build (1 flash wait state)
//
// All measured instructions are 32-bit encodings (.option norvc), except "c.add".

#include "driver.h"

#define ITER   200
#define REPS   "20"

// A benchmark: loop ITER times over a block of 20 copies of BODY.
// a2 = flash table, a3 = GPIOA base, sp = SRAM scratch.
#define BENCH(fn, body)                                                       \
  static __attribute__((noinline)) uint32_t fn(void) {                        \
    uint32_t start, end;                                                      \
    asm volatile(                                                             \
      "  li   t1, %[n]       \n"                                              \
      "  la   a2, bench_tab  \n"                                              \
      "  li   a3, 0x40010800 \n"                                              \
      "  li   a5, 0xE000F008 \n"                                              \
      "  lw   %[s], 0(a5)    \n"                                              \
      "  .balign 4           \n"                                              \
      "1:                    \n"                                              \
      "  .option push        \n"                                              \
      "  .option norvc       \n"                                              \
      "  .rept " REPS "      \n"                                              \
      body "\n"                                                               \
      "  .endr               \n"                                              \
      "  .option pop         \n"                                              \
      "  addi t1, t1, -1     \n"                                              \
      "  bnez t1, 1b         \n"                                              \
      "  lw   %[e], 0(a5)    \n"                                              \
      : [s] "=&r" (start), [e] "=&r" (end)                                    \
      : [n] "i" (ITER) : "t0", "t1", "a2", "a3", "a4", "a5", "memory");       \
    return end - start;                                                       \
  }

__attribute__((used)) const uint32_t bench_tab[4] = {1, 2, 3, 4};

BENCH(b_base,   "")
BENCH(b_add,    "add  t0, t0, t1")
BENCH(b_cadd,   ".option rvc \n c.add a4, a4 \n .option norvc")
BENCH(b_lw,     "lw   t0, 0(sp)")
BENCH(b_sw,     "sw   t0, 0(sp)")
BENCH(b_lwf,    "lw   t0, 0(a2)")
BENCH(b_mmio,   "lw   t0, 8(a3)")
BENCH(b_bt,     "beq  x0, x0, 2f \n 2:")
BENCH(b_bn,     "bne  x0, x0, 2f \n 2:")
BENCH(b_jal,    "jal  t0, 2f \n 2:")

static void cpi(char *name, uint32_t t, uint32_t base) {
  uint32_t per = ((t - base) * 100 + (ITER * 20 / 2)) / (ITER * 20);   // cycles x100
  OLED_print(name);
  OLED_printD(per / 100); OLED_write('.');
  if(per % 100 < 10) OLED_write('0');
  OLED_printD(per % 100);
}

int main(void) {
  JOY_init();
  DLY_ms(10);
  uint32_t base = b_base();
  uint32_t r[9] = { b_add(), b_cadd(), b_lw(), b_sw(), b_lwf(), b_mmio(), b_bt(), b_bn(), b_jal() };
  uint8_t page = 0;
  while(1) {                                         // the terminal font fits 4 lines:
    OLED_clear();                                    // two pages, fire button toggles
    OLED_print("CPI @"); OLED_printD(F_CPU / 1000000); OLED_print("MHz  "); OLED_printD(page + 1); OLED_println("/2");
    if(!page) {
      cpi("add  ", r[0], base); cpi(" c.add", r[1], base); OLED_write('\n');
      cpi("lw   ", r[2], base); cpi(" sw   ", r[3], base); OLED_write('\n');
      cpi("lw fl", r[4], base); cpi(" mmio ", r[5], base);
    } else {
      cpi("b tk ", r[6], base); cpi(" b nt ", r[7], base); OLED_write('\n');
      cpi("jal  ", r[8], base);
    }
    while(JOY_act_released());
    while(JOY_act_pressed());
    page ^= 1;
  }
}
