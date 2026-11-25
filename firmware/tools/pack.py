"""Builds the plataboy26 game library: the EEPROM image and the matching flash image.

    python pack.py --kbase 0x3C40 --kernel kernel.bin --menu menu.bin \\
                   --library library.json --out build/

build/eeprom.bin (128 KiB)  header, directory and the compressed programs, laid out
                            as in menu/src/library.h; the save area stays erased (0xFF)
build/flash.bin  (16 KiB)   the menu installed at 0, the kernel at KBASE, a valid config
build/library.txt           contents and free space
build/writer/writer_NN.bin  the EEPROM contents in chunks of up to 12 KiB, each with the
                            writer program (writer/main.c) that puts it into the EEPROM

Each program's word 0 must be "j <reset handler>" (as the games' startup code makes it).
The packer stores that target in the directory and patches word 0 to "j KBASE" before
compressing, so a power-on always passes through the kernel.
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lz import compress, decompress


def columns(pixels):
    """128 x 64 pixels (one byte each) -> 1024 bytes, column-major, LSB = top pixel"""
    out = bytearray()
    for x in range(128):
        for page in range(8):
            out.append(sum(1 << b for b in range(8) if pixels[(page * 8 + b) * 128 + x] > 128))
    return bytes(out)


def card_image(path):
    """a 128 x 64 screenshot (or a 4x one, as the simulator's thumbnails) -> the menu's
    card: column-major bytes (column 0: pages 0..7, ...), LZ-compressed (tools/lz.py)"""
    from PIL import Image
    im = Image.open(path).convert('L').resize((128, 64), Image.NEAREST)
    return compress_cached(columns(bytes(im.getdata())))

EE_SIZE = 128 * 1024
HDR, DIR, DATA, DIR_MAX = 0x0000, 0x0020, 0x0800, 47
CARDS = DIR + 32 * DIR_MAX                         # card table: u32 address, u16 size per entry
# test scratch pages; the save area (2 KiB) except 0x1F010-0x1F0FF, the unused end of the
# menu's slot (programs may go there)
RESERVED = [(0x0FF00, 0x10100), (0x1F000, 0x1F010), (0x1F100, 0x1F800)]
BANKS = [(DATA, 0x10000), (0, 0x20000)]                # a stream must not cross 0x10000
# The EEPROM save slots: fixed addresses in each program's own code (menu/src/library.h).
# Each game checks at build time that its save fits its slot (a _Static_assert, or
# tools/save_check.cpp); here: no two slots overlap, and a library's "save" agrees with
# this table. A slot past the 2 KiB save area (MicroCity's) is kept free of streams.
SAVE_SLOTS = {                                         # name: (address, size)
    None:            (0x1F000, 0x10),                  # the menu (last selection, contrast, +4 joypad thresholds)
    'Hopper':        (0x1F100, 0xC0),                  # games/hopper/src/arduboy.c
    'Just One Boss': (0x1F1C0, 0x40),                  # games/justoneboss/src/plat.c
    'Arduventure':   (0x1F200, 0x400),                 # games/arduventure/port/platform.cpp
    'MicroCity':     (0x1F600, 0x46C),                 # games/microcity/port/platform.cpp
}
_s = sorted(SAVE_SLOTS.values())
for (a, n), (b, _) in zip(_s, _s[1:]):
    assert a + n <= b, f'save slots overlap at 0x{b:05X}'
MAGIC = 0x36324250
JOYCAL_EE = 0x1F004                     # games/common/joycal.h


def joycal_thresholds(i):
    """games/common/joycal.h joycal_thresholds(): the joypad thresholds for idle level i."""
    mid = [m // 4 * 4 + 2 for m in (46, 147, 231, 304, 368, 415, 476, 539, 591, 680)]
    return bytes(((c * (1024 - 2 * i) + 1024 * i) * 1024 // (1024 * 1024 - c * i) + 3) >> 2 for c in mid)


CFG = 0x3FC0
FLASH = 0x4000
TOOL = 0x01
SIM_WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'sim', 'web')
WR_DATA, WR_MAX = 0x1000, 0x3000 - 16      # writer: data at 0x1000, up to 12 KiB per chunk


def jal_target(word):
    if word & 0xFFF != 0x06F: return None              # JAL x0
    imm = ((word >> 31) & 1) << 20 | ((word >> 12) & 0xFF) << 12 | ((word >> 20) & 1) << 11 | ((word >> 21) & 0x3FF) << 1
    return imm - (1 << 21) if imm & (1 << 20) else imm


def jal(offset):
    assert -(1 << 20) <= offset < (1 << 20) and not offset & 1
    o = offset & 0x1FFFFF
    return (((o >> 20) & 1) << 31 | ((o >> 1) & 0x3FF) << 21 | ((o >> 11) & 1) << 20 | ((o >> 12) & 0xFF) << 12 | 0x06F)


def checksum(data):
    s = 0
    for b in data: s = ((s * 33) ^ b) & 0xFFFFFFFF
    return s


def prepare(name, data, kbase):
    """-> (installed image, reset target)"""
    if len(data) > kbase:
        sys.exit(f'{name}: {len(data)} bytes, larger than the {kbase} bytes below the kernel')
    w0 = struct.unpack_from('<I', data)[0]
    entry = jal_target(w0)
    if entry is None or not 4 <= entry < len(data):
        sys.exit(f'{name}: word 0 is not "j <reset handler>" (0x{w0:08x})')
    img = struct.pack('<I', jal(kbase)) + data[4:]
    return img, entry


def free_ranges(extra=(), reserved=None):
    """free EEPROM ranges for program data, split at 0x10000 and around reserved areas
    (and around programs' own data regions, `extra`)"""
    out = []
    reserved = (RESERVED if reserved is None else reserved) + list(extra)
    for lo, hi in [(DATA, 0x10000), (0x10000, 0x20000)]:
        cuts = [(max(lo, a), min(hi, b)) for a, b in reserved if a < hi and b > lo]
        p = lo
        for a, b in sorted(cuts):
            if a > p: out.append([p, a])
            p = max(p, b)
        if p < hi: out.append([p, hi])
    return out


def compress_cached(data):
    """compress(), remembered in tools/.lzcache/ (the optimal parse takes minutes on 15 KB)"""
    import hashlib
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.lzcache')
    f = os.path.join(d, hashlib.sha1(data).hexdigest() + '.lz')
    if os.path.exists(f): return open(f, 'rb').read()
    c = compress(data)
    os.makedirs(d, exist_ok=True)
    open(f, 'wb').write(c)
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kbase', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--menu', required=True)
    ap.add_argument('--writer', required=True)
    ap.add_argument('--library', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    kbase = a.kbase
    kernel = open(a.kernel, 'rb').read()
    assert kbase % 64 == 0 and kbase + len(kernel) <= CFG, 'kernel does not fit below the config page'

    lib = json.load(open(a.library))
    base = os.path.dirname(os.path.abspath(a.library))
    progs = [{'name': 'Menu', 'file': os.path.abspath(a.menu), 'flags': []}] + lib['programs']
    thumbs = thumbnails()
    items = [load_item(p, base, kbase, thumbs, menu=p is progs[0]) for p in progs]
    regions, ranges, free, used = place(items, lib.get('test_scratch', True))
    reserved = reserved_areas(lib.get('test_scratch', True))
    write(a, kbase, kernel, items, regions, ranges, free, used, reserved)


def thumbnails():
    """menu cards: the simulator's thumbnail of each program (sim/web/roms/index.json,
    local.json), unless the library file gives "card": an image path"""
    thumbs = {}
    for idx in ('index.json', 'local.json'):
        f = os.path.join(SIM_WEB, 'roms', idx)
        if os.path.exists(f):
            for g in json.load(open(f))['games']:
                if g.get('thumb'): thumbs[os.path.basename(g['file'])] = g['thumb']
    return thumbs


def reserved_areas(test_scratch=True):
    """test_scratch false: no board_test / eeprom_test, their scratch pages are free for programs"""
    return [r for r in RESERVED if test_scratch or r != (0x0FF00, 0x10100)]


def load_item(p, base, kbase, thumbs, menu=False):
    """one library entry -> its installed image, compressed stream, data region, card"""
    path = p['file'] if os.path.isabs(p['file']) else os.path.join(base, p['file'])
    data = open(path, 'rb').read()
    name = p['name']
    if len(name.encode()) > 15: sys.exit(f'{name}: name longer than 15 characters')
    img, entry = prepare(name, data, kbase)
    c = compress_cached(img)
    assert decompress(c, len(img)) == img
    flags = TOOL if 'tool' in p.get('flags', []) else 0
    data = None
    if 'data' in p:                      # the program's own EEPROM region (fixed address)
        dpath = p['data']['file']
        dpath = dpath if os.path.isabs(dpath) else os.path.join(base, dpath)
        data = (int(p['data']['at'], 0), open(dpath, 'rb').read())
    save = None
    if 'save' in p:                      # its save slot beyond the save area: left erased
        save = (int(p['save']['at'], 0), int(p['save']['size'], 0))
        if SAVE_SLOTS.get(name) != save:
            sys.exit(f'{name}: "save" 0x{save[0]:05X}+0x{save[1]:X} differs from SAVE_SLOTS in pack.py')
    elif name in SAVE_SLOTS and sum(SAVE_SLOTS[name]) > 0x1F800:
        save = SAVE_SLOTS[name]          # (past the save area: must be kept free anyway)
    card = None
    cpath = p.get('card') or thumbs.get(os.path.basename(path))
    if p.get('card') is False: cpath = None          # "card": false -> the name card
    if cpath and not menu:
        cpath = cpath if os.path.isabs(cpath) else os.path.join(base if 'card' in p else SIM_WEB, cpath)
        card = card_image(cpath)
    return dict(name=name, img=img, entry=entry, c=c, flags=flags, file=path, data=data, card=card, save=save)


def place(items, test_scratch=True):
    """gives every stream and card its EEPROM address (it['offset'], it['card_at']);
    -> (data regions, free ranges left, free bytes, used bytes). Exits when it does not fit."""
    if len(items) > DIR_MAX: sys.exit(f'too many programs ({len(items) - 1}, max {DIR_MAX - 1})')
    reserved = reserved_areas(test_scratch)
    # programs' own data regions first (fixed addresses), then the streams: largest first,
    # best fit into the free ranges
    for it in items:                    # Nitro Nonsense: the region knows its own address
        if it['data'] and it['data'][1][:4] == b'NNOV' and struct.unpack_from('<I', it['data'][1], 8)[0] != it['data'][0]:
            sys.exit(f'{it["name"]}: the data region was built for another address (make DATA_BASE=0x{it["data"][0]:05X})')
    regions = [(it['data'][0], it['data'][0] + len(it['data'][1])) for it in items if it['data']]
    for lo, hi in regions:
        if any(lo < y and hi > x for x, y in reserved) or (lo < 0x10000 < hi):
            sys.exit(f'data region 0x{lo:05X}-0x{hi - 1:05X} overlaps a reserved area or 0x10000')
    for (lo, hi), (lo2, hi2) in zip(sorted(regions), sorted(regions)[1:]):
        if lo2 < hi:
            sys.exit(f'data regions 0x{lo:05X}-0x{hi - 1:05X} and 0x{lo2:05X}-0x{hi2 - 1:05X} overlap')
    saves = [(it['save'][0], it['save'][0] + it['save'][1]) for it in items if it['save']]
    for lo, hi in saves:
        if any(lo < y and hi > x for x, y in regions) or (lo < 0x10000 < hi):
            sys.exit(f'save slot 0x{lo:05X}-0x{hi - 1:05X} overlaps a data region or 0x10000')
    ranges = free_ranges(regions + saves, reserved)
    blobs = [(it, 'c', 'offset') for it in items] + [(it, 'card', 'card_at') for it in items if it['card']]
    # depth first: each blob into the tightest range that fits, backtracking to the next
    # tightest when the rest does not fit (the first path is plain best fit, so a library
    # that best fit can place keeps its layout)
    order = sorted(blobs, key=lambda b: -len(b[0][b[1]]))
    sizes = [len(it[key]) for it, key, at in order]
    need = [sum(sizes[i:]) for i in range(len(sizes) + 1)]
    steps, deepest = [0], [0]

    def dfs(i):
        if i == len(order): return True
        steps[0] += 1
        if steps[0] > 200000 or sum(r[1] - r[0] for r in ranges) < need[i]: return False
        deepest[0] = max(deepest[0], i)
        it, key, at = order[i]; n = sizes[i]; tried = set()
        for r in sorted((r for r in ranges if r[1] - r[0] >= n), key=lambda r: r[1] - r[0]):
            if r[1] - r[0] in tried: continue          # same free size: same outcome
            tried.add(r[1] - r[0])
            it[at] = r[0]; r[0] += n
            if dfs(i + 1): return True
            r[0] -= n
        return False

    if not dfs(0):
        it, key, at = order[deepest[0] + 1 if deepest[0] + 1 < len(order) else deepest[0]]
        short = need[0] - sum(r[1] - r[0] for r in ranges)
        sys.exit(f'EEPROM full: no room for {it["name"]} ({"card, " if key == "card" else ""}{len(it[key])} bytes)'
                 + (f'; {short} bytes short' if short > 0 else f'; fragmentation ({-short} bytes free in all, '
                    f'ranges {sorted(r[1] - r[0] for r in ranges)}, blobs {sorted(sizes)}, {steps[0]} steps)'))
    free = sum(r[1] - r[0] for r in ranges)
    used = sum(len(i['c']) + (len(i['data'][1]) if i['data'] else 0) + len(i['card'] or b'') for i in items)
    return regions, ranges, free, used


def write(a, kbase, kernel, items, regions, ranges, free, used, reserved):
    ee = bytearray(b'\xFF' * EE_SIZE)
    ee[HDR:HDR + 32] = struct.pack('<IBBHII16s', MAGIC, 1, len(items), 0, used, free, b'\xFF' * 16)
    for i, it in enumerate(items):
        ent = struct.pack('<IHHII15sB', it['offset'], len(it['c']), len(it['img']), it['entry'],
                          checksum(it['img']), it['name'].encode(), it['flags'])
        ee[DIR + 32 * i:DIR + 32 * (i + 1)] = ent
        ee[it['offset']:it['offset'] + len(it['c'])] = it['c']
        if it['card']:
            ee[CARDS + 6 * i:CARDS + 6 * i + 6] = struct.pack('<IH', it['card_at'], len(it['card']))
            ee[it['card_at']:it['card_at'] + len(it['card'])] = it['card']
        if it['data']:
            at, blob = it['data']
            ee[at:at + len(blob)] = blob
    ee[JOYCAL_EE:JOYCAL_EE + 10] = joycal_thresholds(0)   # nominal; the menu measures the board's
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'eeprom.bin'), 'wb').write(ee)

    # flash: the menu installed, the kernel, the config for entry 0
    menu = items[0]
    fl = bytearray(b'\xFF' * FLASH)
    fl[:len(menu['img'])] = menu['img']
    fl[kbase:kbase + len(kernel)] = kernel
    fl[CFG:CFG + 16] = struct.pack('<IIII', MAGIC, menu['entry'], 0, ~(menu['entry'] ^ 0) & 0xFFFFFFFF)
    open(os.path.join(a.out, 'flash.bin'), 'wb').write(fl)

    # writer chunks: the used ranges of the EEPROM (header + directory, the streams, the
    # nominal joypad thresholds)
    used_r = sorted([(0, DATA), (JOYCAL_EE, JOYCAL_EE + 10)] + [(it['offset'], it['offset'] + len(it['c'])) for it in items] + regions
                    + [(it['card_at'], it['card_at'] + len(it['card'])) for it in items if it['card']])
    merged = []
    for lo, hi in used_r:
        if merged and lo <= merged[-1][1]: merged[-1][1] = max(merged[-1][1], hi)
        else: merged.append([lo, hi])
    writer = open(a.writer, 'rb').read()
    assert len(writer) <= WR_DATA, f'writer is {len(writer)} bytes, more than 0x{WR_DATA:X}'
    chunks = []
    for lo, hi in merged:
        while lo < hi:
            n = min(hi - lo, WR_MAX); chunks.append((lo, n)); lo += n
    wd = os.path.join(a.out, 'writer'); os.makedirs(wd, exist_ok=True)
    for f in os.listdir(wd): os.remove(os.path.join(wd, f))
    for k, (addr, n) in enumerate(chunks):
        data = bytes(ee[addr:addr + n])
        img = writer.ljust(WR_DATA, b'\xFF') + struct.pack('<IIHBBI', 0x52574250, addr, n, k + 1,
                                                          len(chunks), checksum(data)) + data
        assert len(img) <= FLASH
        open(os.path.join(wd, f'writer_{k + 1:02}.bin'), 'wb').write(img)

    lines = [f'plataboy26 game library   kernel {len(kernel)} bytes at 0x{kbase:04X}, '
             f'programs up to {kbase} bytes', '',
             f'{"#":>2}  {"name":15} {"size":>6} {"stored":>7} {"ratio":>6}  EEPROM']
    for i, it in enumerate(items):
        lines.append(f'{i:2}  {it["name"]:15} {len(it["img"]):6} {len(it["c"]):7} '
                     f'{100 * len(it["c"]) / len(it["img"]):5.1f}%  0x{it["offset"]:05X}'
                     + ('  tool' if it['flags'] & TOOL else '')
                     + (f'  + data {len(it["data"][1])} B at 0x{it["data"][0]:05X}' if it['data'] else '')
                     + (f'  + save 0x{it["save"][0]:05X}-0x{sum(it["save"]) - 1:05X}' if it['save'] else '')
                     + (f'  card {len(it["card"])} B' if it['card'] else ''))
    tu = sum(len(i['img']) for i in items)
    lines += ['', f'    {"total":15} {tu:6} {used:7} {100 * used / tu:5.1f}%',
              f'EEPROM: {used} bytes of programs, {free} bytes free for more '
              f'(~{free * 100 // (75 * 8000)} more 8 KB games at 75%)',
              'free ranges: ' + ', '.join(f'0x{r[0]:05X}-0x{r[1] - 1:05X}' for r in ranges if r[1] > r[0]),
              'reserved: ' + ('0x0FF00-0x100FF test scratch, ' if (0x0FF00, 0x10100) in reserved else '')
              + '0x1F000-0x1F7FF save area (2 KiB)',
              f'production: {len(chunks)} writer chunks (build/writer/), then build/flash.bin']
    open(os.path.join(a.out, 'library.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
