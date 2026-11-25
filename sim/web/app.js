// ===================================================================================
// CH32V003 Game Console Simulator - web front end
// ===================================================================================

import { Emulator, BTN, parseFirmware } from './emu.js';

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const hex = (v, n = 8) => '0x' + (v >>> 0).toString(16).padStart(n, '0');
const store = {
  get(k, d) { try { const v = localStorage.getItem('chsim.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('chsim.' + k, JSON.stringify(v)); } catch {} },
};

const emu = await Emulator.create();
const state = {
  game: null,            // current library entry (or {custom})
  running: true,
  powered: true,
  speed: 1,
  buttons: { kbd: 0, touch: 0, pad: 0 },
  lastFrame: performance.now(),
  stat: { t: performance.now(), cycles: 0, frames: 0, bytes: 0 },
  dev: null,
};

// -----------------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------------
const SCALE = 5;
const canvas = $('#screen');
const ctx = canvas.getContext('2d');
const image = ctx.createImageData(128 * SCALE, 64 * SCALE);
const pix = new Uint32Array(image.data.buffer);
const COLORS = {
  white: [[225, 240, 255]],
  blue:  [[90, 200, 255]],
  green: [[110, 255, 170]],
  duo:   [[255, 214, 60], [90, 200, 255]],   // top 16 rows yellow, rest blue
};
const OFF = 0xFF0C0A12;      // unlit pixel (ABGR)
const GAP = 0xFF07060B;
let palette = null;

function buildPalette() {
  const name = $('#oled-color').value;
  const cols = COLORS[name] || COLORS.white;
  palette = cols.map(([r, g, b]) => {
    const lut = new Uint32Array(256);
    for (let v = 0; v < 256; v++) {
      if (!v) { lut[v] = OFF; continue; }
      const k = v / 255;
      lut[v] = (255 << 24) | (Math.round(b * k) << 16) | (Math.round(g * k) << 8) | Math.round(r * k);
    }
    return lut;
  });
  const [r, g, b] = cols[cols.length - 1];
  canvas.style.setProperty('--oled-glow', `rgba(${r},${g},${b},.5)`);
}

function draw() {
  const fb = state.powered ? emu.framebuffer() : null;
  const grid = $('#oled-grid').checked;
  const W = 128 * SCALE;
  for (let y = 0; y < 64; y++) {
    const lut = palette.length > 1 && y >= 16 ? palette[1] : palette[0];
    for (let x = 0; x < 128; x++) {
      const c = fb ? lut[fb[y * 128 + x]] : OFF;
      const base = y * SCALE * W + x * SCALE;
      for (let dy = 0; dy < SCALE; dy++) {
        const row = base + dy * W;
        for (let dx = 0; dx < SCALE; dx++)
          pix[row + dx] = grid && (dx === SCALE - 1 || dy === SCALE - 1) ? GAP : c;
      }
    }
  }
  ctx.putImageData(image, 0, 0);
}

// -----------------------------------------------------------------------------------
// Audio: the core produces 48 kHz samples of the buzzer current
// -----------------------------------------------------------------------------------
let audio = null, gain = null, playhead = 0;
function ensureAudio() {
  if (audio) { if (audio.state === 'suspended') audio.resume(); return; }
  audio = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  gain = audio.createGain();
  gain.connect(audio.destination);
  setVolume();
}
function setVolume() {
  const v = Number($('#volume').value) / 100;
  if (gain) gain.gain.value = v * v * 0.8;
  store.set('volume', $('#volume').value);
}
function pushAudio(samples) {
  let loud = false;
  for (let i = 0; i < samples.length; i += 16) if (Math.abs(samples[i]) > 600) { loud = true; break; }
  $('#buzzer').classList.toggle('on', loud);
  if (!audio || audio.state !== 'running' || !samples.length || state.speed !== 1) return;
  const buf = audio.createBuffer(1, samples.length, 48000);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < samples.length; i++) ch[i] = samples[i] / 32768;
  const now = audio.currentTime;
  if (playhead < now + 0.02 || playhead > now + 0.25) playhead = now + 0.05;
  const src = audio.createBufferSource();
  src.buffer = buf; src.connect(gain); src.start(playhead);
  playhead += buf.duration;
}

// -----------------------------------------------------------------------------------
// Input: keyboard, on-screen switches (multi-touch), gamepads
// -----------------------------------------------------------------------------------
const KEYMAP = {
  ArrowUp: BTN.UP, KeyW: BTN.UP, ArrowDown: BTN.DOWN, KeyS: BTN.DOWN,
  ArrowLeft: BTN.LEFT, KeyA: BTN.LEFT, ArrowRight: BTN.RIGHT, KeyD: BTN.RIGHT,
  Space: BTN.ACT, KeyX: BTN.ACT, KeyK: BTN.ACT, Enter: BTN.ACT,
  KeyZ: BTN.B, KeyC: BTN.B, KeyJ: BTN.B, ShiftLeft: BTN.B,
};
const isTyping = (e) => /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) && e.target.type !== 'range' && e.target.type !== 'checkbox';

addEventListener('keydown', (e) => {
  if (isTyping(e) || e.metaKey || e.ctrlKey) return;
  ensureAudio();
  if (KEYMAP[e.code]) { state.buttons.kbd |= KEYMAP[e.code]; e.preventDefault(); applyButtons(); return; }
  if (e.repeat) return;
  if (e.code === 'KeyP') togglePause();
  else if (e.code === 'KeyR') powerCycle();
  else if (e.code === 'KeyF') toggleFullscreen();
  else if (e.code === 'F2') { e.preventDefault(); saveState(); }
  else if (e.code === 'F4') { e.preventDefault(); loadState(); }
});
addEventListener('keyup', (e) => {
  if (KEYMAP[e.code]) { state.buttons.kbd &= ~KEYMAP[e.code]; applyButtons(); }
});
addEventListener('blur', () => { state.buttons.kbd = 0; state.buttons.touch = 0; applyButtons(); });

const touchOwners = new Map();   // pointerId -> mask
for (const el of $$('.sw')) {
  const mask = BTN[el.dataset.btn];
  el.addEventListener('pointerdown', (e) => {
    ensureAudio();
    el.setPointerCapture(e.pointerId);
    touchOwners.set(e.pointerId, mask);
    recomputeTouch(); e.preventDefault();
  });
  // a thumb on the joypad can roll from one direction to the next without lifting
  if (el.closest('.dpad')) el.addEventListener('pointermove', (e) => {
    if (!touchOwners.has(e.pointerId)) return;
    const over = document.elementFromPoint(e.clientX, e.clientY)?.closest('.dpad .sw');
    const m = over ? BTN[over.dataset.btn] : touchOwners.get(e.pointerId);
    if (m !== touchOwners.get(e.pointerId)) { touchOwners.set(e.pointerId, m); recomputeTouch(); }
  });
  const up = (e) => { touchOwners.delete(e.pointerId); recomputeTouch(); };
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
  el.addEventListener('lostpointercapture', up);
  el.addEventListener('contextmenu', (e) => e.preventDefault());
}
function recomputeTouch() {
  let m = 0; for (const v of touchOwners.values()) m |= v;
  state.buttons.touch = m; applyButtons();
}

function pollGamepads() {
  let m = 0;
  for (const gp of navigator.getGamepads ? navigator.getGamepads() : []) {
    if (!gp) continue;
    const b = (i) => gp.buttons[i] && gp.buttons[i].pressed;
    const ax = gp.axes[0] || 0, ay = gp.axes[1] || 0;
    if (b(12) || ay < -0.5) m |= BTN.UP;
    if (b(13) || ay > 0.5) m |= BTN.DOWN;
    if (b(14) || ax < -0.5) m |= BTN.LEFT;
    if (b(15) || ax > 0.5) m |= BTN.RIGHT;
    if (b(0) || b(5) || b(7)) m |= BTN.ACT;
    if (b(1) || b(2) || b(3) || b(4) || b(6)) m |= BTN.B;
  }
  if (m !== state.buttons.pad) { state.buttons.pad = m; applyButtons(); }
}

function applyButtons() {
  const m = state.buttons.kbd | state.buttons.touch | state.buttons.pad;
  emu.setButtons(m);
  for (const el of $$('.sw')) el.classList.toggle('pressed', !!(m & BTN[el.dataset.btn]));
}

// -----------------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------------
function frame(now) {
  const dt = Math.min(now - state.lastFrame, 100);
  state.lastFrame = now;
  pollGamepads();
  if (state.running && state.powered) {
    let status = 0;
    if (state.speed > 0) {
      status = emu.runMicros(Math.round(dt * 1000 * state.speed));
    } else {
      const t0 = performance.now();
      while (performance.now() - t0 < 12 && !status) status = emu.runMicros(4000);
    }
    pushAudio(emu.takeAudio());
    if (status) onFault();
  }
  draw();
  updateStats(now);
  requestAnimationFrame(frame);
}

function updateStats(now) {
  if (now - state.stat.t < 500) return;
  const s = emu.stats();
  const dtReal = (now - state.stat.t) / 1000;
  const emuSec = (s.cycles - state.stat.cycles) / (s.hclk || 1);
  const fps = (s.oledFrames - state.stat.frames) / dtReal;
  const bps = (s.oledBytes - state.stat.bytes) / dtReal;
  state.stat = { t: now, cycles: s.cycles, frames: s.oledFrames, bytes: s.oledBytes };

  const live = state.running && state.powered;
  $('#pill-clock').textContent = `${(s.hclk / 1e6).toFixed(s.hclk % 1e6 ? 1 : 0)} MHz`;
  $('#pill-speed').textContent = live ? `${Math.round(emuSec / dtReal * 100)}% speed` : 'paused';
  $('#pill-fps').textContent = live ? `${fps.toFixed(1)} fps` : '— fps';
  eeprom.persist();
  if ($('#tab-hw').classList.contains('active')) $('#ee-info').textContent = eeprom.info();

  if (!$('#tab-debug').classList.contains('active')) return;
  const sym = emu.symbolize(s.pc);
  const rows = [
    ['HCLK', `${(s.hclk / 1e6).toFixed(3)} MHz`],
    ['emulated time', `${(s.cycles / (s.hclk || 1)).toFixed(2)} s`],
    ['real-time ratio', `${(emuSec / dtReal).toFixed(2)}×`],
    ['instructions', s.instret.toLocaleString()],
    ['OLED refresh', `${fps.toFixed(1)} fps`],
    ['I2C payload', `${(bps / 1024).toFixed(1)} KiB/s`],
    ['CPU', s.sleeping ? 'sleeping (WFI)' : 'running'],
    ['PC', hex(s.pc) + (sym ? ` <${sym}>` : '')],
  ];
  $('#stats').innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v.replace(/</g, '&lt;')}</dd>`).join('');
  const r = emu.regs();
  const NAMES = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5'];
  let txt = '';
  for (let i = 0; i < 16; i++) txt += `${NAMES[i].padStart(4)} ${hex(r[i])}${i % 2 ? '\n' : '   '}`;
  $('#regs').textContent = txt;
  appendConsole('#serial', emu.takeSerial());
  appendConsole('#log', emu.takeLog());
}

function appendConsole(sel, text) {
  if (!text) return;
  const el = $(sel);
  el.textContent = (el.textContent + text).slice(-20000);
  el.scrollTop = el.scrollHeight;
}

function onFault() {
  const f = emu.fault();
  if (!f) return;
  const sym = emu.symbolize(f.pc);
  $('#fault').textContent = `CPU exception: ${f.name}\npc   ${hex(f.pc)}${sym ? ' <' + sym + '>' : ''}\ntval ${hex(f.tval)}\n\n` +
    `On real hardware the core would jump to the HardFault vector.\nUntick "Halt on CPU exception" to emulate that.`;
  $('#fault').classList.remove('hidden');
  appendConsole('#log', emu.takeLog());
  selectTab('debug');
  toast('CPU exception: ' + f.name);
}

// -----------------------------------------------------------------------------------
// Firmware loading
// -----------------------------------------------------------------------------------
function boot(image, symbols, game) {
  emu.loadImage(image, symbols);
  applySettings();
  applyButtons();
  state.game = game;
  state.powered = true; $('#power').classList.add('on');
  $('#fault').classList.add('hidden');
  $('#serial').textContent = ''; $('#log').textContent = '';
  state.stat = { t: performance.now(), cycles: 0, frames: 0, bytes: 0 };
  for (const li of $$('.game')) li.classList.toggle('active', li.dataset.id === game.id);
  $('#game-title').textContent = game.title;
  $('#game-desc').textContent = game.description || '';
  $('#game-controls').innerHTML = (game.controls || []).map((c) => `<li>${c.replace(/</g, '&lt;')}</li>`).join('') || '<li class="muted">—</li>';
  $('#game-meta').textContent = `${image.length.toLocaleString()} bytes of 16 KiB flash${game.f_cpu ? ` · built for ${game.f_cpu / 1e6} MHz` : ''}${symbols ? ` · ${symbols.length} symbols` : ''}`;
  if (game.id && !game.custom) { history.replaceState(null, '', '#' + game.id); store.set('last', game.id); }
}

async function loadGame(g) {
  const res = await fetch(g.file);
  const bytes = new Uint8Array(await res.arrayBuffer());
  const fw = parseFirmware(g.file, bytes);
  if (g.eeprom) {                      // e.g. the game library: its EEPROM image comes along
    // the save slots (high scores, saved games, the menu's selection and contrast) are
    // kept, as on a real board where the library is written once and the saves stay.
    // The rest of the save area must come from the new image: 0x1F010..0x1F0FF (the
    // unused end of the menu's slot) and 0x1F800.. hold programs and menu cards
    // (firmware/tools/pack.py RESERVED)
    const SAVES = [[0x1F000, 0x1F010], [0x1F100, 0x1F800]];
    const ee = new Uint8Array(await (await fetch(g.eeprom)).arrayBuffer());
    const m = emu.eeprom();
    const saves = SAVES.map(([a, b]) => m.slice(a, b));
    m.fill(0xFF); m.set(ee.subarray(0, m.length));
    SAVES.forEach(([a], i) => m.set(saves[i], a));
    eeprom.persist(true);
    toast('EEPROM: game data loaded (saves kept)');
  }
  boot(fw.image, fw.symbols, g);
}

async function loadFile(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  try {
    const fw = parseFirmware(file.name, bytes);
    boot(fw.image, fw.symbols, { id: 'file:' + file.name, title: file.name, custom: true, description: 'Firmware loaded from disk.' });
    toast(`Loaded ${file.name}`);
  } catch (err) { toast('Could not load: ' + err.message); }
}

$('#file').addEventListener('change', (e) => { if (e.target.files[0]) loadFile(e.target.files[0]); e.target.value = ''; });
let dragDepth = 0;
addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('#drop').classList.remove('hidden'); });
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('#drop').classList.add('hidden'); } });
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('drop', (e) => {
  e.preventDefault(); dragDepth = 0; $('#drop').classList.add('hidden');
  const f = e.dataTransfer.files[0]; if (f) loadFile(f);
});

// -----------------------------------------------------------------------------------
// Controls
// -----------------------------------------------------------------------------------
function togglePause() {
  state.running = !state.running;
  $('#btn-pause').textContent = state.running ? 'Pause' : 'Resume';
  $('#btn-pause').classList.toggle('active', !state.running);
  if (state.running) state.lastFrame = performance.now();
}
function powerCycle() {
  emu.reset(); applySettings(); applyButtons();
  $('#fault').classList.add('hidden');
  state.powered = true; $('#power').classList.add('on');
}
$('#btn-pause').onclick = togglePause;
$('#btn-reset').onclick = powerCycle;
$('#power').onclick = () => {
  state.powered = !state.powered;
  $('#power').classList.toggle('on', state.powered);
  if (state.powered) powerCycle();
};
$('#speed').onchange = (e) => { state.speed = Number(e.target.value); };
$('#volume').oninput = () => { ensureAudio(); setVolume(); };

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else if (matchMedia('(pointer: coarse)').matches && document.documentElement.requestFullscreen)
    // on a phone the on-screen buttons are needed: fullscreen the whole page instead
    document.documentElement.requestFullscreen().catch(() => toast('Fullscreen not available'));
  else if (canvas.requestFullscreen) canvas.requestFullscreen().catch(() => toast('Fullscreen not available'));
  else toast('Fullscreen not available');
}
$('#btn-full').onclick = toggleFullscreen;

$('#btn-shot').onclick = () => {
  canvas.toBlob((blob) => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${(state.game && state.game.id || 'screen').replace(/[^a-z0-9_-]/gi, '_')}-${Date.now()}.png`;
    a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
};

