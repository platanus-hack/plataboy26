// ===================================================================================
// plataboy26 framework extensions for the games in the library           (sysx.c)
// ===================================================================================
//
// k = SYS_poll(pad) is called with every joypad reading (ADC value of PC4):
//   - returns the joypad state (games/common/joycal.h): JOYCAL_IDLE 0, R 1, U 2, UR 3,
//     D 4, DR 5, UD 6, L 7, UL 8, DL 9, JOYCAL_NONE 10, decoded with the thresholds in
//     SYS_cal, which follow the board's idle level. The ports index an 11-entry table of
//     their button bits with it. A port with an EEPROM driver reads SYS_cal from the
//     EEPROM (the menu measures them at power-on) before its first SYS_poll:
//         EE_read(JOYCAL_EE, SYS_cal, JOYCAL_N);
//     with -DSYSX_SELFCAL (and in the Tiny games) they are computed from the first
//     reading instead.
//   - UP + DOWN held together for 2 s: back to the menu (the kernel's install(0),
//     firmware/kernel/kernel.S). The menu is put back into flash and starts.
//   - low battery: every 30 s (and at the first reading) VDD is measured against the
//     internal 1.2 V reference; below SYSX_BATT_MV a short double beep sounds.
//     -DSYSX_NO_BATTERY leaves it out (games without flash to spare).
// Time base: the free-running SysTick counter (STK->CNT at F_CPU), as every game here
// sets it up (system.h STK_init); compare interrupts don't disturb it.
//
// The original Tiny games get it through ADC_read() (sysx.c's replacement of the
// inline one in gpio.h, games/tiny/Makefile), the ports call SYS_poll() themselves.

#pragma once
#include <stdint.h>
#include "joycal.h"
#include "buzzer.h"                 // BUZ_PULSE_US: longest low phase of the buzzer pin

#ifndef KBASE
#define KBASE         0x3C40            // firmware/Makefile: the kernel's flash address
#endif
#define SYSX_HOLD_S   2                 // seconds to hold for the menu
#ifndef SYSX_BATT_MV
#define SYSX_BATT_MV  2800              // warning threshold (CR2032: 3.0 V nominal)
#endif
#define SYSX_BATT_S   30                // seconds between battery checks
// The OLED contrast chosen in the menu's settings (SSD1306 0x81 value): the menu sets it
// before a game starts and the SSD1306 keeps it (OLED_init doesn't send 0x81), so only a
// game that changes the contrast itself (Arduventure's fades) needs this, to go back to it.
// The kernel keeps it in its config page (firmware/kernel/kernel.S, config + 16).
#define SYSX_CONTRAST (*(const volatile uint8_t *)0x08003FD0)

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t SYS_cal[];
uint8_t SYS_poll(uint16_t pad);   // -> joypad state 0..10 (joycal.h)
void SYS_exit(void) __attribute__((noreturn));
void SYS_trap(void);              // CPU exception / NMI -> the menu (sysx.c, systrap.py)
#ifdef __cplusplus
}
#endif
