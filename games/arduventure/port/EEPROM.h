// Arduino EEPROM library subset: the Arduboy's 1 KiB EEPROM is this game's 1 KiB slot
// in the AT24CM01 save area.
#pragma once
#include <stdint.h>

struct EEPROMClass {
  uint8_t read(int addr);
  void    write(int addr, uint8_t v);
  void    update(int addr, uint8_t v) { if(read(addr) != v) write(addr, v); }
  void    readBlock(int addr, void *p, int n);
  void    writeBlock(int addr, const void *p, int n);
  template<class T> T &get(int addr, T &t) { readBlock(addr, &t, sizeof(T)); return t; }
  template<class T> const T &put(int addr, const T &t) { writeBlock(addr, &t, sizeof(T)); return t; }
};
extern EEPROMClass EEPROM;
