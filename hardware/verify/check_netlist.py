"""Check that build/console.net contains the rev. 1.1 board unchanged.

Every pin of the original board is probed on the rev. 1.1 Gerbers (gerber_nets.py) and
the resulting groups of connected pins must equal the rev. 1.2 nets restricted to the
same pins. Parts added in rev. 1.2 are listed separately.

    python verify/check_netlist.py <rev1.1 gerber dir>
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gerber_nets, netlist

HERE = os.path.dirname(os.path.abspath(__file__))
probes = {k: v for k, v in json.load(open(f'{HERE}/rev11_pads.json')).items() if not k.startswith('_')}
_, nets = netlist.read(f'{HERE}/../build/console.net')

old = {frozenset(g) for g in gerber_nets.nets(sys.argv[1], probes)}
new = {}
for name, pins in nets.items():
    p = frozenset(pins & probes.keys())
    if p: new[p] = name
# a probed pin that is unconnected on the old board must be a single-pin group in both
new_groups = set(new) | {frozenset([p]) for p in probes if not any(p in g for g in new)}

ok = old == new_groups
for g in sorted(old | new_groups, key=lambda g: sorted(g)):
    tag = 'same ' if g in old and g in new_groups else ('ONLY rev1.1' if g in old else 'ONLY rev1.2')
    print(f'{tag:12} {new.get(g, "-"):6} {" ".join(sorted(g))}')
added = sorted({p for pins in nets.values() for p in pins} - probes.keys())
print('\npins added since rev. 1.1:', ' '.join(added))
print('\nRESULT:', 'netlist contains the rev. 1.1 board unchanged' if ok else 'MISMATCH')
sys.exit(0 if ok else 1)
