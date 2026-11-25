// ===================================================================================
// CH32V003 Game Console Simulator - core
// ===================================================================================
//
// CPU     : QingKe V2A, RV32EC + Zicsr, PFIC (vectored, HPE), WFI/WFE
// SoC     : RCC clock tree, FLASH latency, SysTick, GPIO A/C/D, AFIO, EXTI,
//           ADC1, I2C1 (master, bit-timed), DMA1 (7 ch), USART1 (tx), IWDG, AWU
// Board   : SSD1306 128x64 OLED on I2C (0x3C), resistor-ladder joypad on PC4 (ADC),
//           ACT button on PA2, magnetic buzzer on PA1 (active low)
//
// References: CH32V003 Reference Manual V1.x, QingKe V2 Processor Manual,
//             SSD1306 datasheet rev 1.1, CH32V003-GameConsole schematic v1.1

#include "emu.h"

Emu E;

// -----------------------------------------------------------------------------------
// tiny libc replacements (the wasm build has no libc)
// -----------------------------------------------------------------------------------
void *memset(void *d, int c, unsigned long n) {
  uint8_t *p = d; while(n--) *p++ = (uint8_t)c; return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
  uint8_t *p = d; const uint8_t *q = s; while(n--) *p++ = *q++; return d;
}

// -----------------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------------
#define HSI_HZ        24000000u
#define LSI_HZ        128000u

#define IRQ_SYSTICK   12
#define IRQ_SW        14
#define IRQ_EXTI      20
#define IRQ_AWU       21
#define IRQ_DMA1_CH1  22
#define IRQ_ADC       29
#define IRQ_I2C_EV    30
#define IRQ_I2C_ER    31
#define IRQ_USART     32

// I2C bits
#define I2C_SB    0x0001
#define I2C_ADDR  0x0002
#define I2C_BTF   0x0004
#define I2C_STOPF 0x0010
#define I2C_RXNE  0x0040
#define I2C_TXE   0x0080
#define I2C_AF    0x0400
#define I2C_MSL   0x0001
#define I2C_BUSY  0x0002
#define I2C_TRA   0x0004

#define I2C_PE    0x0001
#define I2C_ACK   0x0400
#define I2C_POS   0x0800
#define I2C_LAST  0x1000   // CTLR2: NACK the byte that completes the DMA transfer
#define I2C_DMAEN 0x0800   // CTLR2

#define OLED_I2C_ADDR   0x3C
#define EEPROM_I2C_ADDR 0x50   // 1010 A2 A1 P0: A2 = A1 = GND, P0 = address bit 16
#define EEPROM_TWR_US   5000   // AT24CM01 self-timed write cycle (datasheet max)

uint8_t  eeprom_mem[EEPROM_SIZE];
uint32_t eeprom_dirty;

static uint64_t irq_line;        // level-sensitive interrupt lines from peripherals
static uint8_t  event_flag;      // WFE event latch
static uint64_t awu_at;          // next AWU wake-up (0 = off)
static uint32_t awu_csr, awu_wr, awu_psc;
static uint32_t pwr_ctlr;

// pre-decoded flash instructions (rebuilt on load, not part of the save state)
static uint32_t dcache[FLASH_SIZE / 2];
static uint8_t  dlen[FLASH_SIZE / 2];

static uint8_t  fb[64 * 128];

static void sched(void);
static void i2c_service(void);
static void stk_service(void);
static void adc_service(void);
static void dma_i2c_request(void);
static void system_reset(uint32_t flags);

// -----------------------------------------------------------------------------------
// Logging helpers
// -----------------------------------------------------------------------------------
static void log_str(const char *s) {
  while(*s && E.log_len < UART_BUF - 1) E.log[E.log_len++] = *s++;
}
static void log_hex(uint32_t v) {
  char b[11] = "0x00000000";
  for(int i = 0; i < 8; i++) b[9 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15];
  log_str(b);
}

// -----------------------------------------------------------------------------------
// Clock tree
// -----------------------------------------------------------------------------------
static const uint16_t HPRE_DIV[16] = {1,2,3,4,5,6,7,8,2,4,8,16,32,64,128,256};

static void audio_set_step(void) {
  // cycles per audio sample as 32.32 fixed point
  E.audio_step_fp = ((uint64_t)E.hclk << 32) / AUDIO_RATE;
}

static uint32_t stk_value(uint64_t now);
static void stk_rebase(uint64_t now);

static void clk_update(void) {
  uint32_t sw = E.sys.rcc_cfgr0 & 3, sysclk = HSI_HZ;
  if(sw == 2 && (E.sys.rcc_ctlr & (1u << 24))) sysclk = HSI_HZ * 2;   // PLL (HSI x2)
  uint32_t h = sysclk / HPRE_DIV[(E.sys.rcc_cfgr0 >> 4) & 15];
  if(h != E.hclk) {
    stk_rebase(E.cycles);
    E.hclk = h;
    audio_set_step();
  }
}

static inline uint32_t flash_latency(void) { return (E.sys.actlr & 3) ? 1 : 0; }

// -----------------------------------------------------------------------------------
// Interrupts
// -----------------------------------------------------------------------------------
static inline void irq_set_line(int n, int level) {
  if(level) irq_line |= (1ull << n); else irq_line &= ~(1ull << n);
}
static inline uint64_t irq_pending_all(void) { return E.irq_pending | irq_line; }

// -----------------------------------------------------------------------------------
// Board: buttons, analog pad, pin levels
// -----------------------------------------------------------------------------------
// Joypad ladder: each direction switch connects VCC through a resistor to PAD,
// PAD is pulled to GND by R6 = 2k. ADC reads V(PAD) relative to VDD.
static double pad_ratio(void) {
  double g = 0;
  if(E.buttons & BTN_UP)    g += 1.0 / 8200.0;
  if(E.buttons & BTN_DOWN)  g += 1.0 / 3900.0;
  if(E.buttons & BTN_RIGHT) g += 1.0 / 20000.0;
  if(E.buttons & BTN_LEFT)  g += 1.0 / 2000.0;
  if(E.pad_leak) g += 1.0 / E.pad_leak;     // leakage to VCC (flux residue, a switch held lightly)
  double r = 2000.0 * g;
  return r / (1.0 + r);
}

static uint32_t rng_next(void) {
  E.rng ^= E.rng << 13; E.rng ^= E.rng >> 17; E.rng ^= E.rng << 5;
  return E.rng;
}

// PA2 (ACT, ADC channel A0). Rev. 1.1/1.2: ACT switch to GND, internal pull-up only.
// Rev. 1.3: R10 = 10k pull-up to VCC, ACT to GND, B to GND through R9 = 100k. B alone
// gives 0.91 VDD (still a logic 1 for games that only know ACT); A wins over B.
#define PA2_PULLUP_INT  40000.0         // CH32V003 internal pull-up, typical
static double pa2_ratio(void) {
  if(E.buttons & BTN_ACT) return 0.0;
  GPIOPort *g = &E.gpio[0];
  uint32_t cfg = (g->cfglr >> 8) & 15;
  int pullup = (cfg >> 2) == 2 && (g->outdr & 4);
  double up = (pullup ? 1.0 / PA2_PULLUP_INT : 0) + (E.board_rev >= 13 ? 1.0 / 10000.0 : 0);
  double down = (E.board_rev >= 13 && (E.buttons & BTN_B)) ? 1.0 / 100000.0 : 0;
  if(up == 0) return 0.0;                                             // floating
  return up / (up + down);
}

// External drive of a pin by the board: -1 floating, 0 low, 1 high.
static int board_pin(int port, int pin) {
  if(port == 0 && pin == 2) {                                         // ACT (and B)
    if(E.buttons & BTN_ACT) return 0;
    if(E.board_rev >= 13) return pa2_ratio() > 0.7 ? 1 : 0;           // VIH = 0.7 VDD
    return -1;
  }
  if(port == 0 && pin == 1) return 1;                                 // buzzer to VCC
  if(port == 1 && pin == 4) return pad_ratio() > 0.5 ? 1 : 0;         // PAD (R6 pull-down)
  if(port == 1 && (pin == 1 || pin == 2)) return 1;                   // I2C pull-ups
  if(port == 2 && pin == 1) return 1;                                 // SWIO pull-up
  return -1;
}

static int pin_level(int port, int pin) {
  GPIOPort *g = &E.gpio[port];
  uint32_t cfg = (g->cfglr >> (pin * 4)) & 15, mode = cfg & 3, cnf = cfg >> 2;
  int ext = board_pin(port, pin);
  if(mode) {                                       // output
    int drv;
    if(cnf & 2) drv = 1;                           // alternate function: idle high
    else drv = (g->outdr >> pin) & 1;
    if(cnf & 1) {                                  // open drain
      if(!drv) return 0;
      return ext < 0 ? 0 : ext;
    }
    return drv;                                    // push-pull
  }
  if(cnf == 0) return 0;                           // analog: digital path off
  if(ext >= 0) return ext;
  if(cnf == 2) return (g->outdr >> pin) & 1;       // pull-up / pull-down
  return 0;                                        // floating
}

static uint32_t gpio_indr(int port) {
  uint32_t v = 0;
  for(int i = 0; i < 8; i++) v |= (uint32_t)pin_level(port, i) << i;
  return v;
}

// -----------------------------------------------------------------------------------
// Audio: integrate buzzer current (PA1 low) into 48 kHz samples
// -----------------------------------------------------------------------------------
static void audio_emit(float v) {
  // the magnetic transducer only reacts to changes: simple DC-blocking high pass
  float y = 0.995f * (E.audio_hp + v - E.audio_prev);
  E.audio_prev = v; E.audio_hp = y;
  if(E.audio_len >= AUDIO_BUF) E.audio_len = 0;   // host is not draining: drop
  float s = y * 14000.0f;
  if(s > 32767) s = 32767; if(s < -32768) s = -32768;
  E.audio[E.audio_len++] = (int16_t)s;
}

static void audio_advance(uint64_t now) {
  uint64_t end = E.audio_pos;
  while(now >= end) {
    E.audio_acc += ((end - E.audio_last) * E.buzzer_level) >> 8;
    E.audio_last = end;
    float step = (float)(E.audio_step_fp >> 32);
    audio_emit(step > 0 ? (float)E.audio_acc / step : 0);
    E.audio_acc = 0;
    uint32_t f = E.audio_pos_frac;
    E.audio_pos_frac = f + (uint32_t)E.audio_step_fp;
    E.audio_pos += (E.audio_step_fp >> 32) + (E.audio_pos_frac < f);
    end = E.audio_pos;
  }
  E.audio_acc += ((now - E.audio_last) * E.buzzer_level) >> 8;
  E.audio_last = now;
}

// TIM1 channel 2 PWM on PA1 (its default pin): the buzzer sees the average current.
// Modelled: up-counting PWM mode 1/2 (OC2M 110/111), CC2P polarity, ATRLR, CH2CVR,
// CC2E, MOE, CEN. The carrier is far above audio, so the level is the duty cycle.
static uint16_t tim1_pa1_low(void) {          // fraction of time PA1 is low, 0..256
  uint32_t period = E.tim1.atrlr + 1, ccr = E.tim1.ch2cvr, m = (E.tim1.chctlr1 >> 12) & 7;
  if(ccr > period) ccr = period;
  uint32_t active = (m == 7) ? period - ccr : ccr;                  // PWM2 : PWM1
  if(m != 6 && m != 7) active = (m == 5) ? period : 0;              // forced active/inactive
  uint32_t high = (E.tim1.ccer & (1u << 5)) ? period - active : active;   // CC2P: active low
  return (uint16_t)(((period - high) << 8) / period);
}

static void buzzer_update(void) {
  GPIOPort *g = &E.gpio[0];
  uint32_t cfg = (g->cfglr >> 4) & 15;
  int lvl;
  if((cfg & 3) && (cfg & 8))                                        // PA1: alternate function
    lvl = ((E.tim1.ctlr1 & 1) && (E.tim1.ccer & (1u << 4)) && (E.tim1.bdtr & 0x8000)) ? tim1_pa1_low() : 0;
  else lvl = ((cfg & 3) && !((g->outdr >> 1) & 1)) ? 256 : 0;
  if(lvl != E.buzzer_level) {
    audio_advance(E.cycles);
    E.buzzer_level = lvl;
  }
}

// -----------------------------------------------------------------------------------
// EXTI (lines 0-7 from GPIO, 8 PVD, 9 AWU)
// -----------------------------------------------------------------------------------
static uint32_t exti_levels(void) {
  uint32_t v = 0;
  for(int l = 0; l < 8; l++) {
    int sel = (E.afio_exticr >> (l * 2)) & 3;     // 00 PA, 10 PC, 11 PD
    int port = sel == 0 ? 0 : sel == 2 ? 1 : sel == 3 ? 2 : -1;
    if(port >= 0) v |= (uint32_t)pin_level(port, l) << l;
  }
  return v;
}

