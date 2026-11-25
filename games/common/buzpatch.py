#!/usr/bin/env python3
"""Patch the JOY_sound() of a wagiminator-style driver.h (the Tiny games, the menu, the
template and the games made from it) so that the buzzer pin stays low for at most
BUZ_PULSE_US per period (games/common/buzzer.h): same period, so the same pitch, but
far less current from the coin cell.

    python3 buzpatch.py driver.h [...]     (idempotent; files without JOY_sound are skipped)
"""
import sys

MARK = 'plataboy26 BUZ_PULSE_US'
OLD = '''    if(freq) PIN_low(PIN_BEEP);
    #endif
    DLY_us(255 - freq);
    PIN_high(PIN_BEEP);
    DLY_us(255 - freq);
'''
NEW = '''    if(freq) PIN_low(PIN_BEEP);
    #endif
    // plataboy26 BUZ_PULSE_US: low for at most that long, same period (games/common/buzzer.h)
    DLY_us(lo);
    PIN_high(PIN_BEEP);
    DLY_us(2 * h - lo);
'''
FN = 'void JOY_sound(uint8_t freq, uint8_t dur) {\n'
FN_NEW = FN + '  uint8_t h = 255 - freq, lo = h < BUZ_PULSE_US ? h : BUZ_PULSE_US;\n'


def patch(path):
    s = open(path).read()
    if MARK in s:
        return 'already'
    if FN not in s:
        return 'skipped (no JOY_sound)'
    if s.count(OLD) != 1:
        sys.exit(f'{path}: JOY_sound body not as expected')
    s = s.replace(OLD, NEW).replace(FN, '#include "buzzer.h"\n' + FN_NEW, 1)
    open(path, 'w').write(s)
    return 'patched'


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(f'{patch(p):9s} {p}')
