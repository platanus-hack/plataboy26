// Build-time check (Makefile: savecheck; no code): a saved city (port/platform.cpp
// SaveCity: an 8-byte header, then GameState) fits the slot the libraries reserve for it
// ("save": 0x1F600, 0x46C bytes, in firmware/library*.json, and firmware/tools/pack.py).
// Kept out of port/ and platform/: those files make the overlay tag (OVL_TAG).
#include "Game.h"
static_assert(8 + sizeof(GameState) <= 0x46C, "MicroCity's save is larger than its slot (library \"save\" size)");