static void exti_trigger(uint32_t lines) {
  if(!lines) return;
  if(lines & E.exti_evenr) event_flag = 1;
  E.exti_intfr |= lines & E.exti_intenr;
  irq_set_line(IRQ_EXTI, (E.exti_intfr & 0xFF) != 0);
  irq_set_line(IRQ_AWU, (E.exti_intfr >> 9) & 1);
}

static void exti_edges(uint32_t before, uint32_t after) {
  uint32_t rise = ~before & after, fall = before & ~after;
  exti_trigger((rise & E.exti_rtenr) | (fall & E.exti_ftenr));
}

// -----------------------------------------------------------------------------------
// SSD1306 OLED controller
// -----------------------------------------------------------------------------------
static void oled_reset(void) {
  SSD1306_t *o = &E.oled;
  memset(o, 0, sizeof(*o));
  o->contrast = 0x7F; o->mux = 63; o->mem_mode = 2; o->col_end = 127; o->page_end = 7;
  o->compins = 0x12; o->clkdiv = 0x80; o->precharge = 0x22; o->vcomh = 0x20;
  o->expect_ctrl = 1;
  // GDDRAM content is undefined after power-up: fill with noise like a real panel
  for(int p = 0; p < 8; p++) for(int c = 0; c < 128; c++) o->ram[p][c] = (uint8_t)rng_next();
}

static int oled_argc(uint8_t c) {
  switch(c) {
    case 0x81: case 0x20: case 0xA8: case 0xD3: case 0xD5: case 0xD9:
    case 0xDA: case 0xDB: case 0x8D: case 0xD6: return 1;
    case 0x21: case 0x22: case 0xA3: return 2;
    case 0x26: case 0x27: return 6;
    case 0x29: case 0x2A: return 5;
    default: return 0;
  }
}

static void oled_exec(void) {
  SSD1306_t *o = &E.oled;
  uint8_t c = o->cmd_buf[0], a = o->cmd_buf[1], b = o->cmd_buf[2];
  if(c <= 0x0F)       { o->col = (o->col & 0xF0) | c; return; }
  if(c <= 0x1F)       { o->col = ((o->col & 0x0F) | ((c & 0x07) << 4)) & 0x7F; return; }
  if(c >= 0x40 && c <= 0x7F) { o->start_line = c & 0x3F; return; }
  if(c >= 0xB0 && c <= 0xB7) { o->page = c & 7; return; }
  switch(c) {
    case 0x20: if((a & 3) != 3) o->mem_mode = a & 3; break;
    case 0x21: o->col_start = a & 0x7F; o->col_end = b & 0x7F; o->col = o->col_start; break;
    case 0x22: o->page_start = a & 7; o->page_end = b & 7; o->page = o->page_start; break;
    case 0x81: o->contrast = a; break;
    case 0x8D: o->charge_pump = (a & 0x04) ? 1 : 0; break;
    case 0xA0: case 0xA1: o->seg_remap = c & 1; break;
    case 0xA4: case 0xA5: o->entire_on = c & 1; break;
    case 0xA6: case 0xA7: o->invert = c & 1; break;
    case 0xA8: if((a & 0x3F) >= 15) o->mux = a & 0x3F; break;
    case 0xAE: case 0xAF: o->on = c & 1; break;
    case 0xC0: case 0xC8: o->com_remap = (c >> 3) & 1; break;
    case 0xD3: o->offset = a & 0x3F; break;
    case 0xD5: o->clkdiv = a; break;
    case 0xD9: o->precharge = a; break;
    case 0xDA: o->compins = a; break;
    case 0xDB: o->vcomh = a; break;
    case 0x2E: o->scroll_on = 0; break;
    case 0x2F: o->scroll_on = 1; break;
    default: break;   // scroll setup, zoom, NOP, ...
  }
}

static void oled_command(uint8_t v) {
  SSD1306_t *o = &E.oled;
  if(o->cmd_need) {
    o->cmd_buf[o->cmd_len++] = v;
    if(o->cmd_len > o->cmd_need) { oled_exec(); o->cmd_need = 0; o->cmd_len = 0; }
    return;
  }
  o->cmd_buf[0] = v; o->cmd_len = 1;
  int n = oled_argc(v);
  if(n) o->cmd_need = (uint8_t)n;
  else { oled_exec(); o->cmd_len = 0; }
}

static void oled_data(uint8_t v) {
  SSD1306_t *o = &E.oled;
  o->ram[o->page & 7][o->col & 0x7F] = v;
  o->data_bytes++;
  if(o->mem_mode == 0) {                       // horizontal
    if(o->col >= o->col_end) {
      o->col = o->col_start;
      if(o->page >= o->page_end) { o->page = o->page_start; o->frames++; }
      else o->page++;
    } else o->col++;
  } else if(o->mem_mode == 1) {                // vertical
    if(o->page >= o->page_end) {
      o->page = o->page_start;
      if(o->col >= o->col_end) { o->col = o->col_start; o->frames++; }
      else o->col++;
    } else o->page++;
  } else {                                     // page
    o->col = (o->col >= 127) ? 0 : o->col + 1;
  }
}

// I2C device interface
static int oled_i2c_address(uint8_t addr_byte) {
  SSD1306_t *o = &E.oled;
  o->addressed = ((addr_byte >> 1) == OLED_I2C_ADDR) && !(addr_byte & 1);
  o->expect_ctrl = 1;
  return o->addressed;
}

static int oled_i2c_byte(uint8_t v) {
  SSD1306_t *o = &E.oled;
  if(!o->addressed) return 0;
  if(o->expect_ctrl) {
    o->cont = !(v & 0x80);
    o->dc = (v >> 6) & 1;
    o->expect_ctrl = 0;
    return 1;
  }
  if(o->dc) oled_data(v); else oled_command(v);
  if(!o->cont) o->expect_ctrl = 1;
  return 1;
}

static void oled_i2c_stop(void) { E.oled.addressed = 0; E.oled.expect_ctrl = 1; }

// ---- AT24CM01 ------------------------------------------------------------------------
// Byte/page write (256-byte page latch, wraps inside the page, committed at STOP, then
// tWR during which the chip NACKs its address = acknowledge polling), current address,
// random and sequential reads (wrap at the end of the array).
static void eeprom_commit(void) {
  EEPROM_t *m = &E.eeprom;
  if(!m->latch_any) return;
  uint8_t *dst = eeprom_mem + (m->page << 8);
  for(int k = 0; k < 256; k++)
    if(m->latch_mask[k >> 5] & (1u << (k & 31))) dst[k] = m->latch[k];
  m->latch_any = 0; memset(m->latch_mask, 0, sizeof(m->latch_mask));
  m->busy_until = E.cycles + (uint64_t)E.hclk / 1000000u * EEPROM_TWR_US;
  m->writes++; eeprom_dirty++;
}

static int eeprom_i2c_address(uint8_t addr_byte) {
  EEPROM_t *m = &E.eeprom;
  m->addressed = 0;
  if(!m->present || ((addr_byte >> 2) != (EEPROM_I2C_ADDR >> 1))) return 0;
  if(E.cycles < m->busy_until) return 0;                 // write cycle in progress: NACK
  m->latch_any = 0; memset(m->latch_mask, 0, sizeof(m->latch_mask));   // restart aborts a write
  m->addressed = 1;
  m->reading = addr_byte & 1;
  m->addr = (m->addr & 0xFFFF) | ((uint32_t)((addr_byte >> 1) & 1) << 16);
  m->addr_bytes = 0;
  return 1;
}

static int eeprom_i2c_byte(uint8_t v) {
  EEPROM_t *m = &E.eeprom;
  if(!m->addressed || m->reading) return 0;
  if(m->addr_bytes < 2) {
    if(m->addr_bytes++ == 0) m->addr = (m->addr & 0x100FF) | ((uint32_t)v << 8);
    else                     m->addr = (m->addr & 0x1FF00) | v;
    m->page = m->addr >> 8;
    return 1;
  }
  uint32_t k = m->addr & 0xFF;
  m->latch[k] = v; m->latch_mask[k >> 5] |= 1u << (k & 31); m->latch_any = 1;
  m->addr = (m->addr & ~0xFFu) | ((k + 1) & 0xFF);       // roll over inside the page
  return 1;
}

static uint8_t eeprom_i2c_read(void) {
  EEPROM_t *m = &E.eeprom;
  uint8_t v = eeprom_mem[m->addr & (EEPROM_SIZE - 1)];
  m->addr = (m->addr + 1) & (EEPROM_SIZE - 1);
  return v;
}

static void eeprom_i2c_stop(void) {
  EEPROM_t *m = &E.eeprom;
  if(m->addressed && !m->reading && m->addr_bytes == 2) eeprom_commit();
  m->addressed = 0;
}

// ---- bus: route transfers to the addressed slave ------------------------------------
static int i2c_dev_address(uint8_t addr_byte) {
  E.i2c.dev = 0;
  if(oled_i2c_address(addr_byte)) E.i2c.dev = 1;
  else if(eeprom_i2c_address(addr_byte)) E.i2c.dev = 2;
  return E.i2c.dev != 0;
}
static int i2c_dev_write(uint8_t v) {
  return E.i2c.dev == 1 ? oled_i2c_byte(v) : E.i2c.dev == 2 ? eeprom_i2c_byte(v) : 0;
}
static uint8_t i2c_dev_read(void) {
  return E.i2c.dev == 2 ? eeprom_i2c_read() : 0xFF;      // nobody drives SDA: pull-ups
}
static void i2c_dev_stop(void) { oled_i2c_stop(); eeprom_i2c_stop(); E.i2c.dev = 0; }

void emu_render(uint8_t *out) {
  SSD1306_t *o = &E.oled;
  memset(out, 0, 64 * 128);
  if(!o->on || !o->charge_pump) return;
  uint8_t lit = (uint8_t)(70 + (185u * o->contrast) / 255u);
  int alt = (o->compins >> 4) & 1, lr = (o->compins >> 5) & 1;
  for(int k = 0; k <= o->mux; k++) {
    int row = (k + o->start_line) & 63;               // GDDRAM row output at scan step k
    int com = (k + o->offset) & 63;
    if(o->com_remap) com = 63 - com;
    if(lr) com ^= 32;
    int phys = alt ? com : (com < 32 ? com * 2 : (com - 32) * 2 + 1);
    int y = 63 - phys;                                // module mounting (upright with C8)
    uint8_t *line = out + y * 128;
    const uint8_t *page = o->ram[row >> 3];
    int bit = row & 7;
    for(int col = 0; col < 128; col++) {
      int on = (page[col] >> bit) & 1;
      if(o->entire_on) on = 1;
      if(o->invert) on ^= 1;
      int x = o->seg_remap ? col : 127 - col;
      line[x] = on ? lit : 0;
    }
  }
}

uint8_t *emu_framebuffer(void) { emu_render(fb); return fb; }
uint8_t *emu_sram(void) { return E.sram; }
uint8_t *emu_flash(void) { return E.flash; }
uint32_t emu_flash_ops(void) { return E.sys.flash_ops; }
uint32_t emu_pc(void) { return E.pc; }
static uint32_t i2c_bit_cycles(void);
uint32_t emu_peek32(uint32_t addr) {
  if(addr == 0x50000000) return (uint32_t)E.oled.on << 8 | E.oled.contrast;   // debug: OLED
  if(addr >= 0x40005400 && addr < 0x40005800) {           // I2C: internal state, not DATAR
    I2C_t *i = &E.i2c;
    switch(addr & 0xFF) {
      case 0x00: return i->ctlr1; case 0x14: return i->star1; case 0x18: return i->star2;
      case 0x20: return (uint32_t)i->ev_at; case 0x24: return (uint32_t)E.next_event;
      case 0x28: return i2c_bit_cycles(); case 0x2C: return i->ckcfgr;
    }
    return (uint32_t)(i->ev_at >> 32) << 16 | i->ev_kind << 8 | i->shifting;
  }
  return 0;
}
uint64_t emu_cycles(void) { return E.cycles; }

// -----------------------------------------------------------------------------------
// SysTick (QingKe STK): 32-bit up/down counter, HCLK or HCLK/8
// -----------------------------------------------------------------------------------
#define STK_STE   1u
#define STK_STIE  2u
#define STK_STCLK 4u
#define STK_STRE  8u
#define STK_MODE  16u
#define STK_INIT  32u
#define STK_SWIE  0x80000000u

static uint64_t stk_match_at;     // next cycle at which CNTIF gets set (0 = never)

static inline uint32_t stk_div(void) { return (E.stk.ctlr & STK_STCLK) ? 1 : 8; }

static uint32_t stk_value(uint64_t now) {
  STK_t *s = &E.stk;
  if(!(s->ctlr & STK_STE) || now <= s->t0) return s->cnt0;
  uint64_t e = (now - s->t0) / stk_div();
  uint64_t period = (uint64_t)s->cmp + 1;
  if(!(s->ctlr & STK_MODE)) {                         // count up
    if((s->ctlr & STK_STRE) && s->cnt0 <= s->cmp)
      return (uint32_t)(((uint64_t)s->cnt0 + e) % period);
    return (uint32_t)(s->cnt0 + e);
  } else {                                            // count down
    if(e <= s->cnt0) return (uint32_t)(s->cnt0 - e);
    if(s->ctlr & STK_STRE) return (uint32_t)(s->cmp - ((e - s->cnt0 - 1) % period));
    return (uint32_t)(s->cnt0 - e);
  }
}