const b64 = {
  enc(u8) { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); },
  dec(s) { const b = atob(s); const u = new Uint8Array(b.length); for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i); return u; },
};
function saveState() {
  if (!state.game) return;
  store.set('state.' + state.game.id, b64.enc(emu.saveState()));
  toast('State saved');
}
function loadState() {
  if (!state.game) return;
  const s = store.get('state.' + state.game.id, null);
  if (!s) { toast('No saved state for this game'); return; }
  try { emu.loadState(b64.dec(s)); applySettings(); applyButtons(); $('#fault').classList.add('hidden'); toast('State loaded'); }
  catch (err) { toast(err.message); }
}
$('#btn-save').onclick = saveState;
$('#btn-load').onclick = loadState;

for (const t of $$('.tab')) t.onclick = () => selectTab(t.dataset.tab);
function selectTab(name) {
  for (const t of $$('.tab')) t.classList.toggle('active', t.dataset.tab === name);
  for (const p of $$('.tabpane')) p.classList.toggle('active', p.id === 'tab-' + name);
  state.stat.t = 0;
}
$('#clear-serial').onclick = () => { $('#serial').textContent = ''; };
$('#clear-log').onclick = () => { $('#log').textContent = ''; };

// Hardware settings (persisted)
const SETTINGS = ['oled-color', 'oled-grid', 'oled-glow', 'vcc', 'noise', 'fault-halt', 't-load', 't-store', 't-branch', 't-jump', 't-mmio', 'board-rev'];
for (const id of SETTINGS) {
  const el = document.getElementById(id);
  const saved = store.get('set.' + id, null);
  if (saved !== null) { if (el.type === 'checkbox') el.checked = saved; else el.value = saved; }
  el.addEventListener('change', () => {
    store.set('set.' + id, el.type === 'checkbox' ? el.checked : el.value);
    applySettings();
  });
}
function applySettings() {
  emu.setVcc(Number($('#vcc').value));
  emu.setNoise(Number($('#noise').value));
  emu.setFaultHalt($('#fault-halt').checked);
  const rev = Number($('#board-rev').value);
  emu.setBoard(rev);
  $('#board').dataset.rev = rev;
  emu.setTiming({
    load: Number($('#t-load').value), store: Number($('#t-store').value), branch: Number($('#t-branch').value),
    jump: Number($('#t-jump').value), mmio: Number($('#t-mmio').value),
  });
  buildPalette();
  canvas.classList.toggle('glow', $('#oled-glow').checked);
}
$('#volume').value = store.get('volume', 60);

