#!/usr/bin/env python3
# ===================================================================================
# jbc: compiles the game logic (src/logic.py, Python syntax) to the bytecode of src/vm.c
# ===================================================================================
#   python3 tools/jbc.py src/logic.py src/bytecode.h bin/cold.bin src/sprites.h
#
# The language is a small subset of Python, parsed with Python's own ast module:
#   NAME = <int expression>          constant (inlined)
#   NAME = [1, -2, 3]                table of signed bytes (NAME[i])
#   def f(a, b): ...                 function; @cold puts it in the EEPROM bank
#   locals, G.field (globals), e.field (entity e, an index), ints (16 bit),
#   + - * // % & | ^ << >> (C semantics: // truncates), - not ~, comparisons,
#   and / or, x if c else y, if/elif/else, while, for i in range(a, b[, step]),
#   break, continue, return, calls of functions and natives (NATIVES below),
#   "strings" as arguments (their address in the flash bank).
# wait(n) in a function makes the thread that runs it wait n frames (scripts).
import ast, struct, sys

# ---- what the C side has (keep in sync with src/game.h, checked by static asserts) --
ENT = {  # field: (offset, type)  types: u8 s8 s16 u16
    'type': (0, 'u8'), 'fl': (1, 'u8'), 'fl2': (2, 'u8'), 'dc': (3, 'u8'),
    'x': (4, 's16'), 'y': (6, 's16'), 'alive': (8, 'u16'), 'ftd': (10, 'u8'),
    'layer': (11, 'u8'), 'hit': (12, 'u8'), 'idle': (13, 'u8'), 'a': (14, 'u8'),
    'b': (15, 'u8'), 'c': (16, 'u8'), 'u': (17, 's8'), 'v': (18, 's8'), 'inv': (19, 'u8'),
}
GLOB = [  # in struct order: name, type
    ('boss', 'u8'), ('brefl', 'u8'), ('player', 'u8'), ('prefl', 'u8'), ('figment', 'u8'),
    ('phealth', 'u8'), ('title', 'u8'), ('hard', 'u8'), ('phase', 'u8'), ('paused', 'u8'),
    ('freeze', 'u8'), ('shake', 'u8'), ('mult', 'u8'), ('refl_color', 'u8'),
    ('conjure', 'u8'), ('starting', 'u8'), ('start', 'u8'), ('start_phase', 'u8'),
    ('score_idx', 'u8'), ('hearts', 'u8'), ('ph_anim', 'u8'), ('ph_dc', 'u8'),
    ('bh_health', 'u8'), ('bh_rainbow', 'u8'), ('bh_dc', 'u8'), ('bh_vis', 'u8'),
    ('cur_open', 'u8'), ('cur_dc', 'u8'), ('p_facing', 'u8'), ('p_step_dir', 'u8'),
    ('p_next', 'u8'), ('p_step', 'u8'), ('p_teeter', 'u8'), ('p_stun', 'u8'),
    ('p_col', 'u8'), ('p_row', 'u8'), ('shake_x', 's8'), ('spawn_n', 'u8'), ('owed', 'u8'),
    ('score', 'u16'), ('timer', 'u16'), ('scene', 'u16'),
    ('rec0', 'u16'), ('rec1', 'u16'), ('rec2', 'u16'), ('rec3', 'u16'),
]
NATIVES = [  # name, number of arguments (src/vm.c NATS[] in this order)
    ('rnd', 1), ('sfx', 2), ('shake', 2), ('ent_new', 1), ('die', 1),
    ('move4', 9), ('cancel_move', 1), ('effect', 3), ('particle', 6), ('col', 1),
    ('row', 1), ('fork', 4), ('cancel_threads', 1), ('kill_threads', 0), ('wait', 1),
    ('btnp', 1), ('btn_new', 0), ('spr', 4), ('spr_e', 5), ('text', 4), ('ctext', 4),
    ('num', 3), ('pix', 3), ('fill', 5), ('save', 0),
    ('peek', 1), ('poke', 2), ('sx', 1),
    ('sy', 1), ('ease', 2),
]
# opcodes, grouped by operand size (none, 1 byte, 2 bytes): src/vm.c fetches it once
OP0 = 'RET DUP DROP NEG NOT INV ADD SUB MUL DIV MOD AND OR XOR SHL SHR EQ NE LT LE GT GE'.split()
OP1 = 'PUSHB PUSHX LDL STL LDG STG LDE STE LDLE STLE NAT NATV JMP JZ JNZ'.split()
# the VM knows a field's width from its offset (src/vm.c: WIDE, SIGNED): 16-bit fields
# are x, y, alive (Ent) and score, timer, scene, rec (Glob); u, v are signed
OP2 = 'PUSHW TAB CALL CALLV JMPW JZW JNZW JNEK JEQK'.split()   # J?EK: k, rel8
OPS = {n: i for i, n in enumerate(OP0 + OP1 + OP2)}
LDL0, STL0, PUSHK = 0x40, 0x50, 0x60          # LDL0..15, STL0..15, PUSH -8..31
BINOPS = {ast.Add: 'ADD', ast.Sub: 'SUB', ast.Mult: 'MUL', ast.FloorDiv: 'DIV',
          ast.Mod: 'MOD', ast.BitAnd: 'AND', ast.BitOr: 'OR', ast.BitXor: 'XOR',
          ast.LShift: 'SHL', ast.RShift: 'SHR'}
