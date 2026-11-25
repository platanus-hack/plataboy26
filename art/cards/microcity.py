# MicroCity card in the style of the game's own title screen (white, the logo on black):
# the official logo (Images/logo.png, 72 x 40) over a strip of its developed buildings
# (Images/tiles.png: residential, commercial, industrial, police, fire department).
# Source (not in the repository): games/microcity/upstream (fetch.sh).
import sys, os
from PIL import Image
from common import *

up = sys.argv[1]
logo = Image.open(os.path.join(up, "Images", "logo.png")).convert("L").point(lambda v: 255 if v > 128 else 0, "1")
tiles = Image.open(os.path.join(up, "Images", "tiles.png")).convert("L").point(lambda v: 255 if v > 128 else 0, "1")

scr = Image.new("1", (128, 64), 1)
scr.paste(logo, ((128 - logo.width) // 2, 0))
blocks = [(0, 56), (24, 56), (48, 56), (96, 32), (96, 56)]   # R, C, I (developed), PD, FD
gap = (128 - 24 * len(blocks)) // (len(blocks) + 1)
x = gap + (128 - 24 * len(blocks) - gap * (len(blocks) + 1)) // 2
for tx, ty in blocks:
    scr.paste(tiles.crop((tx, ty, tx + 24, ty + 24)), (x, 40)); x += 24 + gap
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "microcity")
