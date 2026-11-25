"""Host check of the joypad calibration (games/common/joycal.h) against the ladder.

joycal.h is compiled as is with the host C compiler and fed simulated PC4 readings:
ladder R2..R6 from build/console.net + build/bom.csv at the 1 % tolerance corners, ADC error
+-4 LSB, and a leak from PC4 to VCC (flux residue, a switch held lightly by the case) from
none to 15k. For each case the thresholds are computed for the idle level as measured
(+-4 LSB), then every state (idle, 4 directions, 4 diagonals, UP + DOWN for the menu exit)
must come out as its canonical JOY_xx value, the way the games decode it (+-JOY_DEV).
The old fixed windows are shown failing for the same leaks, and the packer's copy of the
threshold formula (nominal thresholds in the EEPROM image) must match joycal.h.

    python verify/check_joycal.py
"""
import itertools, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
TOL, ADC_ERR, DEV = 0.01, 4, 20

comps, nets = netlist.read(os.path.join(HERE, '..', 'build', 'console.net'))
def ohm(ref):
    v = comps[ref]['value']
    m = re.fullmatch(r'(\d+)k(\d*)', v)
    return float(f'{m.group(1)}.{m.group(2) or 0}') * 1e3
R = {k: ohm(r) for k, r in (('U', 'R2'), ('D', 'R3'), ('R', 'R4'), ('L', 'R5'), ('G', 'R6'))}

drv = open(os.path.join(ROOT, 'games', 'template', 'src', 'driver.h')).read()
J = {k: int(v) for k, v in re.findall(r'#define (JOY_\w+)\s+(\d+)', drv)}
STATES = {'': 0, 'R': J['JOY_E'], 'U': J['JOY_N'], 'UR': J['JOY_NE'], 'D': J['JOY_S'],
          'DR': J['JOY_SE'], 'UD': 440, 'L': J['JOY_W'], 'UL': J['JOY_NW'], 'DL': J['JOY_SW']}

def reading(keys, scale, leak):
    """ADC value of PC4 with the buttons in keys held, ladder scaled per resistor."""
    g_up = sum(1 / (R[k] * scale[k]) for k in keys) + (1 / leak if leak else 0)
    return g_up / (g_up + 1 / (R['G'] * scale['G'])) * 1023

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#define JOYCAL_COMPUTE
#define JOYCAL_VALUES
#include "joycal.h"
// argv[1] = idle level measured, then readings -> the JOY_xx value of each
int main(int argc, char **argv) {
  uint8_t t[JOYCAL_N];
  joycal_thresholds(t, atoi(argv[1]));
  if(argc == 2) { for(int k = 0; k < JOYCAL_N; k++) printf("%u\n", t[k]); return 0; }
  for(int i = 2; i < argc; i++) printf("%u\n", joycal_val(joycal_dec((uint16_t)atoi(argv[i]), t)));
  return 0;
}
'''
tmp = tempfile.mkdtemp()
src, exe = os.path.join(tmp, 'h.c'), os.path.join(tmp, 'h')
open(src, 'w').write(HARNESS)
subprocess.check_call(['cc', '-O1', '-Wall', '-I', os.path.join(ROOT, 'games', 'common'), src, '-o', exe])
clamp = lambda v: str(max(0, min(1023, round(v))))
def run(idle, seq):
    return [int(x) for x in subprocess.check_output([exe, clamp(idle)] + [clamp(v) for v in seq]).split()]

decodes = lambda out, want: (abs(out - want) < DEV) if want else out <= 10

errors = 0
def check(ok, msg):
    global errors
    print(('ok   ' if ok else 'FAIL ') + msg)
    if not ok: errors += 1

corners = [dict(zip('UDRLG', c)) for c in itertools.product((1 - TOL, 1 + TOL), repeat=5)]
def cases(leak):
    # every corner; the idle level measured with an error (the menu averages the boot
    # animation's readings: +-ADC_ERR), each state read with +-ADC_ERR
    bad = []
    for sc, e_idle in itertools.product(corners, (-ADC_ERR, 0, ADC_ERR)):
        idle = reading('', sc, leak)
        seq = [reading(keys, sc, leak) + e for keys in STATES for e in (-ADC_ERR, ADC_ERR)]
        out = run(max(0, idle + e_idle), seq)
        for n, keys in enumerate(STATES):
            for m in (0, 1):
                if not decodes(out[2 * n + m], STATES[keys]): bad.append(f'{keys or "idle"} -> {out[2 * n + m]}')
    return not bad, sorted(set(bad))[:4]

def old_windows(leak):
    sc = dict.fromkeys('UDRLG', 1)
    bad = []
    for keys, want in STATES.items():
        v = round(reading(keys, sc, leak))
        ok = (abs(v - want) < DEV) if want else v <= 10
        if not ok: bad.append(f'{keys or "idle"} {v}')
    return bad

for leak in (None, 400e3, 200e3, 100e3, 60e3, 40e3, 30e3, 25e3, 20e3, 15e3):
    name = 'no leak' if not leak else f'leak {leak / 1e3:.0f}k to VCC'
    idle = reading('', dict.fromkeys('UDRLG', 1), leak)
    ok, bad = cases(leak)
    old = old_windows(leak)
    check(ok, f'{name:18s} (idle reads {idle:4.0f}): every state decodes'
          + ('' if ok else ': ' + ', '.join(bad))
          + (f'   [fixed windows: {", ".join(old)} wrong]' if old else ''))

# the packer's copy of joycal_thresholds (firmware/tools/pack.py) gives the same bytes
sys.path.insert(0, os.path.join(ROOT, 'firmware', 'tools'))
import importlib.util
spec = importlib.util.spec_from_file_location('pack', os.path.join(ROOT, 'firmware', 'tools', 'pack.py'))
src_py = open(spec.origin).read()
ns = {}
exec(src_py[src_py.index('def joycal_thresholds'):src_py.index('\n\n\n', src_py.index('def joycal_thresholds'))], ns)
same = all(list(ns['joycal_thresholds'](i)) == [int(x) for x in subprocess.check_output([exe, str(i)]).split()]
           for i in range(0, 160))
check(same, 'pack.py joycal_thresholds() == joycal.h for every idle level 0..159')

print('all checks passed' if not errors else f'{errors} check(s) FAILED')
sys.exit(1 if errors else 0)
