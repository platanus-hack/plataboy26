# Combine 128x64 PGMs into one grid PNG: montage.py out.png cols scale a.pgm b.pgm ...
import sys, zlib, struct
out, cols, scale = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); files = sys.argv[4:]
imgs = [open(f, 'rb').read().split(b'\n', 3)[3] for f in files]
rows_n = (len(imgs) + cols - 1) // cols; W = cols * (128 * scale + 4); H = rows_n * (64 * scale + 4)
canvas = bytearray([60]) * (W * H)
for i, px in enumerate(imgs):
    ox = (i % cols) * (128 * scale + 4); oy = (i // cols) * (64 * scale + 4)
    for y in range(64 * scale):
        for x in range(128 * scale):
            canvas[(oy + y) * W + ox + x] = px[(y // scale) * 128 + x // scale]
raw = b''.join(b'\x00' + bytes(canvas[y * W:(y + 1) * W]) for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
