// ===================================================================================
// CH32V003 Game Console Simulator - JS wrapper around chsim.wasm
// ===================================================================================

export const BTN = { UP: 1, DOWN: 2, LEFT: 4, RIGHT: 8, ACT: 16, B: 32 };
export const FLASH_SIZE = 16384;

const CAUSES = [
  'instruction address misaligned', 'instruction access fault', 'illegal instruction',
  'breakpoint (ebreak)', 'load address misaligned', 'load access fault',
  'store address misaligned', 'store access fault', 'ecall (U)', 'ecall (S)', '-', 'ecall (M)',
];

export class Emulator {
  static async create(url = 'chsim.wasm') {
    const { instance } = await WebAssembly.instantiateStreaming(fetch(url), {});
    return new Emulator(instance.exports);
  }

  constructor(exports) {
    this.w = exports;
    this.w.init();
    this.symbols = null;
    this.decoder = new TextDecoder();
  }

  get mem() { return this.w.memory.buffer; }

  // Load a raw flash image (Uint8Array, starting at address 0)
  loadImage(img, symbols = null) {
    const n = Math.min(img.length, FLASH_SIZE);
    new Uint8Array(this.mem, this.w.rom_ptr(), FLASH_SIZE).fill(0xFF).set(img.subarray(0, n));
    this.w.load(n);
    this.symbols = symbols;
    return n;
  }

  reset()            { this.w.reset(); }
  runMicros(us)      { return this.w.run_us(us); }
  setButtons(mask)   { this.w.set_buttons(mask); }
  setVcc(mv)         { this.w.set_vcc(mv); }
  setNoise(lsb)      { this.w.set_noise(lsb); }
  setFaultHalt(on)   { this.w.set_fault_halt(on ? 1 : 0); }
  setTiming(t)       { this.w.set_timing(t.load, t.store, t.branch, t.jump, t.mmio); }

  framebuffer()      { return new Uint8Array(this.mem, this.w.framebuffer(), 128 * 64); }
  displayOn()        { return !!this.w.oled_on(); }

  takeAudio() {
    const n = this.w.audio_len();
    const out = new Int16Array(this.mem, this.w.audio_ptr(), n).slice();
    this.w.audio_clear();
    return out;
  }

  takeSerial() {
    const n = this.w.uart_len();
    if (!n) return '';
    const s = this.decoder.decode(new Uint8Array(this.mem, this.w.uart_ptr(), n));
    this.w.uart_clear();
    return s;
  }

  takeLog() {
    const n = this.w.log_len();
    if (!n) return '';
    const s = this.decoder.decode(new Uint8Array(this.mem, this.w.log_ptr(), n));
    this.w.log_clear();
    return s;
  }

  stats() {
    const w = this.w;
    return {
      hclk: w.hclk(),
      cycles: w.cycles_hi() * 4294967296 + w.cycles_lo(),
      instret: w.instret_lo() >>> 0,
      pc: w.pc() >>> 0,
      sleeping: w.sleeping(),
      oledBytes: w.oled_bytes() >>> 0,
      oledFrames: w.oled_frames() >>> 0,
      status: w.status(),
    };
  }

  regs() { return Array.from({ length: 16 }, (_, i) => this.w.reg(i) >>> 0); }

  fault() {
    if (this.w.status() !== 1) return null;
    const cause = this.w.fault_cause();
    return { cause, name: CAUSES[cause] || `cause ${cause}`, pc: this.w.pc() >>> 0, tval: this.w.fault_tval() >>> 0 };
  }

  sram() { return new Uint8Array(this.mem, this.w.sram_ptr(), 2048); }

  // AT24CM01 EEPROM on the I2C bus: a live view of its 128 KiB (non-volatile, survives
  // resets, firmware changes and save states), whether it is fitted, and a change counter
  eeprom()           { return new Uint8Array(this.mem, this.w.eeprom_ptr(), this.w.eeprom_size()); }
  setEeprom(on)      { this.w.set_eeprom(on ? 1 : 0); }
  setBoard(rev)      { this.w.set_board(rev); }      // 11, 12 (EEPROM) or 13 (EEPROM + B)
  eepromDirty()      { return this.w.eeprom_dirty() >>> 0; }
  eepromWrites()     { return this.w.eeprom_writes() >>> 0; }

  saveState() {
    this.w.state_save();
    return new Uint8Array(this.mem, this.w.state_ptr(), this.w.state_size()).slice();
  }