static void stk_rebase(uint64_t now) {
  STK_t *s = &E.stk;
  if(!(s->ctlr & STK_STE) || now <= s->t0) { if(now > s->t0) s->t0 = now; return; }
  uint32_t div = stk_div();
  uint64_t e = (now - s->t0) / div;
  s->cnt0 = stk_value(s->t0 + e * div);
  s->t0 += e * div;
}

static void stk_schedule(void) {
  STK_t *s = &E.stk;
  stk_match_at = 0;
  if(!(s->ctlr & STK_STE)) return;
  uint64_t ticks;
  if(!(s->ctlr & STK_MODE)) ticks = (uint32_t)(s->cmp - s->cnt0);
  else ticks = s->cnt0;
  if(ticks == 0) ticks = (s->ctlr & STK_STRE) ? (uint64_t)s->cmp + 1 : (1ull << 32);
  stk_match_at = s->t0 + ticks * stk_div();
}

static void stk_service(void) {
  STK_t *s = &E.stk;
  while(stk_match_at && E.cycles >= stk_match_at) {
    s->sr |= 1;
    uint64_t period = (s->ctlr & STK_STRE) ? (uint64_t)s->cmp + 1 : (1ull << 32);
    stk_match_at += period * stk_div();
  }
  irq_set_line(IRQ_SYSTICK, (s->sr & 1) && (s->ctlr & STK_STIE));
  irq_set_line(IRQ_SW, (s->ctlr & STK_SWIE) != 0);
}

static uint32_t stk_read(uint32_t off) {
  stk_service();
  switch(off) {
    case 0x00: return E.stk.ctlr;
    case 0x04: return E.stk.sr;
    case 0x08: return stk_value(E.cycles);
    case 0x0C: return 0;
    case 0x10: return E.stk.cmp;
    case 0x14: return 0;
  }
  return 0;
}

static void stk_write(uint32_t off, uint32_t v) {
  STK_t *s = &E.stk;
  stk_service();
  stk_rebase(E.cycles);
  switch(off) {
    case 0x00:
      if(!(s->ctlr & STK_STE)) s->t0 = E.cycles;
      s->ctlr = v & ~STK_INIT;
      if(v & STK_INIT) s->cnt0 = (v & STK_MODE) ? s->cmp : 0;
      break;
    case 0x04: s->sr &= v; break;
    case 0x08: s->cnt0 = v; break;
    case 0x10: s->cmp = v; break;
  }
  stk_schedule();
  stk_service();
  sched();
}

// -----------------------------------------------------------------------------------
// I2C1 master (byte-level, bit-timed)
// -----------------------------------------------------------------------------------
static uint32_t i2c_bit_cycles(void) {
  uint32_t ccr = E.i2c.ckcfgr & 0x0FFF;
  if(ccr < 1) ccr = 1;
  if(E.i2c.ckcfgr & 0x8000) return (E.i2c.ckcfgr & 0x4000) ? 25 * ccr : 3 * ccr;
  return 2 * ccr;
}

static void i2c_irq_update(void) {
  I2C_t *i = &E.i2c;
  int ev = 0, er = 0;
  if(i->ctlr2 & 0x0200) {                                    // ITEVTEN
    if(i->star1 & (I2C_SB | I2C_ADDR | I2C_BTF | I2C_STOPF)) ev = 1;
    if((i->ctlr2 & 0x0400) && (i->star1 & (I2C_TXE | I2C_RXNE))) ev = 1;   // ITBUFEN
  }
  if((i->ctlr2 & 0x0100) && (i->star1 & 0x0F00)) er = 1;  // ITERREN
  irq_set_line(IRQ_I2C_EV, ev);
  irq_set_line(IRQ_I2C_ER, er);
}

static void i2c_event(uint8_t kind, uint32_t bits) {
  E.i2c.ev_kind = kind;
  E.i2c.ev_at = E.cycles + (uint64_t)bits * i2c_bit_cycles();
  sched();
}

static void i2c_shift(uint8_t v, int is_addr) {
  E.i2c.shifting = 1; E.i2c.shift_byte = v; E.i2c.is_addr = (uint8_t)is_addr;
  i2c_event(2, 9);
}

// master receiver: clock in the next byte (8 data bits + ACK/NACK from the master)
static void i2c_rx_begin(void) {
  E.i2c.shifting = 1;
  E.i2c.rx_ack_start = (E.i2c.ctlr1 & I2C_ACK) != 0;
  i2c_event(4, 9);
}

static void i2c_write_dr(uint8_t v) {
  I2C_t *i = &E.i2c;
  if(!(i->ctlr1 & 1)) return;
  if(i->star1 & I2C_SB) {                          // address phase
    i->star1 &= ~I2C_SB;
    i2c_shift(v, 1);
  } else if((i->star2 & I2C_MSL) && !i->shifting && !i->addr_pending_clear && !i->dr_full) {
    i->star1 &= ~I2C_BTF;
    i->star1 |= I2C_TXE;
    i2c_shift(v, 0);                               // DR -> shift register at once
  } else {
    i->dr = v; i->dr_full = 1;
    i->star1 &= ~(I2C_TXE | I2C_BTF);
  }
  i2c_irq_update();
}

static void i2c_service(void) {
  I2C_t *i = &E.i2c;
  while(i->ev_at && E.cycles >= i->ev_at) {
    uint8_t kind = i->ev_kind;
    i->ev_at = 0; i->ev_kind = 0;
    if(kind == 1) {                                 // START generated
      i->star1 = (i->star1 & 0x0F00) | I2C_SB;
      i->star2 |= I2C_MSL | I2C_BUSY;
      i->ctlr1 &= ~0x0100;
      i->start_pending = 0;
    } else if(kind == 2) {                          // byte shifted out
      i->shifting = 0;
      if(i->is_addr) {
        int ack = i2c_dev_address(i->shift_byte);
        if(ack) {
          i->star1 |= I2C_ADDR;
          if(!(i->shift_byte & 1)) { i->star1 |= I2C_TXE; i->star2 |= I2C_TRA; }
          else i->star2 &= ~I2C_TRA;
          i->addr_pending_clear = 1; i->star1_read = 0;
        } else {
          i->star1 |= I2C_AF;
        }
      } else {
        if(!i2c_dev_write(i->shift_byte)) i->star1 |= I2C_AF;
      }
      if(i->stop_pending) { i2c_event(3, 1); }
      else if(i->start_pending) { i2c_event(1, 1); }
      else if(!i->is_addr) {
        if(i->dr_full) {
          i->dr_full = 0; i->star1 |= I2C_TXE;
          i2c_shift(i->dr, 0);
        } else {
          i->star1 |= I2C_BTF | I2C_TXE;
        }
      }
    } else if(kind == 3) {                          // STOP generated
      i2c_dev_stop();
      i->star1 &= 0x0F00 | I2C_RXNE | I2C_BTF;      // keep error flags and unread data
      i->star2 = 0;
      i->ctlr1 &= ~0x0200;
      i->stop_pending = 0; i->dr_full = 0; i->addr_pending_clear = 0; i->rx_more = 0;
      if(i->start_pending) i2c_event(1, 1);
    } else if(kind == 4) {                          // byte received
      i->shifting = 0;
      uint8_t v = i2c_dev_read();
      // ACK/NACK: ACK bit at the end of the byte, or at its start with POS (2-byte reads);
      // LAST NACKs the byte that completes the DMA transfer (channel 7 = I2C1_RX)
      int ack = (i->ctlr1 & I2C_POS) ? i->rx_ack_start : (i->ctlr1 & I2C_ACK) != 0;
      if((i->ctlr2 & I2C_LAST) && (i->ctlr2 & I2C_DMAEN) && (E.dma[6].cfgr & 1) &&
         E.dma[6].cntr <= 1 + ((i->star1 & I2C_RXNE) ? 1 : 0)) ack = 0;
      if(!(i->star1 & I2C_RXNE)) { i->dr = v; i->star1 |= I2C_RXNE; }
      else { i->rx_hold = 1; i->rx_hold_byte = v; i->star1 |= I2C_BTF; }   // SCL stretched
      i->rx_more = (uint8_t)ack;
      if(i->stop_pending) i2c_event(3, 1);
      else if(i->start_pending) i2c_event(1, 1);
      else if(ack && !i->rx_hold) i2c_rx_begin();
    }
    i2c_irq_update();
    dma_i2c_request();
  }
}

static uint32_t i2c_read(uint32_t off) {
  I2C_t *i = &E.i2c;
  i2c_service();
  switch(off) {
    case 0x00: return i->ctlr1;
    case 0x04: return i->ctlr2;
    case 0x08: return i->oaddr1;
    case 0x0C: return i->oaddr2;
    case 0x10: {
      uint8_t v = i->dr;
      if(!(i->star2 & I2C_TRA) && (i->star1 & I2C_RXNE)) {
        i->star1 &= ~I2C_RXNE;
        if(i->rx_hold) {                                     // shift register -> DR
          i->dr = i->rx_hold_byte; i->rx_hold = 0;
          i->star1 = (i->star1 & ~I2C_BTF) | I2C_RXNE;
          if(i->rx_more && (i->star2 & I2C_MSL) && !i->stop_pending && !i->start_pending &&
             !i->shifting && !i->ev_at)
            i2c_rx_begin();
        }
        i2c_irq_update();
      }
      return v;
    }
    case 0x14:
      if(i->star1 & I2C_ADDR) i->star1_read = 1;
      return i->star1;
    case 0x18: {
      uint32_t v = i->star2;
      if((i->star1 & I2C_ADDR) && i->star1_read) {          // ADDR cleared by SR1+SR2 read
        i->star1 &= ~I2C_ADDR; i->addr_pending_clear = 0; i->star1_read = 0;
        if(i->dr_full && (i->star2 & I2C_TRA)) {
          i->dr_full = 0; i->star1 |= I2C_TXE;
          i2c_shift(i->dr, 0);
        } else if(!(i->star2 & I2C_TRA) && !i->shifting) {
          i2c_rx_begin();                                    // receiver: start clocking
        }
        i2c_irq_update();
        dma_i2c_request();
      }
      return v;
    }
    case 0x1C: return i->ckcfgr;
  }
  return 0;
}

static void i2c_write(uint32_t off, uint32_t v) {
  I2C_t *i = &E.i2c;
  i2c_service();
  switch(off) {
    case 0x00:
      if(v & 0x8000) {                                       // SWRST
        memset(i, 0, sizeof(*i)); i->ctlr1 = 0x8000; break;
      }
      i->ctlr1 = (uint16_t)v;
      if(!(v & 1)) {                                         // PE = 0: reset state machine
        uint16_t c2 = i->ctlr2, ck = i->ckcfgr, o1 = i->oaddr1, o2 = i->oaddr2;
        memset(i, 0, sizeof(*i));
        i->ctlr1 = (uint16_t)v; i->ctlr2 = c2; i->ckcfgr = ck; i->oaddr1 = o1; i->oaddr2 = o2;
        break;
      }
      if((v & 0x0100) && !i->start_pending && i->ev_kind != 1) {   // START
        i->start_pending = 1;
        if(!(i->star2 & I2C_BUSY)) i2c_event(1, 1);
        else if((i->star2 & I2C_MSL) && !i->shifting && !i->stop_pending) i2c_event(1, 1);
      }
      if((v & 0x0200) && !i->stop_pending) {                 // STOP
        if(i->star2 & I2C_MSL) {
          i->stop_pending = 1;
          if(!i->shifting) i2c_event(3, 1);
        } else i->ctlr1 &= ~0x0200;
      }
      break;
    case 0x04: i->ctlr2 = (uint16_t)v; dma_i2c_request(); break;
    case 0x08: i->oaddr1 = (uint16_t)v; break;
    case 0x0C: i->oaddr2 = (uint16_t)v; break;
    case 0x10: i2c_write_dr((uint8_t)v); break;
    case 0x14: i->star1 &= (uint16_t)(v | 0x00FF); break;   // rc_w0 error bits
    case 0x1C: i->ckcfgr = (uint16_t)v; break;
  }
  i2c_irq_update();
}

// -----------------------------------------------------------------------------------
// Memory bus
// -----------------------------------------------------------------------------------
static int bus_fault;

static uint32_t misc_addr[128], misc_val[128], misc_n;

static const char *periph_name(uint32_t a) {
  if(a >= 0x40000000 && a < 0x40000400) return "TIM2";
  if(a >= 0x40002C00 && a < 0x40003000) return "WWDG";
  if(a >= 0x40012C00 && a < 0x40013000) return "TIM1";
  if(a >= 0x40013000 && a < 0x40013400) return "SPI1";
  if(a >= 0x40023800 && a < 0x40023C00) return "EXTEN/OPA";
  return "peripheral";
}