CMPS = {ast.Eq: 'EQ', ast.NotEq: 'NE', ast.Lt: 'LT', ast.LtE: 'LE', ast.Gt: 'GT', ast.GtE: 'GE'}
COLD_BASE = 0x8000


class Err(Exception): pass


def goff():
    out, o = {}, 0
    for n, t in GLOB:
        if t in ('u16', 's16') and o & 1: o += 1
        out[n] = (o, t)
        o += 2 if t in ('u16', 's16') else 1
    return out
GLOBS = goff()


class Fn:
    def __init__(self, node, cold):
        self.node, self.cold, self.name = node, cold, node.name
        self.args = [a.arg for a in node.args.args]
        self.code = []   # items: bytes, ('label', id), ('jmp', op, id), ('call', name), ('addr', name)


class Compiler:
    def __init__(self, src):
        self.consts, self.tables, self.fns, self.strings = {}, {}, {}, {}
        self.nat = {n: (i, a) for i, (n, a) in enumerate(NATIVES)}
        tree = ast.parse(src)
        for node in tree.body:
            if isinstance(node, ast.Assign):
                name = node.targets[0].id
                if isinstance(node.value, ast.List):
                    self.tables[name] = [self.const(e) for e in node.value.elts]
                else:
                    self.consts[name] = self.const(node.value)
            elif isinstance(node, ast.FunctionDef):
                cold = any(isinstance(d, ast.Name) and d.id == 'cold' for d in node.decorator_list)
                self.fns[node.name] = Fn(node, cold)
            elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):
                pass                                      # docstring
            else:
                raise Err('line %d: unexpected statement' % node.lineno)
        self.label_n = 0
        for f in self.fns.values():
            self.compile_fn(f)

    # ---- constants -------------------------------------------------------------------
    def const(self, e):
        if isinstance(e, ast.Constant) and isinstance(e.value, int): return e.value
        if isinstance(e, ast.Name) and e.id in self.consts: return self.consts[e.id]
        if isinstance(e, ast.UnaryOp):
            v = self.const(e.operand)
            if v is None: return None
            return {ast.USub: -v, ast.Invert: ~v, ast.Not: int(not v), ast.UAdd: v}[type(e.op)]
        if isinstance(e, ast.BinOp) and type(e.op) in BINOPS:
            a, b = self.const(e.left), self.const(e.right)
            if a is None or b is None: return None
            op = type(e.op)
            if op is ast.FloorDiv: return int(a / b)
            if op is ast.Mod: return a - int(a / b) * b
            return {ast.Add: a + b, ast.Sub: a - b, ast.Mult: a * b, ast.BitAnd: a & b,
                    ast.BitOr: a | b, ast.BitXor: a ^ b, ast.LShift: a << b,
                    ast.RShift: a >> b}[op]
        if isinstance(e, ast.Call) and isinstance(e.func, ast.Name) and e.func.id == 'FX':
            v = self.const(e.args[0])
            return None if v is None else v * 16
        return None

    # ---- code emission ----------------------------------------------------------------
    EFFECT = {'PUSHX': 1, 'LDLE': 1, 'STLE': -1, 'LDG': 1, 'STG': -1, 'LDE': 0, 'STE': -2, 'LDLE8': 1, 'LDLES8': 1, 'LDLE16': 1, 'STLE8': -1, 'STLE16': -1,
              'PUSHB': 1, 'PUSHW': 1, 'LDL': 1, 'STL': -1, 'LDG8': 1, 'LDG16': 1, 'STG8': -1,
              'STG16': -1, 'LDE8': 0, 'LDES8': 0, 'LDE16': 0, 'STE8': -2, 'STE16': -2, 'TAB': 0,
              'DUP': 1, 'DROP': -1, 'NEG': 0, 'NOT': 0, 'INV': 0, 'RET': -1}

    def depth(self, d):
        self.d += d
        self.f.maxd = max(self.f.maxd, self.d)

    def emit(self, *b):
        op = b[0]
        if op >= 0x60 or 0x40 <= op < 0x50: self.depth(1)
        elif 0x50 <= op < 0x60: self.depth(-1)
        self.f.code.append(bytes(b))

    def op(self, name, *args):
        if name in self.EFFECT: self.depth(self.EFFECT[name])
        elif name in BINOPS.values() or name in CMPS.values(): self.depth(-1)
        self.f.code.append(bytes((OPS[name],) + args))

    def label(self):
        self.label_n += 1
        return self.label_n

    def place(self, l):
        self.f.code.append(('label', l))

    def jump(self, kind, l):          # kind: JMP JZ JNZ
        if kind != 'JMP': self.depth(-1)
        self.f.code.append(('jmp', kind, l))

    def push(self, v):
        v = int(v)
        if -8 <= v <= 31: self.emit(PUSHK + v + 8)
        elif -128 <= v <= 127: self.op('PUSHB', v & 255)
        elif v % 16 == 0 and -2048 <= v <= 2032: self.op('PUSHX', (v >> 4) & 255)
        else:
            if not -32768 <= v <= 65535: raise Err('constant out of range: %d' % v)
            self.op('PUSHW', v & 255, (v >> 8) & 255)

    def local(self, name, create=False):
        if name not in self.locals:
            if not create: raise Err('%s: unknown name %s' % (self.f.name, name))
            self.locals[name] = len(self.locals)
            if len(self.locals) > 16: raise Err('%s: too many locals' % self.f.name)
        return self.locals[name]

    def compile_fn(self, f):
        self.f, self.locals, self.loops, self.d = f, {}, [], 0
        f.maxd, f.calls = 0, []
        for a in f.args: self.local(a, True)
        # every name assigned anywhere is a local
        for n in ast.walk(f.node):
            if isinstance(n, (ast.Assign, ast.AugAssign)):
                ts = n.targets if isinstance(n, ast.Assign) else [n.target]
                for t in ts:
                    if isinstance(t, ast.Name): self.local(t.id, True)
            elif isinstance(n, ast.For):
                self.local(n.target.id, True)
        f.nargs, f.nlocals = len(f.args), len(self.locals)
        self.body(f.node.body)
        self.push(0); self.op('RET')

    def need(self, name, active=()):   # stack slots from the frame's return pc down
        f = self.fns[name]
        if name in active: return 2 + f.nlocals + f.maxd           # recursion: one more level
        if hasattr(f, 'need'): return f.need
        frame = 2 + f.nlocals
        n = frame + f.maxd + 2
        for callee, d in f.calls:
            n = max(n, frame + d + self.need(callee, active + (name,)))
        if not active: f.need = n
        return n

    def body(self, stmts):
        for s in stmts: self.stmt(s)

    def store(self, t):
        if isinstance(t, ast.Name):
            i = self.local(t.id)
            if i < 16: self.emit(STL0 + i)
            else: self.op('STL', i)
        elif isinstance(t, ast.Attribute):
            if isinstance(t.value, ast.Name) and t.value.id == 'G':
                self.op('STG', GLOBS[t.attr][0])
            else:
                o, ty = ENT[t.attr]
                if isinstance(t.value, ast.Name) and t.value.id in self.locals and self.locals[t.value.id] < 8:
                    self.op('STLE', self.locals[t.value.id] << 5 | o)
                    return
                self.expr(t.value)
                self.op('STE', o)
        else: raise Err('line %d: cannot assign' % t.lineno)

    def stmt(self, s):
        if isinstance(s, ast.Assign):
            if len(s.targets) != 1: raise Err('line %d: one target only' % s.lineno)
            self.expr(s.value); self.store(s.targets[0])
        elif isinstance(s, ast.AugAssign):
            self.expr(s.target); self.expr(s.value); self.op(BINOPS[type(s.op)]); self.store(s.target)
        elif isinstance(s, ast.Expr):
            if isinstance(s.value, ast.Constant): return    # docstring
            self.expr(s.value, void=True)
        elif isinstance(s, ast.If):
            l_else, l_end = self.label(), self.label()
            self.cond(s.test, l_else)
            self.body(s.body)
            if s.orelse:
                self.jump('JMP', l_end)
                self.place(l_else)
                self.body(s.orelse)
                self.place(l_end)
            else:
                self.place(l_else)
        elif isinstance(s, ast.While):
            l_top, l_end = self.label(), self.label()
            self.place(l_top)
            if not (isinstance(s.test, ast.Constant) and s.test.value):
                self.cond(s.test, l_end)
            self.loops.append((l_top, l_end))
            self.body(s.body)
            self.loops.pop()
            self.jump('JMP', l_top)
            self.place(l_end)
        elif isinstance(s, ast.For):
            it = s.iter
            if not (isinstance(it, ast.Call) and it.func.id == 'range'): raise Err('line %d: for range only' % s.lineno)
            a = it.args
            start, stop = (a[0], a[1]) if len(a) > 1 else (ast.Constant(0), a[0])
            step = self.const(a[2]) if len(a) > 2 else 1
            i = s.target.id
            self.expr(start); self.store(ast.Name(i))
            l_top, l_cont, l_end = self.label(), self.label(), self.label()
            self.place(l_top)
            cmp = ast.Compare(ast.Name(i), [ast.Lt() if step > 0 else ast.Gt()], [stop])
            self.cond(cmp, l_end)
            self.loops.append((l_cont, l_end))
            self.body(s.body)
            self.loops.pop()
            self.place(l_cont)
            self.stmt(ast.AugAssign(ast.Name(i), ast.Add(), ast.Constant(step)))
            self.jump('JMP', l_top)
            self.place(l_end)
        elif isinstance(s, ast.Break): self.jump('JMP', self.loops[-1][1])
        elif isinstance(s, ast.Continue): self.jump('JMP', self.loops[-1][0])
        elif isinstance(s, ast.Return):
            if s.value is None: self.push(0)
            else: self.expr(s.value)
            self.op('RET')
            self.d = 0
        elif isinstance(s, ast.Pass): pass
        else: raise Err('line %d: unsupported statement %s' % (s.lineno, type(s).__name__))

    def cond(self, e, l_false):        # jump to l_false when e is false
        if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
            for v in e.values: self.cond(v, l_false)
            return
        if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
            l_true = self.label()
            for v in e.values[:-1]: self.cond_true(v, l_true)
            self.cond(e.values[-1], l_false)
            self.place(l_true)
            return
        if isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not):
            self.cond_true(e.operand, l_false)
            return
        k = self.cmpk(e)
        if k:
            self.expr(e.left); self.depth(-1)
            self.f.code.append(('jk', 'JNEK' if isinstance(e.ops[0], ast.Eq) else 'JEQK', k[0], l_false))
            return
        self.expr(e); self.jump('JZ', l_false)

    def cmpk(self, e):                 # x == k / x != k with a small constant k
        if isinstance(e, ast.Compare) and len(e.ops) == 1 and isinstance(e.ops[0], (ast.Eq, ast.NotEq)):
            k = self.const(e.comparators[0])
            if k is not None and -128 <= k <= 127: return (k,)
        return None

    def cond_true(self, e, l_true):    # jump to l_true when e is true
        if isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not):
            self.cond(e.operand, l_true)
            return
        if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
            for v in e.values: self.cond_true(v, l_true)
            return
        if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
            l_f = self.label()
            for v in e.values[:-1]: self.cond(v, l_f)
            self.cond_true(e.values[-1], l_true)
            self.place(l_f)
            return
        k = self.cmpk(e)
        if k:
            self.expr(e.left); self.depth(-1)
            self.f.code.append(('jk', 'JEQK' if isinstance(e.ops[0], ast.Eq) else 'JNEK', k[0], l_true))
            return
        self.expr(e); self.jump('JNZ', l_true)

    def expr(self, e, void=False):
        if void and not (isinstance(e, ast.Call) and e.func.id != 'FX'):
            self.expr(e); self.op('DROP'); return
        c = self.const(e)
        if c is not None: return self.push(c)
        if isinstance(e, ast.Constant) and isinstance(e.value, str):
            self.f.code.append(('str', e.value)); self.depth(1)
            return
        if isinstance(e, ast.Constant) and e.value in (True, False, None):
            return self.push(int(bool(e.value)))
        if isinstance(e, ast.Name):
            if e.id in self.locals:
                i = self.locals[e.id]
                if i < 16: self.emit(LDL0 + i)
                else: self.op('LDL', i)
            elif e.id in self.fns: self.f.code.append(('addr', e.id)); self.depth(1)
            else: raise Err('%s: unknown name %s' % (self.f.name, e.id))
        elif isinstance(e, ast.Attribute):
            if isinstance(e.value, ast.Name) and e.value.id == 'G':
                if e.attr not in GLOBS: raise Err('unknown global ' + e.attr)
                o, ty = GLOBS[e.attr]
                self.op('LDG', o)
                if ty == 's8': self.push(128); self.op('XOR'); self.push(128); self.op('SUB')
            else:
                if e.attr not in ENT: raise Err('line %d: unknown field %s' % (e.lineno, e.attr))
                o, ty = ENT[e.attr]
                if isinstance(e.value, ast.Name) and e.value.id in self.locals and self.locals[e.value.id] < 8:
                    self.op('LDLE', self.locals[e.value.id] << 5 | o)
                    return
                self.expr(e.value)
                self.op('LDE', o)
        elif isinstance(e, ast.Subscript):
            name = e.value.id
            if name not in self.tables: raise Err('unknown table ' + name)
            self.expr(e.slice)
            self.f.code.append(('tab', name))
        elif isinstance(e, ast.BinOp):
            self.expr(e.left); self.expr(e.right); self.op(BINOPS[type(e.op)])
        elif isinstance(e, ast.UnaryOp):
            self.expr(e.operand)
            self.op({ast.USub: 'NEG', ast.Not: 'NOT', ast.Invert: 'INV'}[type(e.op)])
        elif isinstance(e, ast.Compare):
            if len(e.ops) != 1: raise Err('line %d: one comparison only' % e.lineno)
            self.expr(e.left); self.expr(e.comparators[0]); self.op(CMPS[type(e.ops[0])])
        elif isinstance(e, ast.BoolOp):
            l_end = self.label()
            for i, v in enumerate(e.values):
                self.expr(v)
                if i < len(e.values) - 1:
                    self.op('DUP')
                    self.jump('JZ' if isinstance(e.op, ast.And) else 'JNZ', l_end)
                    self.op('DROP')
            self.place(l_end)
        elif isinstance(e, ast.IfExp):
            l_else, l_end = self.label(), self.label()
            self.cond(e.test, l_else)
            self.expr(e.body); self.jump('JMP', l_end)
            self.d -= 1
            self.place(l_else); self.expr(e.orelse)
            self.place(l_end)
        elif isinstance(e, ast.Call) and e.func.id == 'FX':
            self.expr(e.args[0]); self.push(4); self.op('SHL')
        elif isinstance(e, ast.Call):
            name = e.func.id
            for a in e.args: self.expr(a)
            if name in self.fns:
                f = self.fns[name]
                if len(e.args) != len(f.args): raise Err('line %d: %s takes %d arguments' % (e.lineno, name, len(f.args)))
                self.f.calls.append((name, self.d - len(f.args)))
                self.depth((0 if void else 1) - len(f.args))
                self.f.code.append(('callv' if void else 'call', name))
            elif name in self.nat:
                i, n = self.nat[name]
                if len(e.args) != n: raise Err('line %d: %s takes %d arguments' % (e.lineno, name, n))
                # natives that run bytecode on top of this frame: die (on_death), fork (the thread)
                if name == 'die': self.f.calls.append(('on_death', self.d))
                if name == 'fork' and isinstance(e.args[1], ast.Name) and e.args[1].id in self.fns and self.const(e.args[3]) == 0:
                    self.f.calls.append((e.args[1].id, self.d))      # (a waiting thread runs later)
                self.depth(1 - n)
                self.op('NATV' if void else 'NAT', i)
                if void: self.depth(-1)
            else: raise Err('line %d: unknown function %s' % (e.lineno, name))
        else:
            raise Err('line %d: unsupported expression %s' % (e.lineno, type(e).__name__))

    # ---- assembly ---------------------------------------------------------------------
    def assemble(self):
        hot = [f for f in self.fns.values() if not f.cold]
        cold = [f for f in self.fns.values() if f.cold]
        strs = sorted({x[1] for f in self.fns.values() for x in f.code if isinstance(x, tuple) and x[0] == 'str'})
        longj = set()
        while True:
            addr, labels, size = {}, {}, {}
            def layout(fns, base, extra):
                pc = base
                for f in fns:
                    addr[f.name] = pc
                    pc += 2                                  # header: nargs | nlocals << 4, need
                    for k, it in enumerate(f.code):
                        if isinstance(it, bytes): pc += len(it)
                        elif it[0] == 'label': labels[it[1]] = pc
                        elif it[0] == 'jmp': pc += 3 if (f.name, k) in longj else 2
                        elif it[0] == 'jk': pc += 6 if (f.name, k) in longj else 3
                        elif it[0] in ('call', 'callv', 'addr', 'tab', 'str'): pc += 3
                for name, data in extra:
                    addr[name] = pc; pc += len(data)
                return pc
            tabs = [('T:' + n, bytes(v & 255 for v in t)) for n, t in self.tables.items()]
            strd = [('S:' + s, s.encode() + b'\0') for s in strs]
            hot_end = layout(hot, 0, tabs + strd)
            cold_end = layout(cold, COLD_BASE, [])
            grew = False
            for f in hot + cold:
                pc = addr[f.name] + 2
                for k, it in enumerate(f.code):
                    if isinstance(it, bytes): pc += len(it); continue
                    if it[0] == 'label': continue
                    if it[0] == 'jmp':
                        n = 3 if (f.name, k) in longj else 2
                        d = labels[it[2]] - (pc + n)
                        if n == 2 and not -128 <= d <= 127: longj.add((f.name, k)); grew = True
                        pc += n
                    elif it[0] == 'jk':
                        n = 6 if (f.name, k) in longj else 3
                        d = labels[it[3]] - (pc + n)
                        if n == 3 and not -128 <= d <= 127: longj.add((f.name, k)); grew = True
                        pc += n
                    else: pc += 3
            if not grew: break
        def code(fns, base):
            out = bytearray()
            for f in fns:
                assert addr[f.name] == base + len(out)
                if f.nargs > 15 or f.nlocals > 16: raise Err(f.name + ': too many locals')
                out.append(f.nargs | (f.nlocals - f.nargs) << 4)
                out.append(min(255, self.need(f.name)))
                for k, it in enumerate(f.code):
                    pc = base + len(out)
                    if isinstance(it, bytes): out += it; continue
                    kind = it[0]
                    if kind == 'label': continue
                    if kind == 'jmp':
                        long = (f.name, k) in longj
                        n = 3 if long else 2
                        d = labels[it[2]] - (pc + n)
                        out.append(OPS[it[1] + ('W' if long else '')])
                        out += struct.pack('<h', d) if long else struct.pack('<b', d)
                    elif kind == 'jk' and (f.name, k) in longj:     # PUSHB k, NE/EQ, JZW
                        out += bytes([OPS['PUSHB'], it[2] & 255, OPS['EQ' if it[1] == 'JNEK' else 'NE'], OPS['JZW']])
                        out += struct.pack('<h', labels[it[3]] - (pc + 6))
                    elif kind == 'jk':
                        out.append(OPS[it[1]]); out += struct.pack('<bb', it[2], labels[it[3]] - (pc + 3))
                    elif kind == 'call': out.append(OPS['CALL']); out += struct.pack('<H', addr[it[1]])
                    elif kind == 'callv': out.append(OPS['CALLV']); out += struct.pack('<H', addr[it[1]])
                    elif kind == 'addr': out.append(OPS['PUSHW']); out += struct.pack('<H', addr[it[1]])
                    elif kind == 'tab': out.append(OPS['TAB']); out += struct.pack('<H', addr['T:' + it[1]])
                    elif kind == 'str': out.append(OPS['PUSHW']); out += struct.pack('<H', addr['S:' + it[1]])
            return out
        hb = code(hot, 0)
        for name, data in tabs + strd: hb += data
        cb = code(cold, COLD_BASE)
        return addr, hb, cb


