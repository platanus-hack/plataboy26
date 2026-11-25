// ===================================================================================
// Project:   Hopper for the CH32V003 Mini Game Console
// ===================================================================================
//
// Port of "Hopper" by OBONO (Arduboy, 2017):
//   https://github.com/obono/ArduboyWorks/tree/master/hopper
//   MIT License, Copyright (c) 2016-2026 OBONO
//
// Jump on panels and go up (foreground side). If you fall toward bottom, the game is
// over. Move with the joypad (diagonals too), A or B pauses; in the title menu Up/Down
// select and A or B confirms. Best 10 scores, play count and time are kept in the
// EEPROM save area.
//
// The game logic is the original's, line for line (logo.cpp, title.cpp, game.cpp,
// hopper.ino), with the Arduboy library replaced by arduboy.c. Changes for this
// hardware, none visible in play:
//   - no floating point: atan2 of the 8 joypad directions, sin/cos of the 256 floor
//     directions and the integer sqrt of the title letters come from small tables
//   - the original runs on an 8-bit AVR with 16-bit int: values that rely on 16-bit
//     wrap-around are kept int16_t/uint16_t here
//   - the screen is sent every 2nd frame (30 fps) while the game runs 60 updates/s
//   - no RGB LED: the logo's Morse "OBN" shows only as the letters' shake

#include "arduboy.h"
#include "hopper_data.h"

#define APP_TITLE       "HOPPER"
#define APP_CODE        "OBN-Y02"
#define APP_VERSION     "0.22"
#define APP_RELEASED    "NOVEMBER 2017"

#define abs(n)          (((n) >= 0) ? (n) : -(n))
#define mid(a, b, c)    max(min((b), (c)), (a))
#define sign(n)         (((n) > 0) - ((n) < 0))

#include <stdbool.h>

// ===================================================================================
// Tables replacing floating point
// ===================================================================================

// (uint8_t)(atan2(vy, vx) * 128 / PI) for vx, vy in -1..1: index (vx + 1) * 3 + vy + 1
static const uint8_t ATAN_DIR[9] = { 160, 128, 96, 192, 0, 64, 224, 0, 32 };

// sin(i * PI / 128) * 16384, i = 0..64
static const int16_t SIN_Q14[65] = {
  0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520,
  5897, 6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394,
  10702, 11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623,
  13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557, 15679,
  15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384
};

static int16_t sinQ14(uint8_t i) {          // sin(i * PI / 128) * 16384
  uint8_t q = i & 63;
  int16_t v = (i & 64) ? SIN_Q14[64 - q] : SIN_Q14[q];
  return (i & 128) ? -v : v;
}

static uint8_t isqrt(uint16_t n) {           // floor(sqrt(n))
  uint8_t r = 0;
  while((uint16_t)(r + 1) * (r + 1) <= n) r++;
  return r;
}

// ===================================================================================
// Logo (logo.cpp)
// ===================================================================================
#define SIGNAL_PTN      0xEEE3AA3AUL  // "OBN" in Morse code

static uint8_t logoCounter;
static bool    signalOn;

static void initLogo(void) {
  logoCounter = 120; // 2 secs
}

static bool updateLogo(void) {
  logoCounter--;
  int16_t sh = ((int16_t)logoCounter - 15) / 3;
  signalOn = sh >= 0 ? (SIGNAL_PTN >> sh) & 1 : 0;
  return (logoCounter == 0);
}

static void drawLogo(void) {
  AB_clear();
  int16_t shake = (120 - logoCounter) / 40;
  for(int16_t i = 0; i < 3; i++) {
    int16_t y = 12 + (i == shake) * signalOn;
    AB_drawBitmap(28 + i * 24, y, imgOBN[i], 24, 32, WHITE);
  }
  AB_drawBitmap(68, 44, imgSoft, 32, 8, WHITE);
  AB_printEx(16, 58, APP_CODE " VER " APP_VERSION);
}

// ===================================================================================
// Title (title.cpp)
// ===================================================================================
enum { STATE_MENU = 0, STATE_RECORD, STATE_CREDIT };
enum { MENU_START = 0, MENU_SOUND, MENU_RECORD, MENU_CREDIT };
enum { RECORD_NOT_READ = 0, RECORD_INITIAL, RECORD_STORED };

