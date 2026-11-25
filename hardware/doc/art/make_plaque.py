"""Banana version of the Pioneer plaque (NASA, 1972, public domain; design Carl Sagan and
Frank Drake, artwork Linda Salzman Sagan). Everything is kept from the original except
the two humans, which become a cartoon male and female banana at the same size and spot.

    python make_plaque.py      -> plaque_banana.svg (+ .png preview)
"""
import math, os, re, subprocess
import xml.etree.ElementTree as ET
from svgelements import Path

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'pioneer_plaque_original.svg')
OUT = os.path.join(HERE, 'plaque_banana.svg')
NS = {'svg': 'http://www.w3.org/2000/svg'}
ET.register_namespace('', NS['svg'])

# where the humans stand in the original (plaque units, as drawn)
MAN = (1845, 547, 2318, 1817)
WOMAN = (2434, 641, 2865, 1829)
LINE = 'fill:none;stroke:#000;stroke-width:8;stroke-linecap:round;stroke-linejoin:round'
FINE = 'fill:none;stroke:#000;stroke-width:5.5;stroke-linecap:round;stroke-linejoin:round'
DOT = 'fill:#000;stroke:none'


def remove_humans(root):
    """Drop every path whose box lies inside the humans' area (bodies and eye dots)."""
    removed = 0
    for parent in root.iter():
        for el in list(parent):
            if el.tag.endswith('path') and el.get('d'):
                x0, y0, x1, y1 = Path(el.get('d')).bbox()
                cx, cy = (x0 + x1) / 2 + 79, (y0 + y1) / 2     # group offset of the layer
                if 1830 < cx < 2880 and 530 < cy < 1840 and (x1 - x0) < 1100:
                    parent.remove(el); removed += 1
    return removed


class Banana:
    """A cartoon banana drawn in a 440 x 1270 box (feet at the bottom). Paths are SVG path
    strings with absolute M/L/C/Q/Z commands in that box; mirror=True flips them
    left/right so the pair faces each other."""
    W, H = 440, 1270

    def __init__(self, box, mirror=False):
        self.x0, self.y0, x1, y1 = box
        self.s = (y1 - self.y0) / self.H
        self.mirror = mirror
        self.parts = []

    def xy(self, x, y):
        if self.mirror: x = self.W - x
        return self.x0 + x * self.s, self.y0 + y * self.s

    def path(self, d, style=LINE):
        nums = iter(float(n) for n in re.findall(r'-?\d+\.?\d*', d))
        def pair(m):
            x, y = self.xy(next(nums), next(nums))
            return f'{x:.1f},{y:.1f}'
        out = re.sub(r'-?\d+\.?\d*[ ,]+-?\d+\.?\d*', pair, d)
        self.parts.append(f'<path d="{out}" style="{style}"/>')

    def circle(self, x, y, r, style=LINE):
        cx, cy = self.xy(x, y)
        self.parts.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r * self.s:.1f}" style="{style}"/>')

    def ellipse(self, x, y, rx, ry, style=LINE):
        cx, cy = self.xy(x, y)
        self.parts.append(f'<ellipse cx="{cx:.1f}" cy="{cy:.1f}" rx="{rx * self.s:.1f}" ry="{ry * self.s:.1f}" style="{style}"/>')

    def svg(self):
        return '<g>' + ''.join(self.parts) + '</g>'

    # ---- shared pieces --------------------------------------------------------------
    def classic_body(self):
        self.path('M 200,170 C 340,230 420,520 370,830 C 350,950 300,1010 255,1015 C 238,1016 230,1004 236,990 '
                  'C 205,860 130,560 140,260 C 140,215 165,175 200,170 Z')
        self.path('M 186,172 L 168,96 L 196,88 L 214,168'); self.path('M 166,96 L 198,87')
        self.path('M 232,250 C 320,400 335,700 290,930', FINE)

    def chubby_body(self):
        self.path('M 215,250 C 360,280 430,560 390,820 C 372,940 320,1010 255,1012 C 236,1013 226,1000 233,986 '
                  'C 170,900 95,640 120,380 C 128,300 165,252 215,250 Z')
        self.path('M 202,252 L 190,180 L 222,174 L 232,250'); self.path('M 188,181 L 224,173')

    def slender_body(self):
        self.path('M 205,150 C 300,210 345,520 320,860 C 310,960 282,1010 250,1012 C 236,1013 230,1002 234,990 '
                  'C 215,860 168,560 170,240 C 172,190 185,155 205,150 Z')
        self.path('M 192,152 L 180,70 L 206,64 L 218,150'); self.path('M 178,70 L 208,63')
        self.path('M 228,230 C 292,380 305,700 272,930', FINE)

    def legs(self, x1=232, x2=292, top=960, foot=1215, shoe=(46, 20)):
        self.path(f'M {x1},{top} L {x1 - 20},{foot}'); self.path(f'M {x2},{top} L {x2 + 26},{foot}')
        self.ellipse(x1 - 42, foot + 25, *shoe); self.ellipse(x2 + 48, foot + 25, *shoe)

    def dot_eyes(self, y=410, xs=(232, 302), r=20):
        for ex in xs:
            self.circle(ex, y, r); self.circle(ex + 5, y + 4, r * .4, DOT)

    def lashes(self, y=390, xs=(232, 302)):
        for ex in xs:
            for dx, dy in ((-16, -26), (0, -30), (16, -26)):
                self.path(f'M {ex + dx * .7},{y + 2 + dy * .4} L {ex + dx},{y + dy}', FINE)

    def wave_hand(self, x=64, y=335, r=30):
        self.circle(x, y, r)
        for a, b, c, d in ((x - 18, y - 25, x - 28, y - 67), (x, y - 30, x, y - 77), (x + 18, y - 25, x + 30, y - 65)):
            self.path(f'M {a},{b} L {c},{d}')
        self.path(f'M {x + 28},{y + 10} L {x + 58},{y - 5}')


