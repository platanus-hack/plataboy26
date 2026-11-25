// ===================================================================================
// plataboy26 loader kernel: the resident part of the game library firmware
// ===================================================================================
//
// Flash map (16 KiB):
//   0x0000 .. KBASE-1    the installed program (the menu or one game), word 0 = j KBASE
//   KBASE  .. 0x3FBF     this kernel, never rewritten by an install
//   0x3FC0 .. 0x3FFF     config page: which program is installed and its reset target
//                        (+0 magic, +4 entry, +8 index, +12 check), +16 the OLED contrast
//                        the menu passed to install(n, contrast) (games/common/sysx.h)
//
// Every installed image starts with "j KBASE" (the packer patches word 0), so each
// reset runs boot first:
//   - config invalid                                 -> install the menu (entry 0)
//   - the menu installed                             -> start it
//   - a game installed, power-on or reset pin        -> install the menu
//     (RCC_RSTSCKR PORRSTF / PINRSTF; the menu clears the flags at its start)
//   - a game installed, software reset (the install's own reset, or the game's)
//                                                    -> start the game
// So every power-on ends in the menu, which opens on the game played last.
// At power-on, boot first waits ~100 ms: the OLED module comes up later than the MCU and
// does not ACK its address until then (the provider's boards needed a second power cycle).
// The menu calls install(n) at KBASE + 4 to put game n into flash: the compressed
// image (format: tools/lz.py) is streamed from the AT24CM01 and decompressed straight
// into flash, 64-byte page by page; back references read the pages already written.
// The config page is erased first and written last, after the check of the new image
// passed, so a power loss during an install ends in the menu at the next power-on
// (except in the first ~6 ms, while page 0 is erased and reprogrammed).
//
// Written in assembly to stay within 1 KiB, so that the largest game (Tiny Arkanoid,
// 15196 bytes) fits below it.
// boot uses no SRAM (it belongs to the program); install owns all of it.
//
// EEPROM directory entry (32 bytes at 0x20 + 32 * n; the kernel reads the first 16):
//   +0 offset u32   +4 csize u16   +6 usize u16   +8 entry u32   +12 sum u32
//   sum: s = 0; for each byte b of the installed image: s = s * 33 ^ b

.equ RCC,      0x40021000
.equ FLASHR,   0x40022000
.equ GPIOA,    0x40010800
.equ GPIOC,    0x40011000
.equ I2C,      0x40005400
.equ PFIC_CFGR, 0xE000E048
.equ FLASH,    0x08000000
.equ CFG,      0x08003FC0
.equ MAGIC,    0x36324250       // "PB26"
.equ DIR,      0x20

// flash CTLR bits
.equ STRT,  1 << 6
.equ FTPG,  1 << 16
.equ FTER,  1 << 17
.equ BUFLOAD, 1 << 18
.equ BUFRST, 1 << 19

// I2C STAR1 bits
.equ SB, 1
.equ ADDR, 2
.equ BTF, 4
.equ RXNE, 0x40
.equ TXE, 0x80

// Registers during install:
//   s0 = I2C base   s1 = output position   gp = image size   tp = bit buffer
//   a3 = match bytes left   a5 = match source   sp + 0..63 = page buffer
//   sp + 64..79 = directory entry   sp + 80 = index   sp + 84 = contrast
//   ra save slots: t0 bits, t1 bit / ee_open / ee_close, t2 gamma

.section .entry, "ax"
.option push
.option norvc
.global entry
entry:
  j boot                        // KBASE + 0: reset (word 0 of every program)
  j install_main                // KBASE + 4: install(a0 = directory index, a1 = contrast)
  j unpack                      // KBASE + 8: unpack (see below; for games' code overlays)
.option pop

.text

// ---- boot -------------------------------------------------------------------------
boot:
  li a5, RCC
  lw a1, 0x24(a5)               // RSTSCKR: PINRSTF (26), PORRSTF (27)
  srli a1, a1, 26
  andi a1, a1, 3                // a1 != 0: power-on (or the reset pin)
  beqz a1, 2f
  li a2, 250000                 // power-on: ~100 ms at the reset clock (HSI / 3 = 8 MHz)