#define EEPROM_SIGNATURE    0x024E424FUL // "OBN\x02"

typedef struct { const uint8_t *pImg; uint8_t width; } TITLE_LETTER;

static const TITLE_LETTER letterInfos[] = {
  { imgLetterH, 16 }, { imgLetterO, 13 }, { imgLetterP, 14 },
  { imgLetterP, 14 }, { imgLetterE, 11 }, { imgLetterR, 11 },
};

static const char menuText[] = "START GAME\0SOUND \0RECORD\0CREDIT";
static const char creditText[] = "- " APP_TITLE " -\0\0" APP_RELEASED
        "\0PROGRAMMED BY OBONO\0\0THIS PROGRAM IS\0" "RELEASED UNDER\0" "THE MIT LICENSE.";

static uint8_t  titleState;
static bool     titleToDraw;
static bool     sound;
static int8_t   menuPos;
static int8_t   manX, manVy, manType;
static int8_t   letterY[6];
static int8_t   letterVy[6];
static uint16_t lastScore = 0;
static uint8_t  recordState = RECORD_NOT_READ;
static uint16_t hiScore[10];
static uint16_t playCount;
static uint32_t playFrames;

// the record, as the original keeps it at EEPROM address 800
typedef struct __attribute__((packed)) {
  uint32_t signature;
  uint16_t hiScore[10], playCount;
  uint32_t playFrames;
  uint16_t checkSum;
} RECORD;
// Hopper's save slot (arduboy.c: SAVE_BASE, SAVE_SIZE): this record at 0, the sound byte
// at SAVE_AUDIO (48)
_Static_assert(sizeof(RECORD) <= 48, "Hopper's record would run into its sound byte");

static uint16_t recordCheckSum(void) {
  uint16_t checkSum = (EEPROM_SIGNATURE & 0xFFFF) + (EEPROM_SIGNATURE >> 16) * 2;
  for(int16_t i = 0; i < 10; i++) checkSum += hiScore[i] * (i + 3);
  checkSum += playCount * 13;
  checkSum += (playFrames & 0xFFFF) * 14 + (playFrames >> 16) * 15;
  return checkSum;
}

static void setSound(bool enabled) {
  if(enabled) AB_audioOn(); else AB_audioOff();
  sound = enabled;
}

static void playSound1(void) { AB_playScore2(sound1, 255); }
static void playSound2(void) { AB_playScore2(sound2, 255); }

static void readRecord(void) {
  RECORD r;
  bool isRegular = false;
  if(AB_saveRead(0, &r, sizeof r) == 0 && r.signature == EEPROM_SIGNATURE) {
    for(int16_t i = 0; i < 10; i++) hiScore[i] = r.hiScore[i];
    playCount = r.playCount;
    playFrames = r.playFrames;
    isRegular = (r.checkSum == recordCheckSum());
  }
  if(isRegular) {
    recordState = RECORD_STORED;
  } else {
    for(int16_t i = 0; i < 10; i++) hiScore[i] = 0;
    playCount = 0;
    playFrames = 0;
    recordState = RECORD_INITIAL;
  }
}

static uint8_t setLastScore(uint16_t score, uint32_t frames) {
  lastScore = score;

  /*  Update best 10  */
  int16_t r = 10;
  uint16_t h;
  while(r > 0 && (h = hiScore[r - 1]) < score) {
    if(--r < 9) hiScore[r + 1] = h;
  }
  if(r < 10) hiScore[r] = score;
  playCount++;
  playFrames += frames;

  /*  Store record to EEPROM  */
  RECORD rec;
  rec.signature = EEPROM_SIGNATURE;
  for(int16_t i = 0; i < 10; i++) rec.hiScore[i] = hiScore[i];
  rec.playCount = playCount;
  rec.playFrames = playFrames;
  rec.checkSum = recordCheckSum();
  AB_saveWrite(0, &rec, sizeof rec);

  recordState = RECORD_STORED;
  return (uint8_t)r;
}

static void initObjects(void) {
  manVy = 32;
  for(int16_t i = 0; i < 6; i++) {
    letterY[i] = -72;
    letterVy[i] = 0;
  }
}

