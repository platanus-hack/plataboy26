# Just One Boss card: the game's own title lettering (sprite sheet of the PICO-8 cart) and
# the boss, the magic mirror in a top hat (from the cart label), 2x.
# Sources (not in the repository): games/justoneboss/upstream/just_one_boss.p8 (fetch.sh)
# and the cart label https://www.lexaloffle.com/bbs/thumbs/pico49232.png
import sys, os
from PIL import Image
from common import *

p8, label = sys.argv[1], sys.argv[2]
lines = open(p8).read().split("\n")
i = lines.index("__gfx__") + 1
rows = []
while not lines[i].startswith("__"): rows.append(lines[i]); i += 1
sheet = Image.new("RGB", (128, len(rows)))
for y, r in enumerate(rows):
    for x, c in enumerate(r[:128]): sheet.putpixel((x, y), PICO[int(c, 16)])

logo = to_bits(sheet, {6, 7, 13}, (0, 72, 47, 128))
logo = logo.crop(logo.getbbox())
# the boss at 2x, from palette indices: the mirror's glass dark with the face's features
# (13) lit, so it reads as a smiling mirror; frame and handle (9, 4, 15, 6), hat (5) lit,
# hat band (8) dark
src = Image.open(label).convert("RGB").crop((58, 23, 71, 52))
boss = Image.new("1", src.size)
for y in range(src.height):
    for x in range(src.width):
        if pico_index(src.getpixel((x, y))) in {13, 9, 4, 15, 6, 5}: boss.putpixel((x, y), 1)
boss = scale(boss, 2)

# its white gloves (the open hand of the sprite sheet), one on each side
hand = to_bits(sheet, {6, 7}, (24, 46, 38, 57))
hand = hand.crop(hand.getbbox())

scr = Image.new("1", (128, 64))
lx = 10
scr.paste(logo, (lx, (64 - logo.height) // 2))
bx, by = 128 - 22 - boss.width // 2 - 8, (64 - boss.height) // 2
scr.paste(boss, (bx, by))
hy = by + 25
scr.paste(hand, (bx - hand.width - 2, hy))
scr.paste(hand.transpose(Image.FLIP_LEFT_RIGHT), (bx + boss.width + 2, hy))
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "justoneboss")
print("logo", logo.size, "boss", boss.size)
