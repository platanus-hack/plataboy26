"""PB26-LZ: the game compression of the plataboy26 loader.

LZSS with a bit stream, made for a decoder of ~150 bytes of RV32EC code that writes
its output straight into flash (back references read already-programmed flash, so
the whole 16 KiB of output is the window and no RAM window is needed):

    token  = 0 <8-bit literal>
           | 1 <gamma(len - 1)> <offset>          len >= 2
    offset = gamma((off - 1) >> 7 + 1) <7 bits: (off - 1) & 127>
    gamma(n), n >= 1: (bit length - 1) zeros, then n in binary (MSB first)

Bits are read MSB first from consecutive bytes. The stream has no end marker: the
decoder stops after the uncompressed size from the directory. The parse is optimal
(shortest bit count) for this format.

    python lz.py file.bin ...     prints ratios and checks the round trip
"""
import sys


def gamma_bits(n):
    return 2 * n.bit_length() - 1


def off_bits(off):
    return gamma_bits(((off - 1) >> 7) + 1) + 7


class Bits:
    def __init__(self): self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1); self.n += 1
            if self.n == 8: self.out.append(self.acc); self.acc = self.n = 0

    def gamma(self, v):
        self.put(0, v.bit_length() - 1); self.put(v, v.bit_length())

    def bytes(self):
        return bytes(self.out) + (bytes([self.acc << (8 - self.n)]) if self.n else b'')


def compress(data, max_cand=512):
    n = len(data)
    # candidate matches per position: hash chains on 2-byte prefixes, nearest first
    chains, matches = {}, [None] * n
    for i in range(n):
        lst = []
        if i + 1 < n:
            key = data[i:i + 2]
            best = 1
            for j in reversed(chains.get(key, [])[-max_cand:]):
                l = 2
                while i + l < n and data[j + l] == data[i + l]: l += 1
                if l > best:                     # only offsets that reach a longer length
                    lst.append((i - j, l)); best = l
            chains.setdefault(key, []).append(i)
        matches[i] = lst
    INF = 1 << 60
    cost = [INF] * (n + 1); cost[n] = 0
    choice = [None] * n
    for i in range(n - 1, -1, -1):
        c = 9 + cost[i + 1]; ch = None
        prev = 1
        for off, l in matches[i]:               # increasing offset, increasing length
            ob = 1 + off_bits(off)
            for k in range(prev + 1, l + 1):
                cc = ob + gamma_bits(k - 1) + cost[i + k]
                if cc < c: c, ch = cc, (off, k)
            prev = l
        cost[i], choice[i] = c, ch
    b, i = Bits(), 0
    while i < n:
        ch = choice[i]
        if ch is None: b.put(0, 1); b.put(data[i], 8); i += 1
        else:
            off, l = ch
            b.put(1, 1); b.gamma(l - 1); b.gamma(((off - 1) >> 7) + 1); b.put((off - 1) & 127, 7); i += l
    return b.bytes()


def decompress(src, usize):
    pos = [0, 0]                                 # byte index, bit index

    def bit():
        v = (src[pos[0]] >> (7 - pos[1])) & 1
        pos[1] += 1
        if pos[1] == 8: pos[0] += 1; pos[1] = 0
        return v

    def bits(k):
        v = 0
        for _ in range(k): v = (v << 1) | bit()
        return v

    def gamma():
        z = 0
        while not bit(): z += 1
        return (1 << z) | bits(z)

    out = bytearray()
    while len(out) < usize:
        if not bit(): out.append(bits(8))
        else:
            l = gamma() + 1
            off = ((gamma() - 1) << 7 | bits(7)) + 1
            for _ in range(l): out.append(out[-off])
    return bytes(out[:usize])


if __name__ == '__main__':
    tot_u = tot_c = 0
    for p in sys.argv[1:]:
        d = open(p, 'rb').read(); c = compress(d)
        assert decompress(c, len(d)) == d, p
        tot_u += len(d); tot_c += len(c)
        print(f'{len(d):6} -> {len(c):6}  {100 * len(c) / len(d):5.1f}%  {p}')
    print(f'{tot_u:6} -> {tot_c:6}  {100 * tot_c / tot_u:5.1f}%  total')
