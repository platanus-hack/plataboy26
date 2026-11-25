"""Check footprints and pin meaning against LCSC's own data for the BOM part numbers.

JLCPCB orients parts using LCSC's footprints. Rev. 1.2 uses those footprints directly
(footprints/lcsc.pretty, from `easyeda2kicad --full --lcsc_id <part>`), so this checks:
  1. every assembled part uses the LCSC footprint of the LCSC number in the BOM
  2. polarised parts and ICs: the net on every pin agrees with LCSC's pin names
     (symbol pins from footprints/lcsc.kicad_sym; battery: the '+' printed on the
     LCSC footprint)
  3. the KiCad 0603 footprints match LCSC's 0603 footprints (orientation and pads)

    python verify/check_footprints.py
"""
import csv, math, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist

HERE = os.path.dirname(os.path.abspath(__file__))
FP = os.path.join(HERE, '..', 'footprints')
KICAD_FP = os.environ.get('KICAD_FOOTPRINTS', '')

# expected net for each LCSC pin name: (LCSC part, pin name) -> net
EXPECT = {
    ('C5346354', 'VDD'): 'VCC', ('C5346354', 'VSS'): 'GND', ('C5346354', 'PC1/SDA'): 'SDA',
    ('C5346354', 'PC2/SCL'): 'SCL', ('C5346354', 'PC4/A2'): 'PAD', ('C5346354', 'PA2/OSCO/A0'): 'ACT',
    ('C5346354', 'PD6/A6/OSCI'): 'BEEP', ('C5346354', 'PD4/A7'): 'DIO',
    ('C68393', 'VCC'): 'VCC', ('C68393', 'VSS'): 'GND', ('C68393', 'SDA'): 'SDA', ('C68393', 'SCL'): 'SCL',
    ('C68393', 'A1'): 'GND', ('C68393', 'A2'): 'GND', ('C68393', 'WP'): 'GND', ('C68393', 'NC'): None,
    ('C95297', '+'): 'VCC', ('C95297', '-'): 'BUZ', ('C95297', 'NC'): None,
}
PAD = re.compile(r'\(pad\s+"?([^"\s]*)"?\s+(\w+)\s+\w+\s+\(at\s+([-\d.]+)\s+([-\d.]+)(?:\s+([-\d.]+))?\)\s*\(size\s+([\d.]+)\s+([\d.]+)\)')


def symbol_pins():
    out = {}
    text = open(os.path.join(FP, 'lcsc.kicad_sym')).read()
    for blk in re.split(r'\n  \(symbol "', text)[1:]:
        lcsc = re.search(r'"LCSC Part"\s+"([^"]+)"', blk)
        if lcsc:
            out[lcsc.group(1)] = {num: name for name, num in
                                  re.findall(r'\(pin \w+ \w+.*?\(name "([^"]*)".*?\(number "([^"]*)"', blk, re.S)}
    return out


def pads(path):
    res = []
    for num, kind, x, y, rot, w, h in PAD.findall(open(path).read()):
        w, h = float(w), float(h)
        if rot and round(float(rot)) % 180 == 90: w, h = h, w
        res.append((num, float(x), float(y), w, h))
    return res


def plus_pad(path):
    """Pad nearest to a '+' drawn on the silkscreen (a short horizontal and vertical
    line crossing at their centres)."""
    lines = re.findall(r'\(fp_line \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(layer F.SilkS\)', open(path).read())
    segs = [tuple(map(float, l)) for l in lines]
    hs = [s for s in segs if s[1] == s[3] and 0.5 < abs(s[0] - s[2]) < 1.5]
    vs = [s for s in segs if s[0] == s[2] and 0.5 < abs(s[1] - s[3]) < 1.5]
    for h in hs:
        for v in vs:
            cx, cy = (h[0] + h[2]) / 2, (v[1] + v[3]) / 2
            if abs(v[0] - cx) < 0.05 and abs(h[1] - cy) < 0.05:
                return min(pads(path), key=lambda p: math.hypot(p[1] - cx, p[2] - cy))[0]
    return None


def main():
    comps, nets = netlist.read(os.path.join(HERE, '..', 'build', 'console.net'))
    pin_net = {p: n for n, ps in nets.items() for p in ps}
    bom = {}
    for row in csv.DictReader(open(os.path.join(HERE, '..', 'build', 'bom.csv'))):
        for ref in row['Designator'].split(','): bom[ref] = row['LCSC Part #']
    syms = symbol_pins()
    errors = 0

    def fail(msg):
        nonlocal errors; errors += 1; print('FAIL', msg)

    for ref, c in sorted(comps.items()):
        lib, name = c['footprint'].split(':')
        lcsc = bom.get(ref, '')
        if lib == 'lcsc':
            text = open(os.path.join(FP, 'lcsc.pretty', name + '.kicad_mod')).read()
            m = re.search(r'"LCSC Part" "([^"]+)"', text)
            if not m or m.group(1) != lcsc and not (name.startswith('SOIC-8') or name.startswith('SOP-8')):
                # generic package footprints carry the number they were first fetched for
                fail(f'{ref}: footprint {name} belongs to {m.group(1) if m else "?"}, BOM says {lcsc}')
        for pin_num, pin_name in syms.get(lcsc, {}).items():
            key = (lcsc, pin_name)
            if key in EXPECT:
                got = pin_net.get(f'{ref}.{pin_num}')
                if got != EXPECT[key]: fail(f'{ref} pin {pin_num} ({pin_name}): net {got}, expected {EXPECT[key]}')
                else: print(f'ok   {ref:8} pin {pin_num} {pin_name:12} -> {got}')
        if lcsc == 'C70377':
            plus = plus_pad(os.path.join(FP, 'lcsc.pretty', name + '.kicad_mod'))
            got = pin_net.get(f'{ref}.{plus}')
            if got != 'VBAT': fail(f'{ref}: the + tab (pad {plus}) is on net {got}, expected VBAT')
            else: print(f'ok   {ref:8} pad {plus} next to the LCSC "+" mark -> {got}')

    for kind, mine, theirs in (('R', 'Resistor_SMD.pretty/R_0603_1608Metric', 'R0603'),
                               ('C', 'Capacitor_SMD.pretty/C_0603_1608Metric', 'C0603')):
        a = {p[0]: p for p in pads(os.path.join(KICAD_FP, mine + '.kicad_mod'))}
        b = {p[0]: p for p in pads(os.path.join(FP, '..', 'verify', 'lcsc', theirs + '.kicad_mod'))}
        err = max(math.hypot(a[n][1] - b[n][1], a[n][2] - b[n][2]) for n in a)
        (print if err < 0.15 else fail)(f'{"ok  " if err < 0.15 else ""} {kind} 0603: KiCad vs LCSC pad positions differ by {err:.2f} mm, same orientation and pin 1 side')

    print('RESULT:', 'footprints and pin meanings agree with LCSC' if not errors else f'{errors} problems')
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
