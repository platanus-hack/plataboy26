# Boot animation of the Platanus edition: the full Platanus logo (isotype + "platanus"
# wordmark, hardware/doc/art/logos/platanus-full.svg) and the "PLATABOY 26" caption.
#   1. the isotype's three strokes drop in one after the other, bouncing
#   2. the wordmark is revealed left to right, from behind the isotype, with a bright edge
#   3. the caption types out, as in the PH26 edition's animation (anim.py)
# Usage: python3 platanus.py   (needs rsvg-convert)
#   -> anim/platanus.gif (4x preview), platanus_128x64.png, and
#      firmware/menu/src/bootanim_platanus.h (the menu built with EDITION_PLATANUS)
import os, subprocess, tempfile
from PIL import Image
from make_logo import layers, FONT, SPACING, HERE

SVG = os.path.join(HERE, "..", "..", "hardware", "doc", "art", "logos", "platanus-full.svg")
OUT = os.path.join(HERE, "..", "..", "firmware", "menu", "src", "bootanim_platanus.h")
LOGO_W, THRESH, GAP_CAP = 124, 50, 8

# ---- the logo, 1-bit, split into the three strokes and the wordmark -----------------------
with tempfile.TemporaryDirectory() as d:
    png = os.path.join(d, "logo.png")
    subprocess.run(["rsvg-convert", "-w", str(LOGO_W * 20), SVG, "-o", png], check=True)
    big = Image.open(png).split()[-1]
big = big.crop(big.getbbox())
H = round(big.height * LOGO_W / big.width)

def small(mask):
    return mask.resize((LOGO_W, H), Image.LANCZOS).point(lambda v: 255 if v >= THRESH else 0).convert("1")

# isotype: the columns before the first empty one; its strokes = connected components
W, Hb = big.size; px = big.load()
iso_w = next(x for x in range(W) if not any(px[x, y] > 128 for y in range(Hb)))
seen, strokes = set(), []
for x0 in range(iso_w):
    for y0 in range(Hb):
        if px[x0, y0] > 128 and (x0, y0) not in seen:
            comp, stack = [], [(x0, y0)]
            seen.add((x0, y0))
            while stack:
                x, y = stack.pop(); comp.append((x, y))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < iso_w and 0 <= ny < Hb and (nx, ny) not in seen and px[nx, ny] > 128:
                        seen.add((nx, ny)); stack.append((nx, ny))
            if len(comp) > 500: strokes.append(comp)
assert len(strokes) == 3, len(strokes)
strokes.sort(key=lambda c: min(x for x, _ in c))           # left (short) to right (long)
masks = []
for comp in strokes:
    m = Image.new("L", big.size)
    for p in comp: m.putpixel(p, big.getpixel(p))
    masks.append(m)
word = big.copy(); word.paste(0, (0, 0, iso_w, Hb))

TOP = (64 - (H + GAP_CAP + 7)) // 2
LX = (128 - LOGO_W) // 2
def sprite(img):                                           # cropped, with its position
    s = small(img); bb = s.getbbox()
    return s.crop(bb), (LX + bb[0], TOP + bb[1])
STROKES = [sprite(m) for m in masks]
WORD = sprite(word)

# caption "PLATABOY 26" in the PH26 edition's font, centered under the logo
CAP = "PLATABOY 26"
cw = len(CAP) * (5 + SPACING) - SPACING
CX, CY = (128 - cw) // 2, TOP + H + GAP_CAP
CHARS = []
for i, ch in enumerate(CAP):
    im = Image.new("1", (5, 7))
    for ry, row in enumerate(FONT[ch]):
        for rx, b in enumerate(row):
            if b == "1": im.putpixel((rx, ry), 1)
    CHARS.append((im, (CX + i * (5 + SPACING), CY)))

def cols(im):                                              # as export_c.py
    pages = (im.height + 7) // 8
    out = []
    for x in range(im.width):
        for p in range(pages):
            b = 0
            for r in range(8):
                y = p * 8 + r
                if y < im.height and im.getpixel((x, y)): b |= 1 << r
            out.append(b)
    return out, pages

def carr(name, data):
    s = f"static const uint8_t {name}[{len(data)}] = {{\n"
    for i in range(0, len(data), 16):
        s += "  " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n"
    return s + "};\n"

# ---- the animation ----------------------------------------------------------------------
def clamp(t): return max(0.0, min(1.0, t))
def seg(f, a, b): return clamp((f - a) / (b - a))
def bounce(t):
    n, d = 7.5625, 2.75
    if t < 1 / d: return n * t * t
    if t < 2 / d: t -= 1.5 / d; return n * t * t + .75
    if t < 2.5 / d: t -= 2.25 / d; return n * t * t + .9375
    t -= 2.625 / d; return n * t * t + .984375
