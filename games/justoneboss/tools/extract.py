#!/usr/bin/env python3
# One-time helper: cuts the sprites this port uses out of the PICO-8 cart's sprite
# sheet, maps the 16 colours to 1 bit ('#' lit, 'o' dark but opaque, '.' transparent)
# and shrinks them to 3/4 height (the console's playfield is 128x64, the cart's 128x128
# with tiles 10x8 -> 10x6). The output (tools/sprites.txt) was then edited by hand.
#   python3 tools/extract.py just_one_boss.p8 > tools/sprites.txt
import sys

# name: sx, sy, w, h, frames, frame step x, colour map (pico colour -> # o .)
LIT, DARK = '#', 'o'
def cmap(lit='', dark=''):
    m = {c: '.' for c in '0123456789abcdef'}
    for c in lit: m[c] = LIT
    for c in dark: m[c] = DARK
    return m
SPECS = [
    ('P_LR',    0,  0, 11,  8, 7, 11, cmap('cd9867', '1')),
    ('P_UP',    0,  8, 11, 11, 7, 11, cmap('cd9867', '1')),
    ('P_DN',    0, 19, 11, 11, 7, 11, cmap('cd9867', '1')),
    ('P_STUN', 78, 11, 11, 10, 1,  0, cmap('cd9867', '1')),
    ('FIGMENT',89, 13, 11,  8, 1,  0, cmap('cd5', '1')),
    ('FIGMENT2',89, 5, 11,  8, 1,  0, cmap('cd5', '1')),
    ('HEARTS',  0, 30,  9,  7, 7,  9, cmap('8e5', '')),
    ('TILE',   55, 38,  9,  7, 1,  0, cmap('7', '')),
    ('CARD',   77, 21, 10, 10, 4, 10, cmap('76', '58')),
    ('FLOWER',101, 71,  9,  8, 3,  9, cmap('8e3b', '')),
    ('COIN',    0, 37,  9,  9, 6,  9, cmap('6d57', '')),
    ('SLAM',   47, 71, 11,  7, 2, 11, cmap('9', '')),
    ('MIRROR',115,  0, 13, 30, 1,  0, cmap('9af4', '726')),
    ('FACE',    0, 57, 11, 14, 8, 11, cmap('762abf', 'dc81')),   # redrawn by hand in sprites.txt
    ('HAT_ON',102,  0, 13,  9, 1,  0, cmap('5', '18')),
    ('HAND',    0, 46, 12, 11, 6, 12, cmap('76', 'd')),
    ('BOUQUET',110, 71, 9, 16, 1,  0, cmap('8e3b', '')),
    ('WAND',   91, 57,  7, 13, 2,  7, cmap('56', '')),
    ('POOF',   64, 31, 16, 14, 4, 16, cmap('7', '')),
    ('PAIN',  105, 45, 23, 26, 1,  0, cmap('a', '')),
    ('MULT',   69, 71, 11,  7, 1,  0, cmap('1', '')),
    ('ARROW', 112, 88,  8,  8, 1,  0, cmap('d', '')),
    ('CLOCK', 104, 80,  8,  8, 1,  0, cmap('67', '')),
    ('TOPHAT',100,  9, 15, 12, 1,  0, cmap('5', '18')),
    ('BUNNY',  91, 45, 14, 12, 1,  0, cmap('7f', 'e9')),
]

def main(path):
    src = open(path).read().split('\n')
    i = src.index('__gfx__')
    gfx = src[i + 1:i + 129]
    for name, sx, sy, w, h, n, step, m in SPECS:
        for f in range(n):
            x0 = sx + f * step
            rows = [''.join(m[gfx[sy + y][x0 + x]] for x in range(w)) for y in range(h)]
            out = []
            oh = (h * 3 + 3) // 4
            for r in range(oh):
                a, b = r * 4 // 3, min(h, (r + 1) * 4 // 3) - 1
                b = max(a, b)
                line = ''
                for x in range(w):
                    px = [rows[y][x] for y in range(a, b + 1)]
                    line += DARK if DARK in px else (LIT if LIT in px else '.')
                out.append(line)
            print('%s %d' % (name, f) if n > 1 else name)
            print('\n'.join(out))
            print()

main(sys.argv[1])
