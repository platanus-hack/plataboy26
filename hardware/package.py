"""Manufacturing package for the fab/assembly house: release/plataboy26_v1.3_manufacturing.zip

    python package.py        (after make.sh and doc/3d/build.sh)

Contents:
  00_ORDER_SHEET.pdf       one document: specs, colours, assembly notes, polarity checks, BOM
  01_gerbers/              Gerbers + drill (+ the same as one zip, ready to upload)
  02_assembly/             JLCPCB BOM + CPL, detailed BOM (xlsx/csv), assembly drawings
  03_renders/              front/back/angled/side renders, measured heights
  04_3d/                   STEP model of the assembled board
  05_kicad_source/         board, project, netlist
"""
import csv, os, shutil, subprocess, sys, zipfile, datetime, html

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NAME = 'plataboy26_v1.3.1'
OUT = os.path.join(HERE, 'release', NAME)
QTY = 230
KICAD = os.environ.get('KICAD_APP') or next(p for p in ('/Applications/KiCad/KiCad.app',
        os.path.join(ROOT, 'tools/kicad-dl/mnt/KiCad/KiCad.app')) if os.path.isdir(p))
KCLI = os.path.join(KICAD, 'Contents/MacOS/kicad-cli')
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
PCB = os.path.join(HERE, 'fab', 'console.kicad_pcb')

# manufacturer data per LCSC number (from the LCSC listings used for the footprints)
PARTS = {
    'C5346354': ('WCH', 'CH32V003J4M6', 'RISC-V MCU 48 MHz, 16 KB flash, 2 KB SRAM, SOP-8'),
    'C68393': ('Microchip', 'AT24CM01-SSHM-T', 'EEPROM 1 Mbit I2C, 1.7-5.5 V, SOIC-8 150 mil'),
    'C23873': ('ReliaPro', 'SMD Tactile Switch 6*6*6mm', 'Tact switch 6x6 mm SMD, 4 pads'),
    'C95297': ('HNDZ', 'MLT-5030', 'Magnetic buzzer 5x5 mm SMD'),
    'C70377': ('Q&J', 'CR2032-BS-6-1', 'CR2032 coin cell holder, SMD'),
    'C695418': ('RI SHENG', 'SS-1290L-GL L=1.5mm', 'Slide switch SPDT SMD'),
    'C49257': ('BOOMELE', 'Header 2.54 mm 1x3P', 'Pin header 1x3 2.54 mm, vertical'),
    'C14663': ('YAGEO', 'CC0603KRX7R9BB104', 'MLCC 100 nF 50 V X7R 0603'),
    'C19702': ('Samsung', 'CL10A106KP8NNNC', 'MLCC 10 uF 10 V X5R 0603'),
    'C23345': ('UNI-ROYAL (UniOhm)', '0603WAF220JT5E', 'Resistor 22 R 1% 0603'),
    'C25981': ('UNI-ROYAL (UniOhm)', '0603WAF8201T5E', 'Resistor 8.2 k 1% 0603'),
    'C23018': ('UNI-ROYAL (UniOhm)', '0603WAF3901T5E', 'Resistor 3.9 k 1% 0603'),
    'C4184': ('UNI-ROYAL (UniOhm)', '0603WAF2002T5E', 'Resistor 20 k 1% 0603'),
    'C22975': ('UNI-ROYAL (UniOhm)', '0603WAF2001T5E', 'Resistor 2 k 1% 0603'),
    'C25804': ('UNI-ROYAL (UniOhm)', '0603WAF1002T5E', 'Resistor 10 k 1% 0603'),
    'C25803': ('UNI-ROYAL (UniOhm)', '0603WAF1003T5E', 'Resistor 100 k 1% 0603'),
}
DNP = {'H2': 'Programming header: NOT fitted (units are flashed through a clip / pogo pins on the pads).'}
OLED_NOTE = ('Through-hole, hand/selective soldering after SMT. Module sourced by the assembler (as quoted). '
             'Front side, display facing up, module body over the board area above the buttons; '
             'pin 1 = GND = square pad. Seat flat on its header spacer, parallel and centred; '
             'solder on the back (max 350 C, < 10 s per pin); keep the display film on.')
