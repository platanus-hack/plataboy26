// ===================================================================================
// Just One Boss: the console's hardware for the game (see plat.h)
// ===================================================================================
#include "plat.h"
#include "sysx.h"
#include "system.h"
#include "gpio.h"
#include "at24.h"

#define PIN_ACT     PA2                // fire button (A); B shares the pin (rev 1.3)
#define PIN_BEEP    PA1                // buzzer, active low
#define PIN_PAD     PC4                // joypad resistor ladder
#define FRAME_TICKS (F_CPU / 30)
#define NOTE_TICKS  (F_CPU / 120)      // a PICO-8 sfx speed unit
#define SAVE_AT     0x1F1C0UL          // this game's slot in the EEPROM save area
#define SAVE_SIZE   0x40               // up to Arduventure's slot (0x1F200)
#define SAVE_MAGIC  0x4A31             // "J1"

uint8_t pages[2][128];                // one is drawn while DMA sends the other
uint8_t *pg = pages[0];
uint8_t btn_now, btn_new;
static uint32_t next_frame;

// ---- I2C (shared with the EEPROM: I2C_begin is at24.c's) and the SSD1306 -----------
uint8_t I2C_begin(uint8_t d);

static void i2c_write(uint8_t b) {
  while(!(I2C1->STAR1 & I2C_STAR1_TXE));
  I2C1->DATAR = b;
}
static void i2c_stop(void) {
  while(!(I2C1->STAR1 & I2C_STAR1_BTF));
  I2C1->CTLR1 |= I2C_CTLR1_STOP;
}
static void oled_start(uint8_t mode) {
  while(I2C1->STAR2 & I2C_STAR2_BUSY);
  I2C_begin(0x78);
  i2c_write(mode);
}

static const uint8_t OLED_INIT[] = {
  0xA8, 0x3F, 0x8D, 0x14, 0x20, 0x00, 0xDA, 0x12, 0xA1, 0xC8, 0xAF
};

// A frame is one I2C transfer; each page goes by DMA (channel 6, I2C1 TX) while the
// next one is drawn in the other buffer. Between pages the I2C holds the clock low.
static void dma_wait(void) {
  if(DMA1_Channel6->CFGR & DMA_CFGR1_EN) {
    while(!(DMA1->INTFR & DMA_TCIF6));
    DMA1->INTFCR = DMA_CTCIF6;
    DMA1_Channel6->CFGR = 0;
  }
}

void lcd_begin(void) {
  oled_start(0x00);                    // the whole screen as the window: starts at 0,0
  i2c_write(0x21); i2c_write(0); i2c_write(127);
  i2c_write(0x22); i2c_write(0); i2c_write(7);
  i2c_stop();
  oled_start(0x40);
  I2C1->CTLR2 |= I2C_CTLR2_DMAEN;
}
void lcd_page(void) {
  dma_wait();
  DMA1_Channel6->MADDR = (uint32_t)pg;
  DMA1_Channel6->CNTR = 128;
  DMA1_Channel6->CFGR = DMA_CFGR1_MINC | DMA_CFGR1_DIR | DMA_CFGR1_EN;
  pg = pg == pages[0] ? pages[1] : pages[0];
}
void lcd_end(void) {
  dma_wait();
  I2C1->CTLR2 &= ~I2C_CTLR2_DMAEN;
  i2c_stop();
}

// ---- buttons ----------------------------------------------------------------------
// joypad state from SYS_poll (games/common/joycal.h: idle R U UR D DR UD L UL DL none)
static const uint8_t JOY_B[11] = { 0, BTN_R, BTN_U, BTN_U | BTN_R, BTN_D, BTN_D | BTN_R, 0, BTN_L, BTN_U | BTN_L, BTN_D | BTN_L, 0 };

static uint16_t b_idle;               // idle level of PA2, tracked from non-B readings

static uint8_t read_buttons(void) {
  uint8_t b = JOY_B[SYS_poll(ADC_read())];  // calibrated; UP + DOWN held: back to the menu (sysx.c)
  uint16_t v;
  if(!PIN_read(PIN_ACT)) b |= BTN_A;
  else {                               // B: PA2 at 0.91-0.935 of idle, seen only by the ADC
    ADC_input(PIN_ACT);                // idle can read below 1023 (provider's sample board: 972), so
    v = ADC_read();                    // B is decided against the tracked idle level
    ADC_input(PIN_PAD);                // (same as JOY_b_pressed() in games/template/src/driver.h)
    if(v > 500) {                      // idle climbs from 0 within ~20 reads, never through B
      if(v + (b_idle >> 5) < b_idle) b |= BTN_B;
      else b_idle += (int16_t)(v - b_idle) >> 2;
    }
  }
  return b;
}

void plat_frame(void) {
  while((int32_t)(STK->CNT - next_frame) < 0);
  next_frame += FRAME_TICKS;
  if((int32_t)(STK->CNT - next_frame) > 4 * FRAME_TICKS) next_frame = STK->CNT;
  uint8_t b = read_buttons();
  btn_new = b & ~btn_now;
  btn_now = b;
}

// ---- random numbers ---------------------------------------------------------------
static uint32_t rng_state = 0x2545F491;
uint32_t rnd32(void) {                 // xorshift32
  uint32_t x = rng_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return rng_state = x;
}

