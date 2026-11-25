# The original game's sprites (game.js, Paolo's "Llegamos tarde a la hackathon"): the CSEF
# decoder (parseCSEF in game.js) and the palette, in Python.
import re

P = [0x000000, 0x24222a, 0x4e4b5b, 0x7b768e, 0xaba4c1, 0xd3cde7, 0xfefdfe, 0xffefa8,
     0xe2b35a, 0x9f5611, 0x6e2100, 0x390800, 0x5e2e00, 0x915f01, 0xe6c429, 0xeceab7,
     0xd2fe7d, 0xc1e12c, 0x989800, 0x5b4d00, 0x362400, 0x004d03, 0x0c6d00, 0x2b9200,
     0x7ec43f, 0xb2da73, 0xc8feae, 0x83fe6b, 0xff6600, 0x00cb22, 0x006d45, 0x004d3d,
     0x206100, 0x019000, 0x0bba3d, 0x2eda91, 0x4fffca, 0xd0fff6, 0xa9fbee, 0x01ffff,
     0x009cbe, 0x006092, 0x004373, 0x006cdc, 0x6dd0ff, 0xb6f3ff, 0xa4dbff, 0x687aff,
     0x0147ff, 0x0017c5, 0x140c81, 0x4200a5, 0x8d00f9, 0xc84ff5, 0xea9bf3, 0xf8dcf7,
     0xf49fb3, 0xf6629d, 0xff0092, 0xcc0095, 0xa30092, 0x920030, 0xc1003f, 0xff0000,
     0xf5765d, 0xd11717, 0xa41c1c, 0xab8169, 0x7c6822, 0xffc7ba, 0x254c93, 0xdeac92,
     0x18171a, 0xf7bb1b, 0xBF7F1F]
CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%&*()_+-=[]{};:,/<?`"'

def parse(s, w=30):
    r, p, i = [], [], 0
    while i < len(s):
        if not p:
            if s[i] == '~': r.append(['.'] * w); i += 1; continue
            if s[i] == '^':
                if r: r.append(list(r[-1]))
                i += 1; continue
        if s[i] == '>' and len(p) >= w // 2 and w % 2 == 0:
            h = p[:w // 2]; r.append(h + h[::-1]); p.clear(); i += 1; continue
        n = int(s[i]) if s[i].isdigit() else 0
        if not n or i + 1 >= len(s): i += 1; continue
        p += [s[i + 1]] * n
        while len(p) >= w: r.append(p[:w]); del p[:w]
        i += 2
    if p: r.append(p)
    return r

def rgb(c):
    k = CHARS.find(c)
    if k < 0 or k >= len(P): return None
    v = P[k]; return (v >> 16, (v >> 8) & 255, v & 255)

def load(js):
    """{name: {frame: rows}} for the 8 characters and 'Guard'"""
    src = open(js, encoding='utf-8').read()
    out = []
    body = src[src.index('const CHARACTERS'):src.index('// Mirror sprites for guard')]
    for m in re.finditer(r"name: '([^']*)',\s*raw: \{(.*?)\n    \}", body, re.S):
        fr = dict(re.findall(r"^\s*(I|R0|R1|F)\s*:\s*'([^']*)'", m.group(2), re.M))
        out.append((m.group(1), {k: parse(v) for k, v in fr.items()}))
    g = src[src.index('const GUARD = {'):src.index('// Mirror sprites for guard')]
    fr = dict(re.findall(r"^\s*(I|R0|R1|F)\s*:\s*'([^']*)'", g, re.M))
    out.append(('Guard', {k: parse(v) for k, v in fr.items()}))
    return out
