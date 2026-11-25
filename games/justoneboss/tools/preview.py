#!/usr/bin/env python3
# Renders tools/sprites.txt as a zoomed PNG sheet (lit = white, dark = grey, '.' = blue)
import sys
from PIL import Image, ImageDraw
def load(path):
    out, name, rows = [], None, []
    for line in open(path).read().split('\n') + ['']:
        line = line.rstrip()
        if not line:
            if name: out.append((name, rows))
            name, rows = None, []
        elif name is None: name = line
        else: rows.append(line)
    return out
sp = load(sys.argv[1]); Z = 6
x = y = rowh = 0; W = 1100
pos = []
for name, rows in sp:
    w, h = len(rows[0]) * Z, len(rows) * Z
    if x + w > W: x, y, rowh = 0, y + rowh + 14, 0
    pos.append((x, y)); x += w + 8; rowh = max(rowh, h)
img = Image.new('RGB', (W, y + rowh + 14), (20, 30, 60))
d = ImageDraw.Draw(img)
for (name, rows), (px, py) in zip(sp, pos):
    for j, r in enumerate(rows):
        for i, c in enumerate(r):
            col = {'#': (255, 255, 255), 'o': (0, 0, 0), '.': (40, 50, 90)}[c]
            d.rectangle([px + i * Z, py + j * Z, px + i * Z + Z - 1, py + j * Z + Z - 1], fill=col)
for (name, rows), (px, py) in zip(sp, pos):
    d.text((px, py + len(rows) * Z), name.replace(' ', ''), fill=(255, 200, 0))
img.save(sys.argv[2])
