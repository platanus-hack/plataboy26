"""Build the rev. 1.3 PCB (build/console.kicad_pcb) from build/console.net.

Runs with KiCad's own Python (pcbnew module), see make.sh. Steps:
  1. place every rev. 1.1 part at its rev. 1.1 position, picking side/rotation so that
     its pads land on the rev. 1.1 pads (checked against verify/rev11_pads.json)
  2. place the additions: rev. 1.2 EEPROM (U2, C3, R7, R8) on the back, rev. 1.3 A/B/buzzer
     layout on the front and R9, R10 on the back
  3. outline, mounting holes, silkscreen, GND pours on both layers
  4. export Specctra DSN for Freerouting (route.sh imports the session afterwards)

    python pcb.py place       -> build/console_unrouted.kicad_pcb + build/console.dsn
    python pcb.py finish      -> imports build/console.ses, fills zones -> build/console.kicad_pcb
"""
import json, math, os, sys
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'verify'))
import netlist  # noqa: E402

KICAD_FP = os.environ.get('KICAD_FOOTPRINTS', '')
ORG = (100.0, 60.0)                 # board top-left corner on the KiCad page (mm)
W, H, R = 35.56, 54.8, 2.54         # rev. 1.1 was 35.56 x 50.8; rev. 1.3 is 4 mm taller for MH3

# Rev. 1.1 placement (part centre, mm from board top-left, y down). Side and rotation are
# solved from the pad positions. Rev. 1.2 additions get an explicit side and rotation.
PLACE = {
    'UP': (12.7, 34.29), 'LEFT': (6.35, 40.64), 'RIGHT': (19.05, 40.64), 'DOWN': (12.7, 46.99),
    'SW1': (1.834, 12.7),
    'H1': (17.78, 1.905), 'H2': (34.29, 12.7),
    'MH1': (2.54, 2.54), 'MH2': (33.02, 2.54),
    'MH3': (17.78, 52.5),                          # rev. 1.3: centred in the extra 4 mm strip
}
NEW = {   # ref: (x, y, side, rotation)
    # rev. 1.3 diagonal: A up-right, B down-left, buzzer in the bottom-right corner
    'ACT': (31.0, 36.5, 'top', 90),
    'B': (23.9, 47.2, 'top', 0),
    'BUZZER1': (32.1, 45.9, 'top', 0),
    # rev. 1.3 back: room for the banana plaque (ART). Coin cell moved down (cell still
    # inside the outline), small parts in the pockets around it and along the top edge.
    'B1': (17.78, 40.3, 'bot', 180),
    'U2': (8.2, 2.85, 'bot', 90), 'C3': (12.6, 4.4, 'bot', 90),                 # top edge, left
    'U1': (27.2, 2.85, 'bot', 90), 'C1': (34.2, 7.2, 'bot', 90), 'C2': (34.2, 18.0, 'bot', 90),
    'R3': (1.0, 31.5, 'bot', 90), 'R4': (2.6, 31.5, 'bot', 90), 'R5': (4.2, 31.5, 'bot', 90),
    'R6': (5.8, 31.5, 'bot', 90),                                                # left of the cell
    'R1': (30.6, 31.5, 'bot', 90), 'R9': (32.2, 31.5, 'bot', 90), 'R10': (33.8, 31.5, 'bot', 90),
    'R2': (30.5, 47.2, 'bot', 90), 'R7': (32.1, 47.2, 'bot', 90), 'R8': (33.7, 47.2, 'bot', 90),
}
ART = (2.78, 5.8)            # top-left of the banana plaque on the back (build/plaque_silk.json)
TOP_ONLY = {'MH1', 'MH2', 'MH3'}

mm = pcbnew.FromMM
def pt(x, y): return pcbnew.VECTOR2I(mm(ORG[0] + x), mm(ORG[1] + y))
def to_board(v): return (pcbnew.ToMM(v.x) - ORG[0], pcbnew.ToMM(v.y) - ORG[1])


def load_fp(spec):
    lib, name = spec.split(':')
    path = os.path.join(HERE, 'footprints', lib + '.pretty') if lib in ('console', 'lcsc') else os.path.join(KICAD_FP, lib + '.pretty')
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None: raise SystemExit(f'footprint not found: {spec}')
    return fp


