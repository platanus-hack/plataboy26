// ===================================================================================
// Joypad calibration: the resistor ladder on PC4 decoded against its idle level
// ===================================================================================
//
// The joypad is decoded with 10 thresholds (raw ADC / 4, rising) into a state
//   JOYCAL_IDLE 0, R 1, U 2, UR 3, D 4, DR 5, UD 6, L 7, UL 8, DL 9, JOYCAL_NONE 10
// (NONE: above every single button and diagonal); the ports index their own 11-entry
// bits table with it, joycal_val[] gives the canonical JOY_xx value instead (for code
// that decodes with the JOY_xx +-JOY_DEV windows: the Tiny games, the menu, board_test).
//
// Where the thresholds come from:
//   - the menu measures the idle level while the boot animation plays (nothing is held at
//     power-on), computes the thresholds and keeps them in the EEPROM at JOYCAL_EE (the
//     menu's save slot, firmware/menu/src/library.h). The packer writes the nominal ones
//     there (idle 0), so the EEPROM never holds none. Every game starts from the menu.
//   - the ports with an EEPROM driver read the 10 bytes at their start (no arithmetic
//     in the game: a few compares per reading)
//   - the Tiny games, Llegamos tarde and board_test compute them from their first
//     reading (sysx.c SYSX_SELFCAL), as they start with the joypad released
//
// Why: on the design PC4 idles at 0 (R6 2k to GND), but leakage to VCC (flux residue, a
// switch held lightly by the case) lifts it, e.g. to 60..110, where the fixed windows saw
// RIGHT held and UP fell out of its window. A leak adds a conductance G to the pull-ups,
// v = (Gx + G) / (Gx + G + G6); with i = the idle reading, a state whose reading without
// the leak would be c reads exactly
//     v = 1024 (c (1024 - 2 i) + 1024 i) / (1024^2 - c i).
// The thresholds are the midpoints between the ladder's nominal values (R2..R6 = 8k2 3k9
// 20k 2k 2k: idle 0, R 93, U 201, UR 262, D 347, DR 389, UD 441, L 512, UL 567, DL 616)
// moved this way, so each state tolerates half the distance to its neighbours (UP -53/+30,
// RIGHT -46/+53, the tightest D/DR +-21) instead of +-20 around a fixed value.
// Checked on the host for leaks and resistor tolerance by hardware/verify/check_joycal.py.

#pragma once
#include <stdint.h>

#define JOYCAL_EE        0x1F004UL      // 10 bytes in the menu's save slot (library.h)
#define JOYCAL_N         10
#define JOYCAL_UD        6              // UP + DOWN: back to the menu (sysx.c)
#define JOYCAL_NONE      10
#define JOYCAL_IDLE_MAX  160            // above: a button is held (UP alone is ~201 and up)

// the state of a reading
static inline uint8_t joycal_dec(uint16_t v, const uint8_t *t) {
  uint8_t k = 0;
  v >>= 2;
  while(k < JOYCAL_N && v >= t[k]) k++;
  return k;
}

#ifdef JOYCAL_COMPUTE
// the thresholds for idle level i (0..JOYCAL_IDLE_MAX - 1)
// (the midpoints / 4, rounded down: up to 2 LSB lower than in the comment above)
static void joycal_thresholds(uint8_t *t, uint32_t i) {
  static const uint8_t mid[JOYCAL_N] = { 46/4, 147/4, 231/4, 304/4, 368/4, 415/4, 476/4, 539/4, 591/4, 680/4 };
  for(uint8_t k = 0; k < JOYCAL_N; k++) {
    uint32_t c = mid[k] * 4 + 2, ci = c * i;   // c (1024 - 2 i) + 1024 i = 1024 (c + i) - 2 c i
    t[k] = ((((c + i) << 10) - 2 * ci) * 1024 / ((1UL << 20) - ci) + 3) >> 2;
  }
}
#endif

#ifdef JOYCAL_VALUES
// the JOY_xx values / 4 (x 4: within 3 LSB of them, well inside +-JOY_DEV)
static const uint8_t joycal_val4[JOYCAL_N + 1] = { 0, 90/4, 197/4, 259/4, 346/4, 388/4, 440/4, 511/4, 567/4, 616/4, 255 };
#define joycal_val(k) (joycal_val4[k] * 4u)
#endif
