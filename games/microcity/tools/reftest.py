"""Plays a game on the port (test build, in the simulator) and on the original game
(upstream, unmodified, on the host: tools/ref.cpp), and compares every frame.

    python3 tools/reftest.py

The game is played on the original first, frame by frame (tools/ref.cpp reads each
frame's buttons and reports the game's state): the script below waits for the city
when a fire or the year's budget report comes up, and checks that what it meant to do
happened (brush chosen, city saved, loaded). Those buttons, a byte per frame, then go
to the start of the simulated EEPROM, where the test build (MC_TEST, port/platform.cpp)
takes them from; it prints a hash of each frame's pixels on the serial port, as the
original does.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
B = os.path.join(GAME, 'build', 'test')
SIM = os.path.join(GAME, '..', '..', 'sim', 'core')
COMMIT = 'ae265f1923e9ac9789df601bb438faa29b7ae339'
SOURCES = ['Building', 'Connectivity', 'Draw', 'Font', 'Game', 'Interface', 'Simulation', 'Strings', 'Terrain']
HEADERS = ['Building', 'Connectivity', 'Defines', 'Draw', 'Font', 'Game', 'Interface', 'LogoBitmap', 'Simulation',
           'Strings', 'Terrain', 'Terrain1.inc', 'Terrain2.inc', 'Terrain3.inc', 'TileData']
LEFT, RIGHT, UP, DOWN, A, B_ = 1, 2, 4, 8, 16, 32
START, NEW_CITY, IN_GAME, DISASTER, TOOLBAR, SAVE_LOAD, BUDGET = range(7)   # Interface.h
SAVE_LOAD_BUTTON, BUDGET_BUTTON = 11, 12

def run(cmd, **kw): return subprocess.run(cmd, check=True, **kw)

# ---- build ----------------------------------------------------------------------------
os.makedirs(os.path.join(B, 'ref'), exist_ok=True)
up = os.path.join(GAME, 'upstream')
for n in [s + '.cpp' for s in SOURCES] + [h + '.h' for h in HEADERS]:
    data = run(['git', '-C', up, 'show', f'{COMMIT}:Source/MicroCity/{n}'], capture_output=True).stdout
    open(os.path.join(B, 'ref', n), 'wb').write(data)
ref = os.path.join(B, 'ref', 'ref')
run(['c++', '-O2', '-std=gnu++17', '-w', '-I' + os.path.join(B, 'ref'), '-I' + os.path.join(GAME, 'port'), '-o', ref,
     os.path.join(HERE, 'ref.cpp')] + [os.path.join(B, 'ref', s + '.cpp') for s in SOURCES])
run(['make', '-s', '-C', GAME, 'B=build/test', 'OUT=build/test/microcity', 'EXTRA=-DMC_TEST', 'KBASE=0x4000'],
    stdout=subprocess.DEVNULL)
chsim = os.path.join(B, 'chsim')
run(['cc', '-O2', '-w', '-DUART_BUF=4000000', '-o', chsim, os.path.join(SIM, 'emu.c'), os.path.join(SIM, 'cli.c')])

# ---- the game, played on the original ----------------------------------------------------
class Game:
    def __init__(self):
        self.p = subprocess.Popen([ref, '-'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.script, self.hashes = [], []
        self.state = self.sel = self.brush = self.x = self.y = self.money = self.saves = self.loads = 0

    def frame(self, bits):
        self.p.stdin.write(bytes([bits])); self.p.stdin.flush()
        f = self.p.stdout.readline().split()
        self.hashes.append(f[0].decode())
        self.state, self.sel, self.brush, self.x, self.y, self.money, self.saves, self.loads = map(int, f[1:])
        self.script.append(bits)

    def idle(self, n=1):
        for _ in range(n): self.frame(0)

    def tap(self, bits, n=1):                 # press for 2 frames, release for 2
        for _ in range(n): self.frame(bits); self.frame(bits); self.frame(0); self.frame(0)

    def settle(self):                         # back to the city: past a fire's message or
        while self.state != IN_GAME:          # a budget report, out of a menu
            if self.state == DISASTER: self.idle()
            elif self.state == BUDGET: self.tap(B_) if self.sel >= 16 else self.idle()
            elif self.state in (TOOLBAR, SAVE_LOAD): self.tap(A)
            else: raise RuntimeError(f'unexpected state {self.state}')

    def button(self, n, want):                # toolbar button n, until the state is want
        while True:
            self.settle(); self.tap(A)
            if self.state != TOOLBAR: continue
            while self.state == TOOLBAR and self.sel != n: self.tap(RIGHT if n > self.sel else LEFT)
            if self.state != TOOLBAR: continue
            self.tap(B_)
            if self.state == want and (n > 10 or self.brush == n): return

    def tool(self, n): self.button(n, IN_GAME)

    def goto(self, x, y):
        while (self.x, self.y) != (x, y) or self.state != IN_GAME:
            self.settle()
            if self.x != x: self.tap(RIGHT if x > self.x else LEFT)
            elif self.y != y: self.tap(DOWN if y > self.y else UP)

    def use(self, x, y):                      # the brush on (x, y)
        self.goto(x, y); self.tap(B_)

    def place(self, n, x, y): self.tool(n); self.use(x + 1, y + 1)   # top left at (x, y)

    def lay(self, n, tiles):                  # road or power line
        self.tool(n)
        for x, y in tiles: self.use(x, y)

    def wait(self, frames):                   # time passes: the bulldozer on an empty tile
        self.tool(0); self.goto(44, 4)        # (east of the river: nothing is built there)
        for _ in range(frames // 100): self.idle(96); self.tap(B_)

    def save_load(self, item):                # Save/Load menu item: 0 save, 1 load, 2 new
        while True:                           # city, 3 auto budget
            self.button(SAVE_LOAD_BUTTON, SAVE_LOAD)
            while self.state == SAVE_LOAD and self.sel != item: self.tap(DOWN)
            if self.state != SAVE_LOAD: continue
            saves, loads = self.saves, self.loads
            self.tap(B_)
            if item == 0 and self.saves > saves: return
            if item == 1 and self.loads > loads: return
            if item == 2 and self.state == NEW_CITY: return
            if item == 3 and self.state == SAVE_LOAD: self.tap(B_); self.tap(A); return   # off, on

    def taxes(self, taps):                    # the budget report from the toolbar
        self.button(BUDGET_BUTTON, BUDGET); self.idle(20)
        self.tap(LEFT if taps < 0 else RIGHT, abs(taps)); self.tap(B_)

g = Game()
g.idle(20); g.tap(DOWN); g.tap(UP); g.tap(B_)                # start screen: New City
g.tap(RIGHT, 3); g.tap(B_); g.idle(10)                       # River, after a round of the terrains
g.taxes(-7)                                                  # no taxes: the city grows
g.place(6, 1, 1)                                             # power plant, far from the houses
g.lay(2, [(5, y) for y in range(4, 34)])                     # its power line down to the city
g.place(3, 6, 32); g.place(3, 9, 32)                         # two residential zones
g.place(4, 12, 32)                                           # commercial
g.lay(1, [(x, 35) for x in range(6, 15)])                    # their road
g.lay(2, [(8, 35)] + [(x, 34) for x in range(15, 22)] + [(21, y) for y in range(35, 39)])
g.place(8, 8, 36)                                            # police, powered through (8, 35)
g.place(5, 22, 38)                                           # industrial, away from the houses
g.lay(1, [(x, 41) for x in range(22, 25)])
g.place(7, 2, 32)                                            # park
g.wait(4000)
g.place(3, 6, 25); g.place(3, 9, 25)                         # more housing, north of the first
g.lay(1, [(x, 28) for x in range(6, 12)])
g.lay(2, [(x, 29) for x in range(6, 9)] + [(7, y) for y in range(30, 32)])
g.place(9, 13, 25)                                           # fire department
g.lay(1, [(x, 28) for x in range(12, 16)])
g.wait(4000)
g.save_load(0)                                               # save the city
g.place(0, 2, 32)                                            # bulldoze the park
g.use(10, 35)                                                # and a piece of road
g.save_load(1)                                               # load the city again
g.save_load(3)                                               # auto budget off and on
g.taxes(2)                                                   # taxes 2 %
g.wait(8000)
g.save_load(2); g.tap(LEFT); g.tap(B_)                       # New City: the lake
g.idle(300)
g.save_load(1)                                               # load the saved city
g.wait(6000)
g.p.stdin.close(); g.p.wait()
assert g.saves == 1 and g.loads == 2, (g.saves, g.loads)

# ---- the same buttons on the port ---------------------------------------------------------
frames = len(g.script)
ee = bytearray(open(os.path.join(B, 'microcity.eeprom'), 'rb').read())
assert frames < 0x16000 - 32
ee[:frames] = bytes(g.script)
eep = os.path.join(B, 'test.eeprom')
open(eep, 'wb').write(ee)
open(os.path.join(B, 'script.bin'), 'wb').write(bytes(g.script))

ms = frames * 40 * 11 // 10 + 1000            # frames may run late: room for it
out = run([chsim, os.path.join(B, 'microcity.bin'), '-e', eep, '-t', str(ms)], capture_output=True, text=True).stdout
serial = out.split('--- serial ---')[-1].splitlines() if '--- serial ---' in out else []
got = [l for l in serial if re.fullmatch(r'[0-9a-f]{8}', l)][:frames]
stack = [int(l[1:], 16) for l in serial if re.fullmatch(r's[0-9a-f]{8}', l)]
stats = [l for l in out.splitlines() if l.startswith(('firmware', 'host', 'state'))]

want = g.hashes
bad = [i for i in range(min(len(got), len(want))) if got[i] != want[i]]
print(f'{frames} frames played ({g.saves} save, {g.loads} loads), port reported {len(got)}; '
      f'{len(bad)} differ from the original')
for l in stats: print('  ' + l)
if stack: print(f'  stack: {min(stack)} bytes above the page buffer never used')
if bad:
    runs, start = [], bad[0]
    for a, b in zip(bad, bad[1:] + [None]):
        if b != a + 1: runs.append((start, a)); start = b
    print('  differing frames: ' + ', '.join(f'{a}-{b}' if a != b else f'{a}' for a, b in runs[:20]))
sys.exit(0 if not bad and len(got) == frames else 1)
