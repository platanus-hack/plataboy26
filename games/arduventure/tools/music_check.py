"""Counts music queue underruns (port/platform.cpp, aqUnderruns: each one is a millisecond
the tick queue was empty and the last tick was held) while the game plays a scripted
input file in the simulator.  python tools/music_check.py libemu.dylib bin/arduventure.* walk.txt"""
import ctypes, subprocess, sys
lib_path, flash, ee_path, elf, script = sys.argv[1:6]
lib = ctypes.CDLL(lib_path); lib.emu_sram.restype = ctypes.POINTER(ctypes.c_uint8)
syms = {}
for l in subprocess.run(['../../tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-nm', '-C', elf],
                        capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3: syms[p[2].split('(')[0].split('.')[0]] = int(p[0], 16)
ram = lambda n, k=1: sum(lib.emu_sram()[syms[n] - 0x20000000 + i] << (8 * i) for i in range(k))
lib.emu_init(); lib.emu_set_board(13)
ee = (ctypes.c_uint8 * 131072).in_dll(lib, 'eeprom_mem'); d = open(ee_path, 'rb').read(); ctypes.memmove(ee, d, len(d))
img = open(flash, 'rb').read(); b = (ctypes.c_uint8 * len(img)).from_buffer_copy(img); lib.emu_load(b, len(img))
presses = [l.split() for l in open(script)]
M = {'act': 16, 'up': 1, 'down': 2, 'left': 4, 'right': 8, 'b': 32}
last = 0
for t in range(0, 40000, 10):
    mask = 0
    for nm, a, z in presses:
        if int(a) <= t < int(z): mask |= M[nm]
    lib.emu_set_buttons(mask); lib.emu_run_us(10000)
    if t % 5000 == 4990:
        u = ram('aqUnderruns', 2)
        print(f'{(t + 10) / 1000:4.0f} s: underrun ms {u - last:4} (state {ram("gameState")}, loads {ram("ov_loads", 2)})')
        last = u