static void moveObjects(void) {
  if(manVy < 32) {
    manVy++;
  } else {
    manX = AB_random(25);
    manVy = -AB_random2(32, 97);
    manType = AB_random(4);
  }

  for(int16_t i = 0; i < 6; i++) {
    int8_t y = letterY[i], vy = letterVy[i];
    y += ++vy;
    if(y >= 64) {
      y = 64;
      vy = -isqrt(AB_random2(64, 196));
    }
    letterY[i] = y;
    letterVy[i] = vy;
  }
}

static bool handleButtons(void) {
  bool ret = false;
  if(AB_buttonDown(UP_BUTTON)) {
    if(menuPos-- == MENU_START) menuPos = MENU_CREDIT;
    playSound1();
  }
  if(AB_buttonDown(DOWN_BUTTON)) {
    if(menuPos++ == MENU_CREDIT) menuPos = MENU_START;
    playSound1();
  }
  if(AB_buttonDown(A_BUTTON | B_BUTTON)) {
    switch(menuPos) {
      case MENU_START:
        ret = true;
        AB_audioSaveOnOff();
        break;
      case MENU_SOUND:
        setSound(!sound);
        playSound2();
        break;
      case MENU_RECORD:
        titleState = STATE_RECORD;
        playSound2();
        break;
      case MENU_CREDIT:
        titleState = STATE_CREDIT;
        playSound2();
        break;
    }
  }
  return ret;
}

static void initTitle(void) {
  titleState = STATE_MENU;
  menuPos = MENU_START;
  titleToDraw = true;
  initObjects();
  setSound(AB_audioEnabled());
  if(recordState == RECORD_NOT_READ) readRecord();
}

static bool updateTitle(void) {
  bool ret = false;
  if(titleState == STATE_MENU) {
    moveObjects();
    ret = handleButtons();
    titleToDraw = true;
  } else {
    if(AB_buttonDown(A_BUTTON | B_BUTTON)) {
      titleState = STATE_MENU;
      playSound2();
      titleToDraw = true;
    }
  }
  AB_randomSeed(AB_rand() ^ AB_micros()); // Shuffle random
  return ret;
}

static void drawTitleMenu(void) {
  /*  Last score  */
  if(lastScore > 0) {
    AB_printEx(0, 0, "SCORE");
    AB_setCursor(0, 6);
    AB_printU(lastScore);
  }

  /*  Man  */
  int16_t y = 32 + manVy * manVy / 32;
  if(y < 64) AB_drawBitmap(manX, y, imgMan[manType], 32, 32, WHITE);

  /*  "Hopper" letters  */
  for(int16_t i = 0, x = 22; i < 6; i++) {
    uint8_t w = letterInfos[i].width;
    AB_drawBitmap(x, letterY[i] / 4 + (i > 0) * 5, letterInfos[i].pImg, w, 16, WHITE);
    x += w + 1;
  }

  /*  Menu items  */
  const char *p = menuText;
  for(int16_t i = 0; i < 4; i++) {
    p += AB_printEx(68 - (i == menuPos) * 4, i * 6 + 40, p) + 1;
    if(i == MENU_SOUND) AB_print(sound ? "ON" : "OFF");
  }
  AB_fillRect2(56, menuPos * 6 + 40, 5, 5, WHITE);
}

static void drawTitleRecord(void) {
  AB_printEx(22, 4, "BEST 10 SCORES");
  AB_drawFastHLine2(0, 12, 128, WHITE);
  for(int16_t i = 0; i < 2; i++) {
    for(int16_t j = 0; j < 5; j++) {
      int16_t r = i * 5 + j;
      AB_printEx(i * 60 + 4 - (r == 9) * 6, j * 6 + 14, "[");
      AB_printU(r + 1);
      AB_print("] ");
      AB_printU(hiScore[r]);
    }
  }
  AB_drawFastHLine2(0, 44, 128, WHITE);
  AB_printEx(16, 48, "PLAY COUNT ");
  AB_printU(playCount);
  AB_printEx(16, 54, "PLAY TIME  ");
  AB_printU(playFrames / 3600); // minutes
  uint8_t s = playFrames / 60 % 60; // seconds
  char buf[5] = { '\'', '0' + s / 10, '0' + s % 10, '"', 0 };
  AB_print(buf);
}

