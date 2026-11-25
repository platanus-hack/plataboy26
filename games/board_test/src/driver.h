// ===================================================================================
// Tiny Joypad Drivers for CH32V003                                           * v1.0 *
// ===================================================================================
//
// MCU abstraction layer.
// 2023 by Stefan Wagner:   https://github.com/wagiminator

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "system.h"
#include "gpio.h"
#define JOYCAL_VALUES
#define JOYCAL_COMPUTE
#include "joycal.h"   // games/common: the joypad decoded against its idle level
#include "oled_term.h"

// Pin assignments
#define PIN_ACT     PA2   // pin connected to fire button
#define PIN_BEEP    PA1   // pin connected to buzzer
#define PIN_PAD     PC4   // pin conected to direction buttons
#define PIN_SCL     PC2   // pin connected to OLED (I2C SCL)
#define PIN_SDA     PC1   // pin connected to OLED (I2C SDA)

// Joypad calibration values
#define JOY_N       197   // joypad UP
#define JOY_NE      259   // joypad UP + RIGHT
#define JOY_E       90    // joypad RIGHT
#define JOY_SE      388   // joypad DOWN + RIGHT
#define JOY_S       346   // joypad DOWN
#define JOY_SW      616   // joypad DOWN + LEFT
#define JOY_W       511   // joypad LEFT
#define JOY_NW      567   // JOYPAD UP + LEFT
#define JOY_DEV     20    // deviation

// Second action button B (board rev. 1.3): shares PA2 with ACT. ACT pulls PA2 to GND,
// B to ~0.91 VDD through 100k against the 10k pull-up, so B is only seen by the ADC
// (ACT wins when both are pressed). On rev. 1.1/1.2 boards B never reads as pressed.
// B is decided against the idle level of PA2, not a fixed window: idle can read below 1023
// (the provider's sample board: idle 972, B 908). B alone is 0.91-0.935 of idle; B = above
// JOY_B_MIN (A is ~0) and below idle * (1 - 1/2^JOY_B_SHIFT) = 0.969 idle.
#define JOY_B_MIN   500
#define JOY_B_SHIFT 5

// Sound enable
#define JOY_SOUND   1     // 0: no sound, 1: with sound

// Game slow-down delay
#define JOY_SLOWDOWN()    //DLY_ms(10)

// Init driver
static inline void JOY_init(void) {
  PIN_input_AN(PIN_PAD);
  PIN_input_PU(PIN_ACT);
  PIN_output(PIN_BEEP);
  PIN_high(PIN_BEEP);
  OLED_init();
  ADC_init();
  ADC1->SAMPTR2 |= 7 | 7 << 6;   // PA2 (channel 0) and PC4 (channel 2, joypad): longest sample
                                 // time, so each reading settles after the other channel (at
                                 // 48 MHz the 3-cycle default could leave the joypad reading high)
  ADC_input(PIN_PAD);
}

// OLED commands
#define JOY_OLED_init             OLED_init
#define JOY_OLED_end              I2C_stop
#define JOY_OLED_send(b)          I2C_write(b)
#define JOY_OLED_send_command(c)  OLED_send_command(c)
#define JOY_OLED_data_start(y)    {OLED_setpos(0,y);OLED_data_start();}

// Buttons
#define JOY_act_pressed()         (!PIN_read(PIN_ACT))
#define JOY_act_released()        (PIN_read(PIN_ACT))
// Joypad reading, calibrated: the canonical JOY_xx value of the nearest state (joycal.h).
// JOY_cal: the thresholds; computed from the first reading (nothing held at the start)
// unless set before (the menu: from the EEPROM, measured during the boot animation).
static uint8_t JOY_cal[JOYCAL_N];
__attribute__((noinline)) static uint16_t JOY_pad_read(void) {
  uint16_t v = ADC_read();
#ifndef JOY_CAL_SET                // (the menu: always set first, by cal_update)
  if(!JOY_cal[0]) joycal_thresholds(JOY_cal, v < JOYCAL_IDLE_MAX ? v : 0);
#endif
  return joycal_val(joycal_dec(v, JOY_cal));
}
#define JOY_pad_pressed()         (JOY_pad_read() > 10)
#define JOY_pad_released()        (JOY_pad_read() <= 10)
#define JOY_all_released()        (JOY_act_released() && JOY_pad_released() && !JOY_b_pressed())

