"""Soak test of the Arduventure port: plays it in the simulator for a while and reports
how the overlays behave (loads, flash pages rewritten, projected flash life).

    python tools/bot.py libemu.dylib bin/arduventure.bin bin/arduventure.eeprom \\
                        bin/arduventure.elf MINUTES [SEED]

The emulator core (sim/core/emu.c) is driven through ctypes; the bot reads the game's
`gameState` from simulated RAM to choose its buttons: confirm through the menus into a
new game, wander the overworld in random directions, attack in battles, leave menus.
"""
import ctypes, random, subprocess, sys, collections, time

lib_path, flash_path, ee_path, elf, minutes = sys.argv[1:6]
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
rng = random.Random(seed)
lib = ctypes.CDLL(lib_path)
lib.emu_sram.restype = ctypes.POINTER(ctypes.c_uint8)
lib.emu_flash_ops.restype = ctypes.c_uint32
lib.emu_run_us.restype = ctypes.c_uint32
lib.emu_pc.restype = ctypes.c_uint32
lib.emu_peek32.restype = ctypes.c_uint32

# buttons of sim/core/emu.h
UP, DOWN, LEFT, RIGHT, ACT, B = 1, 2, 4, 8, 16, 32
FIRE = ACT            # the console's fire button = Arduventure's confirm (Arduboy B)
BACK = B              # the console's B button   = Arduventure's back (Arduboy A)

nm = elf.replace('arduventure.elf', '') and None
syms = {}
for l in subprocess.run([sys.argv[7] if len(sys.argv) > 7 else 'riscv-none-elf-nm', '-C', elf],
                        capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3: syms[p[2].split('(')[0].split('.')[0]] = int(p[0], 16)

def ram(name, size=1):
    a = syms[name] - 0x20000000
    s = lib.emu_sram()
    return sum(s[a + i] << (8 * i) for i in range(size))

lib.emu_init()
img = open(flash_path, 'rb').read()
lib.emu_set_board(13)
ee = (ctypes.c_uint8 * 131072).in_dll(lib, 'eeprom_mem')
data = open(ee_path, 'rb').read()
ctypes.memmove(ee, data, len(data))
buf = (ctypes.c_uint8 * len(img)).from_buffer_copy(img)
lib.emu_load(buf, len(img))

NAMES = {0: 'intro', 1: 'main menu', 3: 'new', 6: 'overworld', 7: 'inventory', 10: 'map',
         11: 'game over', 16: 'door', 17: 'battle', 18: 'boss', 19: 'story intro',
         20: 'name entry', 24: 'objects', 25: 'shop', 26: 'inn', 27: 'rewards'}
time_in = collections.Counter()
battles = 0
last_state = None
walk_dir, walk_until = RIGHT, 0
total_ms = int(float(minutes) * 60000)
ops0 = 0
t = 0
frame0 = None
t_host = time.time()
while t < total_ms:
    st = ram('gameState')
    if st != last_state:
        if st == 17 and last_state != 17: battles += 1
        last_state = st
    time_in[NAMES.get(st, str(st))] += 10
    mask = 0
    phase = t % 400 < 60                    # a short press every 400 ms
    if st == 6:                             # overworld: walk around
        if t >= walk_until:
            walk_dir = rng.choice([UP, DOWN, LEFT, RIGHT])
            walk_until = t + rng.randint(500, 3000)
        mask = walk_dir
    elif st in (7, 8, 9, 10, 12, 13, 14, 15, 21, 22, 23, 25, 26):
        mask = BACK if phase else 0         # menus: leave
    elif st == 16:
        mask = 0
    else:                                   # title, name entry, story, battle, rewards...
        mask = FIRE if phase else 0
    if t % 10000 == 0 and t:
        print(f'  {t / 1000:.0f} s (host {time.time() - t_host:.0f} s): state {NAMES.get(st, st)}, '
              f'battles {battles}, overlay loads {ram("ov_loads", 2)}, '
              f'pages rewritten {ram("ov_pages", 2)}, frames {ram("arduboy", 4) >> 16}', flush=True)
    if t >= 80000 and t % 1000 == 0 and '--trace' in sys.argv:
        print(f'    t {t / 1000:.0f} s pc 0x{lib.emu_pc():08x} state {st} host {time.time() - t_host:.1f}', flush=True)
    lib.emu_set_buttons(mask)
    if '--probe' in sys.argv and t >= 89000:
        lib.emu_cycles.restype = ctypes.c_uint64
        p32 = lambda a: lib.emu_peek32(a)
        k = 0
        while lib.emu_cycles() < (1 << 32) + 2000000:
            c0, h0 = lib.emu_cycles(), time.time()
            lib.emu_run_cycles(ctypes.c_uint64(12000))
            k += 1
            if lib.emu_cycles() > (1 << 32) - 60000 or time.time() - h0 > 0.5:
                print(f'    probe k {k} pc 0x{lib.emu_pc():08x} cycles {lib.emu_cycles()} (+{lib.emu_cycles() - c0}) '
                      f'host {time.time() - h0:.2f}s star1 {p32(0x40005414):04x} ev_at {p32(0x40005420)} next {p32(0x40005424)}', flush=True)
                if k > 20000: break
        break
    if lib.emu_run_us(10000):
        print(f'stopped at {t / 1000:.1f} s (CPU fault)'); break
    t += 10

ov_loads, ov_pages = ram('ov_loads', 2), ram('ov_pages', 2)
ops = lib.emu_flash_ops()
hours = total_ms / 3.6e6
print(f'played {total_ms / 60000:.1f} min: {battles} battles')
print('time per state: ' + ', '.join(f'{k} {v / 1000:.0f}s' for k, v in time_in.most_common()))
print(f'overlay loads {ov_loads}, flash pages rewritten {ov_pages} ({ops} erase+program ops)')
if ov_pages:
    window_pages = 70
    per_hour = ov_pages / hours
    # every rewrite of a window page costs that page one of its >= 10000 cycles; the
    # rewrites spread over the window's pages
    print(f'window page rewrites per hour of play: {per_hour:.0f} '
          f'(each page: ~{per_hour / window_pages:.0f}/h) -> rated 10000 cycles reached after '
          f'~{10000 / (per_hour / window_pages):.0f} h of play')
