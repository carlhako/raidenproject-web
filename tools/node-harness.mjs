// Runs dist/raiden.js under Node with a minimal host, for headless wasm testing.
// usage: node --stack-size=984 tools/node-harness.mjs <disc.bin> <frames> [script] [outdir]
// env: MODE=1 boots Modified mode; HOOK=file.mjs loads a module whose optional exports
//   init(ctx), pad(port, frame, pad) -> pad (active-high buttons), frame(frame) are called with
//   ctx = { ram: (addr) => DataView-like reader, B (button bits), outdir, shot(name) }.
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
let mod, hook = null, ramOff = 0, spOff = 0, heapNow = null;
const off = (a) => ((a >>> 0) & 0x1ffffc00) === 0x1f800000 ? spOff + (a & 0x3ff) : ramOff + (a & 0x1fffff);
const R = {                                    // little-endian reads of PSX RAM by virtual address
  u8: (a) => heapNow[off(a)],
  u16: (a) => heapNow[off(a)] | heapNow[off(a + 1)] << 8,
  u32: (a) => (R.u16(a) | R.u16(a + 2) << 16) >>> 0,
  s16: (a) => (R.u16(a) << 16) >> 16, s32: (a) => R.u32(a) | 0,
  bytes: (a, n) => heapNow.slice(ramOff + (a & 0x1fffff), ramOff + (a & 0x1fffff) + n),
};
let lastPx = 0, lastW = 0, lastH = 0, lastStride = 0;
function shot(name) {
  const w = lastW, h = lastH, out = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const s = lastPx + (y * lastStride + x) * 4, d = (y * w + x) * 3; out[d] = heapNow[s]; out[d + 1] = heapNow[s + 1]; out[d + 2] = heapNow[s + 2]; }
  fs.writeFileSync(path.join(outdir, name + '.rgb'), Buffer.concat([Buffer.from(`${w} ${h}\n`), out]));
}
globalThis.psx = {
  onInit() {},
  discSectors: () => sectors,
  discRead(heap, lba, dst) { return fs.readSync(fd, heap, dst, 2352, lba * 2352) === 2352 ? 1 : 0; },
  audio(h16, off, n) { audioFrames += n; },
  pad(port) {
    let p = 0; for (const e of ev) if (e.port === port && frame >= e.f && frame < e.f + (+process.env.HOLD || 6)) p |= e.m;
    if (hook && hook.pad) { heapNow = mod.HEAPU8; p = hook.pad(port, frame, p) | 0; }
    if (port !== 0) return ((~p) & 0xffff) | 0x10000;
    if (process.env.RANDOM_INPUT && frame > 1200) { let x = (Math.floor(frame / 8) * 2654435761) >>> 0; x ^= x >>> 15; x = Math.imul(x, 2246822519) >>> 0; p |= x & 0x0000f9f9 & ~1; }
    return ((~p) & 0xffff) | 0x10000;
  },
  frame(heap, px, w, h, stride) {
    frame++; heapNow = heap; lastPx = px; lastW = w; lastH = h; lastStride = stride;
    if (hook && hook.frame) hook.frame(frame);
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
ramOff = mod._psx_ram_ptr(); spOff = mod._psx_scratch_ptr(); heapNow = mod.HEAPU8;
if (process.env.MODE) mod._psx_set_mode(+process.env.MODE);
if (process.env.HOOK) { hook = await import(path.resolve(process.env.HOOK)); if (hook.init) hook.init({ ram: R, B, outdir, shot }); }
mod.callMain([]);