static uint32_t misc_rw(uint32_t a, uint32_t v, int write) {
  uint32_t blk = (a >> 10) & 0x1FF;
  if(!(E.warned[(blk >> 5) & 15] & (1u << (blk & 31)))) {
    E.warned[(blk >> 5) & 15] |= 1u << (blk & 31);
    log_str("[sim] unmodelled "); log_str(periph_name(a)); log_str(" access at ");
    log_hex(a); log_str(" pc="); log_hex(E.pc); log_str("\n");
  }
  for(uint32_t k = 0; k < misc_n; k++) if(misc_addr[k] == a) {
    if(write) misc_val[k] = v;
    return misc_val[k];
  }
  if(write && misc_n < 128) { misc_addr[misc_n] = a; misc_val[misc_n++] = v; }
  return 0;
}

// ---- DMA ----------------------------------------------------------------------------
static uint32_t bus_read(uint32_t a, int size);
static void     bus_write(uint32_t a, uint32_t v, int size);

static void dma_irq_update(void) {
  for(int c = 0; c < 7; c++) {
    uint32_t f = (E.dma_intfr >> (c * 4)) & 0xF, cfg = E.dma[c].cfgr;
    int lvl = ((f & 2) && (cfg & 2)) || ((f & 4) && (cfg & 4)) || ((f & 8) && (cfg & 8));
    irq_set_line(IRQ_DMA1_CH1 + c, lvl);
  }
}

// Perform one transfer on channel c. Returns 0 if channel is not active.
static int dma_transfer(int c) {
  DMACh *d = &E.dma[c];
  if(!(d->cfgr & 1) || d->cntr == 0) return 0;
  int psize = 1 << ((d->cfgr >> 8) & 3), msize = 1 << ((d->cfgr >> 10) & 3);
  if(psize > 4) psize = 4; if(msize > 4) msize = 4;
  if(d->cfgr & 0x10) {                                       // memory -> peripheral
    uint32_t v = bus_read(d->mcur, msize);
    bus_write(d->pcur, v, psize);
  } else {                                                   // peripheral -> memory
    uint32_t v = bus_read(d->pcur, psize);
    bus_write(d->mcur, v, msize);
  }
  if(d->cfgr & 0x40) d->pcur += psize;
  if(d->cfgr & 0x80) d->mcur += msize;
  d->cntr--;
  uint32_t sh = c * 4;
  if(d->cntr == d->cnt_reload / 2) E.dma_intfr |= (4u | 1u) << sh;
  if(d->cntr == 0) {
    E.dma_intfr |= (2u | 1u) << sh;
    if(d->cfgr & 0x20) { d->cntr = d->cnt_reload; d->pcur = d->paddr; d->mcur = d->maddr; }
  }
  dma_irq_update();
  return 1;
}

static int dma_busy;
static void dma_i2c_request(void) {
  I2C_t *i = &E.i2c;
  if(dma_busy) return;
  dma_busy = 1;
  while((i->ctlr2 & 0x0800) && (i->star1 & I2C_TXE) && (i->star2 & I2C_TRA) &&
        !i->addr_pending_clear && !i->stop_pending && !i->dr_full) {
    if(!dma_transfer(5)) break;                              // channel 6 = I2C1_TX
  }
  while((i->ctlr2 & 0x0800) && (i->star1 & I2C_RXNE) && !(i->star2 & I2C_TRA)) {
    if(!dma_transfer(6)) break;                              // channel 7 = I2C1_RX
  }
  dma_busy = 0;
}

static void dma_write(uint32_t off, uint32_t v) {
  if(off == 0x04) {
    for(int c = 0; c < 7; c++) if(v & (1u << (c * 4))) v |= 0xFu << (c * 4);
    E.dma_intfr &= ~v; dma_irq_update(); return;
  }
  if(off < 0x08 || off >= 0x08 + 7 * 0x14) return;
  int c = (off - 0x08) / 0x14, r = (off - 0x08) % 0x14;
  DMACh *d = &E.dma[c];
  switch(r) {
    case 0x00: {
      uint32_t was = d->cfgr & 1;
      d->cfgr = v & 0x7FFF;
      if(!was && (v & 1)) {
        d->pcur = d->paddr; d->mcur = d->maddr; d->cnt_reload = d->cntr;
        if(v & 0x4000) { while(dma_transfer(c)) E.cycles += 2; }      // MEM2MEM
        else if(c == 5 || c == 6) dma_i2c_request();
      }
      dma_irq_update();
      break;
    }
    case 0x04: d->cntr = v & 0xFFFF; break;
    case 0x08: d->paddr = v; break;
    case 0x0C: d->maddr = v; break;
  }
}

static uint32_t dma_read(uint32_t off) {
  if(off == 0x00) return E.dma_intfr;
  if(off < 0x08 || off >= 0x08 + 7 * 0x14) return 0;
  int c = (off - 0x08) / 0x14, r = (off - 0x08) % 0x14;
  DMACh *d = &E.dma[c];
  switch(r) {
    case 0x00: return d->cfgr;
    case 0x04: return d->cntr;
    case 0x08: return d->paddr;
    case 0x0C: return d->maddr;
  }
  return 0;
}

// ---- ADC ----------------------------------------------------------------------------
static const uint16_t ADC_SMP[8] = {3, 9, 15, 30, 43, 57, 73, 241};

static uint32_t adc_clock_div(void) {
  uint32_t pre = (E.sys.rcc_cfgr0 >> 11) & 0x1F;
  static const uint8_t tab[4] = {2, 4, 6, 8};
  if(pre & 1) return 4u << ((pre >> 1) & 3);    // 0bxxxx1: /4 /8 /16 /32 (approx.)
  return tab[(pre >> 3) & 3];
}

static double adc_channel_ratio(uint32_t ch) {
  switch(ch) {
    case 0: return pa2_ratio();                                     // PA2 (ACT, B)
    case 1: return pin_level(0, 1) ? 1.0 : 0.0;                     // PA1 (BEEP)
    case 2: return pad_ratio();                                     // PC4 (PAD)
    case 8: return 1200.0 / (double)E.vcc_mv;                       // Vref 1.2 V
    case 9: return 0.5;                                             // Vcal
    default: return 0.0;
  }
}

static void adc_start(void) {
  uint32_t ch = E.adc.rsqr3 & 0x1F;
  uint32_t smp = ch < 10 ? (E.adc.samptr2 >> (ch * 3)) & 7 : 0;
  uint32_t adc_cycles = ADC_SMP[smp] + 11;
  E.adc.conv_at = E.cycles + (uint64_t)adc_cycles * adc_clock_div();
  E.adc.statr |= 0x10;                                              // STRT
  sched();
}

static void adc_service(void) {
  ADC_t *a = &E.adc;
  if(a->cal_at && E.cycles >= a->cal_at) { a->cal_at = 0; a->ctlr2 &= ~(0x4u | 0x8u); }
  if(a->conv_at && E.cycles >= a->conv_at) {
    a->conv_at = 0;
    uint32_t ch = a->rsqr3 & 0x1F;
    double r = adc_channel_ratio(ch);
    int32_t v = (int32_t)(r * 1023.0);
    if(E.adc_noise) v += (int32_t)(rng_next() % (2 * E.adc_noise + 1)) - (int32_t)E.adc_noise;
    if(v < 0) v = 0; if(v > 1023) v = 1023;
    a->rdatar = (a->ctlr2 & 0x800) ? ((uint32_t)v << 6) : (uint32_t)v;   // ALIGN left
    a->statr |= 0x02;                                               // EOC
    if(a->ctlr2 & 0x100) dma_transfer(0);                           // DMA -> channel 1
    if(a->ctlr2 & 0x2) adc_start();                                 // continuous
  }
  irq_set_line(IRQ_ADC, (a->statr & 0x02) && (a->ctlr1 & 0x20));
}

static uint32_t adc_read(uint32_t off) {
  ADC_t *a = &E.adc;
  adc_service();
  switch(off) {
    case 0x00: return a->statr;
    case 0x04: return a->ctlr1;
    case 0x08: return a->ctlr2;
    case 0x0C: return a->samptr1;
    case 0x10: return a->samptr2;
    case 0x14: case 0x18: case 0x1C: case 0x20: return a->iofr[(off - 0x14) / 4];
    case 0x24: return a->wdhtr;
    case 0x28: return a->wdltr;
    case 0x2C: return a->rsqr1;
    case 0x30: return a->rsqr2;
    case 0x34: return a->rsqr3;
    case 0x38: return a->isqr;
    case 0x3C: case 0x40: case 0x44: case 0x48: return a->idatar[(off - 0x3C) / 4];
    case 0x4C: a->statr &= ~0x02u; adc_service(); return a->rdatar;
    case 0x50: return a->dlyr;
  }
  return 0;
}

static void adc_write(uint32_t off, uint32_t v) {
  ADC_t *a = &E.adc;
  adc_service();
  switch(off) {
    case 0x00: a->statr &= v; break;
    case 0x04: a->ctlr1 = v; break;
    case 0x08: {
      uint32_t on = a->ctlr2 & 1;
      a->ctlr2 = v & ~(0x400000u | 0x200000u);                      // SWSTART/JSWSTART self-clear
      if(on && (v & 1)) {
        if(v & 0x0C) { a->cal_at = E.cycles + 40ull * adc_clock_div(); sched(); }
        if(v & 0x400000) adc_start();
      }
      break;
    }
    case 0x0C: a->samptr1 = v; break;
    case 0x10: a->samptr2 = v; break;
    case 0x14: case 0x18: case 0x1C: case 0x20: a->iofr[(off - 0x14) / 4] = v; break;
    case 0x24: a->wdhtr = v; break;
    case 0x28: a->wdltr = v; break;
    case 0x2C: a->rsqr1 = v; break;
    case 0x30: a->rsqr2 = v; break;
    case 0x34: a->rsqr3 = v; break;
    case 0x38: a->isqr = v; break;
    case 0x50: a->dlyr = v; break;
  }
  adc_service();
}

// ---- RCC / FLASH ------------------------------------------------------------------
static uint32_t rcc_read(uint32_t off) {
  SysRegs *s = &E.sys;
  switch(off) {
    case 0x00: {
      uint32_t v = s->rcc_ctlr & ~(0x2u | 0x20000u | 0x2000000u);
      if(v & 0x1) v |= 0x2;                    // HSIRDY
      if(v & 0x1000000) v |= 0x2000000;        // PLLRDY  (no HSE crystal on this board)
      return v;
    }
    case 0x04: return (s->rcc_cfgr0 & ~0xCu) | ((s->rcc_cfgr0 & 3) << 2);   // SWS = SW
    case 0x08: return s->rcc_intr;
    case 0x0C: return s->rcc_apb2prstr;
    case 0x10: return s->rcc_apb1prstr;
    case 0x14: return s->rcc_ahbpcenr;
    case 0x18: return s->rcc_apb2pcenr;
    case 0x1C: return s->rcc_apb1pcenr;
    case 0x24: return s->rcc_rstsckr | ((s->rcc_rstsckr & 1) ? 2 : 0);
  }
  return 0;
}

static void rcc_write(uint32_t off, uint32_t v) {
  SysRegs *s = &E.sys;
  switch(off) {
    case 0x00: s->rcc_ctlr = v; break;
    case 0x04: {
      uint32_t sw = v & 3;
      if(sw == 2 && !(s->rcc_ctlr & 0x1000000)) sw = s->rcc_cfgr0 & 3;  // PLL not running
      if(sw == 1) sw = s->rcc_cfgr0 & 3;                                // HSE not fitted
      s->rcc_cfgr0 = (v & ~3u) | sw;
      break;
    }
    case 0x08: s->rcc_intr = v & 0x1F00; break;
    case 0x0C:                                                       // APB2 peripheral reset
      s->rcc_apb2prstr = v;
      if(v & 1) E.afio_pcfr1 = E.afio_exticr = 0;
      for(int i = 0; i < 3; i++)                                     // IOPA, IOPC, IOPD
        if(v & (1u << (i ? i + 3 : 2))) { E.gpio[i].cfglr = 0x44444444; E.gpio[i].outdr = 0; E.gpio[i].lckr = 0; }
      buzzer_update();                                               // PA1 back to input: buzzer off
      if(v & (1u << 9)) memset(&E.adc, 0, sizeof(E.adc));
      if(v & (1u << 14)) memset(&E.usart, 0, sizeof(E.usart));
      break;
    case 0x10:                                                       // APB1 peripheral reset
      s->rcc_apb1prstr = v;
      if(v & (1u << 21)) memset(&E.i2c, 0, sizeof(E.i2c));
      break;
    case 0x14: s->rcc_ahbpcenr = v; break;
    case 0x18: s->rcc_apb2pcenr = v; break;
    case 0x1C: s->rcc_apb1pcenr = v; break;
    case 0x24:
      if(v & (1u << 24)) s->rcc_rstsckr &= 0x00FFFFFF;               // RMVF
      s->rcc_rstsckr = (s->rcc_rstsckr & 0xFF000000) | (v & 0x1);
      break;
  }
  clk_update();
}

