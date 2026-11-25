"""Pre-renders the game's ATMlib songs into one-voice note streams (post-link).

The console has one buzzer, so the runtime only ever plays one tone: the loudest of
tone channels 0-2 (port/platform.cpp did exactly that with the full 4-channel play
routine). This runs that play routine here instead, tick by tick, and records the
resulting tone as (frequency, milliseconds) notes. The loop of a song is found by
hashing the complete player state after every tick: the first state seen twice is
the loop point, so looping songs repeat exactly as the tracker would play them.

Appends to the far data image, after the text index (see port/platform.cpp, Song*):
  song table  u16 count, then per song: u32 far address of the original song data,
              u16 offset of its stream
  stream      u16 notes before the loop, u16 notes in the loop (0 = play once),
              then notes: u16 frequency (Hz, 0 = silence), u16 duration (ms)

    python tools/songs.py game.elf far.bin nm-tool
"""
import struct, subprocess, sys

FAR_BASE = 0x30000000
NOTE = [0,
  262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
  523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
  1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
  2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951,
  4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902,
  8372, 8870, 9397]
DEPTH = 3                                   # track call depth, as ATM_DEPTH in platform.cpp
S8 = lambda v: ((v & 0xFF) ^ 0x80) - 0x80
U8 = lambda v: v & 0xFF
U16 = lambda v: v & 0xFFFF


class Ch:
    FIELDS = ('ptr note sp sc st si repeat delay counter track freq vol vfs vfc vfn arpn arpt '
              'arpc rec rcn trans tdepth tconf tcount gconf gcount').split()

    def __init__(self):
        for f in self.FIELDS: setattr(self, f, 0)
        self.sp, self.sc, self.st = [0] * DEPTH, [0] * DEPTH, [0] * DEPTH

    def state(self):
        return tuple(tuple(v) if isinstance(v, list) else v for v in (getattr(self, f) for f in self.FIELDS))


