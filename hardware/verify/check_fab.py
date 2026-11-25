"""Check the copper of the generated rev. 1.2 Gerbers against build/console.net.

Traces every pad through the fab outputs (gerber_nets.py: copper + plated holes) and
compares the resulting groups with the nets of the netlist. Pads that share a pin
number (both pads of a tact-switch terminal) are joined inside the part.

    python verify/check_fab.py build/fab/gerbers build/rev12_pads.json
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gerber_nets, netlist

HERE = os.path.dirname(os.path.abspath(__file__))
probes = json.load(open(sys.argv[2]))
_, nets = netlist.read(f'{HERE}/../build/console.net')

parent = {}
def find(a):
    parent.setdefault(a, a)
    while parent[a] != a: a = parent[a]
    return a
for g in gerber_nets.nets(sys.argv[1], probes):
    pins = [p.split('#')[0] for p in g]
    for p in pins: find(p)
    if not g[0].startswith('unconnected'):
        for p in pins[1:]: parent[find(p)] = find(pins[0])
copper = {}
for p in list(parent): copper.setdefault(find(p), set()).add(p)
copper = {frozenset(v) for v in copper.values()}

pins = {p.split('#')[0] for p in probes}
expect = {frozenset(v & pins) for v in nets.values() if v & pins}
expect |= {frozenset([p]) for p in pins if not any(p in g for g in expect)}

bad = copper ^ expect
for g in sorted(bad, key=sorted):
    print('copper' if g in copper else 'netlist', ' '.join(sorted(g)))
print(f'{len(pins)} pins, {len(expect)} nets:',
      'fab Gerbers implement the netlist exactly' if not bad else 'MISMATCH')
sys.exit(1 if bad else 0)