def pose(fp, x, y, side, rot):
    if side == 'bot' and fp.GetLayer() == pcbnew.F_Cu: fp.SetLayerAndFlip(pcbnew.B_Cu)
    if side == 'top' and fp.GetLayer() == pcbnew.B_Cu: fp.SetLayerAndFlip(pcbnew.F_Cu)
    fp.SetOrientationDegrees(rot)
    fp.SetPosition(pt(x, y))


def pad_offsets(fp, ref, probes):
    """(probe - pad) vectors for every probed rev. 1.1 pad, None if on the wrong side."""
    out = []
    for pad in fp.Pads():
        key = f'{ref}.{pad.GetNumber()}'
        if key not in probes: continue
        px, py, side = probes[key]
        if (side == 'top') != (fp.GetLayer() == pcbnew.F_Cu): return None
        bx, by = to_board(pad.GetPosition())
        out.append((px - bx, py - by))
    return out or None


def fit(fp, ref, probes, x, y, side, rot):
    """Place with side/rotation, shift so the pads best fit the rev. 1.1 pads; worst error."""
    pose(fp, x, y, side, rot)
    d = pad_offsets(fp, ref, probes)
    if d is None: return 1e9, x, y
    x += sum(a for a, _ in d) / len(d); y += sum(b for _, b in d) / len(d)
    pose(fp, x, y, side, rot)
    return max(math.hypot(a, b) for a, b in pad_offsets(fp, ref, probes)), x, y


def build():
    comps, nets = netlist.read(os.path.join(HERE, 'build', 'console.net'))
    probes = {k: v for k, v in json.load(open(os.path.join(HERE, 'verify', 'rev11_pads.json'))).items() if not k.startswith('_')}
    pin_net = {p: n for n, pins in nets.items() for p in pins}

    board = pcbnew.BOARD()
    board.SetCopperLayerCount(2)
    ds = board.GetDesignSettings()
    nc = ds.m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(mm(0.2)); nc.SetTrackWidth(mm(0.3)); nc.SetViaDiameter(mm(0.6)); nc.SetViaDrill(mm(0.3))
    ds.m_TrackMinWidth = mm(0.15); ds.m_MinClearance = mm(0.15); ds.m_ViasMinSize = mm(0.5)
    ds.m_MinThroughDrill = mm(0.3); ds.m_CopperEdgeClearance = mm(0.3); ds.m_HoleToHoleMin = mm(0.25)

    netinfo = {}
    for name in nets:
        ni = pcbnew.NETINFO_ITEM(board, name); board.Add(ni); netinfo[name] = ni

    report = []
    for ref, c in comps.items():
        fp = load_fp(c['footprint'])
        fp.SetReference(ref); fp.SetValue(c['value'])
        for k in ('LCSC', 'alternative'):                # sourcing info for the BOM, not printed
            if c['fields'].get(k):
                fp.SetField(k, c['fields'][k]); fp.GetFieldByName(k).SetVisible(False)
        board.Add(fp)
        fp.Reference().SetVisible(False)           # no designators on the silkscreen (as rev. 1.1)
        fp.Value().SetVisible(False)
        for pad in fp.Pads():
            n = pin_net.get(f'{ref}.{pad.GetNumber()}')
            if n: pad.SetNet(netinfo[n])
        if ref in NEW:
            x, y, side, rot = NEW[ref]; pose(fp, x, y, side, rot)
            report.append(f'{ref:8} new       {side} rot {rot}')
        elif ref in TOP_ONLY:
            pose(fp, *PLACE[ref], 'top', 0)
        else:
            best = None
            for side in ('top', 'bot'):
                for rot in (0, 90, 180, 270):
                    e, x, y = fit(fp, ref, probes, *PLACE[ref], side, rot)
                    if best is None or e < best[0]: best = (e, side, rot, x, y)
            e, side, rot, x, y = best
            pose(fp, x, y, side, rot)
            report.append(f'{ref:8} rev1.1    {side} rot {rot:3}  max pad offset vs rev. 1.1: {e:.3f} mm')
            if e > 0.65: raise SystemExit(f'{ref}: pads do not match rev. 1.1 ({e:.2f} mm)')

    art(board)
    front_logos(board)
    outline(board)
    silkscreen(board)
    edge_keepout(board)
    board.GetDesignSettings().SetAuxOrigin(pt(0, 0))       # fab outputs relative to the board corner
    print('\n'.join(report))
    return board