// Fast (64-byte page) erase and programming, as used by WCH's FLASH_ROM_ERASE/WRITE:
// KEYR + MODEKEYR unlock (CTLR LOCK bit 7, FLOCK bit 15), FTER (bit 17) + STRT erases
// the page at ADDR, FTPG (bit 16) + BUFRST (bit 19) clears the page buffer, a store to
// the flash address + BUFLOAD (bit 18) latches one word, FTPG + STRT programs the page.
// Erase and program each keep BSY set for FLASH_PAGE_US. Erased flash reads 0xE339E339
// on the CH32V003, not 0xFF, so firmware that tests for 0xFF fails here as it would on chip.
#define FLASH_PAGE_US  3000
#define FLASH_ERASED   0xE339E339u
static void dcache_range(uint32_t a0, uint32_t a1);
static uint32_t flash_latch_addr, flash_latch_val;   // last store to flash (BUFLOAD source)

static int flash_busy(void) { return E.cycles < E.sys.fbusy_until; }

uint32_t flash_page_erases[FLASH_SIZE / 64];   // wear statistics (not part of the saved state)

static void flash_start_op(void) {
  E.sys.fbusy_until = E.cycles + (uint64_t)E.hclk / 1000u * FLASH_PAGE_US / 1000u;
  E.sys.flash_ops++;
}

static uint32_t flash_read(uint32_t off) {
  switch(off) {
    case 0x00: return E.sys.actlr;
    case 0x0C: return (E.sys.statr & ~1u) | (flash_busy() ? 1 : 0);
    case 0x10: return E.sys.ctlr;
    case 0x14: return E.sys.faddr;
    case 0x1C: return E.sys.obr;
  }
  return 0;
}

static void flash_write(uint32_t off, uint32_t v) {
  SysRegs *s = &E.sys;
  switch(off) {
    case 0x00: s->actlr = v & 3; break;
    case 0x04:                                               // KEYR
      if(v == 0x45670123) s->keyr_state = (s->keyr_state & ~3u) | 1;
      else if(v == 0xCDEF89AB && (s->keyr_state & 3) == 1) { s->keyr_state &= ~3u; s->ctlr &= ~0x80u; }
      else s->keyr_state &= ~3u;
      break;
    case 0x0C: s->statr &= ~(v & 0x30); break;               // EOP / WRPRTERR: write 1 to clear
    case 0x14: s->faddr = v; break;
    case 0x24:                                               // MODEKEYR
      if(v == 0x45670123) s->keyr_state = (s->keyr_state & ~0xCu) | 4;
      else if(v == 0xCDEF89AB && (s->keyr_state & 0xC) == 4 && !(s->ctlr & 0x80)) { s->keyr_state &= ~0xCu; s->ctlr &= ~0x8000u; }
      else s->keyr_state &= ~0xCu;
      break;
    case 0x10: {
      uint32_t lockbits = s->ctlr & 0x8080;
      v = (v & ~0x8080u) | lockbits | (v & 0x8080);          // LOCK/FLOCK: only software can set them
      if(v & 0x80) v |= 0x8000;
      s->ctlr = v & ~0x40u;                                  // STRT is self-clearing
      int fast_ok = !(s->ctlr & 0x8080) && !flash_busy();
      if(!fast_ok) break;
      if((v & (1u << 19)) && (v & (1u << 16))) {             // BUFRST
        for(int i = 0; i < 16; i++) s->fbuf[i] = 0xFFFFFFFF;
        s->fbuf_mask = 0; s->ctlr &= ~(1u << 19);
      }
      if((v & (1u << 18)) && (v & (1u << 16))) {             // BUFLOAD: the last flash store
        s->fbuf[(flash_latch_addr >> 2) & 15] = flash_latch_val;
        s->fbuf_mask |= 1u << ((flash_latch_addr >> 2) & 15);
        s->ctlr &= ~(1u << 18);
      }
      if(v & 0x40) {
        uint32_t page = (s->faddr & 0x00FFFFFF) & ~63u;      // 0x0800xxxx or 0x0000xxxx
        if(page >= FLASH_SIZE) { s->statr |= 0x10; break; }
        if(v & (1u << 17)) {                                 // FTER: 64-byte page erase
          flash_page_erases[page >> 6]++;
          for(int i = 0; i < 64; i += 4) memcpy(E.flash + page + i, &(uint32_t){FLASH_ERASED}, 4);
        } else if(v & (1u << 16)) {                          // FTPG: program the page buffer
          for(int i = 0; i < 16; i++) if(s->fbuf_mask & (1u << i)) memcpy(E.flash + page + i * 4, &s->fbuf[i], 4);
        } else break;
        flash_start_op();
        s->statr |= 0x20;                                    // EOP
        dcache_range(page, page + 64);
      }
      break;
    }
  }
}

// ---- GPIO -------------------------------------------------------------------------
static uint32_t gpio_read(int p, uint32_t off) {
  GPIOPort *g = &E.gpio[p];
  switch(off) {
    case 0x00: return g->cfglr;
    case 0x08: return gpio_indr(p);
    case 0x0C: return g->outdr;
    case 0x18: return g->lckr;
  }
  return 0;
}

static void gpio_write(int p, uint32_t off, uint32_t v) {
  GPIOPort *g = &E.gpio[p];
  uint32_t before = exti_levels();
  switch(off) {
    case 0x00: g->cfglr = v; break;
    case 0x0C: g->outdr = v & 0xFF; break;
    case 0x10: g->outdr = (g->outdr | (v & 0xFF)) & ~((v >> 16) & 0xFF); break;
    case 0x14: g->outdr &= ~(v & 0xFF); break;
    case 0x18: g->lckr = v; break;
  }
  if(p == 0) buzzer_update();
  exti_edges(before, exti_levels());
}

// ---- PFIC -------------------------------------------------------------------------
static uint32_t pfic_read(uint32_t off) {
  if(off < 0x20) return off < 8 ? (uint32_t)(E.irq_enabled >> (off * 8)) : 0;
  if(off < 0x40) { off -= 0x20; return off < 8 ? (uint32_t)(irq_pending_all() >> (off * 8)) : 0; }
  if(off == 0x40) return E.pfic_ithresdr;
  if(off == 0x4C) return (E.irq_active ? 1u : 0u) << 8;
  if(off >= 0x300 && off < 0x320) { off -= 0x300; return off < 8 ? (uint32_t)(E.irq_active >> (off * 8)) : 0; }
  if(off >= 0x400 && off < 0x440) {
    uint32_t i = off - 0x400;
    return E.iprior[i] | (E.iprior[i+1] << 8) | (E.iprior[i+2] << 16) | ((uint32_t)E.iprior[i+3] << 24);
  }
  if(off == 0xD10) return E.pfic_sctlr;
  return 0;
}

static int reset_pending;

static void pfic_write(uint32_t off, uint32_t v) {
  uint32_t w = (off & 0x1F) / 4;
  uint64_t m = w < 2 ? ((uint64_t)v << (w * 32)) : 0;
  if(off >= 0x100 && off < 0x120) { E.irq_enabled |= m; return; }
  if(off >= 0x180 && off < 0x1A0) { E.irq_enabled &= ~m; return; }
  if(off >= 0x200 && off < 0x220) { E.irq_pending |= m; return; }
  if(off >= 0x280 && off < 0x2A0) { E.irq_pending &= ~m; return; }
  if(off == 0x40) { E.pfic_ithresdr = v; return; }
  if(off == 0x48) {
    if((v >> 16) == 0xBEEF && (v & 0x80)) reset_pending = 1;      // software reset, after
                                                                    // the current instruction
    return;
  }
  if(off >= 0x400 && off < 0x440) {
    uint32_t i = off - 0x400;
    for(int k = 0; k < 4; k++) E.iprior[i + k] = (uint8_t)(v >> (k * 8));
    return;
  }
  if(off == 0xD10) {
    E.pfic_sctlr = v;
    if(v & 0x20) event_flag = 1;                                      // SETEVENT
    return;
  }
}

// ---- USART1 (transmit to console) ---------------------------------------------------
static uint32_t usart_read(uint32_t off) {
  switch(off) {
    case 0x00: return 0xC0;                    // TXE | TC
    case 0x08: return E.usart.brr;
    case 0x0C: return E.usart.ctlr1;
    case 0x10: return E.usart.ctlr2;
  }
  return 0;
}
static void usart_write(uint32_t off, uint32_t v) {
  switch(off) {
    case 0x04:
      if(E.uart_len < UART_BUF) E.uart[E.uart_len++] = (char)v;
      break;
    case 0x08: E.usart.brr = v; break;
    case 0x0C: E.usart.ctlr1 = v; break;
    case 0x10: E.usart.ctlr2 = v; break;
  }
}

// ---- IWDG ---------------------------------------------------------------------------
static void iwdg_reload(void) {
  uint32_t presc = 4u << (E.iwdg_pr & 7); if(presc > 256) presc = 256;
  uint64_t lsi_ticks = (uint64_t)((E.iwdg_rlr & 0xFFF) + 1) * presc;
  E.iwdg_deadline = E.cycles + lsi_ticks * E.hclk / LSI_HZ;
  sched();
}
static void iwdg_write(uint32_t off, uint32_t v) {
  switch(off) {
    case 0x00:
      v &= 0xFFFF;
      if(v == 0x5555) E.iwdg_unlocked = 1;
      else if(v == 0xAAAA) { E.iwdg_unlocked = 0; if(E.iwdg_running) iwdg_reload(); }
      else if(v == 0xCCCC) { E.iwdg_running = 1; iwdg_reload(); }
      break;
    case 0x04: if(E.iwdg_unlocked) E.iwdg_pr = v & 7; break;
    case 0x08: if(E.iwdg_unlocked) E.iwdg_rlr = v & 0xFFF; break;
  }
}

// ---- PWR / AWU ---------------------------------------------------------------------
static void awu_schedule(void) {
  awu_at = 0;
  if(!(awu_csr & 2) || !awu_psc) return;
  uint32_t presc = awu_psc < 14 ? 1u << (awu_psc - 1) : (awu_psc == 14 ? 10240 : 61440);
  uint64_t ticks = (uint64_t)((awu_wr & 0x3F) ? (awu_wr & 0x3F) : 1) * presc;
  awu_at = E.cycles + ticks * E.hclk / LSI_HZ;
  sched();
}
static uint32_t pwr_read(uint32_t off) {
  switch(off) {
    case 0x00: return pwr_ctlr;
    case 0x04: return 0;
    case 0x08: return awu_csr;
    case 0x0C: return awu_wr;
    case 0x10: return awu_psc;
  }
  return 0;
}
static void pwr_write(uint32_t off, uint32_t v) {
  switch(off) {
    case 0x00: pwr_ctlr = v; break;
    case 0x08: awu_csr = v & 2; awu_schedule(); break;
    case 0x0C: awu_wr = v & 0x3F; awu_schedule(); break;
    case 0x10: awu_psc = v & 0xF; awu_schedule(); break;
  }
}

// ---- EXTI / AFIO ---------------------------------------------------------------------
static uint32_t exti_read(uint32_t off) {
  switch(off) {
    case 0x00: return E.exti_intenr;
    case 0x04: return E.exti_evenr;
    case 0x08: return E.exti_rtenr;
    case 0x0C: return E.exti_ftenr;
    case 0x14: return E.exti_intfr;
  }
  return 0;
}
static void exti_write(uint32_t off, uint32_t v) {
  switch(off) {
    case 0x00: E.exti_intenr = v & 0x3FF; break;
    case 0x04: E.exti_evenr = v & 0x3FF; break;
    case 0x08: E.exti_rtenr = v & 0x3FF; break;
    case 0x0C: E.exti_ftenr = v & 0x3FF; break;
    case 0x10: exti_trigger(v & 0x3FF); break;
    case 0x14: E.exti_intfr &= ~v; break;
  }
  irq_set_line(IRQ_EXTI, (E.exti_intfr & 0xFF) != 0);
  irq_set_line(IRQ_AWU, (E.exti_intfr >> 9) & 1);
}

// ---- dispatch ---------------------------------------------------------------------
static uint32_t *tim1_reg(uint32_t off) {
  switch(off) {
    case 0x00: return &E.tim1.ctlr1;  case 0x18: return &E.tim1.chctlr1; case 0x20: return &E.tim1.ccer;
    case 0x28: return &E.tim1.psc;    case 0x2C: return &E.tim1.atrlr;   case 0x38: return &E.tim1.ch2cvr;
    case 0x44: return &E.tim1.bdtr;
  }
  return 0;
}

