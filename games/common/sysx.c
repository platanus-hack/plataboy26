// ===================================================================================
// plataboy26 framework extensions: UP + DOWN -> menu, low-battery beep (see sysx.h)
// ===================================================================================

#ifdef SYSX_ADC_READ
#define JOYCAL_VALUES
#ifndef SYSX_SELFCAL
#define SYSX_SELFCAL
#endif
#endif
#ifdef SYSX_SELFCAL
#define JOYCAL_COMPUTE
#endif
#include "system.h"
#include "gpio.h"
#include "sysx.h"

void SYS_exit(void) {
  __asm volatile("csrci mstatus, 8");   // no more interrupts: the game's handlers go away
  ((void (*)(uint32_t))(KBASE + 4))(0); // install(0): the menu, then a reset
  while(1);
}

// A CPU exception or the NMI (system.c's vector table / mtvec, games/common/systrap.py):
// back to the menu like SYS_exit. install(0) starts by resetting every peripheral, so the
// buzzer pin (PA1, active low) is released at once instead of staying frozen (a low one
// keeps the coil on DC and can pull the coin cell into a brownout: devkit issue #13).
// Naked, no stack: sp may be what faulted. Interrupts are already off in a trap.
#define SYSX_STR_(x) #x
#define SYSX_STR(x) SYSX_STR_(x)
__attribute__((naked, used, aligned(4))) void SYS_trap(void) {
  __asm volatile("li a0, 0 \n li a5, " SYSX_STR(KBASE) " + 4 \n jr a5");
}

static uint32_t held_at;                // STK->CNT when UP + DOWN were first seen
static uint8_t  held;

#ifndef SYSX_NO_BATTERY
static uint32_t batt_at;                // next check (0: at the first reading)

static void beep(void) {                // ~2 kHz for 70 ms on the buzzer (PA1, active low)
  for(uint8_t i = 0; i < 140; i++) {    // low only BUZ_PULSE_US of each 500 us (buzzer.h)
    GPIOA->BCR = 1 << 1;  DLY_us(BUZ_PULSE_US);
    GPIOA->BSHR = 1 << 1; DLY_us(500 - BUZ_PULSE_US);
  }
}

// (called from SYS_poll, right after a joypad reading: the ADC goes back to PC4, channel 2)
static void battery(void) {
  ADC1->CTLR2 |= ADC_TSVREFE;
  ADC1->SAMPTR2 |= 7UL << 24;           // Vref (channel 8): longest sample time (only it uses it)
  ADC1->RSQR3 = 8;
  ADC1->CTLR2 |= ADC_SWSTART;
  while(!(ADC1->STATR & ADC_EOC));
  uint16_t raw = ADC1->RDATAR;
  ADC1->RSQR3 = 2;
  if(raw > (uint32_t)1200 * 1023 / SYSX_BATT_MV) { beep(); DLY_ms(80); beep(); }
}
#endif

uint8_t SYS_cal[JOYCAL_N];             // joypad thresholds (joycal.h), see sysx.h

uint8_t SYS_poll(uint16_t pad) {
#ifdef SYSX_SELFCAL
  if(!SYS_cal[0]) {                     // the first reading: the idle level (a button held
    if(pad >= JOYCAL_IDLE_MAX) pad = 0;  // then: idle 0, and this reading is taken as idle)
    joycal_thresholds(SYS_cal, pad);
  }
#endif
  uint8_t k = joycal_dec(pad, SYS_cal);
  uint32_t now = STK->CNT;
  if(k == JOYCAL_UD) {
    if(!held) { held = 1; held_at = now; }
    else if(now - held_at > (uint32_t)SYSX_HOLD_S * F_CPU) SYS_exit();
  }
  else held = 0;
#ifndef SYSX_NO_BATTERY
  if(!batt_at || (int32_t)(now - batt_at) >= 0) {
    batt_at = (now + (uint32_t)SYSX_BATT_S * F_CPU) | 1;
    battery();
  }
#endif
  return k;
}

#ifdef SYSX_ADC_READ
// the Tiny games' ADC_read (gpio.h's inline one is renamed away): every joypad reading
// goes through SYS_poll and comes back calibrated
uint16_t ADC_read(void) {
  ADC1->CTLR2 |= ADC_SWSTART;
  while(!(ADC1->STATR & ADC_EOC));
  uint16_t v = ADC1->RDATAR;
  if(ADC1->RSQR3 == 2) v = joycal_val(SYS_poll(v));
  return v;
}

// gcc may emit calls to these at -Os even with -fno-builtin
__attribute__((used)) void *memset(void *d, int c, unsigned n) { uint8_t *p = d; while(n--) *p++ = (uint8_t)c; return d; }
__attribute__((used)) void *memcpy(void *d, const void *s, unsigned n) { uint8_t *p = d; const uint8_t *q = s; while(n--) *p++ = *q++; return d; }
#endif
