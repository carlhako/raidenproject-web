// Runs the wasm game on its own thread. The game never yields, so everything the page must do
// for it (video, audio, input, netplay) goes through shared memory; sends go out via postMessage.
importScripts('raiden.js');

const SEQ = 0, W = 1, H = 2, FRAME = 3, AUD_W = 4, AUD_R = 5, PAD0 = 6, LOGN = 8, WAKE = 9, SLEEP = 10, ROLE = 11, LOCK = 12, P2FROM = 13, DELAY = 14, CATCH = 15;
const AUD_CAP = 32768, AUD_MAX = 8820;      // frames; drop new audio when more than ~200 ms is queued
const ROLE_HOST = 0, ROLE_P2 = 2;
const START_BIT = 8;                         // active-low Start in the pad word
const FRAME_MS = 1000 / 60;                  // NTSC

self.onmessage = async (e) => {
  if (e.data.type !== 'start') return;
  const { ctrl: ctrlSab, fb: fbSab, audio: audSab, log: logSab } = e.data;
  const ctrl = new Int32Array(ctrlSab), fb = new Uint8Array(fbSab), aud = new Int16Array(audSab), log = new Uint32Array(logSab);
  const fatal = (m) => { postMessage({ type: 'fatal', msg: m }); };
  // Firefox's err.stack has no "Name: message" line, so put it in front ourselves.
  const describe = (err) => {
    if (!err || typeof err !== 'object') return String(err);
    const head = (err.name || 'Error') + ': ' + (err.message || String(err)), st = err.stack || '';
    return st.startsWith(head) ? st : head + '\n' + st;
  };

  // ---- disc: OPFS file read synchronously, with 32-sector read-ahead
  let sync, sectors = 0;
  try {
    const root = await navigator.storage.getDirectory();
    const fh = await root.getFileHandle('disc.img');
    try { sync = await fh.createSyncAccessHandle({ mode: 'read-only' }); } catch { sync = await fh.createSyncAccessHandle(); }
    sectors = Math.floor(sync.getSize() / 2352);
  } catch (err) { return fatal('Cannot open the disc image: ' + err); }
  const CH = 32, cache = new Uint8Array(CH * 2352); let cacheLba = -1, cacheN = 0;

  // Per frame the game needs (p1, p2) pad words. The host plays alone from its own pad until the lock frame;
  // after that, and for everybody else (watchers, player 2), they come from the confirmed input log that the
  // page fills from the server. Watching = replaying that log from frame 0 (the game is deterministic).
  let cur = 0, began = -1, t0 = 0, rebuf = true, cp1 = 0xffff, cp2 = 0xffff, catching = false;
  const role = () => Atomics.load(ctrl, ROLE);
  const logn = () => Atomics.load(ctrl, LOGN);

  function waitLog(need) {                     // block until log[cur] exists (+ jitter buffer when watching live)
    for (;;) {
      const w = Atomics.load(ctrl, WAKE), n = logn();
      if (n > cur && n >= cur + need) return;
      Atomics.wait(ctrl, WAKE, w, 100);
    }
  }
  function beginFrame() {
    if (began === cur) return; began = cur;
    const r = role(), lock = Atomics.load(ctrl, LOCK), delay = Atomics.load(ctrl, DELAY), local = Atomics.load(ctrl, PAD0) & 0xffff;
    if (r === ROLE_HOST) {
      if (lock < 0 || cur < lock) { cp1 = local; cp2 = 0xffff; postMessage({ type: 'input', frame: cur, b: local }); }
      else { waitLog(1); const v = log[cur]; cp1 = v & 0xffff; cp2 = v >>> 16; }
      if (lock >= 0 && cur + delay >= lock) postMessage({ type: 'input', frame: cur + delay, b: local });
    } else {
      const lag = logn() - cur;
      if (rebuf || lag <= 0) { waitLog(r === ROLE_P2 ? 1 : 6); rebuf = false; }
      const v = log[cur]; cp1 = v & 0xffff; cp2 = v >>> 16;
      if (r === ROLE_P2) {
        const f2 = Atomics.load(ctrl, P2FROM), T = cur + delay;
        if (T >= f2) {
          let b = local;
          if (T >= f2 + 4 && T < f2 + 14) b &= ~START_BIT & 0xffff;       // the Start press that asked to join, so the game adds P2
          postMessage({ type: 'input', frame: T, b });
        }
      }
    }
  }

  let mod, lastCard = '';
  function saveCard() {                       // push changed memory-card files to the page for storage
    try {
      const FS = mod.FS, files = {}; let sig = '';
      for (const n of FS.readdir('/memcard/bu00')) {
        if (n[0] === '.') continue;
        const d = FS.readFile('/memcard/bu00/' + n); let b = ''; for (let i = 0; i < d.length; i += 8192) b += String.fromCharCode.apply(null, d.subarray(i, i + 8192));
        files[n] = btoa(b); sig += n + files[n].length + files[n].slice(-16);
      }
      if (sig !== lastCard) { lastCard = sig; postMessage({ type: 'memcard', files }); }
    } catch (e) { /* no card yet */ }
  }
  self.psx = {
    onInit() { t0 = performance.now(); },
    discSectors: () => sectors,
    discRead(heap, lba, dst) {
      if (lba < cacheLba || lba >= cacheLba + cacheN) {
        cacheN = Math.min(CH, sectors - lba);
        if (sync.read(cache.subarray(0, cacheN * 2352), { at: lba * 2352 }) !== cacheN * 2352) { cacheLba = -1; cacheN = 0; return 0; }
        cacheLba = lba;
      }
      heap.set(cache.subarray((lba - cacheLba) * 2352, (lba - cacheLba + 1) * 2352), dst);
      return 1;
    },
    audio(h16, off, n) {
      const w = Atomics.load(ctrl, AUD_W), r = Atomics.load(ctrl, AUD_R);
      if (w - r > AUD_MAX || catching) return;
      for (let i = 0; i < n; i++) { const o = ((w + i) & (AUD_CAP - 1)) * 2; aud[o] = h16[off + 2 * i]; aud[o + 1] = h16[off + 2 * i + 1]; }
      Atomics.store(ctrl, AUD_W, w + n);
    },
    pad(p) {
      if (p === 0) beginFrame();
      return (p === 0 ? cp1 : cp2) | 0x10000;
    },
    frame(heap, px, w, h, stride) {
      Atomics.add(ctrl, SEQ, 1);                       // odd = being written
      for (let y = 0; y < h; y++) fb.set(heap.subarray(px + y * stride * 4, px + y * stride * 4 + w * 4), y * 2560);
      Atomics.store(ctrl, W, w); Atomics.store(ctrl, H, h);
      Atomics.add(ctrl, SEQ, 1);
      cur++;
      Atomics.store(ctrl, FRAME, cur);
      if (cur % 120 === 0) postMessage({ type: 'hash', frame: cur, h: mod._psx_state_hash() >>> 0 });
      if (role() === ROLE_HOST && cur % 300 === 0) saveCard();
      const r = role(), lag = logn() - cur;
      // watchers run flat out while far behind the live input, muted; then fall back to real time
      if (r !== ROLE_HOST && (catching ? lag > 8 : lag > 20)) {
        if (!catching) { catching = true; Atomics.store(ctrl, CATCH, 1); }
        t0 = 0; return;
      }
      if (catching) { catching = false; Atomics.store(ctrl, CATCH, 0); Atomics.store(ctrl, AUD_R, Atomics.load(ctrl, AUD_W)); }
      const now = performance.now();
      if (!t0) t0 = now - cur * FRAME_MS;
      const due = t0 + cur * FRAME_MS;
      if (now < due) Atomics.wait(ctrl, SLEEP, 0, due - now);
      else if (now - due > 100) t0 = now - cur * FRAME_MS;   // fell behind: resync
    },
    fatal,
  };

  try { mod = await createPsx({
    print: (s) => postMessage({ type: 'log', msg: s }),
    printErr: (s) => postMessage({ type: 'log', msg: s }),
    preRun: [(m) => {
      m.FS.mkdir('/memcard'); m.FS.mkdir('/memcard/bu00');
      for (const [name, data] of Object.entries(e.data.memcard || {})) m.FS.writeFile('/memcard/bu00/' + name, data);
    }],
  }); } catch (err) { return fatal('Cannot start the game: ' + describe(err)); }
  self.psx.mod = mod;
  postMessage({ type: 'started' });
  try { mod.callMain([]); } catch (err) { fatal(describe(err)); }
};
