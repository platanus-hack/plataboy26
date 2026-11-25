// ===================================================================================
// plataboy26 game library: EEPROM layout (AT24CM01, 128 KiB), shared with the packer
// (firmware/tools/pack.py) and the kernel (firmware/kernel/kernel.S)
// ===================================================================================
//
//   0x00000  header, 32 bytes
//   0x00020  directory: 32-byte entries, entry 0 = this menu (not listed), 47 at most
//   0x00600  card table: per entry the address and size of its menu card (the program's
//            simulator thumbnail, 128 x 64, column-major, LZ: tools/lz.py)
//   0x00800  compressed programs (no stream crosses 0x10000)
//   0x0FF00  512 bytes kept free: scratch pages of board_test / eeprom_test
//   0x1F000  save area, 2 KiB, never touched by the packer: 0x1F000 menu (last
//            selection, contrast; 16 bytes:
//            0x1F010-0x1F0FF holds programs), 0x1F100 Hopper (192), 0x1F1C0 Just One Boss (64),
//            0x1F200-0x1F5FF Arduventure (1 KiB), 0x1F600-0x1FA6B MicroCity (past the
//            area: the libraries keep it free). The table and its checks: SAVE_SLOTS in
//            tools/pack.py; each game checks at build time that its save fits its slot
//   0x1F800  compressed programs again (up to the end)

#pragma once
#include <stdint.h>

#define LIB_MAGIC     0x36324250UL      // "PB26"
#define LIB_HDR       0x00000UL
#define LIB_DIR       0x00020UL
#define LIB_DIR_MAX   47
#define LIB_CARDS     (LIB_DIR + 32 * LIB_DIR_MAX)   // per entry: u32 address, u16 size (0: none)
#define LIB_SAVE      0x1F000UL
#define LIB_SAVE_MENU LIB_SAVE          // the last selection (1 byte)
#define LIB_SAVE_CONTRAST (LIB_SAVE + 1) // the OLED contrast level, 0..7 (else the default)
// LIB_SAVE + 4..13: the joypad thresholds (games/common/joycal.h JOYCAL_EE), measured at
// power-on; the packer writes the nominal ones

#define LIB_TOOL      0x01              // entry flag: shown under "tools"

typedef struct {
  uint32_t magic;
  uint8_t  version, count;              // count includes the menu
  uint16_t reserved;
  uint32_t used, free;                  // bytes of program data / bytes still free
  uint8_t  pad[16];
} LibHeader;

typedef struct {
  uint32_t offset;                      // EEPROM address of the compressed stream
  uint16_t csize, usize;                // compressed / installed size
  uint32_t entry;                       // the program's reset target
  uint32_t sum;                         // check of the installed image
  char     name[15];                    // NUL padded
  uint8_t  flags;
} LibEntry;

// the kernel's install(n, contrast): never returns, the selected program starts after a
// reset; the contrast (SSD1306 0x81 value) is kept in the config page for the games
#define LIB_install(n, c)  ((void (*)(uint32_t, uint32_t))(KBASE + 4))(n, c)