def edge_keepout(board, w=0.45):
    """Thin no-track/no-via rule areas along the edges: Freerouting ignores the board
    edge clearance otherwise. Pads may still sit there (as on rev. 1.1)."""
    for (x0, y0, x1, y1) in ((0, 0, W, w), (0, H - w, W, H), (0, 0, w, H), (W - w, 0, W, H)):
        z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(False)
        z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
        z.SetLayerSet(pcbnew.LSET.AllCuMask())
        o = z.Outline(); o.NewOutline()
        for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)): o.Append(mm(ORG[0] + x), mm(ORG[1] + y))
        board.Add(z)


def outline(board):
    def seg(x0, y0, x1, y1):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT); s.SetLayer(pcbnew.Edge_Cuts)
        s.SetStart(pt(x0, y0)); s.SetEnd(pt(x1, y1)); s.SetWidth(mm(0.1)); board.Add(s)
    def arc(cx, cy, sx, sy, deg):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC); s.SetLayer(pcbnew.Edge_Cuts)
        s.SetCenter(pt(cx, cy)); s.SetStart(pt(sx, sy)); s.SetArcAngleAndEnd(pcbnew.EDA_ANGLE(deg, pcbnew.DEGREES_T), True)
        s.SetWidth(mm(0.1)); board.Add(s)
    seg(R, 0, W - R, 0); seg(W, R, W, H - R); seg(W - R, H, R, H); seg(0, H - R, 0, R)
    arc(W - R, R, W - R, 0, 90); arc(W - R, H - R, W, H - R, 90)
    arc(R, H - R, R, H, 90); arc(R, R, 0, R, 90)


def text(board, s, x, y, layer, size=1.0, bold=False, rot=0):
    t = pcbnew.PCB_TEXT(board); t.SetText(s); t.SetLayer(layer)
    t.SetPosition(pt(x, y)); t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * (0.2 if bold else 0.15))); t.SetTextAngleDegrees(rot)
    if layer == pcbnew.B_SilkS: t.SetMirrored(True)
    board.Add(t)


def silk_rect(board, x0, y0, x1, y1, layer):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_RECT); s.SetLayer(layer)
    s.SetStart(pt(x0, y0)); s.SetEnd(pt(x1, y1)); s.SetWidth(mm(0.15)); board.Add(s)


# under the coin cell; the CC BY-SA credit to wagiminator lives in the linked repository
BACK_LINK = 'github.com/platanus-hack/plataboy26'


def back_texts(board):
    text(board, BACK_LINK, 17.78, 49.4, pcbnew.B_SilkS, 0.8)


def silkscreen(board):
    F, B = pcbnew.F_SilkS, pcbnew.B_SilkS
    silk_rect(board, 5.1, 0.25, 30.45, 27.6, F)                  # OLED module outline
    for i, s in enumerate(('GND', 'VCC', 'SCL', 'SDA')): text(board, s, 13.97 + 2.54 * i, 4.9, F, 0.8, rot=90)
    text(board, 'v1.3', 32.9, 6.4, F, 0.8)
    text(board, 'OFF', 2.2, 7.3, F, 0.8); text(board, 'ON', 2.2, 18.1, F, 0.8)
    text(board, 'VCC GND DIO', 32.2, 12.7, F, 0.8, rot=90)
    text(board, 'A', 27.2, 36.5, F, 1.0, True); text(board, 'B', 27.6, 43.5, F, 1.0, True)

    back_texts(board)


def art_rect():
    a = json.load(open(os.path.join(HERE, 'build', 'plaque_silk.json')))
    return ART[0], ART[1], ART[0] + a['width'], ART[1] + a['height'], a


