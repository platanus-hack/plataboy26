"""Assigns every function to the resident code or to one overlay group.

From the call graph of a build without LTO (every function in its own section):
  - everything reachable from the resident roots (main loop, setup, interrupt, ...)
    is resident
  - every other function is reached only from the state functions of the groups (they
    are called through tables the analysis does not follow: mainGameLoop and the split
    checkInputs); a function that one group alone reaches goes into that group's
    overlay, one that several groups reach stays resident

    python tools/overlays.py nolto.elf objdump build/src out.json

Writes {function name: group} for the overlay functions; prints the sizes.
"""
import json, os, re, subprocess, sys, collections

elf, objdump, src, out = sys.argv[1:5]
nm = objdump.replace('objdump', 'nm')
sys.path.insert(0, os.path.dirname(__file__))
from patch_upstream import GROUPS, EXTRA_ROOTS

size = {}
for l in subprocess.run([nm, '-S', '-C', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 3)
    if len(p) == 4 and p[2] in 'tTwW': size[p[3]] = int(p[1], 16)
calls = collections.defaultdict(set)
# exact references from the relocations kept by the linker (-Wl,--emit-relocs):
# calls, jumps and address loads of each function
cur = None
for l in subprocess.run([objdump, '-d', '-r', '-C', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
    if m:
        if not m.group(1).startswith('.L'): cur = m.group(1)       # local labels: same function
        continue
    m = re.search(r'\sR_RISCV_\w+\s+(.+?)(\+0x[0-9a-f]+)?$', l)
    if cur and m:
        t = m.group(1).strip()
        if t in size and t != cur: calls[cur].add(t)

def base(n): return n.split('(')[0]

# state -> function, from mainGameLoop in the sketch
ino = open(os.path.join(src, 'ARDU_AB.ino')).read()
table = re.search(r'mainGameLoop\[\]\s*=\s*\{(.*?)\};', ino, re.S).group(1)
funcs = [f.strip() for f in table.split(',') if f.strip()]
g_src = open(os.path.join(src, 'globals.h')).read()
state_num = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define\s+(STATE_\w+|WALKING_THROUGH_DOOR)\s+(\d+)', g_src)}
roots = collections.defaultdict(set)
for g, states in GROUPS.items():
    for st in states: roots[g].add(funcs[state_num[st]] + '()')
    roots[g].add('checkInputs_%d()' % g)
    for r in EXTRA_ROOTS.get(g, []): roots[g].add(r + '()')
by_name = {base(n): n for n in size}
for g in roots: roots[g] = {by_name.get(base(r), r) for r in roots[g]}
all_roots = set().union(*roots.values())

def reach(start, stop=set()):
    seen, st = set(), [s for s in start if s in size]
    while st:
        f = st.pop()
        if f in seen or f in stop: continue
        seen.add(f); st += list(calls[f])
    return seen

resident = reach({'main', 'loop()', 'setup()', 'SysTick_Handler', 'reset_handler', 'jump_reset',
                  'default_handler', 'checkInputs()'} & set(size), stop=all_roots)
groups_of = collections.defaultdict(set)
for g, r in roots.items():
    for f in reach(r):
        if f not in resident: groups_of[f].add(g)
assign = {}
for f, gs in groups_of.items():
    if len(gs) == 1: assign[f] = next(iter(gs))
shared = [f for f, gs in groups_of.items() if len(gs) > 1]
json.dump({base(f): g for f, g in assign.items()}, open(out, 'w'), indent=1, sort_keys=True)

gsize = collections.Counter()
for f, g in assign.items(): gsize[g] += size[f]
res = sum(size[f] for f in size if f not in assign)
print(f'resident code {res} B (of which shared between groups {sum(size[f] for f in shared)} B)')
for g in sorted(GROUPS): print(f'  overlay {g}: {gsize[g]} B')
if '-v' in sys.argv:
    for f in sorted(shared, key=lambda f: -size[f])[:20]: print(f'  shared {size[f]:5} {f} {sorted(groups_of[f])}')
