#!/usr/bin/env node
// Serve a frozen, validated production build through the unchanged simulator UI.
// Resource installation is additive: conflicting existing EEPROM bytes are refused.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
const game=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const sdk=path.resolve(game,'../..'),web=path.join(sdk,'sim/web');
const args=process.argv.slice(2),portAt=args.indexOf('--port');
const port=portAt<0?8100:Number(args[portAt+1]);
if(!Number.isInteger(port)||port<1024||port>65535)throw new Error('Invalid preview port');
const base=path.join(game,'bin/nitro_nonsense');
const validate=String.raw`
import importlib.util,sys
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[1])/'tools'))
from overlay_policy import read_policy
spec=importlib.util.spec_from_file_location('game_checker',Path(sys.argv[1])/'tools/check_game.py')
checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
p=sys.argv[2]
import json
policy=json.loads(Path(p+'.policy.json').read_text())
read_policy(p+'.policy.json',Path(p+'.bin').read_bytes(),checker.elf_info(p+'.elf'),(p+'.region',policy['sourceRegion']['address']))
`;
execFileSync('python3',['-c',validate,sdk,base],{stdio:'inherit'});
const policy=JSON.parse(fs.readFileSync(base+'.policy.json'));
const resident=fs.readFileSync(base+'.bin'),region=fs.readFileSync(base+'.region');
const card=fs.readFileSync(path.join(game,'assets/menu-card.png'));
const digest=buffer=>createHash('sha256').update(buffer).digest('hex');
if(digest(resident)!==policy.residentSha256||digest(region)!==policy.sourceRegion.sha256)
  throw new Error('Build changed while the preview was freezing; restart with a complete validated build');
const library={games:[{id:'nitro-nonsense-preview',title:'Nano Nitro Nonsense',
  subtitle:'Fixed powers: A Nitro / B Cannon',thumb:'/api/menu-card.png',file:'/api/game.bin',custom:true,
  description:'Automatic acceleration. Steer with the arrows; Down reverses. Nitro and Cannon are available from the first race. First to three wins.',
  controls:['Arrows: steer / Down: reverse','X or Enter: A / Nitro / confirm','Z or C: B / help / Cannon'],
  f_cpu:48000000,data:{file:'/api/resource',at:policy.sourceRegion.address,sha256:policy.sourceRegion.sha256}}]};
const marker='  boot(fw.image, fw.symbols, g);';
const original=fs.readFileSync(path.join(web,'app.js'),'utf8');
if(original.split(marker).length!==2)throw new Error('Simulator load hook changed; refusing an unverified patch');
const install=String.raw`  if (g.data) {
    const source = new Uint8Array(await (await fetch(g.data.file)).arrayBuffer());
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', source)),
                              b => b.toString(16).padStart(2, '0')).join('');
    if (digest !== g.data.sha256) throw new Error('Preview resource identity mismatch');
    const target = emu.eeprom(), at = g.data.at;
    if (!Number.isInteger(at) || at < 0x800 || at + source.length > target.length)
      throw new Error('Invalid preview resource allocation');
    const fresh = target.every(byte => byte === 255);
    const installed = source.every((byte, i) => target[at + i] === byte);
    if (!fresh && !installed)
      throw new Error('Existing EEPROM data is not this preview fixture; export it and use a fresh preview origin. No data was changed.');
    target.set(source, at);
    eeprom.persist(true);
  }
`;
const application=original.replace(marker,install+marker);
const types={'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8',
  '.css':'text/css; charset=utf-8','.wasm':'application/wasm','.json':'application/json'};
const server=http.createServer((request,response)=>{
  let url;
  try{url=new URL(request.url,'http://localhost');}catch{response.writeHead(400);response.end();return;}
  const send=(body,type='application/octet-stream')=>{
    response.writeHead(200,{'content-type':type,'cache-control':'no-store'});response.end(body);
  };
  if(url.pathname==='/api/game.bin')return send(resident);
  if(url.pathname==='/api/menu-card.png')return send(card,'image/png');
  if(url.pathname==='/api/resource')return send(region);
  if(url.pathname==='/api/build')return send(JSON.stringify(policy),'application/json');
  if(url.pathname==='/api/dev'){response.writeHead(404);response.end();return;}
  if(url.pathname==='/roms/index.json')return send(JSON.stringify(library),'application/json');
  if(url.pathname==='/app.js')return send(application,'text/javascript; charset=utf-8');
  let requested;
  try{requested=decodeURIComponent(url.pathname);}catch{response.writeHead(400);response.end();return;}
  if(requested==='/')requested='/index.html';
  const file=path.resolve(web,'.'+requested),relative=path.relative(web,file);
  if(relative.startsWith('..')||path.isAbsolute(relative)){response.writeHead(403);response.end();return;}
  fs.readFile(file,(error,body)=>{
    if(error){response.writeHead(404);response.end();return;}
    send(body,types[path.extname(file)]||'application/octet-stream');
  });
});
server.listen(port,'127.0.0.1',()=>{
  console.log(`Frozen preview: http://localhost:${port}/`);
  console.log(`Build: ${policy.buildId}; existing EEPROM outside the resource is preserved.`);
});