class Player:
    """ATMlib's play routine (as in port/platform.cpp), on a byte array"""

    def __init__(self, mem, song):
        self.mem = mem
        self.ch = [Ch() for _ in range(4)]
        self.mute, self.tick_rate, self.playing = 0xF0, 25, True
        self.on_cmd = None
        self.osc_vol, self.osc_freq = [0] * 4, [0] * 4
        n = self.rd(song); song += 1
        self.track_list = song
        self.track_base = song + 2 * n + 4
        song += 2 * n
        for c in self.ch: c.ptr = self.track_ptr(self.rd(song)); song += 1

    def rd(self, a): return self.mem[a]
    def track_ptr(self, t): return self.track_base + (self.rd(self.track_list + 2 * t) | self.rd(self.track_list + 2 * t + 1) << 8)

    def vle(self, c):
        q = 0
        while True:
            d = self.rd(c.ptr); c.ptr += 1
            q = ((q << 7) | (d & 0x7F)) & 0xFFFF
            if not d & 0x80: return q

    def state(self):
        return (self.mute, self.tick_rate, tuple(self.osc_vol), tuple(self.osc_freq), tuple(c.state() for c in self.ch))

    def tick(self):
        for n, c in enumerate(self.ch):
            if c.rec:
                if c.rcn >= (c.rec & 3): self.osc_freq[n] = NOTE[c.rec >> 2]; c.rcn = 0
                else: c.rcn = U8(c.rcn + 1)
            if c.gconf:
                if c.gcount >= (c.gconf & 0x7F):
                    c.note = U8(c.note - 1 if c.gconf & 0x80 else c.note + 1)
                    c.note = 1 if c.note < 1 else 63 if c.note > 63 else c.note
                    c.freq = NOTE[c.note]; c.gcount = 0
                else: c.gcount = U8(c.gcount + 1)
            if c.vfs:
                if not c.vfn:
                    vf = (c.freq if c.vfc & 0x40 else c.vol) + S8(c.vfs)
                    vf = ((vf + 0x8000) & 0xFFFF) - 0x8000
                    if not c.vfc & 0x80:
                        if vf < 0: vf = 0
                        elif c.vfc & 0x40:
                            if vf > 9397: vf = 9397
                        elif vf > 63: vf = 63
                    if c.vfc & 0x40: c.freq = U16(vf)
                    else: c.vol = U8(vf)
                old = c.vfn; c.vfn = U8(c.vfn + 1)
                if old >= (c.vfc & 0x3F): c.vfn = 0
            if c.arpn and c.note:
                if (c.arpc & 0x1F) < (c.arpt & 0x1F): c.arpc = U8(c.arpc + 1)
                else:
                    if (c.arpc & 0xE0) == 0: c.arpc = 0x20
                    elif (c.arpc & 0xE0) == 0x20 and not c.arpt & 0x40 and c.arpn != 0xFF: c.arpc = 0x40
                    else: c.arpc = 0
                    an = c.note
                    if c.arpc & 0xE0:
                        an = 0 if c.arpn == 0xFF else U8(an + (c.arpn >> 4))
                    if (c.arpc & 0xE0) == 0x40: an = U8(an + (c.arpn & 0x0F))
                    c.freq = NOTE[U8(an + S8(c.trans)) & 63]
            if c.tdepth:
                vt = c.freq if c.tconf & 0x40 else c.vol
                vt = vt + c.tdepth if c.tcount & 0x80 else vt - c.tdepth
                if vt < 0: vt = 0
                elif c.tconf & 0x40:
                    if vt > 9397: vt = 9397
                elif vt > 63: vt = 63
                if c.tconf & 0x40: c.freq = vt
                else: c.vol = U8(vt)
                if (c.tcount & 0x1F) < (c.tconf & 0x1F): c.tcount = U8(c.tcount + 1)
                else: c.tcount = 0 if c.tcount & 0x80 else 0x80
            if c.delay:
                if c.delay != 0xFFFF: c.delay -= 1
            else:
                while True:
                    cmd = self.rd(c.ptr); c.ptr += 1
                    if self.on_cmd: self.on_cmd(cmd)
                    if cmd < 64:
                        c.note = cmd
                        if c.note: c.note = U8(c.note + S8(c.trans))
                        c.freq = NOTE[c.note & 63]
                        if not c.vfc: c.vol = c.rcn
                        if c.arpt & 0x20: c.arpc = 0
                    elif cmd < 160:
                        k = cmd - 64
                        if k == 0: c.vol = self.rd(c.ptr); c.ptr += 1; c.rcn = c.vol
                        elif k in (1, 4): c.vfs = self.rd(c.ptr); c.ptr += 1; c.vfc = 0 if k == 1 else 0x40
                        elif k in (2, 5): c.vfs = self.rd(c.ptr); c.vfc = self.rd(c.ptr + 1); c.ptr += 2
                        elif k in (3, 6): c.vfs = 0
                        elif k == 7: c.arpn = self.rd(c.ptr); c.arpt = self.rd(c.ptr + 1); c.ptr += 2
                        elif k == 8: c.arpn = 0
                        elif k == 9: c.rec = self.rd(c.ptr); c.ptr += 1
                        elif k == 10: c.rec = 0
                        elif k == 11: c.trans = U8(c.trans + self.rd(c.ptr)); c.ptr += 1
                        elif k == 12: c.trans = self.rd(c.ptr); c.ptr += 1
                        elif k == 13: c.trans = 0
                        elif k in (14, 16):
                            c.tdepth = self.rd(c.ptr); c.tconf = U8(self.rd(c.ptr + 1) + (0 if k == 14 else 0x40)); c.ptr += 2
                        elif k in (15, 17): c.tdepth = 0
                        elif k == 18: c.gconf = self.rd(c.ptr); c.ptr += 1
                        elif k == 19: c.gconf = 0
                        elif k == 20: c.arpn = 0xFF; c.arpt = self.rd(c.ptr); c.ptr += 1
                        elif k == 21: c.arpn = 0
                        elif k == 92: self.tick_rate = U8(self.tick_rate + self.rd(c.ptr)) or 1; c.ptr += 1
                        elif k == 93: self.tick_rate = self.rd(c.ptr) or 1; c.ptr += 1
                        elif k == 94:
                            for i in range(4): self.ch[i].repeat = self.rd(c.ptr + i)
                            c.ptr += 4
                        elif k == 95: self.mute ^= 1 << (n + 4); c.vol = 0; c.delay = 0xFFFF
                    elif cmd < 224: c.delay = cmd - 159
                    elif cmd == 224: c.delay = U16(self.vle(c) + 65)
                    elif cmd < 252: pass
                    elif cmd in (252, 253):
                        if cmd == 252: nc = 0
                        else: nc = self.rd(c.ptr); c.ptr += 1
                        nt = self.rd(c.ptr); c.ptr += 1
                        if nt != c.track and c.si < DEPTH:
                            c.sc[c.si], c.st[c.si] = c.counter, c.track
                            c.sp[c.si] = U16(c.ptr - self.track_base)
                            c.si += 1; c.track = nt
                        c.counter = nc; c.ptr = self.track_ptr(c.track)
                    elif cmd == 254:
                        if c.counter > 0 or c.si == 0:
                            if c.counter: c.counter -= 1
                            c.ptr = self.track_ptr(c.track)
                        else:
                            c.si -= 1
                            c.ptr = c.sp[c.si] + self.track_base
                            c.counter, c.track = c.sc[c.si], c.st[c.si]
                    elif cmd == 255:
                        c.ptr += self.vle(c)
                    if c.delay != 0: break
                if c.delay != 0xFFFF: c.delay -= 1
            if not self.mute & (1 << n):
                if n != 3: self.osc_freq[n] = c.freq
                self.osc_vol[n] = c.vol >> 1 if n == 3 else c.vol
            if not self.mute & 0xF0:
                if sum(x.repeat for x in self.ch):
                    for x in self.ch: x.ptr = self.track_ptr(x.repeat); x.delay = 0
                    self.mute = 0xF0
                else:
                    self.ch = [Ch() for _ in range(4)]
                    self.playing = False

    def tone(self):
        best, f = 0, 0
        for n in range(3):
            if self.osc_vol[n] > best and self.osc_freq[n]: best, f = self.osc_vol[n], self.osc_freq[n]
        return f if self.playing else 0


