#!/usr/bin/env python3
# Converts the cart's sound effects to one-voice note lists for the buzzer (bin/sfx.bin,
# in the game's EEPROM region after the bytecode: 28-byte slots for sfx 7..31).
# A PICO-8 sfx has 32 notes (pitch 0..63 = C2..D#7, waveform, volume, effect) played
# at `speed` 1/120 s each; here: notes of volume 0 become rests, equal consecutive
# notes merge, noise (waveform 6) is kept as a flag, effects are dropped. Encoding per
# note: pitch | noise << 6 (0x7F = rest), then the length in 1/120 s; 0xFF ends.
#   python3 tools/sfx.py just_one_boss.p8 bin/sfx.bin
import sys
USED = [7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
        28, 29, 30, 31]

SLOT = 28

def main(path, dst):
    src = open(path).read().split('\n')
    i = src.index('__sfx__')
    lines = src[i + 1:i + 65]
    out = bytearray()
    for n in range(USED[0], USED[-1] + 1):
        l = lines[n]
        slot = bytearray()
        if n in USED:
            speed = int(l[2:4], 16)
            notes = []
            for k in range(32):
                s = l[8 + 5 * k:13 + 5 * k]
                p, w, v = int(s[0:2], 16), int(s[2], 16), int(s[3], 16)
                key = 0x7F if v == 0 else (p | (0x40 if w == 6 else 0))
                if notes and notes[-1][0] == key and notes[-1][1] + speed < 256: notes[-1][1] += speed
                else: notes.append([key, speed])
            while notes and notes[-1][0] == 0x7F: notes.pop()
            for key, d in notes:
                while d > 255: slot += bytes([key, 255]); d -= 255
                slot += bytes([key, d])
        slot.append(0xFF)
        assert len(slot) <= SLOT, n
        out += slot + b'\xff' * (SLOT - len(slot))
    open(dst, 'wb').write(out)
    print('sfx: %d bytes (EEPROM)' % len(out))

main(sys.argv[1], sys.argv[2])
