// Lean AT24CM01 driver for the Arduventure port (smaller than games/*/eeprom.c):
// one streaming read (the sequence of firmware/kernel/kernel.S) and page writes.
#pragma once
#include <stdint.h>

#define EE_OK       0
#define EE_FAIL     1

uint8_t EE_read(uint32_t addr, uint8_t *buf, uint16_t len);
uint8_t EE_write(uint32_t addr, const uint8_t *buf, uint16_t len);
