#!/usr/bin/env node
// ===================================================================================
// CH32V003 Game Console Simulator - development server (no dependencies)
// ===================================================================================
//
//   node sim/server.mjs [--port 8088] [--watch path/to/firmware.elf] [--name "My Game"]
//
// Serves the web UI. With --watch, the firmware file is offered as the "live" entry
// and every rebuild is pushed to open browser tabs (server-sent events).

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const webRoot = path.join(here, 'web');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 && args[i + 1] ? args[i + 1] : def; };
const port = Number(opt('port', process.env.PORT || 8088));
const watch = opt('watch', null) ? path.resolve(opt('watch')) : null;
const name = opt('name', watch ? path.basename(watch).replace(/\.(elf|bin|hex)$/, '') : null);

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.wasm': 'application/wasm',
  '.png': 'image/png', '.svg': 'image/svg+xml', '.bin': 'application/octet-stream',
};

// ---- live reload --------------------------------------------------------------------
const clients = new Set();
let lastSig = null;
function signature() {
  // an empty file means the linker is still writing it: not a valid signature yet
  try { const s = fs.statSync(watch); return s.size ? `${s.mtimeMs}:${s.size}` : null; } catch { return null; }
}
if (watch) {
  lastSig = signature();
  let pending = null;
  setInterval(() => {
    const sig = signature();
    if (!sig || sig === lastSig) return;
    // wait until the linker has finished writing (signature stable for 200 ms)
    if (pending !== sig) { pending = sig; return; }
    lastSig = sig; pending = null;
    console.log(`[chsim] ${path.relative(process.cwd(), watch)} changed -> reloading ${clients.size} tab(s)`);
    for (const res of clients) res.write(`event: change\ndata: ${sig}\n\n`);
  }, 200);
}

// ---- server -------------------------------------------------------------------------
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (url.pathname === '/api/dev') {
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    res.end(JSON.stringify({ watch: watch ? path.relative(process.cwd(), watch) : null, name }));
    return;
  }
  if (url.pathname === '/api/firmware') {
    if (!watch) { res.writeHead(404); res.end(); return; }
    fs.readFile(watch, (err, data) => {
      if (err) { res.writeHead(404); res.end(String(err)); return; }
      res.writeHead(200, { 'content-type': 'application/octet-stream', 'cache-control': 'no-store' });
      res.end(data);
    });
    return;
  }
  if (url.pathname === '/api/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', connection: 'keep-alive' });
    res.write(': hello\n\n');
    clients.add(res);
    const ping = setInterval(() => res.write(': ping\n\n'), 20000);
    req.on('close', () => { clients.delete(res); clearInterval(ping); });
    return;
  }
  let file = path.normalize(path.join(webRoot, decodeURIComponent(url.pathname)));
  if (!file.startsWith(webRoot)) { res.writeHead(403); res.end(); return; }
  if (file.endsWith(path.sep)) file = path.join(file, 'index.html');
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-cache' });
    res.end(data);
  });
});

server.listen(port, '127.0.0.1', () => {
  console.log(`[chsim] simulator running at http://localhost:${port}/`);
  if (watch) console.log(`[chsim] watching ${path.relative(process.cwd(), watch)} for rebuilds`);
});
