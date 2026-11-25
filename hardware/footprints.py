"""Project footprints (footprints/console.pretty) for parts that have no LCSC footprint:
the OLED module's header, pads as on rev. 1.1. All assembled parts use LCSC's own
footprints (footprints/lcsc.pretty, `easyeda2kicad --full --lcsc_id ...`).

    python footprints.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'footprints', 'console.pretty')


def smd(num, x, y, w, h, shape='roundrect'):
    extra = ' (roundrect_rratio 0.15)' if shape == 'roundrect' else ''
    return (f'  (pad "{num}" smd {shape} (at {x:g} {y:g}) (size {w:g} {h:g}) '
            f'(layers "F.Cu" "F.Paste" "F.Mask"){extra})')


def tht(num, x, y, w, h, drill, shape='circle'):
    return (f'  (pad "{num}" thru_hole {shape} (at {x:g} {y:g}) (size {w:g} {h:g}) (drill {drill:g}) '
            f'(layers "*.Cu" "*.Mask"))')


def npth(x, y, d):
    return f'  (pad "" np_thru_hole circle (at {x:g} {y:g}) (size {d:g} {d:g}) (drill {d:g}) (layers "*.Cu" "*.Mask"))'


def rect(layer, x0, y0, x1, y1, width):
    return f'  (fp_rect (start {x0:g} {y0:g}) (end {x1:g} {y1:g}) (stroke (width {width:g}) (type solid)) (fill none) (layer "{layer}"))'


def line(layer, x0, y0, x1, y1, width=0.12):
    return f'  (fp_line (start {x0:g} {y0:g}) (end {x1:g} {y1:g}) (stroke (width {width:g}) (type solid)) (layer "{layer}"))'


def circle(layer, x, y, r, width=0.12):
    return f'  (fp_circle (center {x:g} {y:g}) (end {x + r:g} {y:g}) (stroke (width {width:g}) (type solid)) (fill none) (layer "{layer}"))'


def footprint(name, descr, body, crt, attr='smd', ref_y=None, model=None):
    x0, y0, x1, y1 = crt
    ref_y = y0 - 1 if ref_y is None else ref_y
    lines = [
        f'(footprint "{name}"',
        '  (version 20241229)', '  (generator "console_footprints")', '  (layer "F.Cu")',
        f'  (descr "{descr}")',
        f'  (attr {attr})',
        f'  (property "Reference" "REF**" (at 0 {ref_y:g} 0) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))',
        f'  (property "Value" "{name}" (at 0 {y1 + 1:g} 0) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))',
        rect('F.CrtYd', x0, y0, x1, y1, 0.05),
        *body,
    ]
    if model:
        lines.append(f'  (model "{model}" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))')
    lines.append(')')
    with open(os.path.join(OUT, name + '.kicad_mod'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    os.makedirs(OUT, exist_ok=True)

    # OLED module header: 1x4, oval pads, 0.89 mm drill (pin 1 = GND)
    footprint('OLED_Header_1x04_P2.54mm', '1x4 2.54 mm header for 0.96 in SSD1306 I2C OLED module (GND VCC SCL SDA)', [
        *[tht(i + 1, -3.81 + 2.54 * i, 0, 1.27, 2.032, 0.889, 'rect' if i == 0 else 'oval') for i in range(4)],
        rect('F.Fab', -5.08, -1.27, 5.08, 1.27, 0.1),
    ], (-5.4, -1.5, 5.4, 1.5), attr='through_hole', ref_y=-2.2,   # the module's own pins: no part
        model='${CONSOLE_FP}/console.3dshapes/OLED_0.96in_I2C_module.wrl')


LCSC = os.path.join(HERE, 'footprints', 'lcsc.pretty')
MODELS = os.path.join(HERE, 'footprints', 'console.3dshapes')


def oled_model():
    """3D model (VRML) of the supplier's OLED module, GoldenMorning EB096-01-04G-WB (datasheet
    V1.0, mechanical drawing p. 5), in the frame of OLED_Header_1x04_P2.54mm (origin = pin
    row centre): module 27.5 x 27.8 mm, pin row 2.0 mm below its top edge, module PCB
    1.2 mm, 3.15 mm from PCB bottom to glass top, glass 26.7 x 19.26 mm, active area
    21.74 x 11.175 mm, M2 holes in the corners. It sits on the 2.5 mm plastic spacer of
    its pin header, so the glass top is 5.65 mm above the console PCB."""
    W, H, TOP = 27.5, 27.8, -2.0                       # module outline, top edge y
    z_pcb, z_glass, z_top = 2.5, 3.7, 5.65
    x0, x1 = -W / 2, W / 2
    gy0 = TOP + 4.27                                    # glass panel top edge
    boxes = [   # (x0, y0, z0, x1, y1, z1) in mm, footprint frame (y down), colour
        ((-5.08, -1.27, 0.0, 5.08, 1.27, z_pcb), (0.05, 0.05, 0.05)),              # header spacer
        ((x0, TOP, z_pcb, x1, TOP + H, z_glass), (0.05, 0.22, 0.55)),              # module PCB (blue)
        ((-13.35, gy0, z_glass, 13.35, gy0 + 19.26, z_top - 0.2), (0.03, 0.03, 0.035)),   # glass
        ((-12.85, gy0 + 0.5, z_top - 0.2, 12.85, gy0 + 14.9, z_top), (0.02, 0.02, 0.025)),  # polarizer
        ((-10.87, gy0 + 2.1, z_top, 10.87, gy0 + 13.28, z_top + 0.02), (0.07, 0.09, 0.16)),  # active area
        ((-6.0, gy0 + 19.26, z_glass, 6.0, gy0 + 21.0, z_glass + 0.2), (0.85, 0.55, 0.15)),  # FPC
    ]
    for i in range(4):
        x = -3.81 + 2.54 * i
        boxes.append(((x - 0.32, -0.32, -3.0, x + 0.32, 0.32, z_glass + 0.5), (0.85, 0.7, 0.3)))   # pins
    out = ['#VRML V2.0 utf8', '# GoldenMorning EB096-01-04G-WB 0.96in OLED module on a 2.5 mm header (units 0.1 in)']
    u = lambda v: v / 2.54                                      # KiCad VRML unit = 0.1 inch
    for (bx0, by0, bz0, bx1, by1, bz1), (r, g, b) in boxes:
        cx, cy, cz = u((bx0 + bx1) / 2), u(-(by0 + by1) / 2), u((bz0 + bz1) / 2)
        out.append(f'Transform {{ translation {cx:.4f} {cy:.4f} {cz:.4f} children [ Shape {{ '
                   f'appearance Appearance {{ material Material {{ diffuseColor {r} {g} {b} specularColor 0.2 0.2 0.2 }} }} '
                   f'geometry Box {{ size {u(bx1 - bx0):.4f} {u(by1 - by0):.4f} {u(bz1 - bz0):.4f} }} }} ] }}')
    for hx in (x0 + 2.0, x1 - 2.0):                              # corner holes (dark discs)
        for hy in (TOP + 2.0, TOP + H - 2.0):
            out.append(f'Transform {{ translation {u(hx):.4f} {u(-hy):.4f} {u(z_glass + 0.01):.4f} rotation 1 0 0 1.5708 '
                       f'children [ Shape {{ appearance Appearance {{ material Material {{ diffuseColor 0.01 0.01 0.01 }} }} '
                       f'geometry Cylinder {{ radius {u(1.0):.4f} height {u(0.02):.4f} }} }} ] }}')
    os.makedirs(MODELS, exist_ok=True)
    open(os.path.join(MODELS, 'OLED_0.96in_I2C_module.wrl'), 'w').write('\n'.join(out) + '\n')


def fix_lcsc():
    """Repairs to the converted LCSC footprints (idempotent):
    - holes with no copper ring (plastic locating pegs) must be non-plated
    - the programming header's outline would hang over the board edge: drop it
      (the square pad marks pin 1)
    - a stray marker dot in the battery holder footprint, 1.4 mm outside its tab"""
    import re
    for name in os.listdir(LCSC):
        path = os.path.join(LCSC, name); s = open(path).read(); o = s
        s = re.sub(r'\(pad "?"? thru_hole circle (\(at [^)]*\)) \(size ([\d.]+) \2\) \(drill \2\)',
                   r'(pad "" np_thru_hole circle \1 (size \2 \2) (drill \2)', s)
        if name.startswith('BAT-'):     # stray 0.03 mm marker dot far outside the holder
            s = re.sub(r'\n\s*\(fp_circle \(center 15\.65 -8\.00\) \(end 15\.68 -8\.00\)[^\n]*', '', s)
        if name.startswith('HDR-TH_3P'):
            s = '\n'.join(l for l in s.split('\n') if not (l.strip().startswith(('(fp_line', '(fp_circle', '(fp_arc')) and 'F.SilkS' in l))
        if s != o:
            open(path, 'w').write(s); print('fixed', name)


if __name__ == '__main__':
    main()
    fix_lcsc()
    oled_model()
    print('footprints written to', OUT)