def ease_out(t): return 1 - (1 - t) ** 3

FRAMES = 40
def params(f):
    """per frame: y offsets of the three strokes, wordmark columns shown, caption characters
    shown, the cursor's x (None: no cursor)"""
    dys = []
    for k, (im, (x, y)) in enumerate(STROKES):
        t = bounce(seg(f, k * 3, k * 3 + 12))
        dys.append(round(-(y + im.height) * (1 - t)))      # from just above the screen
    wim, _ = WORD
    cols_shown = round(wim.width * ease_out(seg(f, 12, 24)))
    start, per, tail = 24, 1, 3
    n = sum(1 for i in range(len(CHARS)) if f >= start + i * per)
    cur = None
    if f >= start and (f // 4) % 2 == 0 and f < start + len(CHARS) * per + tail:
        cur = CHARS[min(n, len(CHARS) - 1)][1][0] + (7 if n == len(CHARS) else 0)
    return dict(dy=dys, word=cols_shown, chars=n, cursor=cur)

def frame(f):
    s = Image.new("1", (128, 64)); P = params(f)
    for (im, (x, y)), dy in zip(STROKES, P["dy"]): s.paste(im, (x, y + dy), im)
    wim, (wx, wy) = WORD
    if P["word"]:
        part = wim.crop((0, 0, P["word"], wim.height)); s.paste(part, (wx, wy), part)
        if P["word"] < wim.width:                          # the bright edge of the reveal
            for yy in range(wy - 1, wy + wim.height + 1): s.putpixel((wx + P["word"], yy), 1)
    for im, (x, y) in CHARS[:P["chars"]]: s.paste(im, (x, y), im)
    if P["cursor"] is not None:
        for yy in range(CY, CY + 7):
            for xx in range(P["cursor"], min(P["cursor"] + 5, 128)): s.putpixel((xx, yy), 1)
    return s

if __name__ == "__main__":
    os.makedirs(os.path.join(HERE, "anim"), exist_ok=True)
    final = frame(FRAMES - 1)
    final.convert("L").save(os.path.join(HERE, "platanus_128x64.png"))
    def big4(im):
        out = Image.new("L", (512, 256), 12)
        for y in range(64):
            for x in range(128):
                if im.getpixel((x, y)): out.paste(235, (x * 4, y * 4, x * 4 + 3, y * 4 + 3))
        return out
    seq = [big4(frame(f)) for f in range(FRAMES)] + [big4(final)] * 30
    seq[0].save(os.path.join(HERE, "anim", "platanus.gif"), save_all=True, append_images=seq[1:],
                duration=40, loop=0)

    # ---- C header for the menu (same layout as bootanim.h; BA_ISO for the about box) -----
    iso, iso_pos = layers()["iso"]
    cap = Image.new("1", (cw, 7))
    for im, (x, y) in CHARS: cap.paste(im, (x - CX, 0), im)
    sprites = [("BA_ISO", iso, iso_pos)] + [(f"BA_S{k}", im, p) for k, (im, p) in enumerate(STROKES)] \
            + [("BA_WORD", *WORD), ("BA_CAP", cap, (CX, CY))]
    h = ["// Generated by art/boot/platanus.py from the Platanus logo: do not edit.",
         "// Platanus edition boot animation: sprites (column-major, LSB = top row) and the",
         "// per-frame table. BA_ISO: the banana of the about box.",
         "#pragma once", "#include <stdint.h>", ""]
    total = 0
    for name, im, (x, y) in sprites:
        d, pages = cols(im); total += len(d)
        h.append(f"#define {name}_X {x}\n#define {name}_Y {y}\n#define {name}_W {im.width}\n#define {name}_H {im.height}\n#define {name}_P {pages}")
        h.append(carr(name, d))
    h.append(f"#define BA_FRAMES {FRAMES}\n#define BA_CHAR_W {5 + 2}   // caption: 5 columns + 2 spacing\n")
    h.append("// per frame: strokes 0, 1, 2 dy; wordmark columns shown; caption chars shown; cursor x (0: none)")
    rows = []
    for f in range(FRAMES):
        P = params(f)
        v = [*P["dy"], P["word"], P["chars"], P["cursor"] or 0]
        assert all(-128 <= x < 128 for x in v)
        rows.append("  {" + ",".join(str(x) for x in v) + "},")
    h.append(f"static const int8_t BA_TABLE[BA_FRAMES][6] = {{\n" + "\n".join(rows) + "\n};\n")
    open(OUT, "w").write("\n".join(h))
    print(f"{OUT}: {total} bytes of sprites, {FRAMES * 6} bytes of table")
