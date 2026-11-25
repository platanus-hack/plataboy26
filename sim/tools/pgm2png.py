# Convert 128x64 PGM screenshots to scaled PNGs (no dependencies).
import sys, zlib, struct
def conv(src, dst, scale=4):
    data = open(src, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split()); px = parts[3]
    rows = []
    for y in range(h):
        line = px[y*w:(y+1)*w]
        row = bytearray()
        for v in line: row += bytes([v]) * scale
        for _ in range(scale): rows.append(b'\x00' + bytes(row))
    raw = b''.join(rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w*scale, h*scale, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
for a in sys.argv[1:]: conv(a, a.rsplit('.', 1)[0] + '.png')