class Classic(Banana):
    def male(self):
        self.classic_body(); self.dot_eyes(); self.legs()
        self.path('M 228,480 Q 268,525 318,470')
        self.path('M 214,382 L 250,376', FINE); self.path('M 286,370 L 322,366', FINE)
        self.path('M 160,560 Q 70,520 66,365'); self.wave_hand()
        self.path('M 378,600 Q 430,700 418,800'); self.circle(416, 826, 26)

    def female(self):
        self.classic_body(); self.dot_eyes(); self.lashes(); self.legs()
        self.path('M 228,480 Q 268,525 318,470')
        self.path('M 182,92 L 128,58 L 128,128 Z'); self.path('M 182,92 L 236,58 L 236,128 Z'); self.circle(182, 92, 12, DOT)
        self.path('M 372,590 Q 440,640 372,720')
        self.path('M 155,600 Q 105,700 120,800'); self.circle(122, 826, 26)


class Kawaii(Banana):
    def face(self):
        for ex, ey in ((220, 520), (310, 512)):
            self.ellipse(ex, ey, 30, 36, DOT); self.circle(ex + 10, ey - 12, 9, 'fill:#e0b794;stroke:none')
        self.ellipse(184, 582, 22, 10, FINE); self.ellipse(348, 572, 22, 10, FINE)
        self.path('M 243,578 Q 256,600 268,582 Q 280,600 293,578')

    def male(self):
        self.chubby_body(); self.face(); self.legs(238, 288, 995, 1195, (40, 22))
        self.path('M 140,650 Q 92,610 92,545'); self.circle(92, 518, 24)
        self.path('M 52,470 Q 40,500 52,530', FINE); self.path('M 30,452 Q 14,500 32,548', FINE)
        self.path('M 392,660 Q 425,705 418,755'); self.circle(417, 778, 22)

    def female(self):
        self.chubby_body(); self.face(); self.legs(238, 288, 995, 1195, (40, 22))
        for a in range(5):                                                       # flower
            import math
            t = a * 2 * math.pi / 5 - math.pi / 2
            self.circle(206 + 26 * math.cos(t), 160 + 26 * math.sin(t), 16)
        self.circle(206, 160, 10, DOT)
        self.path('M 132,650 Q 90,700 100,760'); self.circle(102, 782, 22)
        self.path('M 392,660 Q 430,700 425,760'); self.circle(424, 782, 22)


class RubberHose(Banana):
    def face(self):
        for ex, ey in ((232, 440), (302, 434)):
            self.ellipse(ex, ey, 22, 40)
            self.path(f'M {ex - 12},{ey + 2} Q {ex},{ey + 44} {ex + 12},{ey + 2} L {ex + 4},{ey + 12} L {ex - 4},{ey + 2} Z', DOT)
        self.path('M 212,520 Q 270,612 338,512 Z')
        self.path('M 245,560 Q 270,585 300,560', FINE)

    def glove(self, x, y, r=34):
        self.circle(x, y, r); self.circle(x + r * .95, y - r * .45, r * .42)
        self.path(f'M {x - r * .7},{y + r * 1.05} L {x + r * .7},{y + r * 1.05} L {x + r * .6},{y + r * 1.5} L {x - r * .6},{y + r * 1.5} Z')
        self.path(f'M {x - r * .35},{y - r * .9} L {x - r * .35},{y - r * .2}', FINE)
        self.path(f'M {x + r * .1},{y - r * .95} L {x + r * .1},{y - r * .2}', FINE)

    def noodle_legs(self):
        self.path('M 236,990 C 205,1080 252,1150 214,1210'); self.path('M 290,990 C 322,1080 280,1150 318,1210')
        for x in (180, 352):
            self.ellipse(x, 1240, 62, 28); self.path(f'M {x - 58},{1248} L {x + 58},{1248}', FINE)

    def male(self):
        self.classic_body(); self.face(); self.noodle_legs()
        self.path('M 160,560 C 60,560 30,430 72,345'); self.glove(84, 300)
        self.path('M 380,600 C 455,640 465,760 425,820'); self.glove(424, 850, 30)

    def female(self):
        self.classic_body(); self.face(); self.noodle_legs(); self.lashes(396)
        for a in range(5):
            import math
            t = a * 2 * math.pi / 5 - math.pi / 2
            self.circle(182 + 22 * math.cos(t), 84 + 22 * math.sin(t), 13)
        self.circle(182, 84, 8, DOT)
        self.path('M 375,595 C 450,610 450,700 380,720')
        self.path('M 150,600 C 90,660 80,740 100,800'); self.glove(102, 832, 30)


