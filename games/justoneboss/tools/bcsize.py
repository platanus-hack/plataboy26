#!/usr/bin/env python3
# Prints the size of each bytecode function of src/logic.py (hot = in flash)
import re, sys
src = open('tools/jbc.py').read().replace("if __name__ == '__main__':", "if False:")
exec(src)
spr = re.search(r'enum \{([^}]*)\}', open('src/sprites.h').read()).group(1)
pre = ''.join('%s = %s\n' % tuple(x.strip().split(' = ')) for x in spr.split(','))
c = Compiler(pre + open('src/logic.py').read())
addr, hot, cold = c.assemble()
for bank, data, base in (('hot', hot, 0), ('cold', cold, 0x8000)):
    fs = sorted((addr[n], n) for n, f in c.fns.items() if (base == 0) != f.cold)
    end = min([addr[k] for k in addr if k.startswith(('T:', 'S:'))] or [len(data)]) if base == 0 else base + len(data)
    rows = [(((fs[i + 1][0] if i + 1 < len(fs) else end) - a), n) for i, (a, n) in enumerate(fs)]
    print(bank, len(data), ' '.join('%s %d' % (n, s) for s, n in sorted(rows, reverse=True)))
