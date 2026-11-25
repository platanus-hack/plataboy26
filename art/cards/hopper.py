# Hopper card: its title screen (the simulator's thumbnail, sim/web/roms/hopper.png) without
# the menu (START GAME, SOUND, RECORD, CREDIT): the "Hopper" lettering and the hopper.
import os
from PIL import Image
from common import *

src = Image.open(os.path.join(HERE, "..", "..", "sim/web/roms/hopper.png")).convert("L").resize((128, 64), Image.NEAREST)
scr = src.point(lambda v: 255 if v > 64 else 0, "1")
for y in range(38, 64):                 # the menu, bottom right
    for x in range(56, 128): scr.putpixel((x, y), 0)
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "hopper")