def art(board):
    """The banana Pioneer plaque as filled polygons on the back silkscreen, plus a rule
    area so that no back-side track or via (both show through the ink) crosses it."""
    x0, y0, x1, y1, a = art_rect()
    for poly in a['polygons']:
        ps = pcbnew.SHAPE_POLY_SET(); ps.NewOutline()
        for x, y in poly['outline']: ps.Append(mm(ORG[0] + x0 + x), mm(ORG[1] + y0 + y))
        for h in poly['holes']:
            ps.NewHole()
            for x, y in h: ps.Append(mm(ORG[0] + x0 + x), mm(ORG[1] + y0 + y), 0, ps.HoleCount(0) - 1)
        ps.Fracture()
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY); s.SetLayer(pcbnew.B_SilkS)
        s.SetPolyShape(ps); s.SetFilled(True); s.SetWidth(0); board.Add(s)
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(False)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.B_Cu); z.SetLayerSet(ls)
    o = z.Outline(); o.NewOutline()
    for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)): o.Append(mm(ORG[0] + x), mm(ORG[1] + y))
    board.Add(z)


# front logos (layout A3), from build/front_logos.json: (logo, centre x, centre y, height, rot)
FRONT_LOGOS = [('ph26', 17.78, 29.4, 2.9, 0),                 # PH->26 under the screen
               ('long', 2.6, 28.0, 2.4, 90),                  # platanus hack [26], left
               ('stgo', 32.95, 23.6, 1.0, 90),                # STGO 20-22.NOV.26, right
               ('plataboy', 5.4, 47.0, 0.95, 0),              # PLATABOY.26, left of DOWN
               ('platanus_full', 7.9, 52.5, 2.6, 0),          # bottom strip, left of MH3
               ('buk', 27.66, 52.5, 2.6, 0)]                  # bottom strip, right of MH3


def front_logos(board):
    logos = json.load(open(os.path.join(HERE, 'build', 'front_logos.json')))
    for key, cx, cy, height, rot in FRONT_LOGOS:
        logo = logos[key]
        a = math.radians(rot); c, s = math.cos(a), math.sin(a)
        w = logo['aspect'] * height
        def xy(u, v):
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


def gnd_zone(board, net, layer):
    z = pcbnew.ZONE(board); z.SetLayer(layer); z.SetNet(net)
    z.SetLocalClearance(mm(0.25)); z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.3)); z.SetThermalReliefSpokeWidth(mm(0.4))
    o = z.Outline(); o.NewOutline()
    for (x, y) in ((0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)):
        o.Append(mm(ORG[0] + x), mm(ORG[1] + y))
    board.Add(z)
    return z


def in_art(x, y, margin=0.6):
    x0, y0, x1, y1, _ = art_rect()
    return x0 - margin < x < x1 + margin and y0 - margin < y < y1 + margin


def stitch(board, net, zones, pitch=3.81, margin=0.8):
    """GND vias wherever both pours have solid copper all around the point (a ring of
    radius `margin` lies in the fill on both layers): ties top and bottom planes together
    (no isolated islands) without coming near another net or a hole."""
    holes = [to_board(p.GetPosition()) for p in board.GetPads() if p.HasHole()]
    ring = [(math.cos(a * math.pi / 8) * margin, math.sin(a * math.pi / 8) * margin) for a in range(16)]
    n, y = 0, 1.9
    while y < H - 1.0:
        x = 1.9
        while x < W - 1.0:
            p = pt(x, y)
            if all(math.hypot(x - hx, y - hy) > 2.0 for hx, hy in holes) and not in_art(x, y) and all(
                    z.HitTestFilledArea(z.GetLayer(), pt(x + dx, y + dy)) for z in zones for dx, dy in [(0, 0)] + ring):
                v = pcbnew.PCB_VIA(board); v.SetPosition(p); v.SetNet(net)
                v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetViaType(pcbnew.VIATYPE_THROUGH)
                board.Add(v); n += 1
            x += pitch
        y += pitch
    return n


RULES = {   # mm; within JLCPCB / PCBWay standard 2-layer capabilities
    'min_clearance': 0.15, 'min_copper_edge_clearance': 0.25, 'min_track_width': 0.15,
    'min_via_diameter': 0.5, 'min_via_annular_width': 0.1, 'min_through_hole_diameter': 0.3,
    'min_hole_to_hole': 0.25, 'min_hole_clearance': 0.25, 'min_silk_clearance': 0.0,
    'min_text_height': 0.8, 'min_text_thickness': 0.12, 'min_microvia_diameter': 0.2,
    'min_microvia_drill': 0.1, 'solder_mask_to_copper_clearance': 0.0,
}