static void drawTitleCredit(void) {
  const char *p = creditText;
  for(int16_t i = 0; i < 8; i++) {
    uint8_t len = 0;
    while(p[len]) len++;
    p += AB_printEx(64 - len * 3, i * 6 + 8, p) + 1;
  }
}

static void drawTitle(void) {
  if(titleToDraw) {
    AB_clear();
    switch(titleState) {
      case STATE_MENU:   drawTitleMenu();   break;
      case STATE_RECORD: drawTitleRecord(); break;
      case STATE_CREDIT: drawTitleCredit(); break;
    }
    titleToDraw = false;
  }
}

// ===================================================================================
// Game (game.cpp)
// ===================================================================================
enum { STATE_START = 0, STATE_GAME, STATE_CLEAR, STATE_OVER, STATE_PAUSE };

#define FLOORS_MANAGE   10
#define FLOORS_LEVEL    50

enum { FLRTYPE_NONE = 0, FLRTYPE_NORMAL, FLRTYPE_MOVE, FLRTYPE_VANISH, FLRTYPE_SMALL, FLRTYPE_GOAL };

#define FLOOR_Z_MAX     4096
#define CHIPS           64
#define CHIP_ZRANGE     1024
#define CENTER_X        64
#define CENTER_Y        32

enum { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT };

#define nextFloorIdx(i) (((i) + 1) % FLOORS_MANAGE)

typedef struct {
  uint8_t type;
  uint8_t pos;
  int16_t x, y, z;
  int16_t vx, vy;
  uint8_t size, sizeBack;
} FLOOR;

typedef struct { int16_t x, y; } CHIP;

static uint8_t  state;
static bool     toDraw;
static uint32_t gameFrames;
static int16_t  counter;
static int8_t   ignoreCnt;
static uint16_t score;
static uint16_t level;
static bool     isHiscore;
static int16_t  playerX, playerY;
static int8_t   playerJump, playerTorque;
static uint8_t  playerRotate;
static FLOOR    floorAry[FLOORS_MANAGE];
static uint8_t  floorIdxFirst, floorIdxLast;
static uint8_t  flrTypes[4], flrTypesNum;
static FLOOR    *pFloorBlur;
static CHIP     chipAry[CHIPS];
static uint16_t chipBaseZ;
static bool     isBlink;

static void addFloor(uint8_t type, uint8_t pos, int16_t x, int16_t y, int16_t z) {
  FLOOR *pFloor = &floorAry[floorIdxLast];
  pFloor->type = type;
  pFloor->pos = pos;
  pFloor->x = x;
  pFloor->y = y;
  pFloor->z = z;

  uint8_t vd = AB_random(256);              // direction vd * PI / 128
  int16_t vr = (type == FLRTYPE_MOVE) ? 256 + level * 16 : 0;
  pFloor->vx = (int32_t)sinQ14(vd + 64) * vr / 16384;   // cos
  pFloor->vy = (int32_t)sinQ14(vd) * vr / 16384;

  uint8_t size = (type == FLRTYPE_SMALL) ? 36 - level : 64 - level * 3;
  pFloor->size = max(size, 16);
  pFloor->sizeBack = pFloor->size;
}

static void initLevel(bool isStart) {
  if(isStart) {
    state = STATE_START;
    gameFrames = 0;
    score = 0;
    level = 0;
    AB_playScore2(soundStart, 0);
  } else {
    level++;
    AB_stopScore();
  }

  flrTypesNum = 0;
  if(level < 6) flrTypes[flrTypesNum++] = FLRTYPE_NORMAL;
  if(level == 1) flrTypes[flrTypesNum++] = FLRTYPE_NORMAL;
  if(level & 1) flrTypes[flrTypesNum++] = FLRTYPE_MOVE;
  if(level & 2) flrTypes[flrTypesNum++] = FLRTYPE_VANISH;
  if((level & 4) || level == 8) flrTypes[flrTypesNum++] = FLRTYPE_SMALL;

  for(int16_t i = 0; i < FLOORS_MANAGE; i++) floorAry[i].type = FLRTYPE_NONE;
  floorIdxFirst = 0;
  floorIdxLast = 0;
  addFloor(FLRTYPE_NORMAL, 0, playerX, playerY, FLOOR_Z_MAX);
}

