# Generates src/game_data.h for the port of "Llegamos tarde a la hackathon" (Paolo /
# paoloose, Platanus Hack 25 arcade), Platanus Hack 26 edition: the hackers (the Platanus
# team, tools/team.py, and three of the original's) and the guard, their sprites taken
# from the original game.js (fetch.sh) and reduced to the 128 x 64 monochrome screen,
# plus the fonts, the falling objects and the texts.
#
#   python3 tools/gen_data.py upstream/game.js src/game_data.h
#
# Sprites, all in the Arduboy bitmap layout (bytes = 8-pixel columns, page after page):
#   climbing frames I, R0, R1 (the left-hand frames are drawn mirrored), 15 x 15: the
#     original 30 x 30 halved, a 2 x 2 cell is lit when 2 of its pixels are opaque
#   portrait: F at full size, 30 x 30, in three tones: light colors lit (with dark seams
#     where two different light colors meet), middle ones checkered, dark ones only
#     outlined
import sys, os
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csef import load, rgb
import team

ORIGINALS = ['Condorito', 'Arturo Vidal', 'Bodoque']   # kept from the original's 8
TEAM_CLIMB = 'Engineer'                                 # the team's shared climbing frames

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..')
lum = lambda c: 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]

def half(rows):
    im = Image.new('1', (15, 15))
    for y in range(15):
        for x in range(15):
            n = 0
            for dy in (0, 1):
                for dx in (0, 1):
                    yy, xx = 2 * y + dy, 2 * x + dx
                    if yy < len(rows) and xx < len(rows[yy]) and rgb(rows[yy][xx]): n += 1
            if n >= 2: im.putpixel((x, y), 1)
    return im

