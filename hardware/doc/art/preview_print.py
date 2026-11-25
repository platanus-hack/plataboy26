"""Simulate the plaque printed as PCB silkscreen: white ink on the board colour, lines
thickened to the 0.15 mm minimum, rasterised at the printer's ~0.05 mm dot pitch.

    python preview_print.py <width_mm> <out.png> [plaque.svg]
"""
import re, subprocess, sys
W_MM = float(sys.argv[1]); OUT = sys.argv[2]
UNITS = 3063.0                      # plaque width in SVG units
MIN_LINE = 0.15 / W_MM * UNITS      # 0.15 mm in SVG units
s = open(sys.argv[3] if len(sys.argv) > 3 else 'plaque_banana.svg').read()
s = re.sub(r'stroke-width:([\d.]+)', lambda m: f'stroke-width:{max(float(m.group(1)), MIN_LINE):.1f}', s)
s = s.replace('stroke:#000000', 'stroke:#ffffff').replace('stroke:#000', 'stroke:#fff').replace('fill:#000', 'fill:#fff')
s = re.sub(r'fill:#e0b794', 'fill:none', s)
open('/tmp/_print.svg', 'w').write(s)
px = int(W_MM / 0.05)               # printer resolution ~0.05 mm
subprocess.run(['rsvg-convert', '-w', str(px), '-b', '#24432a', '/tmp/_print.svg', '-o', '/tmp/_print.png'], check=True)
subprocess.run(['magick', '/tmp/_print.png', '-threshold', '55%', '-fill', '#24432a', '-opaque', 'black',
                '-filter', 'point', '-resize', '1000x', OUT], check=True)
print(f'{W_MM} mm wide: min line {MIN_LINE:.0f} units, {px} dots across -> {OUT}')