static void movePlayer(void) {
  int16_t vx = 0, vy = 0, vr;
  if(AB_buttonPressed(LEFT_BUTTON)) vx--;
  if(AB_buttonPressed(RIGHT_BUTTON)) vx++;
  if(AB_buttonPressed(UP_BUTTON)) vy--;
  if(AB_buttonPressed(DOWN_BUTTON)) vy++;
  vr = (vx != 0 && vy != 0) ? 384 : 512;
  playerX += vx * vr;
  playerY += vy * vr;

  if(vx != 0 || vy != 0) {
    uint8_t targetRotate = ATAN_DIR[(vx + 1) * 3 + vy + 1];
    int8_t diffRotate = targetRotate - playerRotate;
    if(abs(playerTorque) >= 2) {
      if(abs(diffRotate) < 3) playerTorque -= sign(playerTorque);
    } else {
      playerTorque = mid(-2, diffRotate, 2);
    }
  }
}

static void springPlayer(FLOOR *pFloor, int16_t diff) {
  score = max(score, level * FLOORS_LEVEL + pFloor->pos);
  playerTorque = (AB_random(2) * 2 - 1) * mid(1, diff / 512 - 2, 8);
  pFloorBlur = pFloor;
  if(pFloor->type == FLRTYPE_GOAL) {
    playerJump = 96;
    state = STATE_CLEAR;
    AB_playScore2(soundClear, 2);
  } else {
    playerJump = 64;
    if(pFloor->type == FLRTYPE_VANISH) pFloor->size--;
    if(state == STATE_START || state == STATE_CLEAR) state = STATE_GAME;
    AB_playScore2(soundJump, 3);
  }
}

static void moveFloors(int16_t scrollX, int16_t scrollY, int16_t scrollZ) {
  FLOOR *pFloor;
  for(int16_t i = 0; i < FLOORS_MANAGE; i++) {
    pFloor = &floorAry[i];
    if(pFloor->type != FLRTYPE_NONE) {
      pFloor->x += scrollX + pFloor->vx;
      pFloor->y += scrollY + pFloor->vy;
      pFloor->z += scrollZ;
      int16_t z = pFloor->z;
      if(pFloor->type == FLRTYPE_VANISH) {
        if(z < 0) {
          pFloor->size = pFloor->sizeBack;
        } else if(pFloor->size != pFloor->sizeBack && pFloor->size > 0) {
          pFloor->size--;
        }
      }
      if(scrollZ <= 0 && z >= 0 && z + scrollZ - 1 < 0) {
        int16_t dx = pFloor->x - playerX, dy = pFloor->y - playerY;   // 16-bit, as on AVR
        int16_t diff = max(abs(dx), abs(dy));
        if(diff < pFloor->size * 128) springPlayer(pFloor, diff);
      }
    }
  }

  pFloor = &floorAry[floorIdxFirst];
  if(pFloor->z > FLOOR_Z_MAX) {
    pFloor->type = FLRTYPE_NONE;
    floorIdxFirst = nextFloorIdx(floorIdxFirst);
  }

  pFloor = &floorAry[floorIdxLast];
  int16_t z = pFloor->z;
  if(state == STATE_GAME && z >= 0 && pFloor->type != FLRTYPE_GOAL) {
    uint8_t pos = pFloor->pos + 1;
    uint8_t type = (pos == FLOORS_LEVEL) ? FLRTYPE_GOAL : flrTypes[AB_random(flrTypesNum)];
    floorIdxLast = nextFloorIdx(floorIdxLast);
    addFloor(type, pos, AB_rand() * 2, AB_rand() * 2, z - (512 + level * 32));
  }
}

static void initChips(void) {
  for(int16_t i = 0; i < CHIPS; i++) {
    chipAry[i].x = AB_rand() * 2;
    chipAry[i].y = AB_rand() * 2;
  }
  chipBaseZ = 0;
}

static void moveChips(int16_t scrollX, int16_t scrollY, int16_t scrollZ) {
  for(int16_t i = 0; i < CHIPS; i++) {
    chipAry[i].x += scrollX;
    chipAry[i].y += scrollY;
  }
  chipBaseZ += scrollZ;
  chipBaseZ %= CHIP_ZRANGE;
}

