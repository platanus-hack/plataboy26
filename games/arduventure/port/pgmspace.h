// avr/pgmspace.h subset. PROGMEM data is either in flash (read directly) or, for
// data the build moves out of flash, in the EEPROM ("far" data, see far.h): all reads
// go through pgm_read_*, which tells the two apart by address.
#pragma once
#include <stdint.h>
#include "far.h"

// PROGMEM data goes to the EEPROM unless the build keeps it in flash (NEAR)
#ifdef ARDU_ALL_NEAR
#define PROGMEM                                       /* experiment: everything in flash */
#else
#define PROGMEM  __attribute__((section(".far")))
#endif
#define NEAR     /* flash */
#define pgm_read_byte(p)  far_byte((const void *)(p))
#define pgm_read_word(p)  far_word((const void *)(p))
#define pgm_read_dword(p) far_dword((const void *)(p))
#define pgm_read_ptr(p)   ((const void *)far_dword((const void *)(p)))