// EEPROM contents: persisted in localStorage whenever the firmware commits a write
const eeprom = {
  saved: 0,
  restore() {
    const s = store.get('eeprom', null);
    if (s) try { emu.eeprom().set(b64.dec(s).subarray(0, emu.eeprom().length)); } catch {}
    this.saved = emu.eepromDirty();
  },
  persist(force = false) {
    const d = emu.eepromDirty();
    if (!force && d === this.saved) return;
    this.saved = d;
    const m = emu.eeprom();
    store.set('eeprom', m.every((b) => b === 0xFF) ? null : b64.enc(m));
  },
  info() {
    const m = emu.eeprom();
    let used = 0;
    for (let p = 0; p < m.length; p += 256) { for (let i = p; i < p + 256; i++) if (m[i] !== 0xFF) { used++; break; } }
    return `${used} of 512 pages used (${(used / 4).toFixed(1)} KiB) · ${emu.eepromWrites()} page writes since power-on`;
  },
};
eeprom.restore();
$('#ee-file').addEventListener('change', async (e) => {
  const f = e.target.files[0]; e.target.value = '';
  if (!f) return;
  const bytes = new Uint8Array(await f.arrayBuffer());
  const m = emu.eeprom();
  m.fill(0xFF); m.set(bytes.subarray(0, m.length));
  eeprom.persist(true);
  toast(`EEPROM: loaded ${f.name}${bytes.length > m.length ? ' (truncated to 128 KiB)' : ''}`);
});
$('#ee-download').onclick = () => {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([emu.eeprom().slice()], { type: 'application/octet-stream' }));
  a.download = 'eeprom.bin'; a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
};
$('#ee-erase').onclick = () => { emu.eeprom().fill(0xFF); eeprom.persist(true); toast('EEPROM erased'); };

