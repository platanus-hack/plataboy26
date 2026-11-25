"""Worst case stack depth of the linked game, from its machine code: every function's
frame (the -msave-restore routine it calls plus its own sp adjustments) and its calls
(the relocations the link keeps, --emit-relocs: they name the callee even in the
overlay window, where the groups' functions share addresses). A jump to a function
(tail call) counts as a call. Prints the deepest chain and what is left of the SRAM; with the page buffer right
below the stack (the last static variable), also the deepest chain while it is in use.

    python3 tools/stack.py ELF OBJDUMP
"""
import re, subprocess, sys
from functools import lru_cache

elf, objdump = sys.argv[1:3]
size = objdump.replace('objdump', 'size')
dis = subprocess.run([objdump, '-d', '-r', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout

funcs, cur, last_ins = {}, None, ''
for l in dis.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
    if m:
        if not m.group(1).startswith('.L'):
            cur = m.group(1); funcs.setdefault(cur, {'frame': 0, 'calls': set(), 'save': None, 'indirect': False})
        continue
    if not cur: continue
    f = funcs[cur]
    m = re.match(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$', l)
    if m and not m.group(1).startswith('R_RISCV'):
        op, args = m.group(1), m.group(2)
        last_ins = op
        a = re.match(r'(?:c\.)?addi(?:16sp)?\s+sp,sp,(-?\d+)', op + ' ' + args)
        if a and int(a.group(1)) < 0: f['frame'] += -int(a.group(1))
        if op in ('jalr', 'c.jalr') and 'R_RISCV' not in args: f['indirect'] = True
        continue
    m = re.search(r'R_RISCV_(JAL|CALL|CALL_PLT|RVC_JUMP)\s+(\S+?)(\+0x[0-9a-f]+)?$', l)
    if m:
        t = m.group(2)
        if t.startswith('__riscv_save'): f['save'] = t
        elif t.startswith('__riscv_restore') or t.startswith('.L'): pass
        elif t != cur: f['calls'].add(t)
        if last_ins == 'jalr': f['indirect'] = False   # auipc + jalr pair: a direct call

# the save routines' names are aliases (__riscv_save_0..2 are one routine for RV32E):
# their frames by address
addr = {}
for l in subprocess.run([objdump.replace('objdump', 'nm'), elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3: addr[p[2]] = int(p[0], 16)
by_addr = {addr[n]: funcs[n]['frame'] for n in funcs if n.startswith('__riscv_save') and n in addr}
saves = {n: by_addr[a] for n, a in addr.items() if n.startswith('__riscv_save') and a in by_addr}
for n, f in funcs.items():
    if f['save']: f['frame'] += saves.get(f['save'], 0)

# what runs while the page buffer is in use (between BeginPage and EndPage, in Draw)
PAGE_TIME = {'DrawStartScreen', 'DrawNewCityMenu', 'DrawInGame', 'DrawSaveLoadMenu', 'DrawBackground',
             'DrawBudgetMenu', 'BeginPage', 'EndPage'}
def base(n):
    m = re.match(r'_Z(\d+)(\w+)', n)
    return m.group(2)[:int(m.group(1))] if m else n

@lru_cache(None)
def depth(n, seen=()):
    if n in seen: sys.exit(f'recursion: {" -> ".join(seen + (n,))}')
    f = funcs.get(n)
    if not f: return 0, (n + '?',)
    best = (0, ())
    for c in f['calls']:
        d = depth(c, seen + (n,))
        if d[0] > best[0]: best = d
    return f['frame'] + best[0], (f'{n} ({f["frame"]})',) + best[1]

static, bottom = 0, 0
for l in subprocess.run([size, '-A', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 2 and p[0] in ('.data', '.bss'): static += int(p[1])
# the page buffer, if it is the last static variable (right below the stack)
syms = {}
for l in subprocess.run([objdump.replace('objdump', 'nm'), '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4: syms[p[3]] = (int(p[0], 16), int(p[1], 16))
if '_ZL4page' in syms and sum(syms['_ZL4page']) == addr.get('_ebss'):
    bottom = syms['_ZL4page'][1]; static -= bottom
ind = sorted(n for n, f in funcs.items() if f['indirect'])
d, chain = depth('main')
ok = static + bottom + d <= 2048 or not bottom
print(f'stack: worst case {d} bytes; SRAM {static} static + {d} = {static + d} of 2048, {2048 - static - d} left')
print('  ' + ' -> '.join(chain))
if bottom:
    # while the page buffer (.stackbottom) is in use: main's frame + what it draws
    dd, dchain = max(depth(n) for n in funcs if base(n) in PAGE_TIME)
    dd += funcs['main']['frame']
    left = 2048 - static - bottom - dd
    print(f'  drawing: worst case {dd} bytes above the page buffer ({bottom} B), {left} left')
    print('  main -> ' + ' -> '.join(dchain))
    ok = static + d <= 2048 and left >= 0
if ind: print('  indirect calls (not followed): ' + ', '.join(ind))
sys.exit(0 if ok else 1)
