// Build-time check (Makefile: savecheck; no code): the game's save, as the original keeps
// it in the Arduboy EEPROM (player.h: GAME_ID at EEPROM_START, the Player after it; the
// sound flag at 1), fits the 1 KiB slot the port maps it to (port/platform.cpp SAVE_SLOT,
// 0x1F200-0x1F5FF: addresses are taken & 1023, so a larger save would wrap onto its own
// start, and a block crossing 1023 would run into MicroCity's slot at 0x1F600).
// Kept out of port/ and platform/: those files make the overlay tag (OVL_TAG).
#include "../build/src2/ARDU_AB.ino"
static_assert(EEPROM_START + 1 + sizeof(Player) <= 1024, "Arduventure's save is larger than its 1 KiB slot");