let toastTimer = 0;
function toast(msg) {
  const t = $('#toast'); t.textContent = msg; t.classList.remove('hidden');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.add('hidden'), 2200);
}

// -----------------------------------------------------------------------------------
// Library + dev server live reload
// -----------------------------------------------------------------------------------
const lib = await (await fetch('roms/index.json')).json();
// roms/local.json (not in the repository): games built locally that may not be shared
try {
  const local = await fetch('roms/local.json');
  if (local.ok) lib.games.unshift(...(await local.json()).games);
} catch {}
function renderLibrary() {
  const items = [...lib.games];
  if (state.dev) items.unshift(state.dev.entry);
  $('#games').innerHTML = items.map((g) => `
    <li class="game${g.custom ? ' custom' : ''}" data-id="${g.id}">
      ${g.thumb ? `<img src="${g.thumb}" alt="">` : '<span class="thumb">LIVE</span>'}
      <div><div class="t">${g.title}</div><div class="s">${g.custom ? g.subtitle : (g.size / 1024).toFixed(1) + ' KiB'}</div></div>
    </li>`).join('');
  for (const li of $$('.game')) {
    li.onclick = () => { ensureAudio(); const g = items.find((x) => x.id === li.dataset.id); if (g) loadGame(g); };
    if (state.game && li.dataset.id === state.game.id) li.classList.add('active');
  }
}

async function setupDev() {
  try {
    const res = await fetch('/api/dev');
    if (!res.ok) return false;
    const info = await res.json();
    if (!info.watch) return false;
    state.dev = info;
    info.entry = { id: 'dev', title: info.name, subtitle: 'live · ' + info.watch.split('/').pop(), file: '/api/firmware', custom: true,
      description: `Live firmware from ${info.watch}. Rebuild it and the simulator reloads automatically.`, controls: info.controls || [] };
    $('#pill-dev').classList.remove('hidden');
    const es = new EventSource('/api/events');
    es.addEventListener('change', async () => {
      await loadGame(info.entry);
      $('#pill-dev').classList.add('flash'); setTimeout(() => $('#pill-dev').classList.remove('flash'), 600);
      toast('Firmware rebuilt, reloaded');
    });
    return true;
  } catch { return false; }
}

const isDev = await setupDev();
renderLibrary();
buildPalette();
const wanted = location.hash.slice(1) || (isDev ? 'dev' : store.get('last', lib.games[0].id));
const first = (isDev && wanted === 'dev') ? state.dev.entry : (lib.games.find((g) => g.id === wanted) || lib.games[0]);
await loadGame(first);
requestAnimationFrame(frame);
