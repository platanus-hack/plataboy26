# Tiny Pacman card: the title in the menu's font at 2x over the chase scene made of the
# game's own 8 x 8 sprites at 3x (Pac-Man eating dots, two ghosts after him).
# Source: ref/CH32V003-GameConsole/software/tiny_pacman/src/spritebank.h (games/tiny/Makefile
# clones it), the font: firmware/menu/src/font.h.
import os, re, sys
from PIL import Image
from common import *

ROOT = os.path.join(HERE, "..", "..")
bank = open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "ref/CH32V003-GameConsole/software/tiny_pacman/src/spritebank.h")).read()
car = [int(v, 16) for v in re.findall(r"0x[0-9A-F]{2}", bank.split("caracters []")[1].split("};")[0])]

def sprite(n):                          # 8 x 8 as the game draws them: byte = row, bottom up
    im = Image.new("1", (8, 8))
    for y in range(8):
        for x in range(8):
            if car[n * 8 + y] >> x & 1: im.putpixel((x, 7 - y), 1)
    return im

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
t = text("TINY PACMAN", 2)
t = t.crop(t.getbbox())
scr.paste(t, ((128 - t.width) // 2, 5))
y = 32
# two ghosts chasing Pac-Man (mouth open to the right), the dots ahead of him
scr.paste(scale(sprite(12), 3), (4, y)); scr.paste(scale(sprite(14), 3), (32, y))
scr.paste(scale(sprite(2), 3), (66, y))
for dx in (96, 108, 120):
    for yy in range(y + 10, y + 13):
        for xx in range(dx, dx + 3): scr.putpixel((xx, yy), 1)
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "tiny_pacman")
