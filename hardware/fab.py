"""JLCPCB-style assembly files from KiCad's position file and our BOM.

    python fab.py build/fab/pos.csv build/bom.csv fab/
"""
import csv, os, sys

pos, bom, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
skip = {'MH1', 'MH2', 'MH3'}             # mounting holes
with open(pos) as f, open(os.path.join(out, 'console_v1.3_cpl.csv'), 'w', newline='') as g:
    w = csv.writer(g)
    w.writerow(['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation'])
    for r in csv.DictReader(f):
        if r['Ref'] in skip: continue
        w.writerow([r['Ref'], r['PosX'] + 'mm', r['PosY'] + 'mm', 'Top' if r['Side'] == 'top' else 'Bottom', r['Rot']])
with open(bom) as f, open(os.path.join(out, 'console_v1.3_bom.csv'), 'w', newline='') as g:
    g.write(f.read())
print('wrote', out + '/console_v1.3_cpl.csv,', out + '/console_v1.3_bom.csv')
