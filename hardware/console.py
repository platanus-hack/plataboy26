"""CH32V003 Mini Game Console, board rev. 1.3: circuit description (SKiDL).

Rev. 1.1 is wagiminator's published design (CC BY-SA 3.0):
https://github.com/wagiminator/CH32V003-GameConsole. This file reproduces it net for net
(checked against the rev. 1.1 Gerbers by verify/check_netlist.py) and adds:

  U2  AT24CM01 1 Mbit I2C EEPROM on the existing OLED bus (SDA = PC1, SCL = PC2).
      A1 = A2 = GND -> I2C address 0x50/0x51 (0x51 = upper 64 KiB), WP = GND
      (writable, so games can also keep high scores).
  C3  100 nF decoupling for U2.
  R7, R8  10k pull-ups on SDA/SCL. Rev. 1.1 relied on the pull-ups of the OLED module;
      these keep the bus defined when the module is unplugged (EEPROM programming)
      and with modules that have weak or no pull-ups.

Rev. 1.3 adds a second action button on the same pin as ACT (no pin is free):
  B   tact switch from PA2 to GND through R9 = 100k; R10 = 10k pulls PA2 up.
      ACT alone: 0 V. B alone: 100k/110k = 0.91 VDD, a logic 1 for every game that
      only knows ACT, and ~930 on the ADC (PA2 = channel A0), which new games read.
      ACT + B reads as ACT.

    python console.py      -> build/console.net (KiCad netlist), ERC report
"""
import csv, os, sys
from skidl import Net, Part, Pin, SKIDL, ERC, POWER, generate_netlist, set_default_tool

set_default_tool(SKIDL)
HERE = os.path.dirname(os.path.abspath(__file__))
FP = 'lcsc'      # LCSC's own footprints for these exact parts (footprints/lcsc.pretty, fetched with
                 # easyeda2kicad): what JLCPCB uses to orient parts, so pin 1 / + always agree
T = Pin.types
PARTS = []


def part(ref, value, footprint, pins, **fields):
    """A part defined in place: pins = [(number, name, type), ...]."""
    p = Part(tool=SKIDL, name=value, ref_prefix=ref.rstrip('0123456789'), dest='NETLIST', tag=ref,
             footprint=footprint, pins=[Pin(num=n, name=nm, func=f) for n, nm, f in pins])
    p.ref, p.value = ref, value
    for k, v in fields.items(): setattr(p, k, v)
    PARTS.append(p)
    return p


def resistor(ref, value, lcsc):
    return part(ref, value, 'Resistor_SMD:R_0603_1608Metric',
                [(1, '~', T.PASSIVE), (2, '~', T.PASSIVE)], LCSC=lcsc)


def capacitor(ref, value, lcsc):
    return part(ref, value, 'Capacitor_SMD:C_0603_1608Metric',
                [(1, '~', T.PASSIVE), (2, '~', T.PASSIVE)], LCSC=lcsc)


def tact(ref, a, b, lcsc='C23873'):
    # 6x6 mm SMD tact switch: pins 1-2 and 3-4 are connected inside; pressing joins the pairs
    sw = part(ref, '6x6mm', f'{FP}:SW-SMD_4P-L6.0-W6.0-P4.50-LS9.0_C23873',
              [(1, 'B', T.PASSIVE), (2, 'B', T.PASSIVE), (3, 'A', T.PASSIVE), (4, 'A', T.PASSIVE)], LCSC=lcsc)
    sw[3] += a; sw[4] += a; sw[1] += b; sw[2] += b
    return sw


# ---- nets -----------------------------------------------------------------------------
vcc, gnd = Net('VCC'), Net('GND')
vcc.drive = gnd.drive = POWER
vbat = Net('VBAT')                  # battery + before the power switch
sda, scl = Net('SDA'), Net('SCL')
pad, act, beep, dio = Net('PAD'), Net('ACT'), Net('BEEP'), Net('DIO')

# ---- microcontroller ------------------------------------------------------------------
u1 = part('U1', 'CH32V003J4M6', f'{FP}:SOP-8_L4.9-W3.8-P1.27-LS6.0-BL', [
    (1, 'PD6/PA1', T.BIDIR), (2, 'VSS', T.PWRIN), (3, 'PA2', T.BIDIR), (4, 'VDD', T.PWRIN),
    (5, 'PC1', T.OPENCOLL), (6, 'PC2', T.OPENCOLL),        # I2C pins run open-drain
    (7, 'PC4', T.BIDIR), (8, 'PD1/PD4/PD5', T.BIDIR)],
    LCSC='C5346354')
u1[1] += beep; u1[2] += gnd; u1[3] += act; u1[4] += vcc
u1[5] += sda; u1[6] += scl; u1[7] += pad; u1[8] += dio

c1 = capacitor('C1', '100n', 'C14663'); c1[1] += gnd; c1[2] += vcc
c2 = capacitor('C2', '10u', 'C19702');  c2[1] += gnd; c2[2] += vcc

# ---- display (SSD1306 0.96" I2C module on a 4-pin header) -----------------------------
h1 = part('H1', 'OLED', 'console:OLED_Header_1x04_P2.54mm', [
    (1, 'GND', T.PASSIVE), (2, 'VCC', T.PASSIVE), (3, 'SCK', T.PASSIVE), (4, 'SDA', T.PASSIVE)])
h1[1] += gnd; h1[2] += vcc; h1[3] += scl; h1[4] += sda