SIDE = {}


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def boms():
    jlc = list(csv.DictReader(open(os.path.join(HERE, 'fab', 'console_v1.3_bom.csv'))))
    for r in csv.DictReader(open(os.path.join(HERE, 'fab', 'console_v1.3_cpl.csv'))):
        SIDE[r['Designator']] = r['Layer']
    rows, jlc_rows = [], []
    for r in jlc:
        refs = sorted(r['Designator'].split(','), key=lambda s: (s.rstrip('0123456789'), int(s[len(s.rstrip('0123456789')):] or 0)))
        lcsc = r['LCSC Part #']
        mfr, mpn, desc = PARTS.get(lcsc, ('', '', ''))
        if 'H1' in refs:
            mfr, mpn = 'ShenZhen GoldenMorning', 'EB096-01-04G-WB'
            desc = '0.96 in OLED module 128x64, SSD1306, I2C, 4-pin header (GND VDD SCL SDA)'
            lcsc = 'sourced by assembler'
        dnp = [x for x in refs if x in DNP]
        note = ' '.join(DNP[x] for x in dnp) or (OLED_NOTE if 'H1' in refs else r['Notes'])
        side = ', '.join(sorted({SIDE.get(x, 'THT') for x in refs if x not in DNP})) or '-'
        if 'H1' in refs: side = 'Top (THT, hand solder)'
        value = 'CR2032 holder' if lcsc == 'C70377' else ('OLED 0.96in' if 'H1' in refs else r['Comment'])
        n = len(refs)
        rows.append({'Designator': ', '.join(refs), 'Qty/board': n, f'Qty x{QTY}': 0 if dnp else n * QTY, 'Value': value,
                     'Description': desc,
                     'Manufacturer': mfr, 'MPN': mpn, 'LCSC': lcsc, 'Footprint': r['Footprint'],
                     'Side': side, 'Assemble': 'NO (DNP)' if dnp else 'yes', 'Notes': note})
        if not dnp:
            jlc_rows.append({'Comment': value, 'Designator': ','.join(refs), 'Footprint': r['Footprint'], 'LCSC Part #': lcsc})
    return rows, jlc_rows


