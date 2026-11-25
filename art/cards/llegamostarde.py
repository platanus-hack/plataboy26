# Llegamos tarde a la hackathon card, after the game's arcade cover (Platanus Hack site,
# fetched by games/llegamostarde/fetch.sh into upstream/cover.png, 800 x 600): the hacker
# climbing the glass building (glasses, open mouth, reaching hand) and the guard after
# him (a dark silhouette, fist up), in two tones, under its title box redrawn in the
# console's fonts, and a badge like its "SANTIAGO CHILE" one: the edition, now 26.
import os, sys
from PIL import Image, ImageOps
from common import *
G = os.path.join(HERE, "..", "..", "games", "llegamostarde")
sys.path.insert(0, os.path.join(G, "tools"))
from gen_data import font, EXTRA, SMALL, art

src = Image.open(os.path.join(G, "upstream", "cover.png")).convert("L")
im = ImageOps.autocontrast(src.crop((100, 102, 760, 432)), cutoff=1).resize((128, 64), Image.LANCZOS)
scr = Image.new("1", (128, 64))
for y in range(64):
    for x in range(128):
        v = im.getpixel((x, y))
        # the guard (dark skin, black suit) gets a lower threshold: his sunglasses, mouth, fist
        scr.putpixel((x, y), 1 if v > (55 if x < 46 and y > 28 else 105) else 0)

def glyph(g, x, y, s=1):
    for cx, col in enumerate(g):
        for r in range(8):
            if col >> r & 1:
                for dy in range(s):
                    for dx in range(s): scr.putpixel((x + cx * s + dx, y + r * s + dy), 1)
def text(s, y, scale=1, bold=False):
    w = len(s) * 6 * scale - scale
    x = 64 - w // 2 + bold
    for ch in s: glyph(font[ord(ch) - 32], x, y, scale); x += 6 * scale
def small(s, y):
    x = 64 - (len(s) * 4 - 1) // 2
    for ch in s:
        g = art(SMALL[ch])
        for yy in range(5):
            for xx in range(3):
                if g.getpixel((xx, yy)): scr.putpixel((x + xx, y + yy), 1)
        x += 4
    return x

# the title box: dark, a light border
def box(x0, y0, x1, y1, top=True):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            scr.putpixel((x, y), 1 if x in (x0, x1) or y == y1 or (top and y == y0) else 0)
box(0, 0, 127, 17, top=False)
text("LLEGAMOS TARDE A LA", 0)
text("HACKATHON", 8); text("HACKATHON", 8, bold=True)
# the badge: the edition, at the left under the title (the menu prints its version and
# the position in the bottom corners)
box(0, 19, 32, 29)
x = 3
for ch in "HACK 26":
    g = art(SMALL[ch])
    for yy in range(5):
        for xx in range(3):
            if g.getpixel((xx, yy)): scr.putpixel((x + xx, 22 + yy), 1)
    x += 4
os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
save_card(scr, "llegamostarde")