static uint32_t periph_read(uint32_t a) {
  if(a >= 0x40012C00 && a < 0x40013000) { uint32_t *r = tim1_reg(a - 0x40012C00); return r ? *r : 0; }
  if(a >= 0x40021000 && a < 0x40021400) return rcc_read(a - 0x40021000);
  if(a >= 0x40022000 && a < 0x40022400) return flash_read(a - 0x40022000);
  if(a >= 0x40010800 && a < 0x40010C00) return gpio_read(0, a - 0x40010800);
  if(a >= 0x40011000 && a < 0x40011400) return gpio_read(1, a - 0x40011000);
  if(a >= 0x40011400 && a < 0x40011800) return gpio_read(2, a - 0x40011400);
  if(a >= 0x40005400 && a < 0x40005800) return i2c_read(a - 0x40005400);
  if(a >= 0x40012400 && a < 0x40012800) return adc_read(a - 0x40012400);
  if(a >= 0x40020000 && a < 0x40020400) return dma_read(a - 0x40020000);
  if(a >= 0x40013800 && a < 0x40013C00) return usart_read(a - 0x40013800);
  if(a >= 0x40010000 && a < 0x40010400) {
    uint32_t o = a - 0x40010000;
    return o == 4 ? E.afio_pcfr1 : o == 8 ? E.afio_exticr : 0;
  }
  if(a >= 0x40010400 && a < 0x40010800) return exti_read(a - 0x40010400);
  if(a >= 0x40007000 && a < 0x40007400) return pwr_read(a - 0x40007000);
  if(a >= 0x40003000 && a < 0x40003400) {
    uint32_t o = a - 0x40003000;
    return o == 4 ? E.iwdg_pr : o == 8 ? E.iwdg_rlr : 0;
  }
  return misc_rw(a, 0, 0);
}

static void periph_write(uint32_t a, uint32_t v) {
  if(a >= 0x40012C00 && a < 0x40013000) {
    uint32_t *r = tim1_reg(a - 0x40012C00);
    if(r) { *r = v & 0xFFFF; buzzer_update(); }
    return;
  }
  if(a >= 0x40021000 && a < 0x40021400) { rcc_write(a - 0x40021000, v); return; }
  if(a >= 0x40022000 && a < 0x40022400) { flash_write(a - 0x40022000, v); return; }
  if(a >= 0x40010800 && a < 0x40010C00) { gpio_write(0, a - 0x40010800, v); return; }
  if(a >= 0x40011000 && a < 0x40011400) { gpio_write(1, a - 0x40011000, v); return; }
  if(a >= 0x40011400 && a < 0x40011800) { gpio_write(2, a - 0x40011400, v); return; }
  if(a >= 0x40005400 && a < 0x40005800) { i2c_write(a - 0x40005400, v); return; }
  if(a >= 0x40012400 && a < 0x40012800) { adc_write(a - 0x40012400, v); return; }
  if(a >= 0x40020000 && a < 0x40020400) { dma_write(a - 0x40020000, v); return; }
  if(a >= 0x40013800 && a < 0x40013C00) { usart_write(a - 0x40013800, v); return; }
  if(a >= 0x40010000 && a < 0x40010400) {
    uint32_t o = a - 0x40010000;
    if(o == 4) E.afio_pcfr1 = v; else if(o == 8) E.afio_exticr = v;
    return;
  }
  if(a >= 0x40010400 && a < 0x40010800) { exti_write(a - 0x40010400, v); return; }
  if(a >= 0x40007000 && a < 0x40007400) { pwr_write(a - 0x40007000, v); return; }
  if(a >= 0x40003000 && a < 0x40003400) { iwdg_write(a - 0x40003000, v); return; }
  misc_rw(a, v, 1);
}

static uint32_t sys_read(uint32_t a) {
  if(a == 0x1FFFF7C4) return 0x00330500;                   // CHIPID: CH32V003J4M6 (0x0033xx00)
  if(a >= 0x1FFFF7E0 && a < 0x1FFFF7F4) {                  // ESIG
    if(a == 0x1FFFF7E0) return 16;                         // flash size (KB)
    if(a == 0x1FFFF7E8) return 0xCD47ABCD;
    if(a == 0x1FFFF7EC) return 0x3B2C1A0B;
    if(a == 0x1FFFF7F0) return 0xFFFFFFFF;
    return 0;
  }
  if(a >= 0x1FFFF800 && a < 0x1FFFF840) {                  // option bytes
    if(a == 0x1FFFF800) return 0xE0175AA5;                 // RDPR/USER defaults
    return 0xFFFFFFFF;
  }
  if(a >= 0xE000E000 && a < 0xE000F000) return pfic_read(a - 0xE000E000);
  if(a >= 0xE000F000 && a < 0xE000F100) return stk_read(a - 0xE000F000);
  if(a == 0xE00000F4 || a == 0xE00000F8) return 0;         // DMDATA (debug)
  if(a >= 0x1FFFF000 && a < 0x1FFFF7C0) return 0;          // system flash (bootloader)
  bus_fault = 1;
  return 0;
}

static void sys_write(uint32_t a, uint32_t v) {
  if(a >= 0xE000E000 && a < 0xE000F000) { pfic_write(a - 0xE000E000, v); return; }
  if(a >= 0xE000F000 && a < 0xE000F100) { stk_write(a - 0xE000F000, v); return; }
  if(a == 0xE00000F4 || a == 0xE00000F8) return;
  bus_fault = 1;
}

static int last_mmio;   // set when the last access went to a peripheral

static uint32_t bus_read(uint32_t a, int size) {
  uint32_t off;
  if((off = a - 0x20000000) < SRAM_SIZE) {
    if(off + size > SRAM_SIZE) { bus_fault = 1; return 0; }
    const uint8_t *p = E.sram + off;
    return size == 4 ? p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)
         : size == 2 ? (uint32_t)(p[0] | (p[1] << 8)) : p[0];
  }
  if((off = a) < FLASH_SIZE || (off = a - 0x08000000) < FLASH_SIZE) {
    if(off + size > FLASH_SIZE) { bus_fault = 1; return 0; }
    const uint8_t *p = E.flash + off;
    return size == 4 ? p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)
         : size == 2 ? (uint32_t)(p[0] | (p[1] << 8)) : p[0];
  }
  last_mmio = 1;
  uint32_t base = a & ~3u, sh = (a & 3) * 8, v;
  if(a >= 0x40000000 && a < 0x40024000) v = periph_read(base);
  else v = sys_read(base);
  v >>= sh;
  return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
}