def portrait(rows, hi=150, lo=70):
    H = len(rows)
    at = lambda x, y: rgb(rows[y][x]) if 0 <= y < H and 0 <= x < len(rows[y]) else None
    im = Image.new('1', (30, 30))
    for y in range(min(H, 30)):
        for x in range(min(len(rows[y]), 30)):
            v = at(x, y)
            if not v: continue
            L = lum(v)
            edge = any(at(x + dx, y + dy) is None for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            seam = L > hi and any((u := at(x + dx, y + dy)) and lum(u) > hi and u != v and abs(lum(u) - L) > 18
                                  for dx, dy in ((-1, 0), (0, -1)))
            if edge or (L > hi and not seam) or (lo < L <= hi and (x + y) % 2 == 0): im.putpixel((x, y), 1)
    return im

def art(lines):
    lines = [l for l in lines.strip('\n').split('\n')]
    im = Image.new('1', (max(len(l) for l in lines), len(lines)))
    for y, l in enumerate(lines):
        for x, c in enumerate(l):
            if c == '#': im.putpixel((x, y), 1)
    return im

def bitmap(im):
    """Arduboy layout: for each 8-row page, one byte per column (LSB = top)"""
    out = []
    for p in range((im.height + 7) // 8):
        for x in range(im.width):
            b = 0
            for r in range(8):
                y = p * 8 + r
                if y < im.height and im.getpixel((x, y)): b |= 1 << r
            out.append(b)
    return out

def carr(name, data, typ='uint8_t'):
    s = f'static const {typ} {name}[{len(data)}] = {{\n'
    for i in range(0, len(data), 16):
        s += '  ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 16]) + ',\n'
    return s + '};\n'

# ---- text: the menu's 5 x 8 font plus accented letters and two symbols --------------
import re
font = [[int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]{2}', l)]
        for l in open(os.path.join(ROOT, 'firmware/menu/src/font.h')).read().split('\n') if l.count('0x') == 5]
assert len(font) == 96
def accent(base):                                  # acute accent over a lowercase letter
    g = list(font[ord(base) - 32]); g[2] |= 0x02; g[3] |= 0x01; return g
EXTRA = {                                          # code: glyph
    'á': accent('a'), 'é': accent('e'), 'í': [0x00, 0x44, 0x7D, 0x41, 0x00], 'ó': accent('o'),
    'ú': accent('u'), 'ñ': [0x7C, 0x0A, 0x05, 0x06, 0x79], '¡': [0x00, 0x00, 0x7D, 0x00, 0x00],
    'Ñ': [0x7E, 0x09, 0x12, 0x22, 0x7D],
    '🍌': [0x30, 0x60, 0x62, 0x3E, 0x0C],           # banana
    '♥': [0x0C, 0x1E, 0x3C, 0x1E, 0x0C],
}
CODES = {ch: 0x80 + i for i, ch in enumerate(EXTRA)}
def enc(s):
    out = ''
    for ch in s:
        if ch in CODES: out += f'\\x{CODES[ch]:02X}""'
        elif 32 <= ord(ch) < 127: out += ch.replace('\\', '\\\\').replace('"', '\\"')
        else: raise ValueError(f'no glyph for {ch!r} in {s!r}')
    return '"' + out + '"'

# ---- small 3 x 5 font: the HUD (timer, floor) and the sign of the goal floor --------
SMALL = {
    '0': '###\n#.#\n#.#\n#.#\n###', '1': '.#.\n##.\n.#.\n.#.\n###', '2': '###\n..#\n###\n#..\n###',
    '3': '###\n..#\n.##\n..#\n###', '4': '#.#\n#.#\n###\n..#\n..#', '5': '###\n#..\n###\n..#\n###',
    '6': '###\n#..\n###\n#.#\n###', '7': '###\n..#\n.#.\n.#.\n.#.', '8': '###\n#.#\n###\n#.#\n###',
    '9': '###\n#.#\n###\n..#\n###', ':': '...\n.#.\n...\n.#.\n...', ' ': '...\n...\n...\n...\n...',
    'P': '##.\n#.#\n##.\n#..\n#..', 'L': '#..\n#..\n#..\n#..\n###', 'A': '.#.\n#.#\n###\n#.#\n#.#',
    'T': '###\n.#.\n.#.\n.#.\n.#.', 'N': '#.#\n###\n###\n#.#\n#.#', 'U': '#.#\n#.#\n#.#\n#.#\n###',
    'S': '.##\n#..\n.#.\n..#\n##.', 'H': '#.#\n#.#\n###\n#.#\n#.#', 'C': '.##\n#..\n#..\n#..\n.##',
    'K': '#.#\n#.#\n##.\n#.#\n#.#', '/': '..#\n..#\n.#.\n#..\n#..',
}
SMALL_ORDER = '0123456789: PLATNUSHCK/'

# ---- the falling objects (the original's emoji: printer, toolbox, plant, box, banana,
# extinguisher, chair, TV, apple, computer), 7 x 7 ------------------------------------
ICONS = [
    '.#####.\n.#...#.\n#######\n#.....#\n#.###.#\n#######\n.#####.',
    '..###..\n..#.#..\n#######\n#.....#\n###.###\n#.....#\n#######',
    '.#.#.#.\n..###..\n.#.#.#.\n...#...\n.#####.\n..###..\n..###..',
    '#######\n#..#..#\n#..#..#\n#######\n#.....#\n#.....#\n#######',
    '.....#.\n.....##\n....##.\n...###.\n.####..\n####...\n.##....',
    '..##...\n.####..\n..##.#.\n.####..\n.####..\n.####..\n.####..',
    '.#.....\n.#.....\n.#.....\n.#####.\n.#...#.\n.#...#.\n.#...#.',
    '.#...#.\n..#.#..\n#######\n#.....#\n#.....#\n#######\n.#...#.',
    '...#...\n..#....\n.##.##.\n#######\n#######\n#######\n.##.##.',
    '#######\n#.....#\n#.....#\n#######\n...#...\n..###..\n#######',
]
HEART = '.#.#.\n#####\n#####\n.###.\n..#..'

# ---- texts (Spanish, as in the original; %s: the hacker's name) -----------------------
TEXTS = {
    'T_TITLE1': 'LLEGAMOS TARDE', 'T_TITLE2': 'A LA HACKATHON', 'T_EDITION': '🍌 Platanus Hack 26 🍌',
    'T_PRESS': 'Presiona A', 'T_BY': 'por paoloose', 'T_ED': 'edición Platanus', 'T_CHOOSE': 'Elige tu hacker', 'T_READY': 'LISTO!',
    'D_HELLO': 'Hola, vengo para la Platanus Hack 2026 🍌',
    'D_SORRY': 'Lo siento, han llegado tarde, ya hemos cerrado las puertas',
    'D_WHAT': 'Qué? No puedo perderme la hackathon!',
    'D_WHERE': 'A dónde vas!?, no puedes trepar el edificio!',
    'D_LETGO': 'Súeltame, suéltame',
    'D_MORNING': 'Buenos días con todos',
    'D_ROLL': 'Antes de comenzar la Platanus Hack 2026, vamos a pasar asistencia',
    'D_TEAM1': 'El equipo de ', 'D_TEAM2': ' está presente?',
    'D_HERE': 'Sí, estamos aquí!',
    'E_WIN1': '¡Felicidades!', 'E_WIN2': 'Estás dentro de la', 'E_WIN3': 'Platanus Hack 2026!',
    'E_TIME': 'Tiempo ',
    'E_LOSE1': 'Tu equipo no se pudo', 'E_LOSE2': 'presentar a la', 'E_LOSE3': 'hackathon',
    'E_BACK': 'A: volver al menú',
}

def backdrop():
    import random
    rnd = random.Random(25)
    prof = [0] * 128
    for i in range(12):                              # 12 peaks, like drwMts
        px = -6 + i * 12 + rnd.randint(0, 5); ph = rnd.randint(26, 40); hw = rnd.randint(20, 33) // 2
        for x in range(max(0, px - hw), min(128, px + hw + 1)):
            prof[x] = max(prof[x], ph - abs(x - px) * ph // (hw + 1))
    sky = []
    x = -4
    while x < 128:
        w, hgt = rnd.randint(7, 13), rnd.randint(12, 40)
        if not (x + w > 30 and x < 98): sky += [x & 0xFF, w, hgt]
        x += w + rnd.randint(1, 3)
    return prof, sky

def main():
    js, out = sys.argv[1], sys.argv[2]
    orig = dict(load(js))
    nt, no = len(team.TEAM), len(ORIGINALS)
    h = ['// Generated by tools/gen_data.py from the original game.js (fetch.sh) and tools/team.py:',
         '// do not edit.', '#pragma once', '#include <stdint.h>', '',
         f'#define N_TEAM {nt}                // the Platanus team, then the original characters',
         f'#define N_PLATANUS {sum(ch["org"] == "Platanus" for ch in team.TEAM)}            // the team: Platanus first, then Buk',
         f'#define N_HACKERS {nt + no}            // + the guard', f'#define GUARD_IDX {nt + no}', '',
         '// climbing frames: the kept originals, the team\'s shared ones, the guard',
         f'#define CLIMB_SET(s) ((s) < N_TEAM ? {no} : (s) == GUARD_IDX ? {no + 1} : (s) - N_TEAM)', '']
    climb, port = [], []
    for name in ORIGINALS + [TEAM_CLIMB, 'Guard']:
        for k in ('I', 'R0', 'R1'): climb += bitmap(half(orig[name][k]))
    for ch in team.TEAM: port += bitmap(portrait(team.rows(ch)))
    for name in ORIGINALS + ['Guard']: port += bitmap(portrait(orig[name]['F']))
    h.append('// climbing frames I, R0, R1 of each set (15 x 15, 30 bytes)')
    h.append(carr('SPR_CLIMB', climb))
    h.append('// portrait, 30 x 30 (120 bytes), per hacker, then the guard')
    h.append(carr('SPR_PORTRAIT', port))
    names = [ch['name'] for ch in team.TEAM] + ORIGINALS
    h.append('static const char *const NAMES[N_HACKERS] = {\n  ' + ', '.join(enc(n) for n in names) + ',\n};\n')
    glyphs = [b for g in font for b in g] + [b for ch in EXTRA for b in EXTRA[ch]]
    h.append(f'// 5 x 8 font: ASCII 32..127, then 0x80.. {"".join(EXTRA)}')
    h.append(carr('FONT', glyphs))
    sm = []
    for ch in SMALL_ORDER:
        im = art(SMALL[ch]); sm += [sum(1 << y for y in range(5) if im.getpixel((x, y))) for x in range(3)]
    h.append(f'#define SMALL_CHARS "{SMALL_ORDER}"   // 3 x 5 font, 3 column bytes each')
    h.append(carr('FONT_SMALL', sm))
    ic = []
    for a in ICONS: ic += bitmap(art(a))
    h.append(f'#define N_ICONS {len(ICONS)}        // falling objects, 7 x 7 (7 bytes each)')
    h.append(carr('ICONS', ic))
    h.append(carr('ICON_HEART', bitmap(art(HEART))))
    prof, sky = backdrop()
    h.append('// the Andes: height of the outline above the horizon, per screen column')
    h.append(carr('MOUNTAINS', prof))
    h.append(f'#define N_SKY {len(sky) // 3}          // skyline beside the building: x (int8), w, h')
    h.append(carr('SKYLINE', sky))
    for k, v in TEXTS.items(): h.append(f'static const char {k}[] = {enc(v)};')
    open(out, 'w').write('\n'.join(h) + '\n')
    print(f'{out}: {len(climb) + len(port)} bytes of sprites, {len(glyphs)} of font')

if __name__ == '__main__':
    main()
