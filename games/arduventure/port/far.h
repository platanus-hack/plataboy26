// "Far" data: read-only game data stored in the EEPROM instead of flash.
// The linker places it at FAR_BASE (an address range nothing else uses); the build
// copies that section into the game's EEPROM area. Reads go through a small cache.
#pragma once
#include <stdint.h>

#define FAR_BASE   0x30000000UL

#ifdef __cplusplus
extern "C" {
#endif
uint8_t  far_read_byte(uint32_t addr);
#ifdef __cplusplus
}
#endif

static inline uint8_t far_byte(const void *p) {
  uint32_t a = (uint32_t)p;
  return (a >= FAR_BASE) ? far_read_byte(a) : *(const uint8_t *)p;
}
static inline uint16_t far_word(const void *p) {
  return far_byte(p) | ((uint16_t)far_byte((const uint8_t *)p + 1) << 8);
}
static inline uint32_t far_dword(const void *p) {
  return far_word(p) | ((uint32_t)far_word((const uint8_t *)p + 2) << 16);
}
