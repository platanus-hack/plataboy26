// ===================================================================================
// Just One Boss: the console's hardware for the game (plat.c)
// ===================================================================================
//   frames   30 updates per second, like the cart (PICO-8 _update)
//   screen   drawn one 8-row page at a time into a 128-byte buffer (no frame buffer:
//            the game needs the RAM), sent by DMA while the next page is drawn
//   buttons  joypad (resistor ladder on PC4), A = fire button, B = B button (rev 1.3)
//   sound    one voice on the buzzer: a sound effect's note list (tools/sfx.py), played
//            from the SysTick compare interrupt
//   storage  4 records (best score / time, normal and hard) in the EEPROM save area
#pragma once
#include <stdint.h>

#define BTN_L  1                       // the cart's button numbers 0..3 as bits
#define BTN_R  2
#define BTN_U  4
#define BTN_D  8
#define BTN_A  16
#define BTN_B  32

extern uint8_t *pg;                    // the page being drawn (128 bytes)
extern uint8_t pages[2][128];          // both page buffers (contiguous)
extern uint8_t btn_now, btn_new;       // held / pressed this frame

void     plat_init(void);
void     plat_frame(void);             // waits for the next 1/30 s, reads the buttons
void     lcd_begin(void);              // a frame: lcd_begin, 8 x lcd_page, lcd_end
void     lcd_page(void);
void     lcd_end(void);
extern volatile uint8_t snd_ch;        // channel of the sound playing (0xFF: none)
void     sfx_play(const uint8_t *notes, uint8_t ch);   // tools/sfx.py's note list
void     sfx_stop(void);
uint32_t rnd32(void);
void     save_read(uint16_t *rec);     // 4 records, 0 when nothing was saved
void     save_write(const uint16_t *rec);