def write_project(out):
    for base in ('console', 'console_unrouted'):
        json.dump({
            'board': {'design_settings': {'rules': RULES, 'defaults': {}}},
            'net_settings': {'classes': [{'name': 'Default', 'clearance': 0.2, 'track_width': 0.3,
                                          'via_diameter': 0.6, 'via_drill': 0.3}],
                             'meta': {'version': 4}},
            'meta': {'filename': base + '.kicad_pro', 'version': 3},
        }, open(os.path.join(out, base + '.kicad_pro'), 'w'), indent=2)


def prune(board):
    """Delete duplicate segments and segments with an end that touches nothing (router
    leftovers: Freerouting sometimes emits a stub twice, back and forth). Works on a
    snapshot and deletes at the end (removing while iterating upsets the SWIG wrappers)."""
    tracks = [t for t in board.GetTracks() if t.GetClass() == 'PCB_TRACK']
    vias = [t for t in board.GetTracks() if t.GetClass() == 'PCB_VIA']
    pads = list(board.GetPads())
    dead, seen = set(), set()
    for i, t in enumerate(tracks):
        a, b = (t.GetStart().x, t.GetStart().y), (t.GetEnd().x, t.GetEnd().y)
        key = (t.GetLayer(), t.GetNetCode(), min(a, b), max(a, b))
        if key in seen or a == b or t.GetLength() < pcbnew.FromMM(0.01): dead.add(i)
        else: seen.add(key)

    def touched(i, p):
        t = tracks[i]
        for j, o in enumerate(tracks):
            if j != i and j not in dead and o.GetLayer() == t.GetLayer() and o.GetNetCode() == t.GetNetCode() \
                    and (o.GetStart() == p or o.GetEnd() == p or o.HitTest(p, 0)):
                return True
        if any(v.GetNetCode() == t.GetNetCode() and v.HitTest(p, 0) for v in vias): return True
        return any(pd.GetNetCode() == t.GetNetCode() and pd.IsOnLayer(t.GetLayer()) and pd.HitTest(p, 0) for pd in pads)

    while True:
        more = {i for i, t in enumerate(tracks) if i not in dead
                and not (touched(i, t.GetStart()) and touched(i, t.GetEnd()))}
        if not more: break
        dead |= more
    for i in dead: board.Delete(tracks[i])
    return len(dead)


DNP = {'H2'}                  # H1 (OLED module) is hand-soldered by the assembler
MASK, SILK = 'Black', 'White'       # board colours (also an order option at the fab)
STACKUP = f'''    (stackup
      (layer "F.SilkS" (type "Top Silk Screen") (color "{SILK}"))
      (layer "F.Paste" (type "Top Solder Paste"))
      (layer "F.Mask" (type "Top Solder Mask") (color "{MASK}") (thickness 0.01))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "core") (thickness 1.51) (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02))
      (layer "B.Cu" (type "copper") (thickness 0.035))
      (layer "B.Mask" (type "Bottom Solder Mask") (color "{MASK}") (thickness 0.01))
      (layer "B.Paste" (type "Bottom Solder Paste"))
      (layer "B.SilkS" (type "Bottom Silk Screen") (color "{SILK}"))
      (copper_finish "HAL lead-free")
      (dielectric_constraints no)
    )
'''


def set_colours(path):
    """Write the stackup (black mask, white silkscreen, 1.6 mm FR4) into the board file;
    pcbnew's Python API does not expose stackup colours."""
    import re as _re
    s = open(path).read()
    s = _re.sub(r'\n\s*\(stackup\b.*?\n\s*\(dielectric_constraints [a-z]+\)\s*\)', '', s, flags=_re.S)
    s = s.replace('(setup\n', '(setup\n' + STACKUP, 1)
    open(path, 'w').write(s)
    print('stackup colours:', MASK, 'mask,', SILK, 'silkscreen')


