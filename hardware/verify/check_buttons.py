"""Worst-case check of the button circuits against the CH32V003 datasheet (V1.1).

Reads the topology and values from build/console.net + build/bom.csv, the decode
windows from games/template/src/driver.h, and checks over resistor tolerance (1 %),
internal pull-up spread (35-55 kOhm, or off), supply 2.8-3.6 V (CR2032 near empty to
LIR2032 full) and ADC total error (4 LSB):
  - PA2: ACT alone is a logic 0; idle and B alone are a logic 1 for every game that
    reads PA2 as a digital input (rev. 1.1 firmware cannot see B)
  - PA2 ADC: idle, B, ACT are told apart by JOY_b_pressed() (B relative to the learned idle)
  - joypad ladder (PC4): every direction and diagonal lands inside its JOY_xx window
  - ADC source impedance below the 10 kOhm limit of the accuracy table

    python verify/check_buttons.py
"""
import csv, itertools, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist

HERE = os.path.dirname(os.path.abspath(__file__))
TOL, RPU, VDD, ADC_ERR = 0.01, (35e3, 55e3, None), (2.8, 3.0, 3.3, 3.6), 4
VIH = lambda vdd: 0.41 * (vdd - 1.8) + 1.3          # datasheet table 3-16, standard I/O
VIL = lambda vdd: 0.28 * (vdd - 1.8) + 0.6

comps, nets = netlist.read(os.path.join(HERE, '..', 'build', 'console.net'))
pin_net = {p: n for n, ps in nets.items() for p in ps}
value = {c: comps[c]['value'] for c in comps}
def ohm(ref):
    v = value[ref]
    m = re.fullmatch(r'(\d+)k(\d*)', v) or re.fullmatch(r'(\d+)R(\d*)', v)
    whole, frac = m.group(1), m.group(2) or '0'
    return float(f'{whole}.{frac}') * (1e3 if 'k' in v else 1)

drv = open(os.path.join(HERE, '..', '..', 'games', 'template', 'src', 'driver.h')).read()
const = {k: int(v) for k, v in re.findall(r'#define (JOY_\w+)\s+(\d+)', drv)}
errors = 0
def check(ok, msg):
    global errors
    print(('ok   ' if ok else 'FAIL ') + msg)
    if not ok: errors += 1

def resistor_between(a, b):
    for ref in comps:
        if ref.startswith('R') and {pin_net.get(f'{ref}.1'), pin_net.get(f'{ref}.2')} == {a, b}: return ref

# ---- topology ----------------------------------------------------------------------------
check(pin_net['U1.3'] == 'ACT', 'U1 pin 3 (PA2) is the ACT net')
check({pin_net['ACT.1'], pin_net['ACT.3']} == {'GND', 'ACT'}, 'A switch: PA2 to GND')
rb, rpu = resistor_between('ACT', 'SW_B'), resistor_between('VCC', 'ACT')
check(rb is not None and rpu is not None, f'B: PA2 -{rb}- SW_B, pull-up {rpu} to VCC')
check({pin_net['B.1'], pin_net['B.3']} == {'GND', 'SW_B'}, 'B switch: SW_B to GND')
RB, RP = ohm(rb), ohm(rpu)

# ---- PA2 -----------------------------------------------------------------------------------
def pa2(b_pressed, rp, rb, rint):
    g_up = 1 / rp + (1 / rint if rint else 0)
    g_dn = 1 / rb if b_pressed else 0
    return g_up / (g_up + g_dn)
corners = list(itertools.product((1 - TOL, 1 + TOL), (1 - TOL, 1 + TOL), RPU))
ratios_b = [pa2(True, RP * a, RB * b, r) for a, b, r in corners]
lo_b, hi_b = min(ratios_b), max(ratios_b)
for vdd in VDD:
    check(lo_b * vdd > VIH(vdd), f'VDD {vdd} V: B alone {lo_b * vdd:.2f} V > VIH {VIH(vdd):.2f} V (old games read "not pressed")')
    check(0 < VIL(vdd), f'VDD {vdd} V: A alone 0.00 V < VIL {VIL(vdd):.2f} V')
adc = lambda r: r * 1023
b_lo, b_hi = adc(lo_b) - ADC_ERR, adc(hi_b) + ADC_ERR
# B is decided against the learned idle level (driver.h JOY_b_pressed): B below
# idle - idle / 2^JOY_B_SHIFT. Idle is learned up to 3 LSB low (integer tracking) and reads
# ADC_ERR low; B alone reads up to hi_b * idle + ADC_ERR. Checked for idle 1023 and for the
# the provider's sample board, whose idle read 972 (5 % low before the sample-time fix).
for idle_true in (1023, 972):
    idle = idle_true - ADC_ERR - 3
    thr = idle - (idle >> const['JOY_B_SHIFT'])
    b_max = hi_b * idle_true + ADC_ERR
    check(b_max < thr, f'idle {idle_true}: B alone <= {b_max:.0f} below the B threshold {thr}')
    check(idle_true - ADC_ERR >= thr, f'idle {idle_true}: idle >= {idle_true - ADC_ERR} not below the B threshold {thr}')
    check(lo_b * idle_true - ADC_ERR > const['JOY_B_MIN'], f'idle {idle_true}: B alone >= {lo_b * idle_true - ADC_ERR:.0f} above JOY_B_MIN')
check(0 + ADC_ERR < const['JOY_B_MIN'], f'ADC A (and A+B) <= {ADC_ERR} below JOY_B_MIN')
zsrc = 1 / (1 / (RP * (1 - TOL)) + 1 / (RB * (1 - TOL)))
check(zsrc < 10e3, f'PA2 source impedance {zsrc / 1e3:.1f} kOhm < 10 kOhm (ADC accuracy condition)')

# ---- joypad ladder --------------------------------------------------------------------------
pad_r = {}
for d in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
    pad_r[d] = ohm(resistor_between('VCC', 'SW_' + d))
pd = ohm(resistor_between('PAD', 'GND'))
combos = {'JOY_N': ['UP'], 'JOY_S': ['DOWN'], 'JOY_E': ['RIGHT'], 'JOY_W': ['LEFT'],
          'JOY_NE': ['UP', 'RIGHT'], 'JOY_SE': ['DOWN', 'RIGHT'], 'JOY_SW': ['DOWN', 'LEFT'], 'JOY_NW': ['UP', 'LEFT']}
for name, keys in combos.items():
    vals = []
    for tols in itertools.product((1 - TOL, 1 + TOL), repeat=len(keys) + 1):
        g = sum(1 / (pad_r[k] * t) for k, t in zip(keys, tols[1:]))
        r = pd * tols[0] * g
        vals.append(adc(r / (1 + r)))
    lo, hi = min(vals) - ADC_ERR, max(vals) + ADC_ERR
    c, dev = const[name], const['JOY_DEV']
    check(c - dev < lo and hi < c + dev, f'{name:6} {"+".join(keys):11} ADC {lo:.0f}..{hi:.0f} inside {c - dev}..{c + dev}')
zpad = min(pad_r.values()) * pd / (min(pad_r.values()) + pd)
check(zpad < 10e3, f'PC4 source impedance {zpad / 1e3:.1f} kOhm < 10 kOhm')

print('RESULT:', 'buttons OK in the worst case' if not errors else f'{errors} problems')
sys.exit(1 if errors else 0)
