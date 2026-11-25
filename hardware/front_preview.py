"""Try front-side logo layouts on the routed board (KiCad's Python).

Takes build/console.kicad_pcb, removes the front texts a layout replaces, adds the logos
from build/front_logos.json, and saves build/front_<layout>.kicad_pcb (render it with
kicad-cli). Silkscreen does not affect copper, so no re-routing is needed.

    python front_preview.py <layout>...
"""
import json, math, os, sys
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
ORG = (100.0, 60.0)
mm = pcbnew.FromMM
LOGOS = json.load(open(os.path.join(HERE, 'build', 'front_logos.json')))

# layout: list of (logo, centre x, centre y, height mm, rotation deg CCW); 'drop' = texts removed
LAYOUTS = {
    'A': {'drop': {'RISC-V MINI GAME CONSOLE', '0.96" OLED', 'PWR', 'PROG'},
          'items': [('ph26', 17.78, 29.4, 2.9, 0),
                    ('long', 2.6, 28.0, 2.4, 90),
                    ('stgo', 32.95, 23.6, 1.0, 90),
                    ('buk', 3.8, 45.3, 2.5, 0),
                    ('platanus', 5.7, 48.6, 2.5, 0)]},
    'B': {'drop': {'RISC-V MINI GAME CONSOLE', '0.96" OLED', 'PWR', 'PROG'},
          'items': [('platanus', 2.4, 29.4, 2.9, 0),
                    ('ph26', 12.6, 29.4, 2.9, 0),
                    ('stgo_city', 25.9, 28.6, 0.95, 0),
                    ('dates', 25.9, 30.0, 0.95, 0),
                    ('long', 32.95, 24.1, 1.8, 90),
                    ('buk', 3.8, 45.6, 2.6, 0)]},
    'C': {'drop': {'RISC-V MINI GAME CONSOLE', '0.96" OLED', 'PWR', 'PROG'},
          'items': [('ph26', 13.45, 29.4, 2.9, 0),
                    ('long', 28.45, 29.4, 1.6, 0),
                    ('stgo', 2.6, 28.0, 1.15, 90),
                    ('buk', 32.95, 23.0, 4.2, 90),
                    ('platanus', 26.3, 40.2, 3.0, 0)]},
    # A + bottom-left mounting hole removed
    'A2': {'drop': {'RISC-V MINI GAME CONSOLE', '0.96" OLED', 'PWR', 'PROG'}, 'remove': {'MH3'},
           'items': [('ph26', 11.65, 29.4, 2.6, 0),
                     ('platanus_full', 25.6, 29.4, 2.4, 0),
                     ('long', 2.6, 28.0, 2.4, 90),
                     ('stgo', 32.95, 23.6, 1.0, 90),
                     ('buk', 3.8, 47.0, 2.8, 0)]},
    'A3': {'drop': {'RISC-V MINI GAME CONSOLE', '0.96" OLED', 'PWR', 'PROG'}, 'remove': {'MH3'},
           'items': [('ph26', 17.78, 29.4, 2.9, 0),
                     ('long', 2.6, 28.0, 2.4, 90),
                     ('stgo', 32.95, 23.6, 1.0, 90),
                     ('platanus', 3.8, 45.3, 2.3, 0),
                     ('letters_bold', 3.8, 47.6, 1.75, 0),
                     ('buk', 3.8, 49.4, 1.9, 0)]},
}


def add_logo(board, key, cx, cy, height, rot):
    logo = LOGOS[key]
    a = math.radians(rot); c, s = math.cos(a), math.sin(a)
    w = logo['aspect'] * height
    def xy(u, v):           # normalised (0..aspect, 0..1) -> board, rotated about the centre
        x, y = u * height - w / 2, v * height - height / 2
        return mm(ORG[0] + cx + x * c + y * s), mm(ORG[1] + cy - x * s + y * c)
    for p in logo['polygons']:
        ps = pcbnew.SHAPE_POLY_SET(); ps.NewOutline()
        for u, v in p['outline']: ps.Append(*xy(u, v))
        for h in p['holes']:
            ps.NewHole()
            for u, v in h: ps.Append(*xy(u, v), 0, ps.HoleCount(0) - 1)
        ps.Fracture()
        sh = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY); sh.SetLayer(pcbnew.F_SilkS)
        sh.SetPolyShape(ps); sh.SetFilled(True); sh.SetWidth(0); board.Add(sh)


def build(name):
    lay = LAYOUTS[name]
    board = pcbnew.LoadBoard(os.path.join(HERE, 'build', 'console.kicad_pcb'))
    for d in list(board.GetDrawings()):
        if isinstance(d, pcbnew.PCB_TEXT) and d.GetLayer() == pcbnew.F_SilkS and d.GetText() in lay['drop']:
            board.Remove(d)
    for ref in lay.get('remove', ()):
        board.Remove(board.FindFootprintByReference(ref))
    for item in lay['items']: add_logo(board, *item)
    if lay.get('remove'): pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    out = os.path.join(HERE, 'build', f'front_{name}.kicad_pcb')
    board.Save(out)
    return out


if __name__ == '__main__':
    for n in sys.argv[1:] or LAYOUTS:
        print(build(n))