static void initGame(void) {
  ignoreCnt = 0;
  playerX = 0;
  playerY = 0;
  playerJump = 0;
  playerTorque = 0;
  playerRotate = 196;
  pFloorBlur = NULL;
  isBlink = false;
  initLevel(true);
  initChips();
}

static bool updateGame(void) {
  /*  In case of pausing  */
  if(state == STATE_PAUSE) {
    if(AB_buttonDown(A_BUTTON | B_BUTTON)) {
      state = STATE_GAME;
      ignoreCnt = 30; // 1/2 sec
      toDraw = true;
    }
    return false;
  }

  /*  Move objects  */
  int16_t scrollX = -playerX / 4;
  int16_t scrollY = -playerY / 4;
  playerX += scrollX;
  playerY += scrollY;
  playerRotate += playerTorque;
  if(playerJump > -64) {
    if(--playerJump == 0) {
      pFloorBlur = NULL;
      if(state == STATE_CLEAR) initLevel(false);
    }
  }
  if(state != STATE_OVER) moveFloors(scrollX, scrollY, playerJump);
  moveChips(scrollX, scrollY, playerJump);

  /*  Player interaction  */
  if(state == STATE_GAME || state == STATE_OVER) {
    movePlayer();
    if(state == STATE_GAME) {
      gameFrames++;
      if(floorAry[floorIdxFirst].z < 0) {
        state = STATE_OVER;
        isHiscore = (setLastScore(score, gameFrames) == 0);
        counter = 480; // 8 secs
        AB_playScore2(soundOver, 1);
      } else if(ignoreCnt == 0 && AB_buttonDown(A_BUTTON | B_BUTTON)) {
        state = STATE_PAUSE;
      }
    } else {
      counter--;
      if(ignoreCnt == 0 && AB_buttonDown(A_BUTTON | B_BUTTON)) initLevel(true);
    }
  }
  if(ignoreCnt > 0) ignoreCnt--;
  toDraw = true;

  return (state == STATE_OVER && counter == 0);
}

static void drawPlayer(void) {
  int8_t  x = playerX / 256 + CENTER_X - 4;
  int8_t  y = playerY / 256 + CENTER_Y - 4;
  uint8_t rotate = (playerRotate + 8) / 16 % 16;

  if(isBlink) {
    for(int16_t p = 0; p < 9; p++)
      AB_drawBitmap(x + (p % 3) - 1, y + (p / 3) - 1, imgPlayers[rotate], 8, 8, BLACK);
  } else {
    uint8_t p = posEyesMask[rotate];
    AB_fillRect2(x + (p % 16), y + (p / 16), 4, 4, BLACK);
  }
  AB_drawBitmap(x, y, imgPlayers[rotate], 8, 8, WHITE);
}

static void fillPatternedRect(int16_t x, int16_t y, uint8_t w, int8_t h, const uint8_t *ptn) {
  /*  Check parameters  */
  if(x < 0) { if(w <= -x) return; w += x; x = 0; }
  if(y < 0) { if(h <= -y) return; h += y; y = 0; }
  if(w == 0 || x >= WIDTH || h <= 0 || y >= HEIGHT) return;
  if(x + w > WIDTH) w = WIDTH - x;
  if(y + h > HEIGHT) h = HEIGHT - y;

  /*  Draw a patterned rectangle (pattern column = buffer position, as on the Arduboy,
      whose buffer starts 4-byte aligned)  */
  uint8_t yOdd = y & 7;
  uint8_t d = 0xFF << yOdd;
  y -= yOdd;
  h += yOdd;
  for(uint8_t *p = AB_buffer + x + (y / 8) * WIDTH; h > 0; h -= 8, p += WIDTH - w) {
    if(h < 8) d &= 0xFF >> (8 - h);
    for(uint8_t i = w; i > 0; i--, p++) {
      *p = (*p & ~d) | (ptn[(p - AB_buffer) % 4] & d);
    }
    d = 0xFF;
  }
}

