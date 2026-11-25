// ===================================================================================
// AT24CM01 (1 Mbit / 128 KiB) I2C EEPROM driver for the CH32V003 game console
// ===================================================================================
//
// The EEPROM shares I2C1 (PC1 = SDA, PC2 = SCL) with the OLED. Call I2C_init() (or
// OLED_init(), which calls it) first. All functions poll with a timeout and return
// EE_OK (0) or an error code; they never hang when no EEPROM is fitted.
//
// Functions available:
// --------------------
// EE_probe()                    EE_OK if the EEPROM answers (not busy, fitted)
// EE_read(addr, buf, len)       random/sequential read of len bytes from addr
// EE_write(addr, buf, len)      write len bytes (any alignment, splits at 256-byte
//                               page boundaries, waits for each write cycle)
// EE_wait()                     acknowledge polling: wait until a write cycle ends
// EE_read_safe(addr, buf, len)  same as EE_read with WCH's reference sequence, 1 byte
//                               per transaction (slow, fallback after a CRC error)
//
// Device address: 1010 A2 A1 P0 R/W with A2 = A1 = GND, P0 = bit 16 of the memory
// address -> 0x50 (lower 64 KiB) and 0x51 (upper 64 KiB).

#pragma once
#include <stdint.h>

#define EE_SIZE       131072UL
#define EE_PAGE       256
#define EE_I2C_ADDR   0x50

#define EE_OK         0
#define EE_NACK       1   // no ACK on the address (absent or busy)
#define EE_TIMEOUT    2   // bus stuck (missing pull-ups, short)

uint8_t EE_probe(void);
uint8_t EE_wait(void);
uint8_t EE_read(uint32_t addr, uint8_t *buf, uint16_t len);
uint8_t EE_read_safe(uint32_t addr, uint8_t *buf, uint16_t len);   // WCH reference sequence
uint8_t EE_write(uint32_t addr, const uint8_t *buf, uint16_t len);
