"""End-to-end tests of the game library firmware in the simulator (sim/chsim).

    python tools/test.py [--shots DIR]

1. every library entry: boot the factory image (build/flash.bin + build/eeprom.bin),
   pick the entry in the menu, release A, let the kernel install it; then a software
   reset (as the install's own) and compare the flash with the original program and the
   screen with a direct run of the original .bin (same time after the start)
2. a power-on with a game installed brings the menu back, with the cursor on the last
   game; the boot animation plays at power-on, not after a software reset
3. power loss in the middle of an install: the next power-on reinstalls the menu
4. a corrupted program in the EEPROM: the check fails, the menu comes back
5. blank EEPROM: the kernel leaves the flash alone
6. production: a blank EEPROM programmed with the writer chunks (build/writer/) holds
   the library afterwards, and build/flash.bin boots into the menu with it
"""
import json, os, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lz import decompress
from pack import CARDS
FW = os.path.dirname(HERE)
SIM = os.path.join(FW, '..', 'sim', 'chsim')
B = os.path.join(FW, os.environ.get('PB26_BUILD', 'build'))      # PB26_BUILD=build-full PB26_LIB=library.full.json
T = tempfile.mkdtemp(prefix='pb26test_')
fails = []


def run(flash, ee, ms, presses=(), shots=(), power=(), eeout=None, soft=()):
    cmd = [SIM, flash, '-e', ee, '-t', str(ms), '-q', '-F', os.path.join(T, 'flash_out.bin')]
    for p in presses: cmd += ['-p', p]
    for t, f in shots: cmd += ['-s', f'{t}:{f}']
    for t in power: cmd += ['-P', str(t)]
    for t in soft: cmd += ['-S', str(t)]
    if eeout: cmd += ['-E', eeout]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: fails.append(f'simulator exit {r.returncode}: {r.stderr.strip()}')
    return open(os.path.join(T, 'flash_out.bin'), 'rb').read()


def pgm(path): return open(path, 'rb').read()[-128 * 64:]


