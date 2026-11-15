// Runs dist/raiden.js under Node with a minimal host, for headless wasm testing.
// usage: node --stack-size=984 tools/node-harness.mjs <disc.bin> <frames> [script] [outdir]
import fs from 'node:fs'; import { createRequire } from 'node:module'; import path from 'node:path';
const require = createRequire(import.meta.url);
const [disc, framesArg, script = '', outdir = 'shots_wasm'] = process.argv.slice(2);
const maxFrames = +framesArg; fs.mkdirSync(outdir, { recursive: true });
const B = { select: 1, start: 8, up: 16, right: 32, down: 64, left: 128, l2: 256, r2: 512, l1: 1024, r1: 2048, triangle: 4096, circle: 8192, cross: 16384, square: 32768 };
const ev = script.split(',').filter(Boolean).map(s => { const [f, n] = s.split(':'); const p2 = n.startsWith('2'); return { f: +f, m: B[p2 ? n.slice(1) : n], port: p2 ? 1 : 0 }; });
const fd = fs.openSync(disc, 'r'); const sectors = Math.floor(fs.fstatSync(fd).size / 2352);
let frame = 0, audioFrames = 0; const hashes = [];
const shotEvery = +(process.env.SHOT_EVERY || 0);
globalThis.self = globalThis;
let mod;
globalThis.psx = {
  onInit() {},
  discSectors: () => sectors,
  discRead(heap, lba, dst) { return fs.readSync(fd, heap, dst, 2352, lba * 2352) === 2352 ? 1 : 0; },
  audio(h16, off, n) { audioFrames += n; },
  pad(port) {
    let p = 0; for (const e of ev) if (e.port === port && frame >= e.f && frame < e.f + (+process.env.HOLD || 6)) p |= e.m;
    if (port !== 0) return ((~p) & 0xffff) | 0x10000;
    if (process.env.RANDOM_INPUT && frame > 1200) { let x = (Math.floor(frame / 8) * 2654435761) >>> 0; x ^= x >>> 15; x = Math.imul(x, 2246822519) >>> 0; p |= x & 0x0000f9f9 & ~1; }
    return ((~p) & 0xffff) | 0x10000;
  },
  frame(heap, px, w, h, stride) {
    frame++;
    if (frame % 100 === 0) { hashes.push(frame + ':' + (mod._psx_state_hash() >>> 0).toString(16)); }
    if (shotEvery && frame % shotEvery === 0) {
      const out = Buffer.alloc(w * h * 3);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const s = px + (y * stride + x) * 4, d = (y * w + x) * 3; out[d] = heap[s]; out[d + 1] = heap[s + 1]; out[d + 2] = heap[s + 2]; }
      fs.writeFileSync(path.join(outdir, `frame_${String(frame).padStart(5, '0')}.rgb`), Buffer.concat([Buffer.from(`${w} ${h}\n`), out]));
    }
    if (frame >= maxFrames) { console.log(hashes.join('\n')); console.log('done', frame, 'frames, audio', audioFrames); process.exit(0); }
  },
  fatal(m) { console.error('FATAL', m); process.exit(2); },
};
fs.mkdirSync('build',{recursive:true}); fs.copyFileSync('dist/raiden.js','build/raiden.cjs'); fs.copyFileSync('dist/raiden.wasm','build/raiden.wasm');
mod = await require('../build/raiden.cjs')({ print: console.log, printErr: console.error });
mod.callMain([]);
