// avr/pgmspace.h subset: on the CH32V003 const data stays in flash, which is memory
// mapped, so "program memory" reads are plain reads
#pragma once
#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PSTR(s)            (s)
#define pgm_read_byte(p)   (*(const uint8_t *)(p))
#define pgm_read_word(p)   (*(const uint16_t *)(p))
#define pgm_read_ptr(p)    (*(const void * const *)(p))
#define strlen_P(s)        strlen(s)

// Port: data kept in the EEPROM instead of flash. The linker places it at EEDATA_BASE
// (an address range nothing else uses); the build copies that section into the
// game's EEPROM region, from where the platform reads it (never directly).
#define EEDATA_BASE        0x30000000UL
#define EEDATA             __attribute__((section(".eedata")))

// Port: code overlays. The code of the title screens, of the game itself and of the
// menus over it shares one flash window, into which the current state's group is
// loaded from the EEPROM (LoadStateCode, port/platform.cpp)
#define OVERLAY_CODE(g)    __attribute__((section(".ov" #g ".text"), noinline))
#define OVERLAY_DATA(g)    __attribute__((section(".ov" #g ".rodata")))
#define TITLE_CODE         OVERLAY_CODE(1)   // start screen, new city menu
#define TITLE_DATA         OVERLAY_DATA(1)
#define GAME_CODE          OVERLAY_CODE(2)   // simulation, building
#define GAME_DATA          OVERLAY_DATA(2)
#define MENU_CODE          OVERLAY_CODE(3)   // save/load and budget menus
#define MENU_DATA          OVERLAY_DATA(3)
