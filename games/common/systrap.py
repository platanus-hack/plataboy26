#!/usr/bin/env python3
"""Patch a CH32V003 SDK system.c (wagiminator's, shared by every program here) so that a
CPU exception or NMI ends in SYS_trap instead of a frozen CPU.

Without a vector table (SYS_USE_VECTORS 0) the startup still selected mtvec mode 3, so a
fault took the code bytes at offsets 8 / 12 as handler addresses: the CPU looped on an
unmapped address forever with every pin frozen, and a buzzer pin (PA1, active low) left
low kept the coil on DC, enough to pull a CR2032 down to a brownout reset (plataboy26-devkit
issues #12 / #13). With a vector table, NMI and HardFault went to default_handler's
while(1), with the same frozen pins.

After the patch:
  - no vector table: mtvec mode 0, every trap enters SYS_trap
  - vector table: its NMI and HardFault entries are SYS_trap
  - SYS_trap (weak, here): software reset. A reset puts every GPIO back to input, which
    releases the buzzer within microseconds. games/common/sysx.c (every game of the
    library) overrides it: back to the menu, like UP + DOWN.

    python3 systrap.py system.c [...]     (idempotent)
"""
import sys

MARK = 'plataboy26 SYS_trap'

TRAP = '''// ---- plataboy26 SYS_trap (games/common/systrap.py) -------------------------------
// Every CPU exception and the NMI end here (see the mtvec setup in reset_handler and the
// vector table): a software reset, which also releases the buzzer pin at once (all GPIOs
// back to input). Weak: games/common/sysx.c replaces it with the way back to the menu.
// Naked, no stack (sp may be what faulted); aligned for mtvec mode 0.
void SYS_trap(void) __attribute__((weak, naked, used, aligned(4)));
void SYS_trap(void) {
  asm volatile("li a5, 0xE000E048 \\n li a4, 0xBEEF0080 \\n sw a4, 0(a5) \\n 1: j 1b");
}

'''

MTVEC = '''
  #if SYS_USE_VECTORS == 0
  // plataboy26: no vector table, so every trap enters SYS_trap directly (mtvec mode 0)
  // and not through the code bytes at 8 / 12 taken as addresses (games/common/systrap.py)
  asm volatile("la a0, SYS_trap \\n csrw mtvec, a0" : : : "a0");
  #endif
'''


def patch(path):
    s = open(path).read()
    if MARK in s:
        return False
    anchor = '// FLASH starts with a jump to the reset handler\n'
    asm_end = '    : : [main] "r" (main) : "a0", "a1" , "memory"\n  );\n'
    for a in (anchor, asm_end, '  NMI_Handler,', '  HardFault_Handler,'):
        if s.count(a) != 1:
            sys.exit(f'{path}: anchor not found once: {a.strip()!r}')
    s = s.replace(anchor, TRAP + anchor)
    s = s.replace(asm_end, asm_end + MTVEC)
    # same column width as the other entries
    s = s.replace('  NMI_Handler,      ', '  SYS_trap,         ')
    s = s.replace('  HardFault_Handler,', '  SYS_trap,         ')
    if '  SYS_trap,' not in s or s.count('  SYS_trap,') != 2:
        sys.exit(f'{path}: vector table entries not replaced')
    open(path, 'w').write(s)
    return True


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(('patched  ' if patch(p) else 'already  ') + p)
