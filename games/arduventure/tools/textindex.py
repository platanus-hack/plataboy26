"""Appends the text index to the far data image (post-link): the start offset of every
entry of `library` and `sentences` (length-prefixed entries, as findBegin walks them).
Layout at __far_end: u16 number of library entries W, W offsets, then the sentence
offsets (u16 each, little endian).

    python tools/textindex.py game.elf far.bin nm-tool
"""
import struct, subprocess, sys
elf, far, nm = sys.argv[1], sys.argv[2], sys.argv[3]
FAR_BASE = 0x30000000
syms = {}
for l in subprocess.run([nm, '-S', '-C', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) in (3, 4):                      # no size column for linker symbols
        name = p[-1].split('.')[0]            # LTO renames internal symbols: x.lto_priv.0
        syms[name] = (int(p[0], 16), int(p[1], 16) if len(p) == 4 else 0)
data = bytearray(open(far, 'rb').read())

def entries(name):
    a, n = syms[name]
    blob = data[a - FAR_BASE:a - FAR_BASE + n]
    offs, i = [], 0
    while i < n:
        offs.append(i); i += blob[i] + 1
    return offs

words, sents = entries('library'), entries('sentences')
end = syms['__far_end'][0] - FAR_BASE if '__far_end' in syms else len(data)
assert end == len(data), 'the index must come right after the far data'
data += struct.pack('<H', len(words)) + b''.join(struct.pack('<H', o) for o in words + sents)
open(far, 'wb').write(data)
print(f'text index: {len(words)} words, {len(sents)} sentences, {2 + 2 * (len(words) + len(sents))} bytes')