// ---- sound ------------------------------------------------------------------------
// The SysTick compare interrupt runs at the note's half period (every 1/120 s when
// silent), toggles the buzzer and counts the note length down.
static const uint16_t HALF_C2[12] = {  // F_CPU / 2 / Hz / 8 for C2..B2 (pitch 0..11)
  F_CPU / 1046.5, F_CPU / 1108.7, F_CPU / 1174.7, F_CPU / 1244.5, F_CPU / 1318.5,
  F_CPU / 1396.9, F_CPU / 1480.0, F_CPU / 1568.0, F_CPU / 1661.2, F_CPU / 1760.0,
  F_CPU / 1864.7, F_CPU / 1975.5
};
static const uint8_t *volatile snd_pos;
static volatile uint32_t snd_half;     // 0 = rest
static volatile int32_t  snd_left;
volatile uint8_t snd_ch = 0xFF;

void sfx_stop(void) {
  STK->CTLR &= ~STK_CTLR_STIE;
  snd_ch = 0xFF; snd_half = 0;
  PIN_high(PIN_BEEP);
}

static void snd_next(void) {
  uint8_t k = *snd_pos;
  if(k == 0xFF) { sfx_stop(); return; }
  snd_left += (int32_t)snd_pos[1] * NOTE_TICKS;
  snd_pos += 2;
  snd_half = k == 0x7F ? 0 : ((uint32_t)HALF_C2[(k & 63) % 12] << 3) >> ((k & 63) / 12);
}

void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void) {
  STK->SR = 0;
  // plataboy26: the pin low for BUZ_PULSE_US, high the rest of the period (same pitch,
  // far less current from the coin cell: games/common/buzzer.h). The highest note
  // (B7, ~4 kHz) still has a half period longer than that.
  uint32_t step = NOTE_TICKS;
  if(snd_half) {
    PIN_toggle(PIN_BEEP);
    step = (GPIOA->OUTDR & 2) ? 2 * snd_half - BUZ_PULSE_US * (F_CPU / 1000000) : BUZ_PULSE_US * (F_CPU / 1000000);
  } else PIN_high(PIN_BEEP);
  STK->CMP += step;
  snd_left -= (int32_t)step;
  if(snd_left <= 0) snd_next();
}

void sfx_play(const uint8_t *notes, uint8_t ch) {
  snd_ch = ch;
  snd_pos = notes;
  snd_left = 0;
  snd_next();
  if(snd_ch == 0xFF) return;
  STK->SR = 0;
  STK->CMP = STK->CNT + 64;
  STK->CTLR |= STK_CTLR_STIE;
}

// ---- records in the EEPROM --------------------------------------------------------
void save_read(uint16_t *rec) {
  uint16_t b[5];
  uint8_t ok = EE_read(SAVE_AT, (uint8_t *)b, sizeof b) == EE_OK && b[0] == SAVE_MAGIC;
  for(uint8_t i = 0; i < 4; i++) rec[i] = ok ? b[i + 1] : 0;
}
void save_write(const uint16_t *rec) {
  uint16_t b[5] = { SAVE_MAGIC, rec[0], rec[1], rec[2], rec[3] };
  _Static_assert(sizeof b <= SAVE_SIZE, "Just One Boss's records are larger than its save slot");
  EE_write(SAVE_AT, (const uint8_t *)b, sizeof b);
}

// ---- init -------------------------------------------------------------------------
void plat_init(void) {
  PIN_input_AN(PIN_PAD);
  PIN_input_PU(PIN_ACT);
  PIN_output(PIN_BEEP);
  PIN_high(PIN_BEEP);
  // I2C1 on PC1 (SDA) / PC2 (SCL), 400 kHz
  RCC->APB2PCENR |= RCC_AFIOEN | RCC_IOPCEN;
  RCC->APB1PCENR |= RCC_I2C1EN;
  GPIOC->CFGLR = (GPIOC->CFGLR & ~((0b1111 << 4) | (0b1111 << 8))) | (0b1101 << 4) | (0b1101 << 8);
  I2C1->CTLR2 = 4;
  I2C1->CKCFGR = (F_CPU / (3 * 400000)) | I2C_CKCFGR_FS;
  I2C1->CTLR1 = I2C_CTLR1_PE;
  RCC->AHBPCENR |= RCC_DMA1EN;
  DMA1_Channel6->PADDR = (uint32_t)&I2C1->DATAR;
  oled_start(0x00);
  for(uint8_t i = 0; i < sizeof OLED_INIT; i++) i2c_write(OLED_INIT[i]);
  i2c_stop();
  ADC_init();
  ADC1->SAMPTR2 = 7 | 7 << 6;          // PA2 and PC4 (joypad): longest sample time, or B reads ~5%
                                       // low and the joypad high (each read after the other channel)
  ADC_input(PIN_PAD);
  EE_read(JOYCAL_EE, SYS_cal, JOYCAL_N);   // joypad thresholds, measured by the menu (sysx.h)
  PFIC->IENR[0] = 1u << 12;            // SysTick interrupt (enabled while a sound plays)
  asm volatile("csrsi mstatus, 0x8");
  next_frame = STK->CNT;
  rng_state ^= STK->CNT;
}
