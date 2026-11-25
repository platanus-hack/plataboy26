// ===================================================================================
// Lean AT24CM01 (128 KiB I2C EEPROM, 0x50/0x51) driver (from the Arduventure port)
// ===================================================================================
//
// Reads: address, repeated START, receive with ACK, end with the BTF sequence of an
// N-byte read (games/eeprom_test/src/eeprom.c, firmware/kernel/kernel.S); at least 3
// bytes (the BTF sequence needs them). Writes: page by page,
// then acknowledge polling for the write cycle. Every wait is bounded, so a missing
// EEPROM cannot hang the game (reads then return EE_FAIL with the buffer unchanged).

#include "at24.h"
#include "ch32v003.h"

#define LOOPS 20000
#define WAIT(cond) do { uint32_t _t = LOOPS; while(!(cond)) if(!--_t) goto fail; } while(0)

static uint8_t dev(uint32_t addr) { return (uint8_t)((0x50 | ((addr >> 16) & 1)) << 1); }

// START + device byte; 0 when the device acknowledged (also the OLED's start, i2c_tx.c)
uint8_t I2C_begin(uint8_t d) {
  I2C1->CTLR1 |= I2C_CTLR1_START;
  WAIT(I2C1->STAR1 & I2C_STAR1_SB);
  I2C1->DATAR = d;
  WAIT(I2C1->STAR1 & (I2C_STAR1_ADDR | I2C_STAR1_AF));
  if(I2C1->STAR1 & I2C_STAR1_AF) { I2C1->STAR1 &= ~I2C_STAR1_AF; goto fail; }
  (void)I2C1->STAR2;
  return 0;
fail:
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  return 1;
}

// START, device, 2 address bytes (write direction)
static uint8_t select(uint32_t addr) {
  WAIT(!(I2C1->STAR2 & I2C_STAR2_BUSY));
  if(I2C_begin(dev(addr))) return 1;
  I2C1->DATAR = (uint8_t)(addr >> 8);
  WAIT(I2C1->STAR1 & I2C_STAR1_TXE);
  I2C1->DATAR = (uint8_t)addr;
  WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
  return 0;
fail:
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  return 1;
}

uint8_t EE_read(uint32_t addr, uint8_t *buf, uint16_t len) {   // len >= 3
  uint8_t *p = buf;
  uint16_t n = len;
  if(select(addr)) return EE_FAIL;
  I2C1->CTLR1 |= I2C_CTLR1_ACK;
  if(I2C_begin(dev(addr) | 1)) return EE_FAIL;
  for(; n > 3; n--) { WAIT(I2C1->STAR1 & I2C_STAR1_RXNE); *p++ = I2C1->DATAR; }
  WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
  I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
  *p++ = I2C1->DATAR;
  WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  *p++ = I2C1->DATAR;
  *p = I2C1->DATAR;
  WAIT(!(I2C1->CTLR1 & I2C_CTLR1_STOP));
  I2C1->CTLR1 |= I2C_CTLR1_ACK;
  return EE_OK;
fail:
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  return EE_FAIL;
}

static inline __attribute__((always_inline)) uint8_t ee_write_body(uint32_t addr, const uint8_t *buf, uint16_t len) {
  while(len) {
    uint16_t n = 256 - (addr & 255);             // never across a 256-byte page
    if(n > len) n = len;
    if(select(addr)) return EE_FAIL;
    for(uint16_t i = 0; i < n; i++) { WAIT(I2C1->STAR1 & I2C_STAR1_TXE); I2C1->DATAR = buf[i]; }
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    for(uint16_t t = 0; t < 2000; t++) {         // write cycle (tWR <= 5 ms): poll
      WAIT(!(I2C1->STAR2 & I2C_STAR2_BUSY));
      if(!I2C_begin(dev(addr))) { I2C1->CTLR1 |= I2C_CTLR1_STOP; break; }
    }
    addr += n; buf += n; len -= n;
  }
  return EE_OK;
fail:
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  return EE_FAIL;
}

// Only saving writes the EEPROM: EE_write is in the menus' overlay group (EE_WRITE_ATTR,
// set by the makefile)
#ifndef EE_WRITE_ATTR
#define EE_WRITE_ATTR
#endif
EE_WRITE_ATTR uint8_t EE_write(uint32_t addr, const uint8_t *buf, uint16_t len) { return ee_write_body(addr, buf, len); }