def render(mem, song, max_ms=180000):
    """-> (intro notes, loop notes): notes = [(freq, ms)]. A song whose player state
    does not repeat within max_ms (some channel drifts, e.g. adds transposition on
    every pass) is cut there and loops as a whole."""
    p = Player(mem, song)
    seen, ticks = {}, []                      # state before tick i -> i; per tick (freq, ms)
    loop_at = None
    total = 0
    while p.playing and total < max_ms:
        s = p.state()
        if s in seen: loop_at = seen[s]; break
        seen[s] = len(ticks)
        ms = 1000 // p.tick_rate
        p.tick()
        ticks.append((p.tone(), ms)); total += ms
    if loop_at is None and p.playing: loop_at = 0         # cut at max_ms: loop everything

    def merge(ts):
        out = []
        for f, ms in ts:
            if out and out[-1][0] == f and out[-1][1] + ms < 65536: out[-1] = (f, out[-1][1] + ms)
            else: out.append((f, ms))
        return out
    if loop_at is None: return merge(ticks), []
    return merge(ticks[:loop_at]), merge(ticks[loop_at:])


def main():
    elf, far, nm = sys.argv[1:4]
    syms = {}
    for l in subprocess.run([nm, '-S', '-C', elf], capture_output=True, text=True).stdout.splitlines():
        q = l.split()
        if len(q) == 4: syms[q[3].split('.')[0]] = (int(q[0], 16), int(q[1], 16))
    data = bytearray(open(far, 'rb').read())
    names = sys.argv[4].split(',')
    mem = {}
    class Mem:
        def __getitem__(self, a): return data[a]
    m = Mem()
    table, streams = [], bytearray()
    report = []
    for name in names:
        addr = syms[name][0]
        intro, loop = render(m, addr - FAR_BASE)
        off = len(streams)
        streams += struct.pack('<HH', len(intro), len(loop))
        for f, ms in intro + loop: streams += struct.pack('<HH', f, ms)
        table.append((addr, off))
        report.append(f'{name}: {len(intro)}+{len(loop)} notes, {sum(ms for _, ms in intro) / 1000:.1f}+{sum(ms for _, ms in loop) / 1000:.1f} s')
    base = len(data) + 2 + 6 * len(table)
    data += struct.pack('<H', len(table))
    for addr, off in table: data += struct.pack('<IH', addr, base + off)
    data += streams
    open(far, 'wb').write(data)
    print(f'songs: {len(table)} rendered, {len(streams) + 2 + 6 * len(table)} bytes')
    for r in report: print('  ' + r)


if __name__ == '__main__':
    main()
