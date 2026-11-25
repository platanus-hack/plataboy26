# Boot animation variations, rendered as 4x GIF previews (25 fps).
# Usage: python3 anim.py [ph26-logo.png]   (see make_logo.py)
import sys, math, os
from PIL import Image
from make_logo import layers, compose, HERE

L = layers(sys.argv[1] if len(sys.argv) > 1 else None)
FINAL = compose(L)
FPS, HOLD = 25, 30   # hold the final frame 1.2 s before looping

def blank(): return Image.new("1", (128, 64))
def put(scr, im, x, y): scr.paste(im, (round(x), round(y)), im)
def clamp(t): return max(0.0, min(1.0, t))
def ease_out(t): return 1 - (1 - t) ** 3
def ease_back(t, s=1.9): t -= 1; return t * t * ((s + 1) * t + s) + 1
def bounce(t):
    n, d = 7.5625, 2.75
    if t < 1 / d: return n * t * t
    if t < 2 / d: t -= 1.5 / d; return n * t * t + .75
    if t < 2.5 / d: t -= 2.25 / d; return n * t * t + .9375
    t -= 2.625 / d; return n * t * t + .984375
def seg(f, a, b): return clamp((f - a) / (b - a))

def typewriter(scr, f, start, per=2, cursor=True, tail=12):
    n = 0
    for i, (im, (x, y)) in enumerate(L["chars"]):
        if f >= start + i * per: put(scr, im, x, y); n = i + 1
    if cursor and f >= start and (f // 4) % 2 == 0 and n < len(L["chars"]) + 1:
        x, y = L["chars"][min(n, len(L["chars"]) - 1)][1]
        if n == len(L["chars"]): x += 7
        if n <= len(L["chars"]) and f < start + len(L["chars"]) * per + tail:
            for yy in range(y, y + 7):
                for xx in range(x, x + 5): scr.putpixel((xx, yy), 1)

# 1. LAUNCH (1.5 s): arrow shoots in diagonally, P H / 2 6 slide in, banana drops, caption
# types. launch_params() is also what export_c.py puts into the menu (bootanim.h).
LAUNCH_FRAMES = 37
def launch_params(f):
    """per frame: offsets (dx, dy) of arrow, x offsets of P H / 2 6, y offset of the banana,
    caption characters shown and the cursor's x (None: no cursor)"""
    g = L["glyphs"]; ax, ay = g[2][1]
    t = ease_back(seg(f, 0, 9), 1.2)
    adx, ady = round(ax - 90 * (1 - t)) - ax, round(ay + 60 * (1 - t)) - ay
    t = ease_out(seg(f, 7, 15))
    ldx, rdx = round(-70 * (1 - t)), round(70 * (1 - t))
    ix, iy = L["iso"][1]; t = bounce(seg(f, 13, 26))
    idy = round(iy - 24 * (1 - t)) - iy
    start, per, tail, chars = 24, 1, 3, L["chars"]
    n = sum(1 for i in range(len(chars)) if f >= start + i * per)
    cur = None
    if f >= start and (f // 4) % 2 == 0 and f < start + len(chars) * per + tail:
        cur = chars[min(n, len(chars) - 1)][1][0] + (7 if n == len(chars) else 0)
    return dict(arrow=(adx, ady), left=ldx, right=rdx, iso=idy, chars=n, cursor=cur)

def launch(f):
    s = blank(); P = launch_params(f); g = L["glyphs"]
    im, (x, y) = g[2]; put(s, im, x + P["arrow"][0], y + P["arrow"][1])
    for k in (0, 1): im, (x, y) = g[k]; put(s, im, x + P["left"], y)
    for k in (3, 4): im, (x, y) = g[k]; put(s, im, x + P["right"], y)
    im, (x, y) = L["iso"]; put(s, im, x, y + P["iso"])
    for im, (x, y) in L["chars"][:P["chars"]]: put(s, im, x, y)
    if P["cursor"] is not None:
        y = L["chars"][0][1][1]
        for yy in range(y, y + 7):
            for xx in range(P["cursor"], P["cursor"] + 5):
                if xx < 128: s.putpixel((xx, yy), 1)
    return s
# 2. WIPE: diagonal reveal following the arrow, with a bright leading edge
def wipe(f):
    s = blank(); t = ease_out(seg(f, 0, 30)) * (128 + 64 + 8)
    for y in range(64):
        for x in range(128):
            d = x + (63 - y)
            if d < t - 3: s.putpixel((x, y), FINAL.getpixel((x, y)))
            elif d < t and (x + y) % 2 == 0: s.putpixel((x, y), 1)
    if 30 <= f < 33: s = Image.eval(s, lambda v: 255 - v) if f == 31 else s  # one-frame flash
    return s
# 3. DISSOLVE: pixels light up in pseudo-random (LFSR) order, then a blink
ORDER = []
_l = 1
for _ in range(8191):
    _l = (_l >> 1) ^ (-(_l & 1) & 0x1B00)   # 13-bit Galois LFSR, period 8191
    ORDER.append(_l - 1)
ORDER = [i for i in ORDER if i < 8192]
def dissolve(f):
    n = int(len(ORDER) * ease_out(seg(f, 0, 32)))
    s = blank()
    for i in ORDER[:n]:
        x, y = i % 128, i // 128
        if FINAL.getpixel((x, y)): s.putpixel((x, y), 1)
    if f in (36, 37): s = blank()
    return s
# 4. POP: glyphs jump up one by one with overshoot, banana falls and bounces, caption types
def pop(f):
    s = blank()
    for k, (im, (x, y)) in enumerate(L["glyphs"]):
        t = seg(f, k * 4, k * 4 + 12)
        if t > 0: put(s, im, x, y + 44 * (1 - ease_back(t)))
    im, (x, y) = L["iso"]; t = seg(f, 22, 40)
    if t > 0: put(s, im, x, -16 + (y + 16) * bounce(t))
    typewriter(s, f, 38, per=2)
    return s
# 5. CRT: power-on dot -> horizontal line -> image expands vertically from the center
def crt(f):
    s = blank()
    if f < 4:                                    # dot grows
        r = f + 1
        for x in range(64 - r * 2, 64 + r * 2):
            s.putpixel((x, 32), 1)
        return s
    if f < 10:                                   # line stretches across
        w = int(64 * ease_out(seg(f, 4, 10)))
        for x in range(64 - w, 64 + w): s.putpixel((x, 32), 1); s.putpixel((x, 31), 1)
        return s
    t = ease_back(seg(f, 10, 22), 1.4)           # vertical expand (with overshoot)
    h = max(1, round(64 * t))
    src = FINAL.resize((128, h), Image.NEAREST) if h <= 64 else FINAL.resize((128, h), Image.NEAREST)
    s.paste(src.crop((0, max(0, (h - 64) // 2), 128, max(0, (h - 64) // 2) + min(64, h))), (0, max(0, 32 - h // 2)))
    if f < 13:                                   # bright scan line while opening
        for x in range(128): s.putpixel((x, 32 - h // 2), 1); s.putpixel((x, min(63, 32 + h // 2)), 1)
    return s

VARIANTS = [("1_launch", launch, LAUNCH_FRAMES), ("2_wipe", wipe, 34),
            ("3_dissolve", dissolve, 40), ("4_pop", pop, 54), ("5_crt", crt, 24)]

def big(scr, S=4):
    out = Image.new("P", (128 * S, 64 * S), 0)
    out.putpalette([8, 10, 14, 235, 245, 255] + [0] * 762)
    for y in range(64):
        for x in range(128):
            if scr.getpixel((x, y)): out.paste(1, (x * S, y * S, x * S + S - 1, y * S + S - 1))
    return out

def main():
  os.makedirs(os.path.join(HERE, "anim"), exist_ok=True)
  for name, fn, n in VARIANTS:
      frames = [big(fn(f)) for f in range(n)] + [big(FINAL)] * HOLD + [big(blank())] * 8
      frames[0].save(os.path.join(HERE, "anim", name + ".gif"), save_all=True,
                     append_images=frames[1:], duration=1000 // FPS, loop=0, optimize=False, disposal=1)
      print(name, f"{n / FPS:.1f}s")

if __name__ == "__main__":
    main()
