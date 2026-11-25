"""Minimal reader for KiCad netlists (.net): {net name: set('REF.PIN')} and components."""
import re


def _sexp(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack, cur = [], []
    for t in tokens:
        if t == '(':
            stack.append(cur); cur = []
        elif t == ')':
            done, cur = cur, stack.pop(); cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def _find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def _val(node, key, default=None):
    f = _find(node, key)
    return f[0][1] if f and len(f[0]) > 1 else default


def read(path):
    root = _sexp(open(path).read())
    comps = {}
    for c in _find(_find(root, 'components')[0], 'comp'):
        fields = {_val(f, 'name'): (f[2] if len(f) > 2 else '') for f in _find(_find(c, 'fields')[0], 'field')} if _find(c, 'fields') else {}
        comps[_val(c, 'ref')] = {'value': _val(c, 'value'), 'footprint': _val(c, 'footprint'), 'fields': fields}
    nets = {}
    for n in _find(_find(root, 'nets')[0], 'net'):
        nets[_val(n, 'name')] = {f"{_val(nd, 'ref')}.{_val(nd, 'pin')}" for nd in _find(n, 'node')}
    return comps, nets