static void drawFloors(void) {
  for(int16_t i = 0; i < FLOORS_MANAGE; i++) {
    FLOOR   *pFloor = &floorAry[(floorIdxFirst + i) % FLOORS_MANAGE];
    uint8_t type = pFloor->type;
    if(pFloor->type == FLRTYPE_NONE) break;
    if(pFloor->z < 0 || pFloor->size == 0) continue;
    int16_t q = 256 + pFloor->z;
    uint8_t s = pFloor->size * 256 / q;
    int16_t x = pFloor->x / q + CENTER_X - s / 2;
    int16_t y = pFloor->y / q + CENTER_Y - s / 2;
    if(pFloor == pFloorBlur) {
      int16_t blur = (playerJump + 16) / 32;
      x += blur * AB_random2(-1, 2);
      y += blur * AB_random2(-1, 2);
    }
    if(type == FLRTYPE_GOAL) {
      AB_fillRect2(x, y, s, s, isBlink ? WHITE : BLACK);
    } else {
      fillPatternedRect(x, y, s, s, imgFloor[type - 1]);
    }
    AB_drawRect2(x, y, s, s, WHITE);
  }
}

static void drawChips(void) {
  for(int16_t i = 0; i < CHIPS; i++) {
    CHIP *pChip = &chipAry[i];
    int16_t q1 = 256 + (chipBaseZ + i * (CHIP_ZRANGE / CHIPS)) % CHIP_ZRANGE;
    int16_t x1 = pChip->x / q1 + CENTER_X;
    int16_t y1 = pChip->y / q1 + CENTER_Y;
    int16_t q2 = q1 + abs(playerJump);
    int16_t x2 = pChip->x / q2 + CENTER_X;
    int16_t y2 = pChip->y / q2 + CENTER_Y;
    AB_drawLine(x1, y1, x2, y2, WHITE);
  }
}

static int16_t drawFigure(int16_t x, int16_t y, uint16_t value, uint8_t align) {
  int16_t k = (value > 9) ? drawFigure(x - align * 4, y, value / 10, align) : 0;
  AB_drawBitmap(x + k, y, imgFigures[value % 10], 8, 16, WHITE);
  return k + 8 - align * 4;
}

static void drawStrings(void) {
  if(state == STATE_START) {
    AB_drawBitmap(39, 12, imgStart, 50, 12, WHITE);
  } else {
    AB_printEx(0, 0, "SCORE");
    drawFigure(0, 6, score, ALIGN_LEFT);
    AB_printEx(98, 0, "LEVEL");
    drawFigure(120, 6, level + 1, ALIGN_RIGHT);
    if(state == STATE_CLEAR) {
      AB_drawBitmap(30, 12, imgClear, 68, 12, WHITE);
      AB_printEx(22, 46, "TRY NEXT LEVEL");
    } else if(state == STATE_OVER) {
      AB_drawBitmap(AB_random2(25, 28), AB_random2(11, 14), imgOver, 76, 12, WHITE);
      if(isHiscore && counter % 8 < 4) AB_printEx(31, 46, "NEW RECORD!");
    } else if(state == STATE_PAUSE) {
      AB_printEx(49, 46, "PAUSE");
    }
  }
}

static void drawGame(void) {
  if(toDraw) {
    AB_clear();
    if(state != STATE_PAUSE) {
      drawChips();
      drawFloors();
    }
    drawPlayer();
    drawStrings();
    isBlink = !isBlink;
    toDraw = false;
  }
}

// ===================================================================================
// Main (hopper.ino)
// ===================================================================================
enum MODE { LOGO_MODE = 0, TITLE_MODE, GAME_MODE };

typedef struct {
  void (*initFunc)(void);
  bool (*updateFunc)(void);
  void (*drawFunc)(void);
} MODULE_FUNCS;

static const MODULE_FUNCS moduleTable[] = {
  { initLogo,  updateLogo,  drawLogo },
  { initTitle, updateTitle, drawTitle },
  { initGame,  updateGame,  drawGame },
};

int main(void) {
  enum MODE mode = LOGO_MODE;
  AB_begin();
  moduleTable[LOGO_MODE].initFunc();
  while(1) {
    uint8_t show = AB_nextFrame();
    bool isDone = moduleTable[mode].updateFunc();
    if(show) {                          // every 2nd frame: draw and send the screen
      moduleTable[mode].drawFunc();
      AB_display();
    }
    if(isDone) {
      mode = (mode == TITLE_MODE) ? GAME_MODE : TITLE_MODE;
      moduleTable[mode].initFunc();
    }
  }
}
