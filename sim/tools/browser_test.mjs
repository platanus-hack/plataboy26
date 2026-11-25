// Headless browser smoke test of the web UI via the Chrome DevTools Protocol.
//   node sim/tools/browser_test.mjs http://localhost:8080/ out_dir
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const url = process.argv[2] || 'http://localhost:8080/';
const out = process.argv[3] || '.';
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const port = 9300 + Math.floor(Math.random() * 500);
const prof = fs.mkdtempSync(path.join(out, 'chrome-'));
const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`,
  '--window-size=1440,960', '--autoplay-policy=no-user-gesture-required', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let ws, id = 0; const waiting = new Map(); const logs = [];
async function connect() {
  for (let i = 0; i < 50; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      const page = list.find((t) => t.type === 'page');
      ws = new WebSocket(page.webSocketDebuggerUrl);
      await new Promise((r) => ws.addEventListener('open', r));
      ws.addEventListener('message', (m) => {
        const msg = JSON.parse(m.data);
        if (msg.id && waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
        if (msg.method === 'Runtime.consoleAPICalled') logs.push(msg.params.type + ': ' + msg.params.args.map((a) => a.value ?? a.description).join(' '));
        if (msg.method === 'Runtime.exceptionThrown') logs.push('EXCEPTION: ' + JSON.stringify(msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text));
      });
      return;
    } catch { await sleep(200); }
  }
  throw new Error('chrome did not start');
}
const send = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const key = async (code, keyName, ms) => {
  await send('Input.dispatchKeyEvent', { type: 'keyDown', code, key: keyName });
  await sleep(ms);
  await send('Input.dispatchKeyEvent', { type: 'keyUp', code, key: keyName });
};
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(out, name), Buffer.from(r.result.data, 'base64'));
};

try {
  await connect();
  await send('Runtime.enable'); await send('Page.enable');
  await send('Page.navigate', { url });
  await sleep(2500);
  console.log('pills after load:', await evalJs(`[...document.querySelectorAll('.pill')].map(p=>p.textContent).join(' | ')`));
  await key('Space', ' ', 150);          // start game
  await sleep(1500);
  await key('ArrowUp', 'ArrowUp', 1200);  // walk
  await key('Space', ' ', 100);           // shoot
  await sleep(1000);
  console.log('pills in game:  ', await evalJs(`[...document.querySelectorAll('.pill')].map(p=>p.textContent).join(' | ')`));
  await shot('ui_game.png');
  // save / load state round trip
  await key('F2', 'F2', 50); await sleep(300);
  await key('F4', 'F4', 50); await sleep(500);
  console.log('toast:', await evalJs(`document.querySelector('#toast').textContent`));
  // debug tab
  await evalJs(`document.querySelector('[data-tab=debug]').click()`); await sleep(1200);
  console.log('stats:', await evalJs(`document.querySelector('#stats').innerText.replace(/\\n/g,' ')`));
  await shot('ui_debug.png');
  console.log('console:', logs.length ? logs.join('\n') : '(clean)');
} finally {
  ws?.close(); chrome.kill(); await sleep(300);
  fs.rmSync(prof, { recursive: true, force: true });
}
