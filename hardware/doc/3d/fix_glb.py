"""Post-process KiCad's GLB export for the web viewer.

KiCad exports the solder mask as a semi-transparent layer (alpha blending) and the
silkscreen as zero-thickness sheets lying on it. Browsers then sort them wrongly at
shallow angles and the artwork fades or flickers. This makes the mask opaque and the
board body match it, and lifts the silkscreen 0.03 mm off the surface.

    python fix_glb.py console_v1.3.glb [silk colour, e.g. #DEC81E]
"""
import sys
import numpy as np
from pygltflib import GLTF2

LIFT = 0.00003                     # metres
BODY = [0.04, 0.04, 0.045, 1.0]    # black board

path = sys.argv[1]
SILK = None
if len(sys.argv) > 2:                                  # recolour the silkscreen (sRGB hex)
    h = sys.argv[2].lstrip('#')
    SILK = [(int(h[i:i + 2], 16) / 255) ** 2.2 for i in (0, 2, 4)] + [1.0]   # glTF colours are linear
g = GLTF2().load(path)
blob = bytearray(g.binary_blob())
silk, body = set(), set()
# silkscreen = matte materials whose geometry is flat sheets on the board's top or bottom
# surface (whatever colour KiCad gave them)
flat = {}
for mesh in g.meshes:
    for prim in mesh.primitives:
        acc = g.accessors[prim.attributes.POSITION]
        if acc.min is None or acc.max is None: continue
        on_surface = abs(acc.max[1] - acc.min[1]) < 2e-6 and (abs(acc.min[1] - 0.001585) < 5e-5 or abs(acc.min[1] + 0.000075) < 5e-5)
        flat.setdefault(prim.material, []).append(on_surface)
for i, m in enumerate(g.materials):
    pbr = m.pbrMetallicRoughness
    c = pbr.baseColorFactor or [1, 1, 1, 1]
    if m.alphaMode == 'BLEND' and max(c[:3]) < 0.2:              # solder mask
        m.alphaMode = 'OPAQUE'; pbr.baseColorFactor = [c[0], c[1], c[2], 1.0]
    elif pbr.metallicFactor == 0 and (pbr.roughnessFactor or 0) > 0.85 and flat.get(i) and all(flat[i]):
        silk.add(i)                                               # silkscreen
        if SILK: pbr.baseColorFactor = SILK
    elif m.name and pbr.metallicFactor == 0 and abs(c[0] - 0.42) < 0.05 and abs(c[1] - 0.45) < 0.05:
        body.add(i); pbr.baseColorFactor = BODY                   # FR4 body
moved = 0
for mesh in g.meshes:
    for prim in mesh.primitives:
        if prim.material not in silk: continue
        acc = g.accessors[prim.attributes.POSITION]
        view = g.bufferViews[acc.bufferView]
        off = (view.byteOffset or 0) + (acc.byteOffset or 0)
        stride = view.byteStride or 12
        pts = []
        for k in range(acc.count):
            o = off + k * stride
            x, y, z = np.frombuffer(blob, dtype='<f4', count=3, offset=o)
            y = y + (LIFT if y > 0.0008 else -LIFT)                # top silk up, bottom silk down
            blob[o + 4:o + 8] = np.float32(y).tobytes()
            pts.append((x, y, z))
        pts = np.array(pts, dtype=np.float32)                      # glTF requires POSITION bounds
        acc.min = [float(v) for v in pts.min(0)]; acc.max = [float(v) for v in pts.max(0)]
        moved += 1
g.set_binary_blob(bytes(blob))
g.save(path)
print(f'mask opaque, body recoloured ({len(body)}), silkscreen lifted ({moved} primitives)')
