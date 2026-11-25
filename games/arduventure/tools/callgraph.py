"""Call graph of the non-LTO build: which functions each game state reaches.

    python tools/callgraph.py build/big_nolto.elf
"""
import re, subprocess, sys, collections
OBJDUMP = sys.argv[2] if len(sys.argv) > 2 else 'riscv-none-elf-objdump'
elf = sys.argv[1]
nm = subprocess.run([OBJDUMP.replace('objdump', 'nm'), '-S', '-C', elf], capture_output=True, text=True).stdout
size, addr2name = {}, {}
for l in nm.splitlines():
    p = l.split(None, 3)
    if len(p) == 4 and p[2] in 'tTwW':
        a, s, n = int(p[0], 16), int(p[1], 16), p[3]
        size[n] = s; addr2name[a] = n
dis = subprocess.run([OBJDUMP, '-d', '-C', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout
calls, refs = collections.defaultdict(set), collections.defaultdict(set)
cur = None
for l in dis.splitlines():
    m = re.match(r'^([0-9a-f]+) <(.+)>:$', l)
    if m: cur = m.group(2); continue
    m = re.search(r'\b(jal|j|call|tail)\s+([0-9a-f]+) <([^+>]+)', l)
    if cur and m and m.group(3) != cur and m.group(3) in size:
        calls[cur].add(m.group(3))
    m = re.search(r'<([^+>]+)>', l)          # address loads (function pointer tables)
    if cur and m and m.group(1) in size and m.group(1) != cur: refs[cur].add(m.group(1))
def reach(roots, stop=()):
    seen, st = set(), list(roots)
    while st:
        f = st.pop()
        if f in seen or f in stop: continue
        seen.add(f); st += list(calls[f] | refs[f])
    return seen
groups = {
  'menu':   ['stateMenuIntro()', 'stateMenuMain()', 'stateMenuContinue()', 'stateMenuNew()', 'stateMenuSound()',
             'stateMenuCredits()', 'stateGameIntro()', 'stateGameNew()', 'stateMenuReboot()', 'stateGameOver()'],
  'field':  ['stateGamePlaying()', 'walkingThroughDoor()', 'stateGameObjects()'],
  'menus':  ['stateGameInventory()', 'stateGameEquip()', 'stateGameStats()', 'stateGameMap()', 'showSubMenuStuff()',
             'stateGameShop()', 'stateGameInn()', 'stateGameSaveSoundEnd()'],
  'battle': ['stateGameBattle()', 'stateGameBoss()', 'battleGiveRewards()'],
}
common = reach(['loop()', 'setup()', 'SysTick_Handler', 'main'], stop={r for g in groups.values() for r in g})
print(f'common (loop, drawTiles, checkInputs, platform): {sum(size[f] for f in common)} bytes')
sets = {g: reach(r) - common for g, r in groups.items()}
for g, s in sets.items():
    others = set().union(*(sets[h] for h in sets if h != g))
    print(f'{g:7} {sum(size[f] for f in s):6} bytes, of which shared with other groups {sum(size[f] for f in s & others)}')
allf = set(size)
print('total code', sum(size.values()), ' unreached', sum(size[f] for f in allf - common - set().union(*sets.values())))
if '-v' in sys.argv:
    for g, s in sets.items():
        print('==', g); [print(f'  {size[f]:5} {f}') for f in sorted(s, key=lambda f: -size[f])[:25]]
