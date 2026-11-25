// ===================================================================================
// CH32V003 Game Console Simulator - core definitions
// ===================================================================================
//
// One SoC instance (CPU + peripherals + SSD1306 + board circuit) lives in a single
// plain struct without pointers, so a save state is just a memcpy of that struct.
//
// Time base: everything is counted in HCLK cycles (E.cycles). The clock tree is
// re-evaluated whenever RCC is written; peripherals compute their state lazily
// from the cycle counter and schedule their next event in E.next_event.

#pragma once
#include <stdint.h>

#ifndef FLASH_SIZE
#define FLASH_SIZE   16384   // -DFLASH_SIZE=65536: development build for oversized ports
#endif
#ifndef SRAM_SIZE
#define SRAM_SIZE    2048    // -DSRAM_SIZE=4096: development build (verification shadows)
#endif
#define AUDIO_RATE   48000
#define AUDIO_BUF    16384
#ifndef UART_BUF
#define UART_BUF     4096
#endif
#define EEPROM_SIZE  131072   // AT24CM01: 1 Mbit

// Buttons (bit mask for emu_set_buttons)
#define BTN_UP      1
#define BTN_DOWN    2
#define BTN_LEFT    4
#define BTN_RIGHT   8
#define BTN_ACT     16
#define BTN_B       32      // board rev. 1.3: second action button (shares PA2, read by ADC)

// Run status
#define ST_OK       0
#define ST_FAULT    1   // CPU exception while fault_halt is enabled
#define ST_HALT     2   // stopped (e.g. breakpoint)

typedef struct {
  // ---- Flash / timing ------------------------------------------------------------
  uint32_t actlr, keyr_state, statr, ctlr, obr;
  uint32_t faddr, fbuf[16], fbuf_mask;  // fast programming: ADDR, 64-byte page buffer
  uint64_t fbusy_until;                 // end of the running erase/program (cycles)
  uint32_t flash_ops;                   // pages erased + programmed since power-on

  // ---- RCC -----------------------------------------------------------------------
  uint32_t rcc_ctlr, rcc_cfgr0, rcc_intr, rcc_apb2prstr, rcc_apb1prstr;
  uint32_t rcc_ahbpcenr, rcc_apb2pcenr, rcc_apb1pcenr, rcc_rstsckr;
} SysRegs;

typedef struct {
  uint32_t cfglr, indr_unused, outdr, lckr;
} GPIOPort;

typedef struct {
  uint32_t ctlr, sr, cmp;
  uint32_t cnt0;        // counter value at cycle t0
  uint64_t t0;          // reference cycle
} STK_t;

typedef struct {
  uint16_t ctlr1, ctlr2, oaddr1, oaddr2, ckcfgr;
  uint16_t star1, star2;
  uint8_t  dr;          // data register (tx buffer)
  uint8_t  dr_full;
  uint8_t  shifting;    // shift register busy
  uint8_t  shift_byte;
  uint8_t  is_addr;     // byte in shifter is the address byte
  uint8_t  addr_pending_clear;  // ADDR set, waiting for STAR1+STAR2 read
  uint8_t  star1_read;  // STAR1 read while ADDR set
  uint8_t  stop_pending, start_pending, nack;
  uint8_t  dev;         // addressed slave: 0 none, 1 OLED, 2 EEPROM
  uint8_t  rx_hold, rx_hold_byte;   // received byte waiting in the shift register (BTF)
  uint8_t  rx_more;     // last received byte was ACKed: keep receiving
  uint8_t  rx_ack_start;            // ACK bit when the current byte started (for POS)
  uint64_t ev_at;       // cycle of next internal event (0 = none)
  uint8_t  ev_kind;     // 1 = start, 2 = byte shifted out, 3 = stop, 4 = byte received
} I2C_t;

// AT24CM01 serial EEPROM (A2 = A1 = GND -> 0x50/0x51, WP = GND). The memory array itself
// lives outside Emu (eeprom_mem) because it is non-volatile: power cycles and save states
// do not touch it.
typedef struct {
  uint8_t  present;
  uint8_t  addressed, reading, addr_bytes;
  uint32_t addr;        // 17-bit internal address counter
  uint32_t page;        // page being written (addr >> 8)
  uint8_t  latch[256];
  uint32_t latch_mask[8];
  uint8_t  latch_any;
  uint64_t busy_until;  // internal write cycle (tWR) end, in cycles
  uint32_t writes;      // page writes committed (statistics)
} EEPROM_t;

typedef struct {
  uint32_t statr, ctlr1, ctlr2, samptr1, samptr2, rsqr1, rsqr2, rsqr3, rdatar, dlyr;
  uint32_t wdhtr, wdltr, isqr, iofr[4], idatar[4];
  uint64_t conv_at;     // end of running conversion (0 = idle)
  uint64_t cal_at;      // end of calibration (0 = idle)
} ADC_t;

typedef struct {
  uint32_t cfgr, cntr, paddr, maddr;
  uint32_t cnt_reload;  // CNTR value written by software (for circular mode)
  uint32_t pcur, mcur;  // current addresses
} DMACh;

typedef struct {
  uint32_t ctlr1, ctlr2, statr, brr, gpr;
} USART_t;