# ---- EEPROM (new in rev. 1.2) ---------------------------------------------------------
u2 = part('U2', 'AT24CM01-SSHM-T', f'{FP}:SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL', [
    (1, 'NC', T.NOCONNECT), (2, 'A1', T.INPUT), (3, 'A2', T.INPUT), (4, 'GND', T.PWRIN),
    (5, 'SDA', T.OPENCOLL), (6, 'SCL', T.INPUT), (7, 'WP', T.INPUT), (8, 'VCC', T.PWRIN)],
    LCSC='C68393', alternative='ST M24M01-RMN6TP (same pinout); not 24LC1025 (different pinout)')
u2[2] += gnd; u2[3] += gnd; u2[4] += gnd; u2[7] += gnd; u2[8] += vcc
u2[5] += sda; u2[6] += scl
c3 = capacitor('C3', '100n', 'C14663'); c3[1] += gnd; c3[2] += vcc
r7 = resistor('R7', '10k', 'C25804'); r7[1] += vcc; r7[2] += sda
r8 = resistor('R8', '10k', 'C25804'); r8[1] += vcc; r8[2] += scl

# ---- joypad: resistor ladder into the ADC pin PC4 -------------------------------------
for ref, rref, val, lcsc in (('UP', 'R2', '8k2', 'C25981'), ('DOWN', 'R3', '3k9', 'C23018'),
                             ('RIGHT', 'R4', '20k', 'C4184'), ('LEFT', 'R5', '2k', 'C22975')):
    n = Net('SW_' + ref)
    r = resistor(rref, val, lcsc); tact(ref, n, pad)
    r[1] += vcc; r[2] += n
r6 = resistor('R6', '2k', 'C22975'); r6[1] += pad; r6[2] += gnd
tact('ACT', act, gnd)

# ---- second action button B on PA2 (new in rev. 1.3) ---------------------------------
sw_b = Net('SW_B')
r9 = resistor('R9', '100k', 'C25803'); r9[1] += act; r9[2] += sw_b
r10 = resistor('R10', '10k', 'C25804'); r10[1] += vcc; r10[2] += act
tact('B', sw_b, gnd)

# ---- buzzer (active low through 22R) --------------------------------------------------
bz = part('BUZZER1', 'MLT-5030', f'{FP}:BUZ-SMD_3P-L5.2-W5.2-P3.50-BR', [   # LCSC pinout
    (1, 'NC', T.NOCONNECT), (2, '+', T.PASSIVE), (3, '-', T.PASSIVE)], LCSC='C95297')
r1 = resistor('R1', '22R', 'C23345')
buz = Net('BUZ')
bz[2] += vcc; bz[3] += buz; r1[2] += buz; r1[1] += beep

# ---- power: CR2032 / LIR2032 and slide switch -----------------------------------------
b1 = part('B1', 'CR2032', f'{FP}:BAT-TH_CR2032-BS-6-1', [     # pin 1 = + (LCSC footprint silk)
    (1, '+', T.PASSIVE), (2, '-', T.PASSIVE)], LCSC='C70377')
sw1 = part('SW1', 'POWER', f'{FP}:SW-SMD_SS-1290L-GLL-1.5MM', [
    (1, 'A', T.NOCONNECT), (2, 'COM', T.PASSIVE), (3, 'B', T.PASSIVE),
    *[(n, 'MNT', T.NOCONNECT) for n in (4, 5, 6, 7)]], LCSC='C695418')
b1[1] += vbat; b1[2] += gnd; sw1[3] += vbat; sw1[2] += vcc

# ---- programming header (WCH-LinkE: SWIO = PD1) ---------------------------------------
h2 = part('H2', 'SWIO', f'{FP}:HDR-TH_3P-P2.54-V-M-1', [
    (1, 'VCC', T.PASSIVE), (2, 'GND', T.PASSIVE), (3, 'DIO', T.PASSIVE)], LCSC='C49257')
h2[1] += vcc; h2[2] += gnd; h2[3] += dio

# ---- mounting holes -------------------------------------------------------------------
for i in (1, 2, 3):                 # M2 holes: top corners + bottom-right (bottom-left holds logos)
    part(f'MH{i}', 'MountingHole', 'MountingHole:MountingHole_2.2mm_M2', [])

NOT_ASSEMBLED = {   # ref prefix -> note in the BOM
    'H1': 'SSD1306 0.96in 128x64 I2C OLED module, pinout GND VCC SCL SDA (soldered by hand)',
    'B1': None, 'MH': None,
}


def write_bom(path):
    """BOM grouped by value + footprint, columns as JLCPCB's assembly service expects."""
    rows = {}
    for p in PARTS:
        if p.ref.startswith('MH'): continue
        key = (p.value, p.footprint, getattr(p, 'LCSC', ''))
        rows.setdefault(key, []).append(p.ref)
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Comment', 'Designator', 'Footprint', 'LCSC Part #', 'Notes'])
        for (value, fp, lcsc), refs in sorted(rows.items(), key=lambda r: r[1][0]):
            notes = NOT_ASSEMBLED.get(refs[0], '') or ''
            alt = [getattr(p, 'alternative', '') for p in PARTS if p.ref == refs[0]][0]
            if alt: notes = (notes + '; ' if notes else '') + 'alternative: ' + alt
            w.writerow([value, ','.join(sorted(refs)), fp.split(':')[1], lcsc, notes])


if __name__ == '__main__':
    os.makedirs(os.path.join(HERE, 'build'), exist_ok=True)
    ERC()
    generate_netlist(tool='kicad9', file_=os.path.join(HERE, 'build', 'console.net'))
    write_bom(os.path.join(HERE, 'build', 'bom.csv'))