class Faithful(Banana):
    def face(self):
        self.circle(232, 330, 10, DOT); self.circle(285, 325, 10, DOT)
        self.path('M 238,372 Q 260,388 282,368', FINE)

    def palm(self, x=72, y=300):
        self.ellipse(x, y, 28, 34)
        for a, b, c, d in ((x - 20, y - 26, x - 26, y - 85), (x - 6, y - 32, x - 8, y - 95),
                           (x + 8, y - 32, x + 12, y - 93), (x + 22, y - 24, x + 30, y - 75)):
            self.path(f'M {a},{b} L {c},{d}')
        self.path(f'M {x + 26},{y + 18} L {x + 56},{y}')

    def male(self):
        self.slender_body(); self.face()
        self.path('M 172,470 L 92,560 L 76,338'); self.palm()
        self.path('M 318,480 C 345,600 352,700 340,800'); self.ellipse(338, 828, 18, 28)
        self.path('M 228,990 L 214,1225'); self.path('M 270,990 L 292,1225')
        self.ellipse(196, 1246, 36, 14); self.ellipse(310, 1246, 36, 14)

    def female(self):
        self.slender_body(); self.face(); self.lashes(318, (232, 285))
        self.path('M 192,74 C 112,50 92,150 126,212 C 140,242 120,280 96,290')        # ponytail
        self.path('M 180,82 C 130,90 120,160 142,205', FINE)
        self.path('M 190,480 C 160,600 158,700 166,800'); self.ellipse(168, 828, 18, 28)
        self.path('M 318,480 C 345,600 352,700 340,800'); self.ellipse(338, 828, 18, 28)
        self.path('M 228,990 L 216,1225'); self.path('M 270,990 L 304,1110 L 290,1225')
        self.ellipse(198, 1246, 34, 13); self.ellipse(306, 1246, 34, 13)


class Classy(Banana):
    def male(self):
        self.classic_body(); self.dot_eyes(); self.legs()
        self.path('M 268,470 Q 230,446 210,482 Q 240,468 268,484 Q 296,468 326,480 Q 306,446 268,470 Z', DOT)  # moustache
        self.path('M 270,575 L 228,548 L 228,602 Z'); self.path('M 270,575 L 312,548 L 312,602 Z'); self.circle(270, 575, 10, DOT)
        self.path('M 120,96 L 262,96'); self.path('M 146,96 L 146,8 L 236,8 L 236,96'); self.path('M 146,70 L 236,70', FINE)
        self.path('M 160,560 Q 70,520 66,365'); self.wave_hand()
        self.path('M 378,600 Q 430,700 418,800'); self.circle(416, 826, 26)
        self.path('M 418,826 L 432,1250'); self.path('M 418,826 Q 412,778 380,792')            # cane

    def female(self):
        self.classic_body(); self.dot_eyes(); self.lashes(); self.legs()
        self.path('M 232,490 Q 268,520 306,486')
        self.ellipse(186, 120, 120, 22); self.path('M 128,112 Q 186,30 244,112')               # sun hat
        self.path('M 132,104 Q 186,90 240,104', FINE)
        for i in range(9):                                                                       # pearls
            t = i / 8
            x = 168 + (360 - 168) * t; y = 600 + 70 * 4 * t * (1 - t) - 20 * t
            self.circle(x, y, 9)
        self.path('M 372,590 Q 440,640 372,720')
        self.path('M 155,600 Q 105,700 120,800'); self.circle(122, 826, 26)
        self.path('M 92,850 L 150,850 L 160,920 L 82,920 Z'); self.path('M 100,850 Q 121,808 142,850', FINE)   # handbag


VARIANTS = {'classic': Classic, 'kawaii': Kawaii, 'rubberhose': RubberHose, 'faithful': Faithful, 'classy': Classy}


def couple(name):
    v = VARIANTS[name]
    m = v(MAN); m.male()
    f = v(WOMAN, mirror=True); f.female()
    return m.svg() + f.svg()


def build(name, out):
    tree = ET.parse(SRC)
    root = tree.getroot()
    n = remove_humans(root)
    root.append(ET.fromstring(f'<g xmlns="{NS["svg"]}" id="bananas">{couple(name)}</g>'))
    tree.write(out)
    return n


def main():
    import sys
    names = sys.argv[1:] or ['classic']
    for name in names:
        out = OUT if name == 'classic' and len(names) == 1 else os.path.join(HERE, f'plaque_banana_{name}.svg')
        build(name, out)
        subprocess.run(['rsvg-convert', '-w', '1400', '-b', '#e0b794', out, '-o', out[:-4] + '.png'], check=True)
        print(name, '->', out)


if __name__ == '__main__':
    main()
