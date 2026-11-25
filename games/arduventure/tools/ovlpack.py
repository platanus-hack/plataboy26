"""Overlay build steps (see port/platform.cpp, code overlays).

  window ELF OBJDUMP          print the window size: largest overlay, 64-byte pages
  check  ELF OBJDUMP          verify the final link: resident code never references an
                              overlay function except through the dispatch tables
                              (checkInputsGroup, mainGameLoop); groups never reference
                              each other (the linker's NOCROSSREFS checks that too)
  image  ELF OBJDUMP FAR EEADDR OUT
                              EEPROM image: header at EEADDR-32 ("AVOV", then per group
                              u32 address + u16 size), far data at EEADDR, then the
                              overlay images
"""
import re, struct, subprocess, sys

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
    nm = objdump.replace('objdump', 'nm')
    syms = []                                  # (addr, size, name, section group)
    for l in subprocess.run([nm, '-S', '-C', elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split(None, 3)
        if len(p) == 4 and p[2] in 'tTwW': syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    # which section each function lives in: from the disassembly section headers
    where, cur_sec, func = {}, None, None
    dis = subprocess.run([objdump, '-d', '-r', '-C', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout
    bad, refs = [], 0
    for l in dis.splitlines():
        m = re.match(r'^Disassembly of section (\S+):', l)
        if m: cur_sec = m.group(1); continue
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
        if m:
            if not m.group(1).startswith('.L'): func = m.group(1); where[func] = cur_sec
            continue
    allowed = {'checkInputsGroup', 'mainGameLoop', 'tileRefillGroup', 'EE_write'}
    def group(sec): return int(sec[3:]) if sec and sec.startswith('.ov') else 0
    def base(n): return re.sub(r'\.(isra|constprop|lto_priv|part|cold)\.\d+$', '', n.split('(')[0].split(' [')[0])
    func, cur_sec = None, None
    for l in dis.splitlines():
        m = re.match(r'^Disassembly of section (\S+):', l)
        if m: cur_sec = m.group(1); continue
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
        if m:
            if not m.group(1).startswith('.L'): func = m.group(1)
            continue
        m = re.search(r'\sR_RISCV_\w+\s+(.+?)(\+0x[0-9a-f]+)?$', l)
        if m and func:
            t = m.group(1).strip()
            if t in where and t != '__ovl_base':     # the window address, not code
                refs += 1
                g_from, g_to = group(cur_sec), group(where[t])
                if g_to and g_from != g_to and base(func) not in allowed:
                    bad.append(f'{func} ({cur_sec}) -> {t} ({where[t]})')
    if bad:
        print('overlay check FAILED:'); [print('  ' + b) for b in sorted(set(bad))[:30]]; sys.exit(1)
    sizes = ', '.join(f'{g}: {ov[g][0]} B' for g in sorted(ov))
    print(f'overlay check ok ({refs} code references); window at 0x{min(v for _, v, _ in ov.values()):04X}, overlays {sizes}')

elif cmd == 'image':
    far_path, eeaddr, out = sys.argv[4], int(sys.argv[5], 0), sys.argv[6]
    far = open(far_path, 'rb').read()
    raw = {}
    for g, (size, vma, lma) in ov.items():
        subprocess.run([objdump.replace('objdump', 'objcopy'), '-O', 'binary', '-j', f'.ov{g}', elf, f'{far_path}.ov{g}'], check=True)
        raw[g] = open(f'{far_path}.ov{g}', 'rb').read()
    ee = bytearray(b'\xFF' * 131072)
    addr = eeaddr + len(far)
    hdr = b'AVOV'
    blobs = b''
    for g in range(1, 5):
        img = raw[g]
        hdr += struct.pack('<IH', addr + len(blobs), len(img))
        blobs += img + b'\xFF' * (-len(img) % 64)
    ee[eeaddr - 32:eeaddr - 32 + len(hdr)] = hdr
    ee[eeaddr:eeaddr + len(far)] = far
    ee[addr:addr + len(blobs)] = blobs
    assert addr + len(blobs) <= len(ee)
    open(out, 'wb').write(ee)
    # the region alone, for the game library packer (firmware/tools/pack.py, "data")
    open(out.rsplit('.', 1)[0] + '.region', 'wb').write(bytes(ee[eeaddr - 32:addr + len(blobs)]))
    print(f'EEPROM: far data {len(far)} B at 0x{eeaddr:05X}, overlays {len(blobs)} B, '
          f'region 0x{eeaddr - 32:05X}-0x{addr + len(blobs) - 1:05X} ({addr + len(blobs) - eeaddr + 32} B)')
