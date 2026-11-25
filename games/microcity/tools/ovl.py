"""Overlay build steps (see port/platform.cpp, code overlays).

  window ELF OBJDUMP          print the window size: the largest group, in 64-byte pages
  check  ELF OBJDUMP          verify the final link: resident code references a group's
                              code only from the functions that load it first (CALLERS);
                              groups never reference each other (the linker's NOCROSSREFS
                              checks that too)
  image  ELF OBJDUMP EEDATA EEADDR OUT
                              the game's EEPROM region: header at EEADDR-32 ("MCOV", then
                              per group u32 address + u16 size), the EEDATA section at
                              EEADDR, then the group images as LZ streams (firmware/tools/
                              lz.py; the kernel unpacks them, games/common/kunpack.S). OUT.eeprom: a full 128 KiB
                              image (simulator), OUT.region: the region alone (the game
                              library packer, firmware/tools/pack.py "data")
"""
import hashlib, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'firmware', 'tools'))
from lz import compress, decompress


def lz_cached(data):
    """the library's LZ format (unpacked by the kernel), cached as pack.py does"""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'firmware', 'tools', '.lzcache')
    f = os.path.join(d, hashlib.sha1(data).hexdigest() + '.lz')
    if os.path.exists(f): return open(f, 'rb').read()
    c = compress(data)
    assert decompress(c, len(data)) == data
    os.makedirs(d, exist_ok=True)
    open(f, 'wb').write(c)
    return c


GROUPS = 3
# resident functions that call into the groups, each after LoadStateCode() (with LTO the
# ones of the game's files may be inlined into main)
CALLERS = {'HandleInput', 'Draw', 'TickGame', 'main', 'ProcessInput', 'CalculatePowerConnectivity'}


def sections(elf, objdump):
    out = {}
    for l in subprocess.run([objdump, '-h', elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) >= 6 and p[1].startswith('.ov') and p[1][3:].isdigit():
            out[int(p[1][3:])] = (int(p[2], 16), int(p[3], 16), int(p[4], 16))   # size, vma, lma
    return out


cmd, elf, objdump = sys.argv[1:4]
ov = sections(elf, objdump)

if cmd == 'window':
    print((max(s for s, _, _ in ov.values()) + 63) // 64 * 64)

elif cmd == 'check':
    def group(sec): return int(sec[3:]) if sec and sec.startswith('.ov') else 0
    def base(n): return re.sub(r'\.(isra|constprop|lto_priv|part|cold)\.\d+$', '', n.split('(')[0])
    dis = subprocess.run([objdump, '-d', '-r', '-C', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout
    where, sec = {}, None
    for l in dis.splitlines():
        m = re.match(r'^Disassembly of section (\S+):', l)
        if m: sec = m.group(1); continue
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
        if m and not m.group(1).startswith('.L'): where[m.group(1)] = sec
    bad, refs, func, sec = [], 0, None, None
    for l in dis.splitlines():
        m = re.match(r'^Disassembly of section (\S+):', l)
        if m: sec = m.group(1); continue
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
        if m:
            if not m.group(1).startswith('.L'): func = m.group(1)
            continue
        m = re.search(r'\sR_RISCV_\w+\s+(.+?)(\+0x[0-9a-f]+)?$', l)
        if m and func:
            t = m.group(1).strip()
            if t in where and t != '__ovl_base':
                refs += 1
                g_from, g_to = group(sec), group(where[t])
                if g_to and g_from != g_to and (g_from or base(func) not in CALLERS):
                    bad.append(f'{func} ({sec}) -> {t} ({where[t]})')
    if bad:
        print('overlay check FAILED:'); [print('  ' + b) for b in sorted(set(bad))[:30]]; sys.exit(1)
    sizes = ', '.join(f'{g}: {ov[g][0]} B' for g in sorted(ov))
    print(f'overlay check ok ({refs} code references); window at 0x{min(v for _, v, _ in ov.values()):04X}, groups {sizes}')

elif cmd == 'image':
    eedata_path, eeaddr, out = sys.argv[4], int(sys.argv[5], 0), sys.argv[6]
    eedata = open(eedata_path, 'rb').read()
    objcopy = objdump.replace('objdump', 'objcopy')
    addr = eeaddr + (len(eedata) + 63) // 64 * 64
    hdr, blobs = b'MCOV', b''
    for g in range(1, GROUPS + 1):
        subprocess.run([objcopy, '-O', 'binary', '-j', f'.ov{g}', elf, f'{out}.ov{g}'], check=True)
        img = open(f'{out}.ov{g}', 'rb').read()
        hdr += struct.pack('<IH', addr + len(blobs), len(img))
        blobs += lz_cached(img)
    region = hdr + b'\xFF' * (32 - len(hdr)) + eedata + b'\xFF' * (addr - eeaddr - len(eedata)) + blobs
    ee = bytearray(b'\xFF' * 131072)
    assert eeaddr - 32 + len(region) <= len(ee)
    ee[eeaddr - 32:eeaddr - 32 + len(region)] = region
    open(out + '.eeprom', 'wb').write(ee)
    open(out + '.region', 'wb').write(region)
    print(f'EEPROM: region 0x{eeaddr - 32:05X}-0x{eeaddr - 33 + len(region):05X} ({len(region)} B): '
          f'EEDATA {len(eedata)} B, overlays {len(blobs)} B')
