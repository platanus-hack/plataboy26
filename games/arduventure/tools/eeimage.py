"""EEPROM image (128 KiB, erased = 0xFF) with a file placed at an address.
    python tools/eeimage.py data.bin 0x18000 out.eeprom [base.eeprom]"""
import sys
data, at, out = open(sys.argv[1], 'rb').read(), int(sys.argv[2], 0), sys.argv[3]
ee = bytearray(open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b'\xFF' * 131072)
assert at + len(data) <= len(ee), 'does not fit'
ee[at:at + len(data)] = data
open(out, 'wb').write(ee)
