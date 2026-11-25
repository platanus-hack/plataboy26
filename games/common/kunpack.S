// kunpack(a0 = EEPROM source, a1 = first flash offset, a2 = end, a3 = 96-byte scratch):
// calls the library kernel's unpack (firmware/kernel/kernel.S, KBASE + 8) with its
// register convention: s0 = I2C1, s1 = output position, gp = end, sp = the scratch
// (+0..63 page buffer, +88 its return address). Interrupts are off meanwhile (gp is
// not the global pointer). Flash must be unlocked in fast mode. Used by the games whose
// code overlays are LZ streams: Nitro Nonsense, MicroCity. The output's last page is
// written whole (bytes past `end` in it are left over from the page buffer).
.equ KUNPACK, 0x3C48
.section .text.kunpack, "ax"
.global kunpack
kunpack:
  csrr t0, mstatus
  csrci mstatus, 8
  sw t0, 92(a3)
  sw ra, 64(a3)
  sw sp, 68(a3)
  sw gp, 72(a3)
  sw tp, 76(a3)
  sw s0, 80(a3)
  sw s1, 84(a3)
  mv sp, a3
  li s0, 0x40005400
  mv s1, a1
  mv gp, a2
  li t0, KUNPACK
  jalr t0
  lw ra, 64(sp)
  lw gp, 72(sp)
  lw tp, 76(sp)
  lw s0, 80(sp)
  lw s1, 84(sp)
  lw t0, 92(sp)
  lw sp, 68(sp)
  andi t0, t0, 8
  csrs mstatus, t0
  ret