def fill(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


if __name__ == '__main__':
    out = os.path.join(HERE, 'build')
    if sys.argv[1] == 'place':
        write_project(out)
        board = build()
        board.Save(os.path.join(out, 'console_unrouted.kicad_pcb'))
        pcbnew.ExportSpecctraDSN(board, os.path.join(out, 'console.dsn'))
    elif sys.argv[1] == 'dnp':
        # parts the assembler does not fit: H2 = programming header (units are flashed with
        # a clip / pogo pins)
        board = pcbnew.LoadBoard(os.path.join(out, 'console.kicad_pcb'))
        for fp in board.GetFootprints():
            fp.SetDNP(fp.GetReference() in DNP)
        board.Save(os.path.join(out, 'console.kicad_pcb'))
        print('DNP:', ', '.join(sorted(DNP)))
    elif sys.argv[1] == 'colours':
        set_colours(os.path.join(out, 'console.kicad_pcb'))
    elif sys.argv[1] == 'relogo':
        # silkscreen-only change: replace the front logos on the routed board
        board = pcbnew.LoadBoard(os.path.join(out, 'console.kicad_pcb'))
        for d in list(board.GetDrawings()):
            if d.GetLayer() == pcbnew.F_SilkS and d.GetClass() == 'PCB_SHAPE' and d.GetShape() == pcbnew.SHAPE_T_POLY:
                board.Delete(d)
        front_logos(board)
        for d in list(board.GetDrawings()):
            if d.GetLayer() == pcbnew.B_SilkS and d.GetClass() == 'PCB_TEXT':
                board.Delete(d)
        back_texts(board)
        board.Save(os.path.join(out, 'console.kicad_pcb'))
        print('front logos and back texts replaced')
    elif sys.argv[1] == 'cleanup':
        # delete the track stubs KiCad's DRC reports as dangling, then refill the pours
        import re as _re
        rpt = open(os.path.join(out, 'drc.rpt')).read()
        stubs = _re.findall(r'\[track_dangling\][^\n]*\n[^\n]*\n\s*@\(([\d.]+) mm, ([\d.]+) mm\): Track \[[^\]]*\] on [^,]*, length ([\d.]+) mm', rpt)
        board = pcbnew.LoadBoard(os.path.join(out, 'console.kicad_pcb'))
        n = 0
        for x, y, length in stubs:
            p = pcbnew.VECTOR2I(mm(float(x)), mm(float(y)))
            for t in list(board.GetTracks()):
                if t.GetClass() == 'PCB_TRACK' and (t.GetStart() == p or t.GetEnd() == p) \
                        and abs(pcbnew.ToMM(t.GetLength()) - float(length)) < 0.002:
                    board.Delete(t); n += 1; break
        fill(board)
        board.Save(os.path.join(out, 'console.kicad_pcb'))
        print('removed dangling stubs', n)
    elif sys.argv[1] == 'pads':
        # every copper pad with its position on the fab outputs, for verify/check_fab.py
        board = pcbnew.LoadBoard(os.path.join(out, 'console.kicad_pcb'))
        probes = {}
        for fp in board.GetFootprints():
            for k, pad in enumerate(fp.Pads()):
                if not pad.GetNumber() or pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH: continue
                x, y = to_board(pad.GetPosition())
                side = 'top' if pad.IsOnLayer(pcbnew.F_Cu) else 'bot'
                probes[f'{fp.GetReference()}.{pad.GetNumber()}#{k}'] = [x, y, side]
        json.dump(probes, open(os.path.join(out, 'rev12_pads.json'), 'w'), indent=0)
        print(len(probes), 'pads')
    elif sys.argv[1] == 'finish':
        board = pcbnew.LoadBoard(os.path.join(out, 'console_unrouted.kicad_pcb'))
        if not pcbnew.ImportSpecctraSES(board, os.path.join(out, 'console.ses')):
            raise SystemExit('session import failed')
        gnd = board.FindNet('GND')
        print('removed dangling track stubs', prune(board))
        zones = [gnd_zone(board, gnd, layer) for layer in (pcbnew.F_Cu, pcbnew.B_Cu)]
        fill(board)
        print('stitching vias', stitch(board, gnd, zones))
        fill(board)
        board.Save(os.path.join(out, 'console.kicad_pcb'))
        print('tracks', len(board.GetTracks()))
