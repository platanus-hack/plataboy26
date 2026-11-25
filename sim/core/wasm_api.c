// ===================================================================================
// CH32V003 Game Console Simulator - WebAssembly exports
// ===================================================================================

#include "emu.h"

#define EXPORT(name) __attribute__((export_name(#name)))

uint32_t emu_state_size(void);
void     emu_state_save(uint8_t *buf);
void     emu_state_load(const uint8_t *buf);

static uint8_t rom_buf[FLASH_SIZE];
static uint8_t state_buf[80 * 1024];

EXPORT(init)         void     w_init(void)               { emu_init(); }
EXPORT(reset)        void     w_reset(void)              { emu_reset(); }
EXPORT(rom_ptr)      uint8_t *w_rom_ptr(void)            { return rom_buf; }
EXPORT(load)         uint32_t w_load(uint32_t len)       { return emu_load(rom_buf, len); }
EXPORT(run_us)       uint32_t w_run_us(uint32_t us)      { return emu_run_us(us); }
EXPORT(run_cycles)   uint32_t w_run_cycles(uint32_t n)   { return emu_run_cycles(n); }
EXPORT(set_buttons)  void     w_set_buttons(uint32_t m)  { emu_set_buttons(m); }
EXPORT(framebuffer)  uint8_t *w_framebuffer(void)        { return emu_framebuffer(); }

EXPORT(audio_ptr)    int16_t *w_audio_ptr(void)          { return E.audio; }
EXPORT(audio_len)    uint32_t w_audio_len(void)          { return E.audio_len; }
EXPORT(audio_clear)  void     w_audio_clear(void)        { E.audio_len = 0; }

EXPORT(uart_ptr)     char    *w_uart_ptr(void)           { return E.uart; }
EXPORT(uart_len)     uint32_t w_uart_len(void)           { return E.uart_len; }
EXPORT(uart_clear)   void     w_uart_clear(void)         { E.uart_len = 0; }
EXPORT(log_ptr)      char    *w_log_ptr(void)            { return E.log; }
EXPORT(log_len)      uint32_t w_log_len(void)            { return E.log_len; }
EXPORT(log_clear)    void     w_log_clear(void)          { E.log_len = 0; }

EXPORT(hclk)         uint32_t w_hclk(void)               { return E.hclk; }
EXPORT(cycles_lo)    uint32_t w_cycles_lo(void)          { return (uint32_t)E.cycles; }
EXPORT(cycles_hi)    uint32_t w_cycles_hi(void)          { return (uint32_t)(E.cycles >> 32); }
EXPORT(instret_lo)   uint32_t w_instret_lo(void)         { return (uint32_t)E.instret; }
EXPORT(pc)           uint32_t w_pc(void)                 { return E.pc; }
EXPORT(reg)          uint32_t w_reg(uint32_t i)          { return i < 16 ? E.x[i] : 0; }
EXPORT(status)       uint32_t w_status(void)             { return E.status; }
EXPORT(fault_cause)  uint32_t w_fault_cause(void)        { return E.fault_cause; }
EXPORT(fault_tval)   uint32_t w_fault_tval(void)         { return E.fault_tval; }
EXPORT(sleeping)     uint32_t w_sleeping(void)           { return E.sleeping; }
EXPORT(oled_bytes)   uint32_t w_oled_bytes(void)         { return E.oled.data_bytes; }
EXPORT(oled_frames)  uint32_t w_oled_frames(void)        { return E.oled.frames; }
EXPORT(oled_on)      uint32_t w_oled_on(void)            { return E.oled.on && E.oled.charge_pump; }
EXPORT(sram_ptr)     uint8_t *w_sram_ptr(void)           { return E.sram; }

EXPORT(set_vcc)      void     w_set_vcc(uint32_t mv)     { E.vcc_mv = mv; }
EXPORT(set_noise)    void     w_set_noise(uint32_t lsb)  { E.adc_noise = lsb; }
EXPORT(set_fault_halt) void   w_set_fault_halt(uint32_t v) { E.fault_halt = (uint8_t)v; }
EXPORT(set_timing)   void     w_set_timing(uint32_t ld, uint32_t st, uint32_t br, uint32_t jmp, uint32_t mmio) {
  E.t_load = (uint8_t)ld; E.t_store = (uint8_t)st; E.t_branch = (uint8_t)br; E.t_jump = (uint8_t)jmp; E.t_mmio = (uint8_t)mmio;
}

EXPORT(eeprom_ptr)     uint8_t *w_eeprom_ptr(void)       { return eeprom_mem; }
EXPORT(eeprom_size)    uint32_t w_eeprom_size(void)      { return EEPROM_SIZE; }
EXPORT(eeprom_dirty)   uint32_t w_eeprom_dirty(void)     { return eeprom_dirty; }
EXPORT(eeprom_writes)  uint32_t w_eeprom_writes(void)    { return E.eeprom.writes; }
EXPORT(set_eeprom)     void     w_set_eeprom(uint32_t on) { E.eeprom.present = (uint8_t)on; }
EXPORT(set_board)      void     w_set_board(uint32_t rev) { emu_set_board(rev); }

EXPORT(state_size)   uint32_t w_state_size(void)         { return emu_state_size(); }
EXPORT(state_ptr)    uint8_t *w_state_ptr(void)          { return state_buf; }
EXPORT(state_save)   void     w_state_save(void)         { emu_state_save(state_buf); }
EXPORT(state_load)   void     w_state_load(void)         { emu_state_load(state_buf); }
