"""Front-side logos and texts as silkscreen polygons (build/front_logos.json).

Each item becomes filled polygons, normalised to a height of 1 (x from 0 to the aspect
ratio), so pcb_front.py can place it at any size. Vector logos keep their outlines
(even-odd fill, so letter counters stay open); raster logos are traced with potrace;
texts are set in JetBrains Mono Bold (the event's mono style) and traced.

    python logos_to_silk.py
"""
import json, os, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFont
from svgelements import SVG, Shape, Path, Color
from shapely.geometry import Polygon
from shapely.ops import unary_union
from shapely import affinity

HERE = os.path.dirname(os.path.abspath(__file__))
LOGOS = os.path.join(HERE, 'doc', 'art', 'logos')
FONT = os.path.expanduser('~/Library/Fonts/JetBrainsMono-Bold.ttf')
TEXTS = {'stgo': 'STGO 20-22.NOV.26', 'stgo_city': 'SANTIAGO [CHL]', 'dates': '20-22.NOV.26',
         't_stgo': 'STGO', 't_dates': '20-22', 't_month': 'NOV.26', 'plataboy': 'PLATABOY.26'}


def svg_polygons(path):
    """Filled shapes of an SVG; each path is filled even-odd (subpaths XOR)."""
    svg = SVG.parse(path, reify=True)
    out = []
    for e in svg.elements():
        if not isinstance(e, Shape) or e.fill is None or e.fill.value is None: continue
        if Color(e.fill).alpha == 0: continue
        acc = None
        for sub in Path(e).as_subpaths():
            sp = Path(sub)
            n = max(16, int(sp.length(error=1e-3) / 0.5))
            pts = [(p.x, p.y) for p in (sp.point(i / n) for i in range(n + 1)) if p is not None]
            if len(pts) < 4: continue
            poly = Polygon(pts).buffer(0)
            if poly.is_empty: continue
            acc = poly if acc is None else acc.symmetric_difference(poly)
        if acc is not None: out.append(acc)
    return unary_union(out)


def trace(png_path, invert_alpha=True):
    with tempfile.TemporaryDirectory() as d:
        pbm, svg = os.path.join(d, 'a.pbm'), os.path.join(d, 'a.svg')
        im = Image.open(png_path).convert('RGBA')
        ink = im.getchannel('A').point(lambda a: 0 if a > 128 else 255) if invert_alpha else im.convert('L')
        ink.convert('1').save(pbm)
        subprocess.run(['potrace', '-s', '--flat', '-o', svg, pbm], check=True)
        return svg_polygons(svg)


def text_png(text, path):
    font = ImageFont.truetype(FONT, 400)
    l, t, r, b = font.getbbox(text)
    im = Image.new('RGBA', (r - l + 40, b - t + 40), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((20 - l, 20 - t), text, font=font, fill=(0, 0, 0, 255))
    im.save(path)


def normalise(geom):
    x0, y0, x1, y1 = geom.bounds
    g = affinity.translate(geom, -x0, -y0)
    g = affinity.scale(g, 1 / (y1 - y0), 1 / (y1 - y0), origin=(0, 0))
    polys = [g] if g.geom_type == 'Polygon' else list(g.geoms)
    return {'aspect': (x1 - x0) / (y1 - y0),
            'polygons': [{'outline': [list(map(lambda v: round(v, 5), c)) for c in p.exterior.coords],
                          'holes': [[list(map(lambda v: round(v, 5), c)) for c in h.coords] for h in p.interiors]}
                         for p in polys if p.area > 1e-6]}


def main():
    items = {
        'ph26': svg_polygons(os.path.join(LOGOS, 'ph26.svg')),
        'platanus': svg_polygons(os.path.join(LOGOS, 'platanus-isotype.svg')),
        'buk': svg_polygons(os.path.join(LOGOS, 'buk-accent.svg')),
        'platanus_full': svg_polygons(os.path.join(LOGOS, 'platanus-full.svg')),
        'wordmark': svg_polygons(os.path.join(LOGOS, 'platanus-wordmark.svg')),
        'long': trace(os.path.join(LOGOS, 'platanus-hack-26-long.png')),
    }
    with tempfile.TemporaryDirectory() as d:
        for key, text in TEXTS.items():
            p = os.path.join(d, key + '.png'); text_png(text, p); items[key] = trace(p)
    # "platanus" lettering on its own, 0.03 mm bolder per side at its printed height
    # (1.75 mm): the brand font's strokes are ~0.15 mm there, right at the print limit
    word = items['wordmark']
    x0 = word.bounds[0]; h = word.bounds[3] - word.bounds[1]
    letters = unary_union([g for g in getattr(word, 'geoms', [word]) if g.bounds[0] > x0 + 1.2 * h])   # drop the mark
    items['letters_bold'] = letters.buffer(0.03 / 1.75 * h)
    out = {k: normalise(v) for k, v in items.items()}
    os.makedirs(os.path.join(HERE, 'build'), exist_ok=True)
    json.dump(out, open(os.path.join(HERE, 'build', 'front_logos.json'), 'w'))
    for k, v in out.items(): print(f'{k:10} aspect {v["aspect"]:.2f}, {len(v["polygons"])} polygons')


if __name__ == '__main__':
    main()
