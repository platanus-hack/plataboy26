"""Recover the electrical netlist of a 2-layer board from its Gerber + drill files.

Rasterises top and bottom copper, labels connected copper areas, joins the layers
through every plated hole (vias and THT pads) and reports which probe points (pad
centres) end up on the same net. Used to check that our redrawn rev 1.2 design is
electrically identical to wagiminator's published rev 1.1 board.

    python gerber_nets.py <gerber_dir> [probes.json]
"""
import json, math, os, sys, warnings
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from gerbonara import GerberFile, ExcellonFile
from gerbonara import graphic_primitives as gp
from gerbonara.utils import MM

warnings.filterwarnings('ignore')
PX = 40                                   # pixels per mm
W, H = 35.56, 50.8                        # board size (mm), origin top left, y down


def raster(path):
    """Copper of one layer as a boolean image. Drawn primitive by primitive in file
    order with dark/clear polarity (SVG converters lose the cut-outs of the keyhole
    contours EasyEDA uses for copper pours)."""
    img = Image.new('1', (int(W * PX), int(H * PX)), 0)
    d = ImageDraw.Draw(img)
    xy = lambda x, y: (x * PX, -y * PX)
    for o in GerberFile.open(path).objects:
        for p in o.to_primitives(MM):
            c = 1 if p.polarity_dark else 0
            if isinstance(p, gp.Line):
                r = p.width / 2 * PX
                d.line([xy(p.x1, p.y1), xy(p.x2, p.y2)], fill=c, width=max(1, round(p.width * PX)))
                for (x, y) in ((p.x1, p.y1), (p.x2, p.y2)):
                    cx, cy = xy(x, y); d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=c)
            elif isinstance(p, gp.Circle):
                cx, cy = xy(p.x, p.y); r = p.r * PX
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=c)
            elif isinstance(p, gp.Rectangle):
                co, si = math.cos(p.rotation), math.sin(p.rotation)
                pts = [(p.x + dx * co - dy * si, p.y + dx * si + dy * co)
                       for dx, dy in ((-p.w/2, -p.h/2), (p.w/2, -p.h/2), (p.w/2, p.h/2), (-p.w/2, p.h/2))]
                d.polygon([xy(*q) for q in pts], fill=c)
            elif isinstance(p, gp.ArcPoly):
                d.polygon([xy(*q) for q in p.outline], fill=c)
    return np.array(img, dtype=bool)


def holes(path):
    out = []
    for o in ExcellonFile.open(path).objects:
        (x0, y0), (x1, y1) = o.bounding_box('mm')
        out.append(((x0 + x1) / 2, -(y0 + y1) / 2))
    return out


def find_files(gdir):
    """Top copper, bottom copper and plated drill files (EasyEDA or KiCad names)."""
    import glob
    def one(*pats):
        for pat in pats:
            m = sorted(glob.glob(os.path.join(gdir, pat)))
            if m: return m
        raise SystemExit(f'no {pats} in {gdir}')
    return (one('Gerber_TopLayer.GTL', '*-F_Cu.gtl')[0], one('Gerber_BottomLayer.GBL', '*-B_Cu.gbl')[0],
            one('Drill_PTH_Through*.DRL', '*-PTH.drl'))


def board_size(gdir):
    """Width and height of the outline (origin at the board's top-left corner)."""
    import glob
    f = (glob.glob(os.path.join(gdir, '*Edge_Cuts*')) + glob.glob(os.path.join(gdir, '*.GKO')))[0]
    (x0, y0), (x1, y1) = GerberFile.open(f).bounding_box(MM)
    return round(x1, 2), round(-y0, 2)


def nets(gdir, probes):
    global W, H
    W, H = board_size(gdir)
    ftop, fbot, drills = find_files(gdir)
    top, bot = raster(ftop), raster(fbot)
    lt, nt = ndimage.label(top)
    lb, nb = ndimage.label(bot)
    parent = list(range(nt + nb + 1))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]; a = parent[a]
        return a

    def at(lbl, x, y, off):
        px, py = int(round(x * PX)), int(round(y * PX))
        v = lbl[py, px]
        return v + off if v else 0

    for (x, y) in [h for d in drills for h in holes(d)]:
        a, b = at(lt, x, y, 0), at(lb, x, y, nt)
        if a and b: parent[find(a)] = find(b)

    groups = {}
    for name, (x, y, side) in probes.items():
        v = at(lt, x, y, 0) if side == 'top' else at(lb, x, y, nt)
        key = find(v) if v else ('unconnected', name)
        groups.setdefault(key, []).append(name)
    return sorted(sorted(g) for g in groups.values())


if __name__ == '__main__':
    probes = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else {}
    for g in nets(sys.argv[1], probes):
        print(' '.join(g))