1:addi a2, a2, -1               // for the OLED module (its regulator and reset RC); an
  bnez a2, 1b                   // I2C start before it is up was never ACKed -> hang
2:  li a5, CFG
  lw a0, 0(a5)
  li a2, MAGIC
  bne a0, a2, menu
  lw a0, 4(a5)                  // entry
  lw a2, 8(a5)                  // index
  lw a3, 12(a5)                 // check = ~(entry ^ index)
  xor a4, a0, a2
  not a4, a4
  bne a4, a3, menu
  beqz a2, run                  // the menu is installed: start it
  bnez a1, menu                 // a game, at power-on: the menu instead
run:
  jr a0                         // (the flags stay set: the menu reads and clears them)
menu:
  li a0, 0

// ---- install ----------------------------------------------------------------------
install_main:
  li sp, 0x20000800             // the caller's stack and data are gone after this
  addi sp, sp, -92
  sw a0, 80(sp)
  sw a1, 84(sp)                 // a1 = the OLED contrast, kept in the config page
  // known state, whatever the caller left running: interrupts off, HSI 24 MHz,
  // no flash wait state, all peripherals reset
  csrci mstatus, 8
  li a5, RCC
  sw zero, 4(a5)                // CFGR0: HSI, HCLK = SYSCLK
1:lw a4, 4(a5)
  andi a4, a4, 12
  bnez a4, 1b
  li a1, FLASHR
  sw zero, 0(a1)                // ACTLR: 0 wait states
  li a4, -1
  sw a4, 12(a5)                 // APB2PRSTR
  sw zero, 12(a5)
  sw a4, 16(a5)                 // APB1PRSTR
  sw zero, 16(a5)
  li a4, 4
  sw a4, 0x14(a5)               // AHBPCENR = SRAM only (DMA off)
  li a4, 0x11
  sw a4, 0x18(a5)               // APB2PCENR = AFIO + IOPC
  li a4, 1 << 21
  sw a4, 0x1C(a5)               // APB1PCENR = I2C1
  li a3, GPIOC
  li a4, 0x44444DD4             // PC1 SDA, PC2 SCL: AF open drain 10 MHz
  sw a4, 0(a3)
  li s0, I2C
  li a4, 24
  sh a4, 4(s0)                  // CTLR2: 24 MHz
  li a4, 0x8000 | 20
  sh a4, 0x1C(s0)               // CKCFGR: fast mode, 24 MHz / (3 * 20) = 400 kHz
  li a4, 1
  sh a4, 0(s0)                  // CTLR1: PE
  // unlock flash (also fast mode) and erase the config: a power loss from here on
  // leads to the menu
  li a4, 0x45670123
  li a2, 0xCDEF89AB
  sw a4, 4(a1)                  // KEYR
  sw a2, 4(a1)
  sw a4, 0x24(a1)               // MODEKEYR
  sw a2, 0x24(a1)
  // directory entry
  slli a0, a0, 5
  addi a0, a0, DIR
  call ee_open
  addi a3, sp, 64
  addi a2, sp, 80
1:call ee_byte
  sb a0, 0(a3)
  addi a3, a3, 1
  bne a3, a2, 1b
  call ee_close
  lhu gp, 70(sp)                // usize
  beqz gp, halt
  la a4, entry
  bltu a4, gp, halt             // no library in the EEPROM: leave the flash alone
  li a0, CFG
  li a1, FLASHR
  call fl_erase
  // decompress
  lw a0, 64(sp)
  li s1, 0
  call unpack
  // check the installed image
  li a0, 0
  li a1, FLASH
1:slli a4, a0, 5
  add a0, a0, a4
  lbu a4, 0(a1)
  xor a0, a0, a4
  addi a1, a1, 1
  addi gp, gp, -1
  bnez gp, 1b
  lw a4, 76(sp)
  bne a0, a4, reset             // bad: reset without a config -> the menu
  li a4, MAGIC                  // good: record the program as installed
  sw a4, 0(sp)
  lw a0, 72(sp)
  sw a0, 4(sp)
  lw a1, 80(sp)
  sw a1, 8(sp)
  xor a0, a0, a1
  not a0, a0
  sw a0, 12(sp)
  lw a0, 84(sp)
  sb a0, 16(sp)                 // config + 16: contrast (games: SYSX_CONTRAST)
  li a0, CFG
  call fl_page
