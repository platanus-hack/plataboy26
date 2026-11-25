# Tiny Conway card: the title in the menu's font at 2x, "GAME OF LIFE" under it, and a
# flight of gliders (the Game of Life's famous spaceship) in their four phases, each
# a step further down than the last, heading to the bottom right.
# The font: firmware/menu/src/font.h.
import os, re
from PIL import Image
from common import *

ROOT = os.path.join(HERE, "..", "..")
font = [[int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", l)]
        for l in open(os.path.join(ROOT, "firmware/menu/src/font.h")).read().split("\n") if l.count("0x") == 5]
def text(s, k, gap=1):                  # each character k times larger, gap pixels apart
    im = Image.new("1", (len(s) * (5 * k + gap), 8 * k))
    for i, ch in enumerate(s):
        g = Image.new("1", (5, 8))
        for x, col in enumerate(font[ord(ch) - 32]):
            for y in range(8):
                if col >> y & 1: g.putpixel((x, y), 1)
        im.paste(scale(g, k), (i * (5 * k + gap), 0))
    return im

scr = Image.new("1", (128, 64))
def title(s, k, y):
    t = text(s, k); t = t.crop(t.getbbox())
    scr.paste(t, ((128 - t.width) // 2, y))
    return y + t.height

y = title("TINY CONWAY", 2, 3)
title("GAME OF LIFE", 1, y + 3)

# the glider's four phases (each moves one cell down-right every 4 generations)
GLIDER = [[".#.", "..#", "###"], ["#.#", ".##", ".#."], ["..#", "#.#", ".##"], ["#..", ".##", "##."]]
def cell(cx, cy):                       # a 4 x 4 dot on a 5 x 5 grid, from (2, 33)
    for yy in range(4):
        for xx in range(4):
            scr.putpixel((2 + cx * 5 + xx, 33 + cy * 5 + yy), 1)
for i, phase in enumerate(GLIDER):      # glider i: 6 cells right, 1 cell down of the last
    for r, row in enumerate(phase):
        for c, ch in enumerate(row):
            if ch == "#": cell(i * 6 + c + 1, i + r)
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "tiny_conway")
