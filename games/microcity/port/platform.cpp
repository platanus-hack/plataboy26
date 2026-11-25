// ===================================================================================
// MicroCity platform layer for the CH32V003 game console
// ===================================================================================
//
// MicroCity draws through a few platform functions (PutPixel, DrawBitmap, ...: the
// Arduboy sketch, MicroCity.ino, implements them on Arduboy2). Here:
//
// Display  no 1 KiB frame buffer (the city alone takes 1.1 KiB of the 2 KiB SRAM):
//          Draw() (microcity.patch) draws the screen 8 times, once per page of 8 rows,
//          into a 128 byte buffer; each page is sent right after it is drawn, unless the
//          OLED already shows it. Drawing outside the current page is dropped.
// Frames   25 per second, as on the Arduboy (setFrameRate(25)); the simulation steps
//          once per frame, so this is the game's speed
// Buttons  joypad with diagonals; the console's fire button is the Arduboy's B (build,
//          confirm), the second button (board rev. 1.3) its A (toolbar, back)
// Saving   the city goes to this game's slot in the AT24CM01 save area
// Terrain  the 3 terrain maps are in the EEPROM (EEDATA); the one in use is loaded into
//          RAM (288 bytes)
// Power    the power grid (a bit per tile, 288 bytes): the Arduboy borrows the frame
//          buffer, here the terrain map's RAM, which is loaded again afterwards

extern "C" {
#include "driver.h"
#include "at24.h"
#include "../../common/sysx.h"
}
#include "Game.h"
#include "Draw.h"
#include "Interface.h"
#include "Connectivity.h"

#define FRAME_TICKS  (F_CPU / 25)
// the city's slot in the EEPROM save area: 0x1F600..0x1FA6B, so the save area must reach
// up to 0x1FC00 (it ends at 0x1F800 in firmware/menu/src/library.h so far)
#define SAVE_SLOT    0x1F600UL
#define SAVE_SIZE    (8 + sizeof(GameState))
#define EE_DATA      MC_EE_DATA        // EEPROM address of the EEDATA section (makefile)

static_assert(SAVE_SIZE <= 0x600, "the city does not fit the save slot");

// ---- drawing: one page (8 rows) at a time -----------------------------------------------
// The page buffer is the last static variable, right below the stack (platform/game.ld
// puts it last): the drawing, which runs while the buffer is in use, never reaches it;
// deeper code (building, simulation) may use it as stack at other times (tools/stack.py)
static uint8_t page[DISPLAY_WIDTH] __attribute__((section(".bss.~page")));
static uint8_t drawPage;
static uint32_t pageHash[DISPLAY_HEIGHT / 8];   // what the OLED shows (0 = unknown)

uint8_t* GetPageBuffer() { return page; }
uint8_t GetDrawPage() { return drawPage; }

void BeginPage(uint8_t pg)
{
  drawPage = pg;
  memset(page, 0, sizeof page);
}

// three SSD1306 command bytes
static void oled_cmd(uint8_t a, uint8_t b, uint8_t c)
{
  I2C_start(OLED_ADDR);
  I2C_write(OLED_CMD_MODE); I2C_write(a); I2C_write(b); I2C_write(c);
  I2C_stop();
}

#ifdef MC_TEST
static uint32_t frameHash;                    // test build: the frame's pages (tools/reftest.py)
#endif

void EndPage()
{
  // djb2: a page that did not change is not sent again
  uint32_t h = 5381;
  for(uint8_t i = 0; i < DISPLAY_WIDTH; i++) h = (h << 5) + h + page[i];
  if(!h) h = 1;
#ifdef MC_TEST
  frameHash = frameHash * 33 + h;
#endif
  if(h == pageHash[drawPage]) return;
  pageHash[drawPage] = h;
  oled_cmd(OLED_COLUMNS, 0, DISPLAY_WIDTH - 1);   // horizontal addressing: the page's window
  oled_cmd(OLED_PAGES, drawPage, 7);
  OLED_data_start();
  for(uint8_t i = 0; i < DISPLAY_WIDTH; i++) I2C_write(page[i]);
  I2C_stop();
}

void PutPixel(uint8_t x, uint8_t y, uint8_t colour)
{
  if(x >= DISPLAY_WIDTH || (y >> 3) != drawPage) return;
  uint8_t m = 1 << (y & 7);
  if(colour) page[x] |= m; else page[x] &= ~m;
}

void DrawFilledRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t colour)
{
  int16_t top = y - drawPage * 8, bottom = top + h;     // rows of the page: 0..7
  if(bottom <= 0 || top >= 8) return;
  uint8_t m = 0xFF;
  if(top > 0) m <<= top;
  if(bottom < 8) m &= 0xFF >> (8 - bottom);
  for(uint16_t i = x; i < x + w && i < DISPLAY_WIDTH; i++)
    if(colour) page[i] |= m; else page[i] &= ~m;
}