reset:
  li a1, FLASHR
  li a4, 0x8080                 // FLOCK, LOCK
  sw a4, 0x10(a1)
  li a1, PFIC_CFGR
  li a4, 0xBEEF0080             // KEY3 | RESETSYS
  sw a4, 0(a1)
halt:
  j halt

// ---- unpack: the LZ stream at EEPROM a0 -> flash offset s1 .. gp - 1 (s1 page aligned) --
// install's decompressor, also entry KBASE + 8 for programs with code overlays, with
// its own register convention: s0 = I2C base (I2C1 set up, bus idle), flash unlocked
// in fast mode, sp + 0..63 a page buffer, sp + 88 a word for the return address.
// Clobbers a0-a5, t0-t2, tp, s1. Interrupts may stay on (their code must not be in the
// pages being written). Back references read the pages already written, so they must
// stay within the output.
unpack:
  sw ra, 88(sp)
  call ee_open
  li tp, 0
  li a3, 0
loop:
  bgeu s1, gp, done
  bnez a3, copy
  call bit
  bnez a0, match
  li a1, 8                      // literal
  call bits
  j store
match:
  call gamma                    // length - 1
  addi a3, a2, 1
  call gamma                    // offset - 1 = (gamma - 1) << 7 | 7 bits
  addi a5, a2, -1
  slli a5, a5, 7
  li a1, 7
  call bits
  or a5, a5, a2
  sub a5, s1, a5
  addi a5, a5, -1               // match source
copy:
  andi a4, s1, -64
  bltu a5, a4, 1f
  andi a4, a5, 63               // in the page being assembled
  add a4, a4, sp
  j 2f
1:li a4, FLASH                  // in a page already written
  add a4, a4, a5
2:lbu a2, 0(a4)
  addi a5, a5, 1
  addi a3, a3, -1
store:
  andi a4, s1, 63
  add a4, a4, sp
  sb a2, 0(a4)
  addi s1, s1, 1
  andi a4, s1, 63
  beqz a4, 1f
  bne s1, gp, loop
1:addi a0, s1, -1               // page full, or the end: write it
  andi a0, a0, -64
  li a4, FLASH
  add a0, a0, a4
  call fl_page
  j loop
done:
  lw ra, 88(sp)
  j ee_close

// ---- flash: fast page erase / program, the sequence of WCH's FLASH_ROM_ERASE/WRITE --
// a0 = page address (0x0800xxxx), a1 = FLASHR. Clobbers a2, a4, t0.
fl_wait:
  lw a4, 0x0C(a1)               // STATR.BSY
  andi a4, a4, 1
  bnez a4, fl_wait
  ret

fl_erase:
  mv t0, ra
  li a4, FTER
  sw a4, 0x10(a1)
  sw a0, 0x14(a1)               // ADDR
  li a4, FTER | STRT
  sw a4, 0x10(a1)
  call fl_wait
  sw zero, 0x10(a1)
  jr t0

// erase + program the page at a0 with the 64 bytes at sp (caller's sp)
fl_page:
  li a1, FLASHR
  mv t1, ra
  call fl_erase
  li a4, FTPG
  sw a4, 0x10(a1)
  li a4, FTPG | BUFRST
  sw a4, 0x10(a1)
  call fl_wait
  li a2, 0
1:add a4, sp, a2
  lw t0, 0(a4)
  add a4, a0, a2
  sw t0, 0(a4)                  // into the page buffer
  li a4, FTPG | BUFLOAD
  sw a4, 0x10(a1)
  call fl_wait
  addi a2, a2, 4
  li a4, 64
  bne a2, a4, 1b
  sw a0, 0x14(a1)               // ADDR
  li a4, FTPG | STRT
  sw a4, 0x10(a1)
  call fl_wait
  sw zero, 0x10(a1)
  jr t1

