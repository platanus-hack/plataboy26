"""Enters a battle in the simulator (as checkBattle does: gameState = battle, battlePending)
and presses fire once at a given time after it starts; reports whether the battle ended
in an escape (back to the overworld within 3 s). Repeats for a range of press times.
    python tools/battle_check.py libemu.dylib bin/arduventure.bin bin/arduventure.eeprom bin/arduventure.elf walk.txt"""
import ctypes, subprocess, sys
lib_path, flash, ee_path, elf, script = sys.argv[1:6]
lib = ctypes.CDLL(lib_path); lib.emu_sram.restype = ctypes.POINTER(ctypes.c_uint8)
lib.emu_state_size.restype = ctypes.c_uint32
syms = {}
for l in subprocess.run(['../../tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-nm', '-C', elf],
                        capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3: syms[p[2].split('(')[0].split('.')[0]] = int(p[0], 16)
def ram(n, k=1): return sum(lib.emu_sram()[syms[n] - 0x20000000 + i] << (8 * i) for i in range(k))
def poke(n, v): lib.emu_sram()[syms[n] - 0x20000000] = v
M = {'act': 16, 'up': 1, 'down': 2, 'left': 4, 'right': 8}
def boot_to_overworld():
    lib.emu_init(); lib.emu_set_board(13)
    ee = (ctypes.c_uint8 * 131072).in_dll(lib, 'eeprom_mem'); d = open(ee_path, 'rb').read(); ctypes.memmove(ee, d, len(d))
    img = open(flash, 'rb').read(); b = (ctypes.c_uint8 * len(img)).from_buffer_copy(img); lib.emu_load(b, len(img))
    t = 0
    while t < 40000:                          # fire every 250 ms until the overworld shows up
        if ram('gameState') == 6 and t > 3000:
            for _ in range(100): lib.emu_set_buttons(0); lib.emu_run_us(10000)   # settle 1 s
            return ram('gameState') == 6
        lib.emu_set_buttons(16 if t >= 3000 and t % 250 < 60 else 0)
        lib.emu_run_us(10000); t += 10
    return False
results = []
for press_ms in ([int(a) for a in sys.argv[6:]] or range(100, 2600, 100)):
    if not boot_to_overworld(): print('could not reach the overworld'); sys.exit(1)
    poke('battlePending', 1); poke('gameState', 17)
    trace, escaped = [], False
    for k in range(400):                       # 4 s
        t = k * 10
        lib.emu_set_buttons(16 if press_ms <= t < press_ms + 80 else 0)
        lib.emu_run_us(10000)
        st, bp = ram('gameState'), ram('battleProgress')
        if not trace or trace[-1][1:] != (st, bp, ram('cursorX'), ram('cursorY')):
            trace.append((t, st, bp, ram('cursorX'), ram('cursorY')))
        if t > press_ms and st == 6: escaped = True; break
    results.append((press_ms, escaped))
    print(f'fire at {press_ms:4} ms: {"ESCAPED" if escaped else "stays"}  '
          + ' '.join(f'{t}:{st}/{bp}/c{x}{y}' for t, st, bp, x, y in trace if t >= press_ms - 300))