  loadState(buf) {
    if (buf.length !== this.w.state_size()) throw new Error('save state does not match this simulator build');
    new Uint8Array(this.mem, this.w.state_ptr(), buf.length).set(buf);
    this.w.state_load();
  }

  // Map an address to "function+offset" if an ELF with symbols was loaded
  symbolize(addr) {
    if (!this.symbols) return '';
    let best = null;
    for (const s of this.symbols) if (s.addr <= addr && (!best || s.addr > best.addr)) best = s;
    if (!best || addr - best.addr > 0x2000) return '';
    return addr === best.addr ? best.name : `${best.name}+0x${(addr - best.addr).toString(16)}`;
  }
}

// -----------------------------------------------------------------------------------
// Firmware file formats: .bin, Intel .hex, .elf
// -----------------------------------------------------------------------------------
const flashAddr = (a) => {
  if (a < FLASH_SIZE) return a;
  if (a >= 0x08000000 && a < 0x08000000 + FLASH_SIZE) return a - 0x08000000;
  return -1;
};

export function parseFirmware(name, bytes) {
  const lower = name.toLowerCase();
  if (lower.endsWith('.hex') || bytes[0] === 0x3A) return parseHex(new TextDecoder().decode(bytes));
  if (bytes[0] === 0x7F && bytes[1] === 0x45 && bytes[2] === 0x4C && bytes[3] === 0x46) return parseElf(bytes);
  return { image: bytes.slice(0, FLASH_SIZE), symbols: null };
}

function parseHex(text) {
  const img = new Uint8Array(FLASH_SIZE).fill(0xFF);
  let base = 0, max = 0;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line.startsWith(':')) continue;
    const n = parseInt(line.substr(1, 2), 16), addr = parseInt(line.substr(3, 4), 16), type = parseInt(line.substr(7, 2), 16);
    if (type === 0) {
      for (let i = 0; i < n; i++) {
        const a = flashAddr(base + addr + i);
        if (a >= 0) { img[a] = parseInt(line.substr(9 + i * 2, 2), 16); max = Math.max(max, a + 1); }
      }
    } else if (type === 4) base = parseInt(line.substr(9, 4), 16) << 16;
    else if (type === 2) base = parseInt(line.substr(9, 4), 16) << 4;
    else if (type === 1) break;
  }
  return { image: img.subarray(0, max), symbols: null };
}

function parseElf(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes[4] !== 1) throw new Error('only 32-bit ELF files are supported');
  const phoff = dv.getUint32(28, true), phentsize = dv.getUint16(42, true), phnum = dv.getUint16(44, true);
  const shoff = dv.getUint32(32, true), shentsize = dv.getUint16(46, true), shnum = dv.getUint16(48, true);
  const img = new Uint8Array(FLASH_SIZE).fill(0xFF);
  let max = 0;
  for (let i = 0; i < phnum; i++) {
    const o = phoff + i * phentsize;
    if (dv.getUint32(o, true) !== 1) continue;                    // PT_LOAD
    const off = dv.getUint32(o + 4, true), paddr = dv.getUint32(o + 12, true), filesz = dv.getUint32(o + 16, true);
    const a = flashAddr(paddr);
    if (a < 0 || !filesz) continue;
    img.set(bytes.subarray(off, off + Math.min(filesz, FLASH_SIZE - a)), a);
    max = Math.max(max, a + filesz);
  }
  // symbol table
  const symbols = [];
  for (let i = 0; i < shnum; i++) {
    const o = shoff + i * shentsize;
    if (dv.getUint32(o + 4, true) !== 2) continue;                // SHT_SYMTAB
    const off = dv.getUint32(o + 16, true), size = dv.getUint32(o + 20, true), link = dv.getUint32(o + 24, true);
    const so = shoff + link * shentsize, stroff = dv.getUint32(so + 16, true);
    for (let p = off; p < off + size; p += 16) {
      const nameOff = dv.getUint32(p, true), value = dv.getUint32(p + 4, true), info = bytes[p + 12];
      if ((info & 15) !== 2) continue;                              // STT_FUNC
      let e = stroff + nameOff; while (bytes[e]) e++;
      symbols.push({ addr: value, name: new TextDecoder().decode(bytes.subarray(stroff + nameOff, e)) });
    }
  }
  return { image: img.subarray(0, Math.min(max, FLASH_SIZE)), symbols: symbols.length ? symbols : null };
}