// Raw ADC value of PA2 (ACT/B); the ADC is switched back to the joypad afterwards.
// PA2 has a ~9k source impedance and the sample cap still holds the joypad voltage (~0 idle):
// with the default 3-cycle sample it read ~5% low on real boards (idle 972, B 908), so idle
// fell inside the B window. JOY_init() gives channel 0 the 241-cycle sample time.
static inline uint16_t JOY_actb_raw(void) {
  ADC_input(PIN_ACT);
  uint16_t val = ADC_read();
  ADC_input(PIN_PAD);
  return val;
}

// Idle level of PA2, learned from every reading that is not A or B: it climbs from 0 within
// ~20 reads (B is never seen while it is below the real level) and then follows it, 1/4 of
// the way per reading. B held at power-on: missed until released.
static uint16_t JOY_b_idle;

static inline uint8_t JOY_b_pressed(void) {
  uint16_t val = JOY_actb_raw();
  if(val <= JOY_B_MIN) return 0;                         // A (wins over B)
  if(val + (JOY_b_idle >> JOY_B_SHIFT) < JOY_b_idle) return 1;
  JOY_b_idle += (int16_t)(val - JOY_b_idle) >> 2;
  return 0;
}
#define JOY_b_released()          (!JOY_b_pressed())

static inline uint8_t JOY_up_pressed(void) {
 uint16_t val = JOY_pad_read();
 return(   ((val > JOY_N  - JOY_DEV) && (val < JOY_N  + JOY_DEV))
         | ((val > JOY_NE - JOY_DEV) && (val < JOY_NE + JOY_DEV))
         | ((val > JOY_NW - JOY_DEV) && (val < JOY_NW + JOY_DEV)) );
}

static inline uint8_t JOY_down_pressed(void) {
 uint16_t val = JOY_pad_read();
 return(   ((val > JOY_S  - JOY_DEV) && (val < JOY_S  + JOY_DEV))
         | ((val > JOY_SE - JOY_DEV) && (val < JOY_SE + JOY_DEV))
         | ((val > JOY_SW - JOY_DEV) && (val < JOY_SW + JOY_DEV)) );
}

static inline uint8_t JOY_left_pressed(void) {
 uint16_t val = JOY_pad_read();
 return(   ((val > JOY_W  - JOY_DEV) && (val < JOY_W  + JOY_DEV))
         | ((val > JOY_NW - JOY_DEV) && (val < JOY_NW + JOY_DEV))
         | ((val > JOY_SW - JOY_DEV) && (val < JOY_SW + JOY_DEV)) );
}

static inline uint8_t JOY_right_pressed(void) {
 uint16_t val = JOY_pad_read();
 return(   ((val > JOY_E  - JOY_DEV) && (val < JOY_E  + JOY_DEV))
         | ((val > JOY_NE - JOY_DEV) && (val < JOY_NE + JOY_DEV))
         | ((val > JOY_SE - JOY_DEV) && (val < JOY_SE + JOY_DEV)) );
}

// Buzzer
#include "buzzer.h"
void JOY_sound(uint8_t freq, uint8_t dur) {
  uint8_t h = 255 - freq, lo = h < BUZ_PULSE_US ? h : BUZ_PULSE_US;
  while(dur--) {
    #if JOY_SOUND == 1
    if(freq) PIN_low(PIN_BEEP);
    #endif
    // plataboy26 BUZ_PULSE_US: low for at most that long, same period (games/common/buzzer.h)
    DLY_us(lo);
    PIN_high(PIN_BEEP);
    DLY_us(2 * h - lo);
  }
}

// Pseudo random number generator
uint16_t rnval = 0xACE1;
uint16_t JOY_random(void) {
  rnval = (rnval >> 0x01) ^ (-(rnval & 0x01) & 0xB400);
  return rnval;
}

// Delays
#define JOY_DLY_ms    DLY_ms
#define JOY_DLY_us    DLY_us

// Additional Defines
#define abs(n) ((n>=0)?(n):(-(n)))

long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

#ifdef __cplusplus
};
#endif