def main(src, dst, cold_out, sprites_h):
    import re
    spr = re.search(r'enum \{([^}]*)\}', open(sprites_h).read()).group(1)
    pre = ''.join('%s = %s\n' % tuple(x.strip().split(' = ')) for x in spr.split(','))
    c = Compiler(pre + open(src).read())
    addr, hot, cold = c.assemble()
    with open(dst, 'w') as f:
        f.write('// generated by tools/jbc.py from %s - do not edit\n#pragma once\n' % src.split('/')[-1])
        for name in sorted(c.fns):
            f.write('#define F_%s 0x%04X\n' % (name, addr[name]))
        for n, v in sorted(c.consts.items()):
            if n.isupper(): f.write('#define K_%s %d\n' % (n, v))
        f.write('#define BC_COLD_SIZE %d\n' % len(cold))
        f.write('#define N_WAIT %d\n' % c.nat['wait'][0])
        f.write('enum { %s };\n' % ', '.join('OP_%s' % n for n in OP0 + OP1 + OP2))
        f.write('#define OP_1 OP_%s\n#define OP_2 OP_%s\n' % (OP1[0], OP2[0]))
        f.write('static const uint8_t NAT_N[%d] = {%s};\n' % (len(NATIVES), ','.join(str(n) for _, n in NATIVES)))
        # the work stack: the deepest of the calls of game.c and of the threads (vm_tick)
        entries = ['update', 'draw', 'hurt', 'on_death', 'gain_health', 'frozen', 'start_game_now',
                   'hud', 'curtains', 'tick', 'init']
        threads = {x[1] for fn in c.fns.values() for x in fn.code if isinstance(x, tuple) and x[0] == 'addr'}
        f.write('#define VM_STACK %d\n' % max(c.need(n) for n in entries + sorted(threads)))
        for n, (o, t) in ENT.items():
            f.write('_Static_assert(offsetof(Ent, %s) == %d, "Ent layout");\n' % (n, o))
            f.write('_Static_assert(ENT_WIDE(%d) == %d && ENT_SIGNED(%d) == %d, "Ent widths");\n' % (o, t in ('u16', 's16'), o, t == 's8'))
        for n, (o, t) in GLOBS.items():
            f.write('_Static_assert(G_WIDE(%d) == %d, "Glob widths");\n' % (o, t in ('u16', 's16')))
        for n, (o, t) in GLOBS.items():
            if n.startswith('rec'):
                f.write('_Static_assert(offsetof(Glob, rec) + %d == %d, "Glob layout");\n' % (2 * int(n[3]), o))
            else:
                f.write('_Static_assert(offsetof(Glob, %s) == %d, "Glob layout");\n' % (n, o))
        f.write('static const uint8_t BC_HOT[%d] = {\n' % len(hot))
        for i in range(0, len(hot), 24):
            f.write('  ' + ','.join(str(b) for b in hot[i:i + 24]) + ',\n')
        f.write('};\n')
    open(cold_out, 'wb').write(cold)
    print('bytecode: %d bytes in flash, %d in the EEPROM' % (len(hot), len(cold)))

if __name__ == '__main__':
    try: main(*sys.argv[1:5])
    except Err as e: sys.exit('jbc: ' + str(e))
