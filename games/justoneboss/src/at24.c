// ===================================================================================
// Minimal AT24CM01 (128 KiB I2C EEPROM, 0x50/0x51) driver for Just One Boss
// ===================================================================================
// Blocking (the game cannot run without its EEPROM: its bytecode is there). Reads: the
// address, repeated START, receive with ACK, the BTF sequence of an N-byte read
// (games/eeprom_test/src/eeprom.c), at least 3 bytes. Writes: within one 256-byte page,
// then acknowledge polling for the write cycle. I2C_begin is shared with the OLED.
#include "at24.h"
#include "ch32v003.h"

#define STAR1 I2C1->STAR1
static uint8_t dev(uint32_t a) { return (uint8_t)((0x50 | ((a >> 16) & 1)) << 1); }

uint8_t I2C_begin(uint8_t d) {         // START + device byte; 0 when acknowledged
  I2C1->CTLR1 |= I2C_CTLR1_START;
  while(!(STAR1 & I2C_STAR1_SB));
  I2C1->DATAR = d;
  while(!(STAR1 & (I2C_STAR1_ADDR | I2C_STAR1_AF)));
  if(STAR1 & I2C_STAR1_AF) { STAR1 &= ~I2C_STAR1_AF; I2C1->CTLR1 |= I2C_CTLR1_STOP; return 1; }
  (void)I2C1->STAR2;
  return 0;
}

void dbg_sp(void);
static void put(uint8_t b) {
#ifdef DEBUG
  dbg_sp();
#endif
  while(!(STAR1 & I2C_STAR1_TXE)); I2C1->DATAR = b; }

static void select(uint32_t a) {       // START, device, the 2 address bytes
  while(I2C1->STAR2 & I2C_STAR2_BUSY);
  I2C_begin(dev(a));
  put(a >> 8); put(a);
  while(!(STAR1 & I2C_STAR1_BTF));
}

uint8_t EE_read(uint32_t a, uint8_t *p, uint16_t n) {   // n >= 3
  select(a);
  I2C1->CTLR1 |= I2C_CTLR1_ACK;
  I2C_begin(dev(a) | 1);
  for(; n > 3; n--) { while(!(STAR1 & I2C_STAR1_RXNE)); *p++ = I2C1->DATAR; }
  while(!(STAR1 & I2C_STAR1_BTF));
  I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
  *p++ = I2C1->DATAR;
  while(!(STAR1 & I2C_STAR1_BTF));
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  *p++ = I2C1->DATAR;
  *p = I2C1->DATAR;
  while(I2C1->CTLR1 & I2C_CTLR1_STOP);
  I2C1->CTLR1 |= I2C_CTLR1_ACK;
  return EE_OK;
}

uint8_t EE_write(uint32_t a, const uint8_t *p, uint16_t n) {   // within one page
  select(a);
  while(n--) put(*p++);
  while(!(STAR1 & I2C_STAR1_BTF));
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  do while(I2C1->STAR2 & I2C_STAR2_BUSY); while(I2C_begin(dev(a)));   // write cycle
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
  return EE_OK;
}
