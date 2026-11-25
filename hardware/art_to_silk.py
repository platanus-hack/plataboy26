"""Convert the banana Pioneer plaque (doc/art/plaque_banana_classic.svg) into silkscreen
polygons for the back of the board: every stroke becomes an outline at least 0.16 mm
wide, filled shapes stay filled, everything is merged and mirrored so it reads correctly
from the back. Output: build/plaque_silk.json, polygons in mm relative to the art's
top-left corner (outer ring + holes), read by pcb.py.

    python art_to_silk.py [width_mm]
"""
import json, os, sys
from svgelements import SVG, Shape, Color, Path
from shapely.geometry import LineString, Polygon, Point
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'doc', 'art', 'plaque_banana_classic.svg')
OUT = os.path.join(HERE, 'build', 'plaque_silk.json')
WIDTH_MM = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
MIN_LINE_MM = 0.16


def main():
    svg = SVG.parse(SRC, reify=True)
    W, H = svg.viewbox.width, svg.viewbox.height
    mm = WIDTH_MM / W
    geoms = []
    for e in svg.elements():
        if not isinstance(e, Shape) or e.fill is None and e.stroke is None: continue
        fill = e.fill is not None and e.fill.value is not None and Color(e.fill) != Color('#e0b794') and Color(e.fill).alpha > 0
        stroke = e.stroke is not None and e.stroke.value is not None and Color(e.stroke).alpha > 0
        if not (fill or stroke): continue
        sw = (e.stroke_width or 0) * mm
        p = Path(e)
        for sub in p.as_subpaths():
            sp = Path(sub)
            n = max(8, int(sp.length(error=1e-2) * mm / 0.05))      # one point per 0.05 mm
            pts = [(pt.x * mm, pt.y * mm) for pt in (sp.point(i / n) for i in range(n + 1)) if pt is not None]
            if len(pts) < 2: continue
            closed = abs(pts[0][0] - pts[-1][0]) < 1e-3 and abs(pts[0][1] - pts[-1][1]) < 1e-3
            if fill and closed and len(pts) > 3:
                geoms.append(Polygon(pts).buffer(0))
            if stroke:
                width = max(sw, MIN_LINE_MM)
                geoms.append(LineString(pts).buffer(width / 2, quad_segs=6, cap_style='round', join_style='round'))
    art = unary_union([g for g in geoms if not g.is_empty]).simplify(0.01)
    polys = [art] if art.geom_type == 'Polygon' else list(art.geoms)
    out = []
    for poly in polys:
        if poly.area < 0.005: continue                                 # specks below printer resolution
        mirror = lambda ring: [(round(WIDTH_MM - x, 4), round(y, 4)) for x, y in ring.coords]
        out.append({'outline': mirror(poly.exterior), 'holes': [mirror(h) for h in poly.interiors]})
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump({'width': WIDTH_MM, 'height': round(H * mm, 3), 'polygons': out}, open(OUT, 'w'))
    print(f'{len(out)} polygons, {WIDTH_MM} x {H * mm:.1f} mm, ink area {sum(p.area for p in polys):.0f} mm2 -> {OUT}')


if __name__ == '__main__':
    main()