def cards(eed):
    """the menu cards stored in an EEPROM image, as lit-pixel tuples (listed entry order)"""
    out = []
    for i in range(1, eed[5]):
        at, n = struct.unpack_from('<IH', eed, CARDS + 6 * i)
        if n in (0, 0xFFFF): out.append(None); continue
        col = decompress(eed[at:at + n], 1024)
        out.append(lit([255 * ((col[x * 8 + y // 8] >> (y % 8)) & 1) for y in range(64) for x in range(128)]))
    return out


def lit(a):
    """lit pixels, without the menu's bottom row overlays (rows 56..: the version label at the
    start, bottom left, and the position badge, bottom right)"""
    return tuple(int(v > 64) if i < 56 * 128 else 0 for i, v in enumerate(a))


def is_menu(a, eed, i=None):
    """the screen (128 x 64 pixels, one byte each) shows a menu card: entry i's, or any"""
    c = cards(eed)
    return lit(a) == c[i] if i is not None else lit(a) in c


def check(cond, msg):
    print(('ok    ' if cond else 'FAIL  ') + msg)
    if not cond: fails.append(msg)


MENU_AT = 3500     # the menu's card is up (after the boot animation and its fade, ~2.6 s)


def nav(n, t0=MENU_AT):
    """presses that move n cards right from the first, then A; -> (presses, release time)"""
    p, t = [], t0
    for _ in range(n): p.append(f'right@{t}-{t + 90}'); t += 600     # a slide takes ~350 ms
    p.append(f'act@{t}-{t + 120}')
    return p, t + 120


def main():
    flash, ee = os.path.join(B, 'flash.bin'), os.path.join(B, 'eeprom.bin')
    lib = json.load(open(os.path.join(FW, os.environ.get('PB26_LIB', 'library.json'))))['programs']
    eed = open(ee, 'rb').read()
    shots = sys.argv[sys.argv.index('--shots') + 1] if '--shots' in sys.argv else None
    if shots: os.makedirs(shots, exist_ok=True)

    # 1. install every entry
    for i, p in enumerate(lib):
        orig = open(os.path.join(FW, p['file']), 'rb').read()
        presses, rel = nav(i)
        pw = rel + 4000                                     # installs take < 2 s
        sa, sb = os.path.join(T, 'a.pgm'), os.path.join(T, 'b.pgm')
        fl = run(flash, ee, pw + 3000, presses, [(pw + 2500, sa)], (), os.path.join(T, 'ee.bin'), [pw])
        ok_flash = fl[4:len(orig)] == orig[4:]
        cfg = struct.unpack_from('<IIII', fl, 0x3FC0)
        ok_cfg = cfg[0] == 0x36324250 and cfg[2] == i + 1
        # reference: the original program from power-on (the kernel adds a few cycles)
        # (with the contrast the kernel leaves at config + 16, as the menu's default: 127)
        d = os.path.join(T, 'orig.bin')
        # (and the kernel in place: Nitro Nonsense unpacks its code overlays with it)
        kern = open(os.path.join(B, '..', 'build', 'kernel.bin'), 'rb').read()
        open(d, 'wb').write((orig.ljust(0x3C40, b'\xFF') + kern).ljust(0x3FD0, b'\xFF') + bytes([127]))
        subprocess.run([SIM, d, '-e', ee, '-t', '2600', '-q', '-s', f'2500:{sb}'], capture_output=True)
        same = pgm(sa) == pgm(sb)
        diff = sum(x != y for x, y in zip(pgm(sa), pgm(sb)))
        check(ok_flash and ok_cfg and diff < (256 if 'tool' in p.get('flags', []) else 64),  # animations / live ADC values
              f'install {i + 1:2} {p["name"]:14} flash {"=" if ok_flash else "!="} original, '
              f'config {"ok" if ok_cfg else "BAD"}, screen after a reset '
              + ('identical' if same else f'{diff} px different') + ' to a direct run')
        if shots:
            os.replace(sa, os.path.join(shots, f'{i + 1:02}_installed.pgm'))
            os.replace(sb, os.path.join(shots, f'{i + 1:02}_direct.pgm'))

    # 2. power-on with a game installed -> menu, cursor on the last game (Tiny Pacman =
    # entry 3); the menu takes ~1 s to reinstall, then the animation
    presses, rel = nav(2)
    s = os.path.join(T, 'm.pgm'); s0 = os.path.join(T, 'm0.pgm'); sa = os.path.join(T, 'ma.pgm')
    pw = rel + 4000
    fl = run(flash, ee, pw + 5000, presses, [(pw + 1800, sa), (pw + 4500, s)], [pw],
             os.path.join(T, 'ee2.bin'))
    run(flash, os.path.join(T, 'ee2.bin'), MENU_AT + 100, (), [(MENU_AT, s0)])
    check(struct.unpack_from('<I', fl, 0x3FC8)[0] == 0 and pgm(s) == pgm(s0) and is_menu(pgm(s), eed, 2),
          'power-on with a game installed reinstalls the menu, cursor on the last game')
    check(not is_menu(pgm(sa), eed) and sum(v > 64 for v in pgm(sa)) > 500,
          '  ... after the boot animation')
    if shots:
        os.replace(s, os.path.join(shots, 'menu_after_game.pgm'))
        os.replace(sa, os.path.join(shots, 'boot_animation.pgm'))
    # a software reset in the menu (as after the exit from a game): the card at once
    run(flash, ee, 4600, (), [(4300, s)], (), None, [4000])
    check(is_menu(pgm(s), eed), 'software reset (exit from a game): the menu without the animation')

    # 3. power loss in the middle of an install (Tiny Arkanoid, ~1.7 s)
    presses, rel = nav(1)
    fl = run(flash, ee, rel + 6000, presses, [(rel + 5500, s)], [rel + 700])
    cfg = struct.unpack_from('<IIII', fl, 0x3FC0)
    check(cfg[0] == 0x36324250 and cfg[2] == 0 and is_menu(pgm(s), eed),
          'power loss during an install: the menu is back at the next power-on')

    # 4. corrupted stream: flip one byte in the middle of a program's data (the 7th entry,
    # or the next one with a card: is_menu recognises the menu by the card on screen)
    k = next(k for k in list(range(6, len(lib))) + list(range(6)) if cards(eed)[k])
    ent = struct.unpack_from('<IHHII', eed, 0x20 + 32 * (k + 1))
    bad = bytearray(eed); bad[ent[0] + ent[1] // 2] ^= 0x10
    badp = os.path.join(T, 'bad.bin'); open(badp, 'wb').write(bad)
    presses, rel = nav(k)
    fl = run(flash, badp, rel + 5000, presses, [(rel + 4500, s)])
    cfg = struct.unpack_from('<IIII', fl, 0x3FC0)
    check(cfg[0] == 0x36324250 and cfg[2] == 0 and is_menu(pgm(s), eed),
          'corrupted program: the check fails, the menu is reinstalled')

    # 5. blank EEPROM: no flash write at all
    blank = os.path.join(T, 'blank.bin'); open(blank, 'wb').write(b'\xFF' * 131072)
    fl = run(flash, blank, 1000, ['act@0-800'])
    check(fl == open(flash, 'rb').read(), 'blank EEPROM: the kernel leaves the flash untouched')

    # 6. production programming with the writer chunks
    cur = blank
    wd = os.path.join(B, 'writer')
    ws = sorted(os.listdir(wd))
    ok = True
    for k, w in enumerate(ws):
        nxt = os.path.join(T, f'ee_w{k}.bin')
        r = subprocess.run([SIM, os.path.join(wd, w), '-e', cur, '-E', nxt, '-t', '3000', '-q',
                            '-s', f'2900:{os.path.join(T, "w.pgm")}'], capture_output=True)
        ok &= r.returncode == 0
        cur = nxt
    if shots: os.replace(os.path.join(T, 'w.pgm'), os.path.join(shots, 'writer_last.pgm'))
    got = open(cur, 'rb').read()
    check(ok and got == eed, f'production: {len(ws)} writer chunks on a blank EEPROM give build/eeprom.bin exactly')
    run(flash, cur, MENU_AT + 100, (), [(MENU_AT, s)])
    check(pgm(s) == pgm(s0) or is_menu(pgm(s), eed), '  ... and build/flash.bin boots into the menu')

    print(f'\n{len(fails)} failure(s)' if fails else '\nall tests passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