typedef struct {
  uint8_t  ram[8][128];
  uint8_t  on, invert, entire_on, charge_pump;
  uint8_t  contrast, mux, offset, start_line, compins, seg_remap, com_remap;
  uint8_t  mem_mode, col_start, col_end, page_start, page_end, col, page;
  uint8_t  clkdiv, precharge, vcomh, scroll_on;
  // protocol state
  uint8_t  addressed, expect_ctrl, cont, dc;
  uint8_t  cmd_buf[8], cmd_len, cmd_need;
  uint32_t data_bytes;  // total data bytes received (for statistics)
  uint32_t frames;      // number of full GDDRAM wrap-arounds / page-0 restarts
} SSD1306_t;

typedef struct Emu {
  // ---- CPU ----------------------------------------------------------------------
  uint32_t x[16];
  uint32_t pc;
  uint64_t cycles;
  uint32_t mstatus, mtvec, mepc, mcause, mtval, mscratch, intsyscr, corecfgr;
  uint32_t hpe_depth;           // nesting depth of hardware-stacked interrupts
  uint8_t  sleeping;            // WFI
  uint8_t  status;              // ST_*
  uint32_t fault_pc, fault_cause, fault_tval;
  uint64_t instret;

  // ---- memory --------------------------------------------------------------------
  uint8_t  flash[FLASH_SIZE];
  uint8_t  sram[SRAM_SIZE];
  uint32_t flash_len;

  // ---- clock ---------------------------------------------------------------------
  uint32_t hclk;                // current HCLK in Hz
  SysRegs  sys;

  // ---- peripherals ---------------------------------------------------------------
  GPIOPort gpio[3];             // A, C, D
  uint32_t afio_pcfr1, afio_exticr;
  uint32_t exti_intenr, exti_evenr, exti_rtenr, exti_ftenr, exti_intfr;
  STK_t    stk;
  I2C_t    i2c;
  ADC_t    adc;
  DMACh    dma[7];
  uint32_t dma_intfr;
  USART_t  usart;

  // PFIC
  uint64_t irq_enabled, irq_pending, irq_active;
  uint8_t  iprior[64];
  uint32_t pfic_sctlr, pfic_ithresdr;

  // IWDG
  uint32_t iwdg_pr, iwdg_rlr, iwdg_unlocked, iwdg_running;
  uint64_t iwdg_deadline;

  // generic storage for peripherals that are not modelled (TIM1/2, SPI, PWR, ...)
  uint32_t misc_regs[256];

  // ---- board ---------------------------------------------------------------------
  SSD1306_t oled;
  EEPROM_t  eeprom;
  uint32_t buttons;             // BTN_* currently pressed
  uint32_t vcc_mv;              // supply voltage (battery)
  uint32_t adc_noise;           // LSB of random noise added to conversions
  uint32_t pad_leak;            // ohms from PAD to VCC (0: none): a board whose joypad idles above 0
  uint32_t rng;

  // ---- options -------------------------------------------------------------------
  uint8_t  board_rev;           // 11, 12 (+EEPROM) or 13 (+EEPROM, B button) - see hardware/
  uint8_t  fault_halt;          // stop on CPU exception instead of jumping to vector
  uint8_t  t_load, t_store, t_branch, t_jump, t_mmio;   // cycle model

  // ---- scheduling ----------------------------------------------------------------
  uint64_t next_event;

  // ---- audio (buzzer on PA1, active low) -----------------------------------------
  int16_t  audio[AUDIO_BUF];
  uint32_t audio_len;
  uint64_t audio_pos;            // position of next sample in cycles (whole part)
  uint32_t audio_pos_frac;       //   and its fraction (/2^32): a 32.32 value would overflow
                                 //   at 2^32 cycles (89 s at 48 MHz)
  uint64_t audio_step_fp;       // cycles per sample (32.32 fixed)
  uint64_t audio_last;          // cycle at which the level integral was last updated
  uint64_t audio_acc;           // integral of level within current sample (cycles)
  uint16_t buzzer_level;        // current through the buzzer: 0..256 (256 = on all the time;
                                //   between: TIM1 PWM on PA1, its low-time fraction)
  struct { uint32_t ctlr1, chctlr1, ccer, psc, atrlr, ch2cvr, bdtr; } tim1;   // PWM only
  float    audio_hp;            // high pass state (removes DC like the real transducer)
  float    audio_prev;

  // ---- debug output --------------------------------------------------------------
  char     uart[UART_BUF];
  uint32_t uart_len;
  char     log[UART_BUF];       // simulator warnings (unmodelled peripherals etc.)
  uint32_t log_len;
  uint32_t warned[16];          // bit set of warned register blocks
} Emu;

extern Emu E;

void     emu_init(void);
void     emu_reset(void);
void     emu_soft_reset(void);          // system reset (SFTRSTF), not a power cycle
uint32_t emu_load(const uint8_t *data, uint32_t len);
uint32_t emu_run_cycles(uint64_t n);
uint32_t emu_run_us(uint32_t us);
void     emu_set_buttons(uint32_t mask);
void     emu_set_board(uint32_t rev);   // 11, 12 or 13
void     emu_render(uint8_t *out);      // 128x64 bytes, pixel brightness 0..255
uint8_t *emu_framebuffer(void);
uint8_t *emu_sram(void);                 // test harnesses: the simulated SRAM and flash
uint8_t *emu_flash(void);
uint32_t emu_flash_ops(void);
extern uint32_t flash_page_erases[FLASH_SIZE / 64];   // erases per 64-byte page since start
uint32_t emu_pc(void);
uint32_t emu_peek32(uint32_t addr);       // debug: a bus read without side effects on memory
uint64_t emu_cycles(void);            // flash pages erased + programmed since power-on

extern uint8_t  eeprom_mem[EEPROM_SIZE];
extern uint32_t eeprom_dirty;   // incremented on every committed write (for persistence)
