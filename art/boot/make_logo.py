# Boot frame: Platanus isotype on top, PH↗26 logo, "PLATABOY 26" caption.
# Usage: python3 make_logo.py [ph26-logo.png]
#   with the brand PNG (~/hack/public/hack-26/landing/ph26-logo.png): reduces it and saves
#   the result as ph26_logo.png; without: uses that saved ph26_logo.png
# Outputs boot_128x64.png (1 px = 1 OLED pixel) and boot_preview.png (8x).
import sys
from PIL import Image

import os
HERE = os.path.dirname(os.path.abspath(__file__))
exec(open(os.path.join(HERE, "iso_sprite.py")).read())  # -> ISO

FONT = {
 "P": ["11110","10001","10001","11110","10000","10000","10000"],
 "L": ["10000","10000","10000","10000","10000","10000","11111"],
 "T": ["11111","00100","00100","00100","00100","00100","00100"],
 "A": ["01110","10001","10001","11111","10001","10001","10001"],
 "R": ["11110","10001","10001","11110","10100","10010","10001"],
 "D": ["11110","10001","10001","10001","10001","10001","11110"],
 "U": ["10001","10001","10001","10001","10001","10001","01110"],
 "B": ["11110","10001","10001","11110","10001","10001","11110"],
 "O": ["01110","10001","10001","10001","10001","10001","01110"],
 "Y": ["10001","10001","01010","00100","00100","00100","00100"],
 "2": ["01110","10001","00001","00010","00100","01000","11111"],
 "6": ["00110","01000","10000","11110","10001","10001","01110"],
 " ": ["00000"] * 7,
}
CAPTION, SPACING = "PLATABOY 26", 2
LOGO_W, GAP_ISO, GAP_CAP = 120, 4, 6

LOGO_1BIT = os.path.join(HERE, "ph26_logo.png")   # the logo at its final size, 1-bit

def logo(path=None):
    """the PH↗26 logo, LOGO_W wide, 1-bit: from the brand PNG (path), or the saved copy"""
    if path is None: return Image.open(LOGO_1BIT).convert("1")
    src = Image.open(path).convert("RGB")
    m = Image.new("L", src.size); px, mp = src.load(), m.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b = px[x, y]
            mp[x, y] = 255 if (r > 180 and g > 180 and b < 120) else 0  # yellow glyphs only
    m = m.crop(m.getbbox())
    h = round(m.height * LOGO_W / m.width)
    out = m.resize((LOGO_W, h), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0, "1")
    out.save(LOGO_1BIT)
    return out

def sprite(rows):
    im = Image.new("1", (len(rows[0]), len(rows)))
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c == "#": im.putpixel((x, y), 1)
    return im

def text(scr, s, x, y):
    for i, ch in enumerate(s):
        for ry, row in enumerate(FONT[ch]):
            for rx, b in enumerate(row):
                if b == "1": scr.putpixel((x + i * (5 + SPACING) + rx, y + ry), 1)

def preview(scr, name, S=8):
    prev = Image.new("RGB", (128 * S, 64 * S), (8, 10, 14))
    for y in range(64):
        for x in range(128):
            if scr.getpixel((x, y)):
                prev.paste((235, 245, 255), (x * S, y * S, x * S + S - 1, y * S + S - 1))
    prev.save(name)

def layers(path=None):
    """Final positions of every piece: iso, logo glyphs (P, H, arrow, 2, 6), caption chars."""
    lg, iso = logo(path), sprite(ISO)
    tw = len(CAPTION) * (5 + SPACING) - SPACING
    top = (64 - (iso.height + GAP_ISO + lg.height + GAP_CAP + 7)) // 2
    ly = top + iso.height + GAP_ISO
    lx = (128 - lg.width) // 2
    # split the logo into glyphs at empty columns
    cols = [any(lg.getpixel((x, y)) for y in range(lg.height)) for x in range(lg.width)]
    glyphs, x = [], 0
    while x < lg.width:
        if cols[x]:
            x0 = x
            while x < lg.width and cols[x]: x += 1
            glyphs.append((lg.crop((x0, 0, x, lg.height)), (lx + x0, ly)))
        else: x += 1
    cy, cx = ly + lg.height + GAP_CAP, (128 - tw) // 2
    chars = []
    for i, ch in enumerate(CAPTION):
        im = sprite(["".join("#" if b == "1" else "." for b in row) for row in FONT[ch]])
        chars.append((im, (cx + i * (5 + SPACING), cy)))
    return {"iso": (iso, ((128 - iso.width) // 2, top)), "glyphs": glyphs, "chars": chars}

def compose(L):
    scr = Image.new("1", (128, 64))
    for im, p in [L["iso"], *L["glyphs"], *L["chars"]]:
        scr.paste(im, p, im)
    return scr

if __name__ == "__main__":
    L = layers(sys.argv[1] if len(sys.argv) > 1 else None)
    print("glyphs:", [(im.size, p) for im, p in L["glyphs"]])
    scr = compose(L)
    scr.save(os.path.join(HERE, "boot_128x64.png")); preview(scr, os.path.join(HERE, "boot_preview.png"))
