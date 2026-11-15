// Pulls stereo Int16 frames from the ring buffer the game worker fills.
class PsxAudio extends AudioWorkletProcessor {
  constructor(o) {
    super();
    this.ctrl = new Int32Array(o.processorOptions.ctrl);
    this.ring = new Int16Array(o.processorOptions.audio);
    this.primed = false;
  }
  process(_in, outs) {
    const [l, r] = outs[0], n = l.length, ctrl = this.ctrl;
    const rd = Atomics.load(ctrl, 5), avail = Atomics.load(ctrl, 4) - rd;
    if (!this.primed && avail >= 2048) this.primed = true;
    if (!this.primed || avail < n) { if (avail < n) this.primed = false; return true; }
    for (let i = 0; i < n; i++) {
      const o = ((rd + i) & 32767) * 2;
      l[i] = this.ring[o] / 32768; r[i] = this.ring[o + 1] / 32768;
    }
    Atomics.store(ctrl, 5, rd + n);
    return true;
  }
}
registerProcessor('psx-audio', PsxAudio);
