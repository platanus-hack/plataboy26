// ===================================================================================
// AT24CM01 (1 Mbit / 128 KiB) I2C EEPROM driver for the CH32V003 game console
// ===================================================================================
//
// Master receiver sequences follow the standard I2C1 polling procedures for this
// peripheral (1 byte, 2 bytes with POS, N > 2 bytes with BTF), which is what the
// hardware needs for a correct NACK on the last byte.

#include "eeprom.h"
#include "ch32v003.h"

#define EE_TIMEOUT_LOOPS  20000           // ~ several ms at 12-48 MHz
static void EE_stop(void) { I2C1->CTLR1 |= I2C_CTLR1_STOP; }

// on a timeout, release the bus so the OLED driver does not hang afterwards
#define WAIT(cond) do { uint32_t _t = EE_TIMEOUT_LOOPS; \
  while(!(cond)) if(!--_t) { EE_stop(); return EE_TIMEOUT; } } while(0)

// START + device address. Returns EE_OK once ADDR is set (ADDR still to be cleared).
// A read is always a repeated START inside a transaction, so the bus stays BUSY.
static uint8_t EE_start(uint32_t addr, uint8_t rd) {
  if(!rd) WAIT(!(I2C1->STAR2 & I2C_STAR2_BUSY));
  I2C1->CTLR1 |= I2C_CTLR1_START;
  WAIT(I2C1->STAR1 & I2C_STAR1_SB);
  I2C1->DATAR = (uint8_t)(((EE_I2C_ADDR | ((addr >> 16) & 1)) << 1) | rd);
  WAIT(I2C1->STAR1 & (I2C_STAR1_ADDR | I2C_STAR1_AF));
  if(I2C1->STAR1 & I2C_STAR1_AF) {
    I2C1->STAR1 &= ~I2C_STAR1_AF;
    EE_stop();
    return EE_NACK;
  }
  return EE_OK;
}

static inline void EE_clear_addr(void) { (void)I2C1->STAR1; (void)I2C1->STAR2; }

// START + address + 2 word-address bytes (write direction, ADDR cleared)
static uint8_t EE_select(uint32_t addr) {
  uint8_t r = EE_start(addr, 0);
  if(r) return r;
  EE_clear_addr();
  WAIT(I2C1->STAR1 & I2C_STAR1_TXE);
  I2C1->DATAR = (uint8_t)(addr >> 8);
  WAIT(I2C1->STAR1 & I2C_STAR1_TXE);
  I2C1->DATAR = (uint8_t)addr;
  WAIT(I2C1->STAR1 & I2C_STAR1_TXE);
  return EE_OK;
}

uint8_t EE_probe(void) {
  uint8_t r = EE_start(0, 0);
  if(r) return r;
  EE_clear_addr();
  EE_stop();
  return EE_OK;
}

uint8_t EE_wait(void) {
  for(uint16_t i = 0; i < 2000; i++) {       // tWR max 5 ms; one poll is ~25 us at 400 kHz
    uint8_t r = EE_probe();
    if(r != EE_NACK) return r;
  }
  return EE_TIMEOUT;
}

uint8_t EE_read(uint32_t addr, uint8_t *buf, uint16_t len) {
  if(!len) return EE_OK;
  uint8_t r = EE_select(addr);                    // dummy write sets the address counter
  if(r) return r;
  WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
  r = EE_start(addr, 1);                          // repeated START, read direction
  if(r) return r;

  if(len == 1) {
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    EE_clear_addr();
    EE_stop();
    WAIT(I2C1->STAR1 & I2C_STAR1_RXNE);
    buf[0] = I2C1->DATAR;
  } else if(len == 2) {
    I2C1->CTLR1 |= I2C_CTLR1_POS | I2C_CTLR1_ACK; // NACK goes to the 2nd byte
    EE_clear_addr();
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);            // byte 1 in DR, byte 2 in shift reg
    EE_stop();
    buf[0] = I2C1->DATAR;
    buf[1] = I2C1->DATAR;
    I2C1->CTLR1 &= ~I2C_CTLR1_POS;
  } else {
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    EE_clear_addr();
    while(len > 3) {
      WAIT(I2C1->STAR1 & I2C_STAR1_RXNE);
      *buf++ = I2C1->DATAR; len--;
    }
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);            // N-2 in DR, N-1 in shift reg
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;                // NACK the last byte
    *buf++ = I2C1->DATAR;
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);            // N-1 in DR, N in shift reg
    EE_stop();
    *buf++ = I2C1->DATAR;
    *buf++ = I2C1->DATAR;
  }
  WAIT(!(I2C1->CTLR1 & I2C_CTLR1_STOP));
  I2C1->CTLR1 |= I2C_CTLR1_ACK;
  return EE_OK;
}

// One byte per transaction, register sequence of WCH's official CH32V003 example
// (EVT/EXAM/I2C/I2C_EEPROM, AT24CXX_ReadOneByte): ACK is switched off while the byte
// arrives, STOP after reading it. Slow (~9 KB/s) but proven on the real chip; used as
// a fallback when a CRC check after EE_read fails.
uint8_t EE_read_safe(uint32_t addr, uint8_t *buf, uint16_t len) {
  while(len--) {
    uint8_t r = EE_select(addr);
    if(r) return r;
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
    r = EE_start(addr, 1);
    if(r) return r;
    EE_clear_addr();
    WAIT(I2C1->STAR1 & I2C_STAR1_RXNE ? 1 : (I2C1->CTLR1 &= ~I2C_CTLR1_ACK, 0));
    *buf++ = I2C1->DATAR;
    EE_stop();
    WAIT(!(I2C1->CTLR1 & I2C_CTLR1_STOP));
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    addr++;
  }
  return EE_OK;
}

uint8_t EE_write(uint32_t addr, const uint8_t *buf, uint16_t len) {
  while(len) {
    uint16_t n = EE_PAGE - (addr & (EE_PAGE - 1));     // never cross a page: it would wrap
    if(n > len) n = len;
    uint8_t r = EE_select(addr);
    if(r) return r;
    for(uint16_t i = 0; i < n; i++) {
      WAIT(I2C1->STAR1 & I2C_STAR1_TXE);
      I2C1->DATAR = buf[i];
    }
    WAIT(I2C1->STAR1 & I2C_STAR1_BTF);
    EE_stop();                                        // STOP starts the write cycle
    r = EE_wait();
    if(r) return r;
    addr += n; buf += n; len -= n;
  }
  return EE_OK;
}