// Arduboy2 drawBitmap(x, y, bmp, w, h, WHITE): rows of 8 pixels, a byte per column,
// set bits drawn (only the title screens draw bitmaps)
TITLE_CODE void DrawBitmap(const uint8_t* bmp, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
  int16_t rows = (h + 7) >> 3, r = drawPage - (y >> 3);  // source row on this page...
  uint8_t s = y & 7;                                     // ...shifted down by s
  for(uint16_t i = 0; i < w && x + i < DISPLAY_WIDTH; i++) {
    uint8_t b = 0;
    if(r >= 0 && r < rows) b = pgm_read_byte(&bmp[r * w + i]) << s;
    if(s && r >= 1 && r - 1 < rows) b |= pgm_read_byte(&bmp[(r - 1) * w + i]) >> (8 - s);
    page[x + i] |= b;
  }
}

// ---- terrain and power grid ------------------------------------------------------------
#define MAP_BYTES (MAP_WIDTH * MAP_HEIGHT / 8)
static uint8_t mapRAM[MAP_BYTES];             // the terrain map in use, or the power grid
static const uint8_t* mapLoaded;              // which terrain map is in mapRAM (0: none)

const uint8_t* LoadTerrain(const uint8_t* eeData)
{
  if(eeData != mapLoaded) {
    EE_read(EE_DATA + ((uint32_t)eeData - EEDATA_BASE), mapRAM, MAP_BYTES);
    mapLoaded = eeData;
  }
  return mapRAM;
}

uint8_t* GetPowerGrid() { return mapRAM; }

void CalculatePowerConnectivity_body();       // upstream's (renamed by the makefile)
GAME_CODE void CalculatePowerConnectivity()
{
  mapLoaded = nullptr;                        // the grid overwrites the terrain map: load it
  CalculatePowerConnectivity_body();          // again now, not later deep in some call
  GetTerrainData(State.terrainType);          // chain (stack)
}

// ---- buttons ---------------------------------------------------------------------------
// joypad state from SYS_poll (games/common/joycal.h: idle R U UR D DR UD L UL DL none)
static const uint8_t PAD[11] = { 0, INPUT_RIGHT, INPUT_UP, INPUT_UP | INPUT_RIGHT, INPUT_DOWN, INPUT_DOWN | INPUT_RIGHT, 0, INPUT_LEFT, INPUT_UP | INPUT_LEFT, INPUT_DOWN | INPUT_LEFT, 0 };

#ifdef MC_TEST
// test build: the buttons of each frame come from a script at the start of the EEPROM
// (a byte per frame: INPUT_* bits), and each frame's pixels are reported on the serial
// port (the simulator prints them): tools/reftest.py compares them with upstream's
static uint16_t testFrame;
static uint8_t testInput[4];

uint8_t GetInput()
{
  if(!(testFrame & 3)) EE_read(testFrame, testInput, 4);
  return testInput[testFrame++ & 3];
}

// and the stack: bytes above the page buffer never used ("s" lines, every 256 frames)
extern "C" uint32_t _ebss[];          // the page buffer ends there
static void test_hex(uint32_t v, char tag)
{
  volatile uint32_t* dr = (volatile uint32_t*)0x40013804;    // USART1 data
  if(tag) *dr = tag;
  for(int8_t i = 28; i >= 0; i -= 4) *dr = "0123456789abcdef"[(v >> i) & 15];
  *dr = '\n';
}

static void test_report()
{
  test_hex(frameHash, 0);
  frameHash = 0;
  if(!(testFrame & 255)) {
    uint32_t* p = _ebss;
    while(*p == 0xA5A5A5A5) p++;
    test_hex((uint32_t)((uint8_t*)p - (uint8_t*)_ebss), 's');
  }
}
#else
static void test_report() { }

uint8_t GetInput()
{
  uint8_t b = PAD[SYS_poll(ADC_read())]; // calibrated; UP + DOWN held 2 s: back to the menu
  if(JOY_act_pressed()) b |= INPUT_B;
  else if(JOY_b_pressed()) b |= INPUT_A;
  return b;
}
#endif

// ---- saving: "CTY1", a check of the game state, the game state --------------------------
// The Arduboy sketch saves "CTY1" and the state. Here the state goes first and the header
// with its check last, and a city is only loaded if the check matches: a save cut short
// (power lost) or partly overwritten is not loaded (the current city stays as it is).
static const uint8_t SaveHeader[4] = { 'C', 'T', 'Y', '1' };

static uint32_t save_sum(uint32_t s, const uint8_t* p, uint8_t n)
{
  while(n--) s = s * 33 ^ *p++;
  return s;
}

