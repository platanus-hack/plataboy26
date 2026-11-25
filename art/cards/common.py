# Shared helpers for the menu cards (128 x 64, 1-bit): PICO-8 palette, previews.
import os
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
PICO = [(0,0,0),(29,43,83),(126,37,83),(0,135,81),(171,82,54),(95,87,79),(194,195,199),(255,241,232),
        (255,0,77),(255,163,0),(255,236,39),(0,228,54),(41,173,255),(131,118,156),(255,119,168),(255,204,170)]

def pico_index(c):
    return min(range(16), key=lambda i: sum((a - b) ** 2 for a, b in zip(PICO[i], c[:3])))

def to_bits(im, on, box=None):
    """PICO-8 colored image -> 1-bit: pixels whose palette index is in `on` light up"""
    im = im.convert("RGB")
    if box: im = im.crop(box)
    out = Image.new("1", im.size)
    for y in range(im.height):
        for x in range(im.width):
            if pico_index(im.getpixel((x, y))) in on: out.putpixel((x, y), 1)
    return out

def scale(im, k): return im.resize((im.width * k, im.height * k), Image.NEAREST)

def preview(scr, path, S=4):
    prev = Image.new("RGB", (128 * S, 64 * S), (8, 10, 14))
    for y in range(64):
        for x in range(128):
            if scr.getpixel((x, y)):
                prev.paste((235, 245, 255), (x * S, y * S, x * S + S - 1, y * S + S - 1))
    prev.save(path)

def save_card(scr, name):
    """the card as the simulator thumbnail format the packer reads (any size, gray)"""
    scr.convert("L").save(os.path.join(HERE, name + ".png"))
    preview(scr, os.path.join(HERE, "preview", name + ".png"))