static void bus_write(uint32_t a, uint32_t v, int size) {
  uint32_t off;
  if((off = a - 0x20000000) < SRAM_SIZE) {
    if(off + size > SRAM_SIZE) { bus_fault = 1; return; }
    uint8_t *p = E.sram + off;
    p[0] = (uint8_t)v;
    if(size >= 2) p[1] = (uint8_t)(v >> 8);
    if(size == 4) { p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
    return;
  }
  if(a < FLASH_SIZE || (a - 0x08000000) < FLASH_SIZE) {      // only as a page-buffer load
    if(size != 4 || (E.sys.ctlr & 0x8080) || !(E.sys.ctlr & (1u << 16))) { bus_fault = 1; return; }
    flash_latch_addr = a; flash_latch_val = v;
    return;
  }
  last_mmio = 1;
  uint32_t base = a & ~3u, sh = (a & 3) * 8;
  int periph = a >= 0x40000000 && a < 0x40024000;
  if(size < 4) {
    // narrow access to a 32-bit register: merge with the current contents.
    // I2C/USART data registers are skipped (reading them has side effects).
    uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << sh, cur = 0;
    if(periph) {
      if(!((base >= 0x40005400 && base < 0x40005800) || (base >= 0x40013800 && base < 0x40013C00)))
        cur = periph_read(base);
    } else cur = sys_read(base);
    v = (cur & ~mask) | ((v << sh) & mask);
  }
  if(periph) periph_write(base, v);
  else sys_write(base, v);
}

// -----------------------------------------------------------------------------------
// Scheduler
// -----------------------------------------------------------------------------------
static void sched(void) {
  uint64_t n = UINT64_MAX;
  if(E.i2c.ev_at && E.i2c.ev_at < n) n = E.i2c.ev_at;
  if(E.adc.conv_at && E.adc.conv_at < n) n = E.adc.conv_at;
  if(E.adc.cal_at && E.adc.cal_at < n) n = E.adc.cal_at;
  if(stk_match_at && stk_match_at < n) n = stk_match_at;
  if(E.iwdg_running && E.iwdg_deadline < n) n = E.iwdg_deadline;
  if(awu_at && awu_at < n) n = awu_at;
  E.next_event = n;
}

static void service_events(void) {
  i2c_service();
  stk_service();
  adc_service();
  if(awu_at && E.cycles >= awu_at) {
    exti_trigger(1u << 9);
    if(E.exti_rtenr & (1u << 9)) event_flag = 1;
    awu_schedule();
  }
  if(E.iwdg_running && E.cycles >= E.iwdg_deadline) {
    log_str("[sim] IWDG timeout -> reset\n");
    system_reset(1u << 29);
  }
  sched();
}

// -----------------------------------------------------------------------------------
// CPU: traps
// -----------------------------------------------------------------------------------
#define MSTATUS_MIE  0x8u
#define MSTATUS_MPIE 0x80u

static void hpe_push(void) {
  // QingKe V2 hardware prologue: caller-saved registers to the stack
  static const uint8_t regs[10] = {1, 5, 6, 7, 10, 11, 12, 13, 14, 15};
  uint32_t sp = E.x[2] - 48;
  for(int i = 0; i < 10; i++) bus_write(sp + i * 4, E.x[regs[i]], 4);
  E.x[2] = sp;
}
static void hpe_pop(void) {
  static const uint8_t regs[10] = {1, 5, 6, 7, 10, 11, 12, 13, 14, 15};
  uint32_t sp = E.x[2];
  for(int i = 0; i < 10; i++) E.x[regs[i]] = bus_read(sp + i * 4, 4);
  E.x[2] = sp + 48;
}

static uint32_t vector_target(uint32_t n) {
  uint32_t base = E.mtvec & ~3u;
  if(!(E.mtvec & 1)) return base;
  if(E.mtvec & 2) return bus_read(base + 4 * n, 4) & ~1u;     // table of absolute addresses
  return base + 4 * n;                                          // table of jump instructions
}

static void enter_trap(uint32_t cause, uint32_t epc, uint32_t vec) {
  E.mepc = epc;
  E.mcause = cause;
  E.mstatus = (E.mstatus & ~(MSTATUS_MPIE | MSTATUS_MIE)) |
              ((E.mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0) | 0x1800;
  int hpe = (E.intsyscr & 1) && (cause & 0x80000000u);
  E.hpe_depth = (E.hpe_depth << 1) | (hpe ? 1 : 0);
  if(hpe) hpe_push();
  E.pc = vector_target(vec);
  E.sleeping = 0;
}

static const char *CAUSE_NAMES[] = {
  "instruction address misaligned", "instruction access fault", "illegal instruction",
  "breakpoint", "load address misaligned", "load access fault",
  "store address misaligned", "store access fault", "ecall U", "ecall S", "-", "ecall M"
};

static void exception(uint32_t cause, uint32_t tval) {
  E.mtval = tval;
  if(E.fault_halt) {
    E.status = ST_FAULT;
    E.fault_pc = E.pc; E.fault_cause = cause; E.fault_tval = tval;
    log_str("[sim] CPU exception: "); log_str(cause < 12 ? CAUSE_NAMES[cause] : "?");
    log_str(" at pc="); log_hex(E.pc); log_str(" tval="); log_hex(tval); log_str("\n");
    return;
  }
  enter_trap(cause, E.pc, 3);                                  // HardFault vector
}

static int take_interrupt(void) {
  uint64_t p = irq_pending_all() & E.irq_enabled;
  if(!p) return 0;
  int best = -1; uint32_t bp = 256;
  for(int n = 2; n < 40; n++) if((p >> n) & 1) {
    if(E.iprior[n] < bp) { bp = E.iprior[n]; best = n; }
  }
  if(best < 0) return 0;
  E.irq_pending &= ~(1ull << best);
  E.irq_active |= 1ull << best;
  enter_trap(0x80000000u | (uint32_t)best, E.pc, (uint32_t)best);
  E.cycles += 4;                                               // interrupt latency
  return 1;
}

// -----------------------------------------------------------------------------------
// CPU: RV32C expansion
// -----------------------------------------------------------------------------------
#define ILLEGAL 0u

static inline uint32_t enc_i(uint32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline uint32_t enc_s(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  return (((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1F) << 7) | op;
}
static inline uint32_t enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline uint32_t enc_b(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
  return (((imm >> 12) & 1) << 31) | (((imm >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) |
         (f3 << 12) | (((imm >> 1) & 0xF) << 8) | (((imm >> 11) & 1) << 7) | 0x63;
}
static inline uint32_t enc_j(uint32_t imm, uint32_t rd) {
  return (((imm >> 20) & 1) << 31) | (((imm >> 1) & 0x3FF) << 21) | (((imm >> 11) & 1) << 20) |
         (((imm >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static uint32_t expand_c(uint32_t c) {
  uint32_t op = c & 3, f3 = (c >> 13) & 7;
  uint32_t rd = (c >> 7) & 31, rs2 = (c >> 2) & 31;
  uint32_t rdp = 8 + ((c >> 2) & 7), rs1p = 8 + ((c >> 7) & 7);
  if(op == 0) {
    switch(f3) {
      case 0: {                                                   // C.ADDI4SPN
        uint32_t imm = ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 3 | ((c >> 11) & 3) << 4 | ((c >> 7) & 15) << 6;
        if(!imm) return ILLEGAL;
        return enc_i(imm, 2, 0, rdp, 0x13);
      }
      case 2: {                                                   // C.LW
        uint32_t imm = ((c >> 6) & 1) << 2 | ((c >> 10) & 7) << 3 | ((c >> 5) & 1) << 6;
        return enc_i(imm, rs1p, 2, rdp, 0x03);
      }
      case 6: {                                                   // C.SW
        uint32_t imm = ((c >> 6) & 1) << 2 | ((c >> 10) & 7) << 3 | ((c >> 5) & 1) << 6;
        return enc_s(imm, rdp, rs1p, 2, 0x23);
      }
    }
    return ILLEGAL;
  }
  if(op == 1) {
    int32_t imm6 = sext(((c >> 12) & 1) << 5 | ((c >> 2) & 31), 6);
    switch(f3) {
      case 0: return enc_i((uint32_t)imm6, rd, 0, rd, 0x13);     // C.ADDI / C.NOP
      case 1: case 5: {                                           // C.JAL / C.J
        uint32_t imm = ((c >> 3) & 7) << 1 | ((c >> 11) & 1) << 4 | ((c >> 2) & 1) << 5 |
                       ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 | ((c >> 9) & 3) << 8 |
                       ((c >> 8) & 1) << 10 | ((c >> 12) & 1) << 11;
        return enc_j((uint32_t)sext(imm, 12), f3 == 1 ? 1 : 0);
      }
      case 2: return enc_i((uint32_t)imm6, 0, 0, rd, 0x13);      // C.LI
      case 3:
        if(rd == 2) {                                             // C.ADDI16SP
          uint32_t imm = ((c >> 6) & 1) << 4 | ((c >> 2) & 1) << 5 | ((c >> 5) & 1) << 6 |
                         ((c >> 3) & 3) << 7 | ((c >> 12) & 1) << 9;
          int32_t s = sext(imm, 10);
          if(!s) return ILLEGAL;
          return enc_i((uint32_t)s, 2, 0, 2, 0x13);
        } else {                                                  // C.LUI
          if(!imm6) return ILLEGAL;
          return ((uint32_t)imm6 << 12) | (rd << 7) | 0x37;
        }
      case 4: {
        uint32_t f2 = (c >> 10) & 3;
        uint32_t sh = ((c >> 12) & 1) << 5 | ((c >> 2) & 31);
        if(f2 == 0) { if(sh & 32) return ILLEGAL; return enc_i(sh, rs1p, 5, rs1p, 0x13); }           // C.SRLI
        if(f2 == 1) { if(sh & 32) return ILLEGAL; return enc_i(sh | 0x400, rs1p, 5, rs1p, 0x13); }   // C.SRAI
        if(f2 == 2) return enc_i((uint32_t)imm6, rs1p, 7, rs1p, 0x13);                               // C.ANDI
        if((c >> 12) & 1) return ILLEGAL;
        switch((c >> 5) & 3) {
          case 0: return enc_r(0x20, rdp, rs1p, 0, rs1p, 0x33);  // C.SUB
          case 1: return enc_r(0, rdp, rs1p, 4, rs1p, 0x33);     // C.XOR
          case 2: return enc_r(0, rdp, rs1p, 6, rs1p, 0x33);     // C.OR
          case 3: return enc_r(0, rdp, rs1p, 7, rs1p, 0x33);     // C.AND
        }
        return ILLEGAL;
      }
      case 6: case 7: {                                           // C.BEQZ / C.BNEZ
        uint32_t imm = ((c >> 3) & 3) << 1 | ((c >> 10) & 3) << 3 | ((c >> 2) & 1) << 5 |
                       ((c >> 5) & 3) << 6 | ((c >> 12) & 1) << 8;
        return enc_b((uint32_t)sext(imm, 9), 0, rs1p, f3 == 6 ? 0 : 1);
      }
    }
    return ILLEGAL;
  }
  if(op == 2) {
    switch(f3) {
      case 0: {                                                   // C.SLLI
        uint32_t sh = ((c >> 12) & 1) << 5 | rs2;
        if(sh & 32) return ILLEGAL;
        return enc_i(sh, rd, 1, rd, 0x13);
      }
      case 2: {                                                   // C.LWSP
        if(!rd) return ILLEGAL;
        uint32_t imm = ((c >> 4) & 7) << 2 | ((c >> 12) & 1) << 5 | ((c >> 2) & 3) << 6;
        return enc_i(imm, 2, 2, rd, 0x03);
      }
      case 4:
        if(!((c >> 12) & 1)) {
          if(!rs2) { if(!rd) return ILLEGAL; return enc_i(0, rd, 0, 0, 0x67); }    // C.JR
          return enc_r(0, rs2, 0, 0, rd, 0x33);                                    // C.MV
        } else {
          if(!rs2 && !rd) return 0x00100073;                                       // C.EBREAK
          if(!rs2) return enc_i(0, rd, 0, 1, 0x67);                                // C.JALR
          return enc_r(0, rs2, rd, 0, rd, 0x33);                                   // C.ADD
        }
      case 6: {                                                   // C.SWSP
        uint32_t imm = ((c >> 9) & 15) << 2 | ((c >> 7) & 3) << 6;
        return enc_s(imm, rs2, 2, 2, 0x23);
      }
    }
    return ILLEGAL;
  }
  return ILLEGAL;
}

static void dcache_build(void) { dcache_range(0, FLASH_SIZE); }

static void dcache_range(uint32_t a0, uint32_t a1) {
  a0 = a0 >= 4 ? (a0 - 4) & ~1u : 0;                        // a 32-bit instruction may straddle
  if(a1 > FLASH_SIZE) a1 = FLASH_SIZE;
  for(uint32_t i = a0 / 2; i < a1 / 2; i++) {
    uint32_t a = i * 2;
    uint32_t lo = E.flash[a] | (E.flash[a + 1] << 8);
    if((lo & 3) != 3) { dcache[i] = expand_c(lo); dlen[i] = 2; }
    else if(a + 3 < FLASH_SIZE) {
      dcache[i] = lo | ((uint32_t)(E.flash[a + 2] | (E.flash[a + 3] << 8)) << 16);
      dlen[i] = 4;
    } else { dcache[i] = ILLEGAL; dlen[i] = 4; }
  }
}

// -----------------------------------------------------------------------------------
// CPU: CSRs
// -----------------------------------------------------------------------------------
static int csr_rw(uint32_t csr, uint32_t *val, uint32_t wval, int write) {
  uint32_t v;
  switch(csr) {
    case 0x300: v = E.mstatus; break;
    case 0x301: v = 0x40000014; break;                       // misa: RV32, E, C
    case 0x305: v = E.mtvec; break;
    case 0x340: v = E.mscratch; break;
    case 0x341: v = E.mepc; break;
    case 0x342: v = E.mcause; break;
    case 0x343: v = E.mtval; break;
    case 0x800: v = E.mstatus & 0x88; break;                 // GINTENR
    case 0x804: v = E.intsyscr; break;                       // INTSYSCR
    case 0xBC0: v = E.corecfgr; break;
    case 0xF11: v = 0x00000612; break;                       // mvendorid (WCH)
    case 0xF12: v = 0xDC68D882; break;                       // marchid  (V2A)
    case 0xF13: v = 0; break;
    case 0xF14: v = 0; break;                                // mhartid
    case 0x7B0: case 0x7B1: case 0x7B2: case 0x7B3: v = 0; break;   // debug CSRs
    default: return 0;
  }
  *val = v;
  if(write) {
    switch(csr) {
      case 0x300: E.mstatus = (wval & 0x1888) | 0x1800; break;
      case 0x305: E.mtvec = wval; break;
      case 0x340: E.mscratch = wval; break;
      case 0x341: E.mepc = wval & ~1u; break;
      case 0x342: E.mcause = wval; break;
      case 0x343: E.mtval = wval; break;
      case 0x800: E.mstatus = (E.mstatus & ~0x88u) | (wval & 0x88); break;
      case 0x804: E.intsyscr = wval & 0xFF; break;
      case 0xBC0: E.corecfgr = wval; break;
      default: break;
    }
  }
  return 1;
}

// -----------------------------------------------------------------------------------
// CPU: execute one instruction. Returns cycles consumed.
// -----------------------------------------------------------------------------------
#define RD   ((ins >> 7) & 31)
#define RS1  ((ins >> 15) & 31)
#define RS2  ((ins >> 20) & 31)
#define F3   ((ins >> 12) & 7)
#define F7   (ins >> 25)

static inline uint32_t step(void) {
  uint32_t pc = E.pc, ins, len;
  uint32_t off = pc < FLASH_SIZE ? pc : pc - 0x08000000;
  int from_flash = 0;
  if(off < FLASH_SIZE && !(pc & 1)) {
    ins = dcache[off >> 1]; len = dlen[off >> 1];
    from_flash = 1;
  } else if((pc - 0x20000000) < SRAM_SIZE - 1 && !(pc & 1)) {
    uint32_t lo = bus_read(pc, 2);
    if((lo & 3) != 3) { ins = expand_c(lo); len = 2; }
    else {
      if((pc - 0x20000000) + 4 > SRAM_SIZE) { exception(1, pc); return 1; }
      ins = lo | (bus_read(pc + 2, 2) << 16); len = 4;
    }
  } else {
    exception((pc & 1) ? 0 : 1, pc);
    return 1;
  }

  uint32_t cyc = 1, npc = pc + len;
  uint32_t rd = RD, rs1 = RS1, rs2 = RS2;
  uint32_t *x = E.x;
  uint32_t lat = from_flash ? flash_latency() : 0;
  if(len == 4) cyc += lat;                         // 32-bit fetch needs a second flash cycle

  switch(ins & 0x7F) {
    case 0x37:                                     // LUI
      if(rd > 15) goto illegal;
      if(rd) x[rd] = ins & 0xFFFFF000;
      break;
    case 0x17:                                     // AUIPC
      if(rd > 15) goto illegal;
      if(rd) x[rd] = pc + (ins & 0xFFFFF000);
      break;
    case 0x6F: {                                   // JAL
      if(rd > 15) goto illegal;
      int32_t imm = sext(((ins >> 31) & 1) << 20 | ((ins >> 12) & 0xFF) << 12 |
                         ((ins >> 20) & 1) << 11 | ((ins >> 21) & 0x3FF) << 1, 21);
      if(rd) x[rd] = npc;
      npc = pc + (uint32_t)imm;
      cyc += E.t_jump - 1 + lat;
      break;
    }
    case 0x67: {                                   // JALR
      if(rd > 15 || rs1 > 15 || F3) goto illegal;
      uint32_t t = (x[rs1] + (uint32_t)sext(ins >> 20, 12)) & ~1u;
      if(rd) x[rd] = npc;
      npc = t;
      cyc += E.t_jump - 1 + lat;
      break;
    }
    case 0x63: {                                   // BRANCH
      if(rs1 > 15 || rs2 > 15) goto illegal;
      uint32_t a = x[rs1], b = x[rs2]; int t;
      switch(F3) {
        case 0: t = a == b; break;
        case 1: t = a != b; break;
        case 4: t = (int32_t)a < (int32_t)b; break;
        case 5: t = (int32_t)a >= (int32_t)b; break;
        case 6: t = a < b; break;
        case 7: t = a >= b; break;
        default: goto illegal;
      }
      if(t) {
        int32_t imm = sext(((ins >> 31) & 1) << 12 | ((ins >> 7) & 1) << 11 |
                           ((ins >> 25) & 0x3F) << 5 | ((ins >> 8) & 15) << 1, 13);
        npc = pc + (uint32_t)imm;
        cyc += E.t_branch - 1 + lat;
      }
      break;
    }
    case 0x03: {                                   // LOAD
      if(rd > 15 || rs1 > 15) goto illegal;
      uint32_t a = x[rs1] + (uint32_t)sext(ins >> 20, 12), v;
      int f3 = F3, size = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
      if(f3 == 3 || f3 > 5) goto illegal;
      if(a & (size - 1)) { exception(4, a); return cyc; }
      uint32_t so = a - 0x20000000;
      if(so < SRAM_SIZE - 3) {
        const uint8_t *p = E.sram + so;
        v = size == 4 ? p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)
          : size == 2 ? (uint32_t)(p[0] | (p[1] << 8)) : p[0];
      } else {
        last_mmio = 0; bus_fault = 0;
        v = bus_read(a, size);
        if(bus_fault) { exception(5, a); return cyc; }
        if(last_mmio) cyc += E.t_mmio;
        else if(a < FLASH_SIZE || (a - 0x08000000) < FLASH_SIZE) cyc += flash_latency();
      }
      if(f3 == 0) v = (uint32_t)(int32_t)(int8_t)v;
      else if(f3 == 1) v = (uint32_t)(int32_t)(int16_t)v;
      if(rd) x[rd] = v;
      cyc += E.t_load - 1;
      break;
    }
    case 0x23: {                                   // STORE
      if(rs1 > 15 || rs2 > 15) goto illegal;
      uint32_t imm = ((ins >> 25) << 5) | ((ins >> 7) & 31);
      uint32_t a = x[rs1] + (uint32_t)sext(imm, 12);
      int f3 = F3, size = f3 == 0 ? 1 : f3 == 1 ? 2 : 4;
      if(f3 > 2) goto illegal;
      if(a & (size - 1)) { exception(6, a); return cyc; }
      uint32_t so = a - 0x20000000;
      if(so < SRAM_SIZE - 3) {
        uint8_t *p = E.sram + so; uint32_t v = x[rs2];
        p[0] = (uint8_t)v;
        if(size >= 2) p[1] = (uint8_t)(v >> 8);
        if(size == 4) { p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
      } else {
        last_mmio = 0; bus_fault = 0;
        bus_write(a, x[rs2], size);
        if(bus_fault) { exception(7, a); return cyc; }
        if(last_mmio) cyc += E.t_mmio;
      }
      cyc += E.t_store - 1;
      break;
    }
    case 0x13: {                                   // OP-IMM
      if(rd > 15 || rs1 > 15) goto illegal;
      uint32_t a = x[rs1], imm = (uint32_t)sext(ins >> 20, 12), v;
      switch(F3) {
        case 0: v = a + imm; break;
        case 1: if(F7) goto illegal; v = a << (imm & 31); break;
        case 2: v = (int32_t)a < (int32_t)imm; break;
        case 3: v = a < imm; break;
        case 4: v = a ^ imm; break;
        case 5:
          if(F7 == 0) v = a >> (imm & 31);
          else if(F7 == 0x20) v = (uint32_t)((int32_t)a >> (imm & 31));
          else goto illegal;
          break;
        case 6: v = a | imm; break;
        default: v = a & imm; break;
      }
      if(rd) x[rd] = v;
      break;
    }
    case 0x33: {                                   // OP
      if(rd > 15 || rs1 > 15 || rs2 > 15) goto illegal;
      uint32_t a = x[rs1], b = x[rs2], v;
      if(F7 == 0) {
        switch(F3) {
          case 0: v = a + b; break;
          case 1: v = a << (b & 31); break;
          case 2: v = (int32_t)a < (int32_t)b; break;
          case 3: v = a < b; break;
          case 4: v = a ^ b; break;
          case 5: v = a >> (b & 31); break;
          case 6: v = a | b; break;
          default: v = a & b; break;
        }
      } else if(F7 == 0x20 && F3 == 0) v = a - b;
      else if(F7 == 0x20 && F3 == 5) v = (uint32_t)((int32_t)a >> (b & 31));
      else goto illegal;                           // includes M extension (not on CH32V003)
      if(rd) x[rd] = v;
      break;
    }
    case 0x0F: break;                              // FENCE / FENCE.I
    case 0x73: {                                   // SYSTEM
      uint32_t f3 = F3;
      if(f3 == 0) {
        if(ins == 0x00000073) { exception(11, 0); return cyc; }     // ECALL (M-mode)
        if(ins == 0x00100073) { exception(3, pc); return cyc; }     // EBREAK
        if(ins == 0x30200073) {                                     // MRET
          if(E.hpe_depth & 1) hpe_pop();
          E.hpe_depth >>= 1;
          if(E.mcause & 0x80000000u) E.irq_active &= ~(1ull << (E.mcause & 63));
          E.mstatus = (E.mstatus & ~MSTATUS_MIE) | ((E.mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0) | MSTATUS_MPIE;
          npc = E.mepc;
          cyc += 2;
          break;
        }
        if(ins == 0x10500073) {                                     // WFI
          E.pc = npc;
          int wfe = (E.pfic_sctlr & 0x8) != 0;
          if(wfe && event_flag) { event_flag = 0; return cyc; }
          E.sleeping = wfe ? 2 : 1;
          return cyc;
        }
        goto illegal;
      }
      if(f3 == 4) goto illegal;
      if(rd > 15) goto illegal;
      uint32_t src = (f3 & 4) ? rs1 : (rs1 > 15 ? 0xFFFFFFFF : x[rs1]);
      if(!(f3 & 4) && rs1 > 15) goto illegal;
      uint32_t csr = ins >> 20, old, nv;
      int wr = !((f3 & 3) != 1 && rs1 == 0);
      if(!csr_rw(csr, &old, 0, 0)) goto illegal;
      switch(f3 & 3) {
        case 1: nv = src; break;
        case 2: nv = old | src; break;
        default: nv = old & ~src; break;
      }
      if(wr) csr_rw(csr, &old, nv, 1);
      if(rd) x[rd] = old;
      break;
    }
    default:
      goto illegal;
  }
  E.pc = npc;
  return cyc;

illegal:
  exception(2, ins);
  return cyc;
}

// -----------------------------------------------------------------------------------
// Run loop
// -----------------------------------------------------------------------------------
uint32_t emu_run_cycles(uint64_t n) {
  uint64_t end = E.cycles + n;
  if(E.status != ST_OK) return E.status;
  while(E.cycles < end) {
    if(E.cycles >= E.next_event) service_events();
    uint64_t pend = irq_pending_all() & E.irq_enabled;
    if(E.sleeping) {
      int wake = E.sleeping == 1 ? pend != 0
                                 : (event_flag || ((E.pfic_sctlr & 0x10) && irq_pending_all()) || pend);
      if(!wake) {
        uint64_t t = E.next_event < end ? E.next_event : end;
        if(t > E.cycles) E.cycles = t;
        continue;
      }
      if(E.sleeping == 2) event_flag = 0;
      E.sleeping = 0;
    }
    if(pend && (E.mstatus & MSTATUS_MIE)) take_interrupt();
    E.cycles += step();
    E.instret++;
    if(reset_pending) { reset_pending = 0; system_reset(1u << 28); }  // SFTRSTF
    if(E.status != ST_OK) break;
  }
  audio_advance(E.cycles);
  return E.status;
}

uint32_t emu_run_us(uint32_t us) {
  return emu_run_cycles((uint64_t)us * E.hclk / 1000000u);
}

// -----------------------------------------------------------------------------------
// Buttons
// -----------------------------------------------------------------------------------
void emu_set_board(uint32_t rev) {
  E.board_rev = (uint8_t)rev;
  E.eeprom.present = rev >= 12;
}

void emu_set_buttons(uint32_t mask) {
  uint32_t before = exti_levels();
  E.buttons = mask;
  exti_edges(before, exti_levels());
}

// -----------------------------------------------------------------------------------
// Reset / init / load
// -----------------------------------------------------------------------------------
static void system_reset(uint32_t flags) {
  uint32_t keep_rst = E.sys.rcc_rstsckr & 0xFF000000, flash_ops = E.sys.flash_ops;
  // CPU
  memset(E.x, 0, sizeof(E.x));
  E.pc = 0; E.mstatus = 0x1800; E.mtvec = 0; E.mepc = 0; E.mcause = 0; E.mtval = 0;
  E.intsyscr = 0; E.hpe_depth = 0; E.sleeping = 0; E.status = ST_OK;
  // clock: HSI, HPRE = /3 -> 8 MHz after reset
  memset(&E.sys, 0, sizeof(E.sys));
  E.sys.rcc_ctlr = 0x00000083;
  E.sys.rcc_cfgr0 = 0x00000020;
  E.sys.rcc_ahbpcenr = 0x14;
  E.sys.rcc_rstsckr = keep_rst | flags;
  E.sys.actlr = 0;
  E.sys.ctlr = 0x8080;                                       // LOCK | FLOCK
  E.sys.flash_ops = flash_ops;
  E.hclk = 0;
  // peripherals
  for(int i = 0; i < 3; i++) { E.gpio[i].cfglr = 0x44444444; E.gpio[i].outdr = 0; E.gpio[i].lckr = 0; }
  E.afio_pcfr1 = E.afio_exticr = 0;
  memset(&E.tim1, 0, sizeof(E.tim1)); E.tim1.atrlr = 0xFFFF;
  E.exti_intenr = E.exti_evenr = E.exti_rtenr = E.exti_ftenr = E.exti_intfr = 0;
  memset(&E.stk, 0, sizeof(E.stk)); E.stk.t0 = E.cycles; stk_match_at = 0;
  memset(&E.i2c, 0, sizeof(E.i2c));
  memset(&E.adc, 0, sizeof(E.adc));
  memset(E.dma, 0, sizeof(E.dma)); E.dma_intfr = 0;
  memset(&E.usart, 0, sizeof(E.usart));
  E.irq_enabled = E.irq_pending = E.irq_active = 0; irq_line = 0;
  memset(E.iprior, 0, sizeof(E.iprior));
  E.pfic_sctlr = 0; E.pfic_ithresdr = 0;
  E.iwdg_running = 0; E.iwdg_unlocked = 0; E.iwdg_pr = 0; E.iwdg_rlr = 0xFFF;
  awu_at = 0; awu_csr = awu_wr = awu_psc = 0; pwr_ctlr = 0; event_flag = 0;
  misc_n = 0;
  clk_update();
  buzzer_update();
  sched();
}

void emu_soft_reset(void) {
  // system reset (PFIC RESETSYS), as a program would do it: SFTRSTF, the OLED keeps running
  system_reset(1u << 28);
}

void emu_reset(void) {
  // power-on reset: also resets the OLED (it shares the supply)
  E.cycles = 0; E.instret = 0;
  E.audio_len = 0; E.audio_pos = 0; E.audio_pos_frac = 0; E.audio_last = 0; E.audio_acc = 0;
  E.audio_hp = 0; E.audio_prev = 0; E.buzzer_level = 0;
  E.uart_len = 0; E.log_len = 0;
  memset(E.warned, 0, sizeof(E.warned));
  memset(E.sram, 0, SRAM_SIZE);
  for(int i = 0; i < SRAM_SIZE; i++) E.sram[i] = (uint8_t)rng_next();   // SRAM is random at power-up
  E.sys.rcc_rstsckr = 0;
  system_reset((1u << 26) | (1u << 27));                                // PINRSTF | PORRSTF
  E.audio_pos = E.audio_step_fp >> 32; E.audio_pos_frac = (uint32_t)E.audio_step_fp;
  oled_reset();
  uint8_t present = E.eeprom.present;
  memset(&E.eeprom, 0, sizeof(E.eeprom));
  E.eeprom.present = present;
}

void emu_init(void) {
  memset(&E, 0, sizeof(E));
  E.vcc_mv = 3300;
  E.adc_noise = 1;
  E.rng = 0x12345678;
  E.fault_halt = 1;
  E.board_rev = 13;
  E.eeprom.present = 1;
  memset(eeprom_mem, 0xFF, EEPROM_SIZE);                  // shipped erased
  E.t_load = 2; E.t_store = 2; E.t_branch = 2; E.t_jump = 2; E.t_mmio = 1;
  memset(E.flash, 0xFF, FLASH_SIZE);
  dcache_build();
  emu_reset();
}

uint32_t emu_load(const uint8_t *data, uint32_t len) {
  if(len > FLASH_SIZE) len = FLASH_SIZE;
  memset(E.flash, 0xFF, FLASH_SIZE);
  memcpy(E.flash, data, len);
  E.flash_len = len;
  dcache_build();
  emu_reset();
  return len;
}

// -----------------------------------------------------------------------------------
// Save states: the Emu struct plus the few file-static variables
// -----------------------------------------------------------------------------------
typedef struct {
  uint64_t irq_line, stk_match_at, awu_at;
  uint32_t awu_csr, awu_wr, awu_psc, pwr_ctlr, misc_n;
  uint8_t  event_flag;
  uint32_t misc_addr[128], misc_val[128];
} Extra;

uint32_t emu_state_size(void) { return sizeof(Emu) + sizeof(Extra); }

void emu_state_save(uint8_t *buf) {
  Extra x;
  x.irq_line = irq_line; x.stk_match_at = stk_match_at; x.awu_at = awu_at;
  x.awu_csr = awu_csr; x.awu_wr = awu_wr; x.awu_psc = awu_psc; x.pwr_ctlr = pwr_ctlr;
  x.misc_n = misc_n; x.event_flag = event_flag;
  memcpy(x.misc_addr, misc_addr, sizeof(misc_addr)); memcpy(x.misc_val, misc_val, sizeof(misc_val));
  memcpy(buf, &E, sizeof(Emu));
  memcpy(buf + sizeof(Emu), &x, sizeof(Extra));
}

void emu_state_load(const uint8_t *buf) {
  Extra x;
  memcpy(&E, buf, sizeof(Emu));
  memcpy(&x, buf + sizeof(Emu), sizeof(Extra));
  irq_line = x.irq_line; stk_match_at = x.stk_match_at; awu_at = x.awu_at;
  awu_csr = x.awu_csr; awu_wr = x.awu_wr; awu_psc = x.awu_psc; pwr_ctlr = x.pwr_ctlr;
  misc_n = x.misc_n; event_flag = x.event_flag;
  memcpy(misc_addr, x.misc_addr, sizeof(misc_addr)); memcpy(misc_val, x.misc_val, sizeof(misc_val));
  E.audio_len = 0;
  dcache_build();
}