def write_boms(rows, jlc_rows, d):
    with open(os.path.join(d, f'{NAME}_BOM_jlcpcb.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(jlc_rows[0])); w.writeheader(); w.writerows(jlc_rows)
    with open(os.path.join(d, f'{NAME}_BOM_detailed.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    from openpyxl import Workbook
    from openpyxl.styles import Font, PatternFill, Alignment
    wb = Workbook(); ws = wb.active; ws.title = 'BOM'
    ws.append(list(rows[0]))
    for r in rows: ws.append(list(r.values()))
    for c in ws[1]: c.font = Font(bold=True, color='FFFFFF'); c.fill = PatternFill('solid', fgColor='222222')
    for col, width in zip('ABCDEFGHIJKL', (26, 9, 9, 16, 40, 14, 26, 10, 34, 12, 11, 70)):
        ws.column_dimensions[col].width = width
    for row in ws.iter_rows(min_row=2):
        for c in row: c.alignment = Alignment(wrap_text=True, vertical='top')
    ws.freeze_panes = 'A2'
    wb.save(os.path.join(d, f'{NAME}_BOM_detailed.xlsx'))
    shutil.copy(os.path.join(HERE, 'fab', 'console_v1.3_cpl.csv'), os.path.join(d, f'{NAME}_CPL_jlcpcb.csv'))


def drawings(d):
    env = dict(os.environ, CONSOLE_FP=os.path.join(HERE, 'footprints'))
    for side, layers, extra in (('top', 'F.Fab,Edge.Cuts', []), ('bottom', 'B.Fab,Edge.Cuts', ['--mirror'])):
        out = os.path.join(d, f'{NAME}_assembly_{side}.pdf')
        subprocess.run([KCLI, 'pcb', 'export', 'pdf', '-l', layers, '--mode-single', '--sketch-pads-on-fab-layers',
                        '--crossout-DNP-footprints-on-fab-layers', '--black-and-white', '--scale', '3',
                        *extra, '-o', out, PCB], check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def sheet(rows, d):
    r3 = os.path.join(OUT, '03_renders')
    img = lambda n: 'file://' + os.path.join(r3, n)
    trs = ''.join('<tr>' + ''.join(f'<td>{html.escape(str(r[k]))}</td>' for k in
                  ('Designator', 'Qty/board', f'Qty x{QTY}', 'Value', 'MPN', 'LCSC', 'Side', 'Assemble')) + '</tr>' for r in rows)
    today = datetime.date.today().isoformat()
    doc = f'''<!doctype html><html><head><meta charset="utf-8"><title>{NAME} order sheet</title><style>
    @page {{ size: A4; margin: 14mm; }}
    body {{ font-family: -apple-system, Helvetica, Arial, sans-serif; font-size: 10.5pt; color: #111; }}
    h1 {{ font-size: 18pt; margin: 0 0 2mm; }} h2 {{ font-size: 12pt; margin: 6mm 0 2mm; border-bottom: 1px solid #999; }}
    table {{ border-collapse: collapse; width: 100%; font-size: 9pt; }} td, th {{ border: 1px solid #bbb; padding: 3px 5px; vertical-align: top; }}
    th {{ background: #222; color: #fff; text-align: left; }} .kv td:first-child {{ width: 38%; font-weight: 600; }}
    .imgs {{ display: flex; gap: 4mm; justify-content: center; }} .imgs img {{ max-height: 58mm; max-width: 48%; border: 1px solid #ccc; }}
    .warn {{ background: #fff4e6; border: 1px solid #e8590c; padding: 3mm; }} .pb {{ page-break-before: always; }}
    </style></head><body>
    <h1>{NAME} — PCB + assembly order sheet</h1>
    <div>Platanus Hack 26 game console (CH32V003). Quantity: <b>{QTY} boards</b>. Revision 1.3, {today}.</div>
    <h2>1. PCB fabrication</h2><table class="kv">
    <tr><td>Files</td><td>01_gerbers/{NAME}_gerbers.zip (Gerber X2 + Excellon, KiCad 10)</td></tr>
    <tr><td>Size</td><td>35.56 × 54.80 mm, rounded corners R2.54</td></tr>
    <tr><td>Layers / thickness</td><td>2 layers, FR-4, 1.6 mm, 1 oz copper</td></tr>
    <tr><td>Solder mask / silkscreen</td><td><b>BLACK</b> solder mask, <b>WHITE</b> silkscreen, both sides</td></tr>
    <tr><td>Surface finish</td><td>HASL lead-free (ENIG also fine)</td></tr>
    <tr><td>Min. track / clearance / drill</td><td>0.15 mm / 0.15 mm / 0.3 mm (vias 0.6/0.3 mm, tented)</td></tr>
    <tr><td>Holes</td><td>3 × Ø2.2 mm non-plated mounting holes (M2)</td></tr>
    <tr><td>Panelisation</td><td>at the fab's discretion</td></tr></table>
    <h2>2. Assembly (PCBA)</h2><table class="kv">
    <tr><td>Files</td><td>02_assembly/{NAME}_BOM_jlcpcb.csv, {NAME}_CPL_jlcpcb.csv (24 SMT placements + H1)</td></tr>
    <tr><td>Sides</td><td><b>Both sides SMT</b>. Top: 6 tact switches, buzzer, slide switch. Bottom: MCU, EEPROM, coin cell holder, resistors, capacitors.</td></tr>
    <tr><td>Through-hole (hand solder, last)</td><td><b>H1: OLED module EB096-01-04G-WB</b> (as quoted), front side. See section 3.</td></tr>
    <tr><td>Not fitted (DNP)</td><td>H2 programming header</td></tr>
    <tr><td>Not included</td><td>CR2032 cells, firmware programming (unless agreed separately)</td></tr></table>
    <h2>3. OLED module (H1) — hand soldering</h2><div class="warn">
    <b>Front side</b> (the side with the buttons), display facing up. The module body covers the board area <b>above the buttons</b>, as in the front render below.<br>
    <b>Pin 1 = GND = square pad</b>, the leftmost of the 4 holes (front view, buttons at the bottom). Module pin order GND, VDD, SCL, SDA matches left to right.<br>
    Seat the module <b>flat on its plastic header spacer</b>, parallel to the board and centred (it clears the power switch by only ~0.4 mm). Solder from the back, max 350 °C and &lt; 10 s per pin; trim pin tails to ≤ 1.5 mm. Keep the protective film on the display.<br>
    Solder H1 <b>after</b> SMT reflow.</div>
    <h2>4. Please check orientation in the placement preview</h2><div class="warn">
    <b>U1</b> CH32V003J4M6 and <b>U2</b> AT24CM01: pin 1 per the dot on the assembly drawing (bottom side).<br>
    <b>B1</b> coin cell holder: the <b>+</b> tab goes to the pad marked + (polarised; reversed = battery shorted).<br>
    <b>BUZZER1</b> MLT-5030: pin 2 = +, the pad layout is asymmetric (3 pads).<br>
    All footprints are LCSC's own footprints for these part numbers.</div>
    <div class="imgs" style="margin-top:4mm"><img src="{img('front.png')}"><img src="{img('back.png')}"></div>
    <h2 class="pb">5. Parts list</h2><table><tr><th>Designators</th><th>Qty/board</th><th>Qty ×{QTY}</th><th>Value</th><th>MPN</th><th>LCSC</th><th>Side</th><th>Assemble</th></tr>{trs}</table>
    <p style="font-size:9pt">Alternative for U2: ST M24M01-RMN6TP (same pinout). Do <b>not</b> substitute 24LC1025 (different pinout) or AT24CM01-SHD-T / C36057 (208 mil wide body).
    Full details incl. manufacturers and notes: 02_assembly/{NAME}_BOM_detailed.xlsx.</p>
    <h2>6. Reference</h2><div class="imgs"><img src="{img('iso_top.png')}"><img src="{img('iso_bottom.png')}"></div>
    <img src="{img('side_annotated.png')}" style="width:100%;margin-top:4mm;border:1px solid #ccc">
    <p style="font-size:9pt">Heights above the board: buttons 6.0 mm, OLED glass 5.65 mm (EB096-01-04G-WB on its 2.5 mm pin-header spacer); below: coin cell holder 5.6 mm.
    Contact: github.com/platanus-hack/plataboy26</p></body></html>'''
    src = os.path.join(OUT, '_sheet.html'); open(src, 'w').write(doc)
    run(CHROME, '--headless=new', '--no-pdf-header-footer', '--allow-file-access-from-files',
        f'--print-to-pdf={os.path.join(d, "00_ORDER_SHEET.pdf")}', 'file://' + src)
    os.remove(src)


def main():
    if os.path.isdir(OUT): shutil.rmtree(OUT)
    dirs = {k: os.path.join(OUT, k) for k in ('01_gerbers', '02_assembly', '03_renders', '04_3d', '05_kicad_source')}
    for p in dirs.values(): os.makedirs(p)
    # 01 gerbers
    g = os.path.join(HERE, 'build', 'fab', 'gerbers')
    for f in sorted(os.listdir(g)): shutil.copy(os.path.join(g, f), dirs['01_gerbers'])
    shutil.copy(os.path.join(HERE, 'fab', 'console_v1.3_gerbers.zip'), os.path.join(dirs['01_gerbers'], f'{NAME}_gerbers.zip'))
    # 02 assembly
    rows, jlc_rows = boms(); write_boms(rows, jlc_rows, dirs['02_assembly']); drawings(dirs['02_assembly'])
    # 03 renders
    for f in ('front.png', 'back.png', 'iso_top.png', 'iso_bottom.png', 'side_front.png', 'side_right.png', 'side_annotated.png', 'heights.json'):
        shutil.copy(os.path.join(HERE, 'doc', '3d', f), dirs['03_renders'])
    # 04 3d, 05 source
    shutil.copy(os.path.join(HERE, 'fab', 'console_v1.3.step'), os.path.join(dirs['04_3d'], f'{NAME}.step'))
    for f in ('console.kicad_pcb', 'console.kicad_pro'):
        shutil.copy(os.path.join(HERE, 'fab', f), os.path.join(dirs['05_kicad_source'], f.replace('console', NAME)))
    shutil.copy(os.path.join(HERE, 'build', 'console.net'), os.path.join(dirs['05_kicad_source'], f'{NAME}.net'))
    # order sheet (needs the renders in place)
    sheet(rows, OUT)
    # zip
    z = os.path.join(HERE, 'release', f'{NAME}_manufacturing.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for base, _, files in os.walk(OUT):
            for f in sorted(files):
                p = os.path.join(base, f); zf.write(p, os.path.join(NAME, os.path.relpath(p, OUT)))
    print(z, f'{os.path.getsize(z) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