// ---- I2C EEPROM (AT24CM01 at 0x50/0x51), read as one open-ended sequential read ----
// ee_open sets the address and starts receiving with ACK, ee_byte returns the next
// byte (the master holds SCL low while DATAR and the shift register are full, so the
// slow flash writes in between are fine), ee_close ends the read with NACK and STOP.
// Streams never cross the 64 KiB boundary (the packer keeps them apart), so the bank
// bit of the device address stays valid.

// a4 = STAR1 flag to wait for. Clobbers a0.
i2c_wait:
  lhu a0, 0x14(s0)
  and a0, a0, a4
  beqz a0, i2c_wait
  ret

// START + device address a1. Clobbers a0, a4, t0.
i2c_start:
  mv t0, ra
  lhu a4, 0(s0)
  ori a4, a4, 0x100             // START
  sh a4, 0(s0)
  li a4, SB
  call i2c_wait
  sh a1, 0x10(s0)
  li a4, ADDR
  call i2c_wait
  lhu a4, 0x18(s0)              // STAR2: clears ADDR
  jr t0

// a0 = EEPROM address. Clobbers a0, a1, a2, a4, t0.
ee_open:
  mv t1, ra
  mv a2, a0
  srli a1, a0, 15
  andi a1, a1, 2
  ori a1, a1, 0xA0              // 0x50 | bank, write
  call i2c_start
  srli a4, a2, 8
  andi a4, a4, 0xFF
  sh a4, 0x10(s0)
  li a4, TXE
  call i2c_wait
  andi a4, a2, 0xFF
  sh a4, 0x10(s0)
  li a4, BTF
  call i2c_wait
  lhu a4, 0(s0)
  ori a4, a4, 0x400             // ACK
  sh a4, 0(s0)
  ori a1, a1, 1                 // repeated START, read
  mv ra, t1
  j i2c_start

// next byte -> a0
ee_byte:
  lhu a0, 0x14(s0)
  andi a0, a0, RXNE
  beqz a0, ee_byte
  lhu a0, 0x10(s0)
  ret

// The closing sequence of an N-byte read (games/eeprom_test/src/eeprom.c, EE_read):
// wait for BTF (one byte in DATAR, the next in the shift register, SCL held low; this
// is reached whether or not the CPU kept up), NACK what comes next, read; wait for BTF
// again, STOP, read the last two.
ee_close:
  mv t1, ra
  li a4, BTF
  call i2c_wait
  lhu a4, 0(s0)
  andi a4, a4, ~0x400           // no ACK for the next byte
  sh a4, 0(s0)
  lhu a4, 0x10(s0)
  li a4, BTF
  call i2c_wait
  lhu a4, 0(s0)
  ori a4, a4, 0x200             // STOP
  sh a4, 0(s0)
  lhu a4, 0x10(s0)
  lhu a4, 0x10(s0)
1:lhu a4, 0(s0)
  andi a4, a4, 0x200
  bnez a4, 1b
  jr t1

// ---- decompressor bit reader --------------------------------------------------------
// tp = bit buffer, MSB first, with a marker bit after the data.
// bit -> a0. Clobbers a4, t1.
bit:
  slli a4, tp, 1
  bnez a4, 1f
  mv t1, ra                     // only the marker left: next byte
  call ee_byte
  mv ra, t1
  slli tp, a0, 24
  lui a4, 0x800
  or tp, tp, a4
1:srli a0, tp, 31
  slli tp, tp, 1
  ret

// a1 bits -> a2 (bits_cont: appended to a2). Clobbers a0, a1, a4, t0, t1.
bits:
  li a2, 0
bits_cont:
  mv t0, ra
1:beqz a1, 2f
  call bit
  slli a2, a2, 1
  or a2, a2, a0
  addi a1, a1, -1
  j 1b
2:jr t0

// Elias gamma -> a2: z zeros, then a 1 and z more bits. Clobbers a0, a1, a4, t0-t2.
gamma:
  mv t2, ra
  li a1, 0
1:call bit
  bnez a0, 2f
  addi a1, a1, 1
  j 1b
2:li a2, 1
  call bits_cont
  jr t2