MENU_CODE void SaveCity()
{
  uint32_t h[2] = { 0, 0 };
  for(uint16_t i = 0; i < sizeof(GameState); i += 64)
    h[1] = save_sum(h[1], (const uint8_t*)&State + i, sizeof(GameState) - i < 64 ? sizeof(GameState) - i : 64);
  memcpy(h, SaveHeader, 4);
  EE_write(SAVE_SLOT + 8, (const uint8_t*)&State, sizeof(GameState));
  EE_write(SAVE_SLOT, (const uint8_t*)h, 8);
}

// inlined: the title and the menus each have a copy in their group (flash size)
#pragma GCC diagnostic ignored "-Wattributes"     // always_inline without inline: see above
__attribute__((always_inline)) bool LoadCity()
{
  uint32_t h[2], s = 0;
  uint8_t buf[16];
  if(EE_read(SAVE_SLOT, (uint8_t*)h, 8) != EE_OK || memcmp(h, SaveHeader, 4)) return false;
  for(uint16_t i = 0; i < sizeof(GameState); i += 16) {        // check first...
    uint8_t n = sizeof(GameState) - i < 16 ? sizeof(GameState) - i : 16;
    if(EE_read(SAVE_SLOT + 8 + i, buf, 16) != EE_OK) return false;
    s = save_sum(s, buf, n);
  }
  if(s != h[1]) return false;
  return EE_read(SAVE_SLOT + 8, (uint8_t*)&State, sizeof(GameState)) == EE_OK;   // ...then load
}

// ---- code overlays ----------------------------------------------------------------------
// The groups (title, game, menus; TITLE_CODE etc. in port/avr/pgmspace.h) share one flash
// window. LoadStateCode() before running a state's code: if the window holds another
// group (its first word: the group number), the library kernel unpacks the group's LZ
// stream from the EEPROM into the window (games/common/kunpack.S; region header at
// EE_DATA - 32: "MCOV", then u32 address + u16 size per group). A load cut by a power
// loss is harmless: every power-on reinstalls the menu, and the game's install rewrites
// the window.
extern "C" char __ovl_base[];
extern "C" char OVL_TAG[];                    // linker symbol: this build's ID << 16
extern "C" void kunpack(uint32_t src, uint32_t from, uint32_t to, uint8_t* scratch);

static void ov_load(uint8_t g)
{
  uint8_t hdr[6];
  if(EE_read(EE_DATA - 32, hdr, 4) != EE_OK || memcmp(hdr, "MCOV", 4)   // no game data, or a
     || (*(volatile uint32_t*)0x08003C48 & 0xFFF) != 0x6F) for(;;);     // kernel without unpack:
  EE_read(EE_DATA - 32 + 4 + 6 * (g - 1), hdr, 6);                       // stop here
  uint32_t base = (uint32_t)__ovl_base;
  uint32_t buf[24];
  FLASH->KEYR = 0x45670123; FLASH->KEYR = 0xCDEF89AB;
  FLASH->MODEKEYR = 0x45670123; FLASH->MODEKEYR = 0xCDEF89AB;
  kunpack(hdr[0] | hdr[1] << 8 | (uint32_t)hdr[2] << 16, base, base + (hdr[4] | hdr[5] << 8), (uint8_t*)buf);
  FLASH->CTLR = (1u << 15) | (1u << 7);       // lock
}

void LoadStateCode()
{
  static const uint8_t GroupOf[] = { 1, 1, 2, 2, 2, 3, 3 };   // per state (Interface.h)
  uint8_t g = GroupOf[UIState.state];
  if(*(volatile uint32_t*)__ovl_base - (uint32_t)OVL_TAG != g) ov_load(g);
}

// ---- main loop --------------------------------------------------------------------------
int main()
{
#ifdef MC_TEST
  { uint32_t* sp; asm volatile("mv %0, sp" : "=r"(sp));
    for(uint32_t* p = _ebss; p < sp - 8; p++) *p = 0xA5A5A5A5; }
#endif
  JOY_init();
  EE_read(JOYCAL_EE, SYS_cal, JOYCAL_N);   // joypad thresholds, measured by the menu (sysx.h)
  LoadStateCode();                            // the start screen's (InitGame is there too)
  InitGame();
  uint32_t next = STK->CNT;
  for(;;) {
    while((int32_t)(STK->CNT - next) < 0);
    next += FRAME_TICKS;
    if((int32_t)(STK->CNT - next) > 4 * FRAME_TICKS) next = STK->CNT;   // late: catch up no more
    TickGame();
    test_report();
  }
}

// plataboy26 framework: UP + DOWN -> menu (games/common/sysx.c; no battery check: no flash)
#define SYSX_NO_BATTERY
#include "../../common/sysx.c"
