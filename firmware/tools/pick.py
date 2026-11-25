"""Choose the games of a library in the terminal, with the EEPROM use shown as you go.

    python pick.py --kbase 0x3C40 --menu build/menu.bin --out library.custom.json
    python pick.py ... --select "Hopper,MicroCity,Tiny Lander"     (no terminal: check and write)

Lists every program of catalog.json with what it takes in the 128 KiB EEPROM (compressed
program + its data region + its menu card). Each change is checked with the packer's own
placement (pack.py place()): fixed data regions, save slots, the 64 KiB boundary and
fragmentation included. The library file is written only when the selection fits.

    Up/Down move    Space select    [ ] move the game up/down in the menu    W write    Q quit
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ENTRY_KEYS = ('name', 'file', 'card', 'data', 'save', 'flags')     # what goes into the library file


class Game:
    def __init__(self, p, kbase, thumbs):
        self.p, self.name, self.item, self.why = p, p['name'], None, ''
        files = [p['file']] + ([p['data']['file']] if 'data' in p else [])
        if not all(os.path.exists(os.path.join(FW, f)) for f in files):
            self.why = 'not built: ' + p.get('build', '')
            return
        try:
            self.item = pack.load_item(p, FW, kbase, thumbs)
        except SystemExit as e:                    # e.g. larger than the space below the kernel
            self.why = str(e).split(': ', 1)[-1]

    @property
    def size(self):
        it = self.item
        return len(it['c']) + (len(it['data'][1]) if it['data'] else 0) + len(it['card'] or b'') if it else 0

    def notes(self):
        if not self.item: return self.why
        it, n = self.item, []
        if it['data']: n.append(f'data {len(it["data"][1])} B at 0x{it["data"][0]:05X}')
        if it['save']: n.append(f'save at 0x{it["save"][0]:05X}')
        if 'tool' in self.p.get('flags', []): n.append('tool')
        if self.p.get('test_scratch'): n.append('keeps the test pages')
        return ', '.join(n)


class Model:
    def __init__(self, kbase, menu, catalog, selected=None):
        thumbs = pack.thumbnails()
        self.menu = pack.load_item({'name': 'Menu', 'file': os.path.abspath(menu)}, FW, kbase, thumbs, menu=True)
        games = []
        for i, p in enumerate(catalog):
            if sys.stderr.isatty(): print(f'\rreading {i + 1}/{len(catalog)}: {p["name"]:20}', end='', file=sys.stderr)
            games.append(Game(p, kbase, thumbs))       # (compresses a game the first time: slow once)
        if sys.stderr.isatty(): print('\r' + ' ' * 50 + '\r', end='', file=sys.stderr)
        by_name = {g.name: g for g in games}
        unknown = [n for n in selected or [] if n not in by_name]
        if unknown: sys.exit('not in catalog.json: ' + ', '.join(unknown))
        # the selected games first, in their menu order; then the rest
        self.on = [by_name[n] for n in selected or [] if by_name[n].item]
        self.rest = [g for g in games if g not in self.on]
        self.check()

    @property
    def rows(self): return self.on + self.rest

    def scratch(self): return any(g.p.get('test_scratch') for g in self.on)

    def check(self):
        """-> sets used, capacity, fits, message"""
        items = [dict(self.menu)] + [dict(g.item) for g in self.on]
        self.used = len(self.menu['c']) + sum(g.size for g in self.on)
        saves = [(it['save'][0], sum(it['save'])) for it in items if it['save']]
        self.capacity = sum(b - a for a, b in pack.free_ranges(saves, pack.reserved_areas(self.scratch())))
        try:
            pack.place(items, self.scratch())
            self.fits, self.message = True, f'fits: {self.capacity - self.used} bytes free'
        except SystemExit as e:
            over = self.used - self.capacity
            self.fits, self.message = False, (f'does not fit: {over} bytes over' if over > 0 else 'does not fit: ' + str(e))
        return self.fits

    def toggle(self, g):
        if not g.item: return
        if g in self.on:
            self.on.remove(g); self.rest.insert(0, g)
        else:
            self.rest.remove(g); self.on.append(g)
        self.check()

    def move(self, g, d):
        if g not in self.on: return
        i = self.on.index(g); j = i + d
        if 0 <= j < len(self.on): self.on[i], self.on[j] = self.on[j], self.on[i]

    def library(self):
        progs = [{k: g.p[k] for k in ENTRY_KEYS if k in g.p} for g in self.on]
        return {'comment': 'Written by tools/pick.py (make pick): the games chosen from catalog.json, in menu order.',
                'programs': progs, 'test_scratch': self.scratch()}

    def table(self):
        lines = [f'{"":3} {"game":16}{"EEPROM":>7}  notes']
        for g in self.rows:
            mark = '[x]' if g in self.on else ('[ ]' if g.item else ' - ')
            lines.append(f'{mark} {g.name:16}{g.size if g.item else "":>7}  {g.notes()}')
        lines.append(f'    {"Menu":16}{len(self.menu["c"]):>7}  always')
        lines.append(f'used {self.used} of {self.capacity} bytes ({len(self.on)} games)   {self.message}')
        return lines


def bar(used, capacity, width):
    n = min(width, used * width // capacity)
    return '#' * n + '.' * (width - n)


def interactive(m, out):
    import curses

    def run(scr):
        curses.curs_set(0); curses.use_default_colors()
        for i, c in enumerate((curses.COLOR_GREEN, curses.COLOR_RED, curses.COLOR_CYAN), 1): curses.init_pair(i, c, -1)
        cur, top, note = 0, 0, ''
        while True:
            scr.erase()
            h, w = scr.getmaxyx()
            rows = m.rows
            put = lambda y, x, s, a=0: scr.addnstr(y, x, s, max(0, w - x - 1), a) if 0 <= y < h else None
            put(0, 0, 'plataboy26 library: choose the games', curses.A_BOLD)
            put(1, 0, 'Up/Down move   Space select   [ ] order in the menu   W write   Q quit', curses.color_pair(3))
            put(3, 0, f'     {"game":16}{"EEPROM":>7}  notes', curses.A_UNDERLINE)
            body = max(1, h - 9)
            top = min(max(top, cur - body + 1), cur)
            for k, g in enumerate(rows[top:top + body]):
                i = top + k
                mark = '[x]' if g in m.on else ('[ ]' if g.item else ' - ')
                a = (curses.A_REVERSE if i == cur else 0) | (0 if g.item else curses.A_DIM)
                put(4 + k, 0, f'{">" if i == cur else " "} {mark} {g.name:16}{g.size if g.item else "":>7}  {g.notes()}', a)
            y = 4 + min(body, len(rows)) + 1
            col = curses.color_pair(1 if m.fits else 2) | curses.A_BOLD
            put(y, 0, f'[{bar(m.used, m.capacity, max(10, min(50, w - 40)))}] {m.used} / {m.capacity} bytes', col)
            put(y + 1, 0, f'{len(m.on)} games + menu ({len(m.menu["c"])} B).  ' + m.message, col)
            put(y + 2, 0, note)
            scr.refresh()
            c = scr.getch(); note = ''
            g = rows[cur]
            if c in (curses.KEY_UP, ord('k')): cur = max(0, cur - 1)
            elif c in (curses.KEY_DOWN, ord('j')): cur = min(len(rows) - 1, cur + 1)
            elif c == ord(' '):
                if not g.item: note = f'{g.name}: {g.why}'
                else:
                    m.toggle(g); cur = min(m.rows.index(g), len(rows) - 1)
            elif c in (ord('['), ord(']')):
                m.move(g, -1 if c == ord('[') else 1); cur = m.rows.index(g)
            elif c in (ord('w'), ord('W'), 10, curses.KEY_ENTER):
                if m.fits and m.on: return True
                note = 'not written: ' + ('nothing selected' if m.fits else 'the selection does not fit')
            elif c in (ord('q'), ord('Q')): return False

    return curses.wrapper(run)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kbase', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--menu', required=True)
    ap.add_argument('--catalog', default=os.path.join(FW, 'catalog.json'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--select', help='comma-separated names: no terminal UI, check and write')
    a = ap.parse_args()
    catalog = json.load(open(a.catalog))['programs']
    if a.select is not None:
        start = [n.strip() for n in a.select.split(',') if n.strip()]
    else:                                   # start from the last choice, or the full library
        f = a.out if os.path.exists(a.out) else os.path.join(FW, 'library.full.json')
        known = {p['name'] for p in catalog}
        start = [p['name'] for p in json.load(open(f))['programs'] if p['name'] in known]
    m = Model(a.kbase, a.menu, catalog, start)
    if a.select is not None:
        missing = [n for n in start if n not in [g.name for g in m.on]]
        print('\n'.join(m.table()))
        if missing: sys.exit('cannot be selected: ' + ', '.join(f'{n} ({next(g.why for g in m.rest if g.name == n)})' for n in missing))
        if not m.fits: sys.exit(1)
    elif not (sys.stdin.isatty() and sys.stdout.isatty()):
        sys.exit('no terminal for the checklist: use make pick SELECT="Hopper,MicroCity,..."')
    elif not interactive(m, a.out):
        print('nothing written'); sys.exit(2)
    json.dump(m.library(), open(a.out, 'w'), indent=2)
    print(f'{os.path.relpath(a.out)}: {len(m.on)} games, {m.used} of {m.capacity} bytes, {m.capacity - m.used} free')


if __name__ == '__main__':
    main()
