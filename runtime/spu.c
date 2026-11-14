/* SPU emulation: 24 ADPCM voices with ADSR, mixed with CD-XA input, output at 44.1kHz. */
#include "psx.h"

static uint8_t spuram[512 * 1024];
static uint16_t regs[0x200];           /* raw register file (halfwords, offset/2) */
static uint32_t xfer_addr;
static uint32_t endx;

enum { ADSR_OFF, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE };
typedef struct {
    uint32_t addr, repeat;
    uint32_t counter;
    int16_t buf[28];
    int32_t s1, s2;
    int16_t prev_out[4];
    int phase; int32_t level; int adsr_wait;
    int16_t out;      /* last output, for pitch modulation */
    int on;
} Voice;
static Voice v[24];
static uint32_t noise_lfsr = 1; static int noise_timer;

#define R16(off) regs[(off) >> 1]

static void decode_block(Voice *vc) {
    const uint8_t *b = &spuram[vc->addr & 0x7fff0];
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    static const int K0[5] = {0, 60, 115, 98, 122}, K1[5] = {0, 0, -52, -55, -60};
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (vc->s1 * K0[filt] + vc->s2 * K1[filt] + 32) >> 6;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        vc->s2 = vc->s1; vc->s1 = s;
        vc->buf[i] = (int16_t)s;
    }
    if (b[1] & 4) vc->repeat = vc->addr;
}

static void key_on(int n) {
    Voice *vc = &v[n];
    vc->addr = (uint32_t)R16(n * 16 + 6) * 8;
    vc->repeat = (uint32_t)R16(n * 16 + 14) * 8;
    vc->counter = 0; vc->s1 = vc->s2 = 0;
    memset(vc->prev_out, 0, sizeof vc->prev_out);
    vc->phase = ADSR_ATTACK; vc->level = 0; vc->adsr_wait = 0;
    vc->on = 1;
    endx &= ~(1u << n);
    decode_block(vc);
}
static void key_off(int n) { if (v[n].phase != ADSR_OFF) v[n].phase = ADSR_RELEASE; v[n].adsr_wait = 0; }

static void adsr_tick(int n) {
    Voice *vc = &v[n];
    uint16_t lo = R16(n * 16 + 8), hi = R16(n * 16 + 10);
    int exp_mode, dec, shift, step;
    switch (vc->phase) {
    case ADSR_ATTACK: exp_mode = lo >> 15; dec = 0; shift = (lo >> 10) & 31; step = 7 - ((lo >> 8) & 3); break;
    case ADSR_DECAY: exp_mode = 1; dec = 1; shift = ((lo >> 4) & 15) * 4; step = -8; break;
    case ADSR_SUSTAIN:
        exp_mode = hi >> 15; dec = (hi >> 14) & 1; shift = (hi >> 8) & 31;
        step = dec ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3); break;
    case ADSR_RELEASE: exp_mode = (hi >> 5) & 1; dec = 1; shift = (hi & 31) * 4; step = -8; break;
    default: return;
    }
    if (vc->adsr_wait > 0) { vc->adsr_wait--; return; }
    int cyc = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int32_t st = step << (11 - shift > 0 ? 11 - shift : 0);
    if (exp_mode && !dec && vc->level > 0x6000) cyc *= 4;
    if (exp_mode && dec) st = (st * vc->level) >> 15;
    vc->adsr_wait = cyc - 1;
    vc->level += st;
    if (vc->level > 0x7fff) vc->level = 0x7fff;
    if (vc->level < 0) vc->level = 0;
    if (vc->phase == ADSR_ATTACK && vc->level >= 0x7fff) { vc->phase = ADSR_DECAY; vc->adsr_wait = 0; }
    else if (vc->phase == ADSR_DECAY && vc->level <= (int32_t)(((lo & 15) + 1) * 0x800)) { vc->phase = ADSR_SUSTAIN; vc->adsr_wait = 0; }
    else if (vc->phase == ADSR_RELEASE && vc->level == 0) { vc->phase = ADSR_OFF; vc->on = 0; }
}

static inline int32_t vol_of(uint16_t r) {
    if (r & 0x8000) return 0x3fff;           /* sweep mode: approximate as full */
    return (int16_t)(r << 1) >> 1;
}

static int16_t voice_sample(int n) {
    Voice *vc = &v[n];
    if (!vc->on) return 0;
    uint32_t step = R16(n * 16 + 4);
    if (n > 0 && (R16(0x190 + (n >= 16 ? 2 : 0)) & (1u << (n & 15)))) {
        int32_t f = v[n - 1].out + 0x8000;
        step = (uint32_t)(((int32_t)step * f) >> 15) & 0xffff;
    }
    if (step > 0x4000) step = 0x4000;
    int idx = vc->counter >> 12;
    int frac = vc->counter & 0xfff;
    int32_t s0 = vc->buf[idx], s1 = idx + 1 < 28 ? vc->buf[idx + 1] : vc->buf[idx];
    int32_t smp = s0 + (((s1 - s0) * frac) >> 12);
    if (R16(0x194 + (n >= 16 ? 2 : 0)) & (1u << (n & 15))) smp = (int16_t)noise_lfsr;
    vc->counter += step;
    while ((vc->counter >> 12) >= 28) {
        vc->counter -= 28 << 12;
        uint8_t flags = spuram[(vc->addr & 0x7fff0) + 1];
        if (flags & 1) {
            endx |= 1u << n;
            if (flags & 2) vc->addr = vc->repeat;
            else { vc->addr = vc->repeat; vc->phase = ADSR_OFF; vc->level = 0; vc->on = 0; }
        } else vc->addr = (vc->addr + 16) & 0x7ffff;
        decode_block(vc);
    }
    adsr_tick(n);
    int32_t out = (smp * vc->level) >> 15;
    vc->out = (int16_t)out;
    return (int16_t)out;
}

void spu_mix(int16_t *out, int frames) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("PSX_AUDIO_ONLY") ? (getenv("PSX_AUDIO_ONLY")[0] == 'x' ? 1 : 2) : 0;
    uint16_t cnt = R16(0x1aa);
    for (int f = 0; f < frames; f++) {
        /* noise generator (rate from SPUCNT bits 8-13) */
        int nshift = (cnt >> 10) & 15, nstep = ((cnt >> 8) & 3) + 4;
        noise_timer -= nstep;
        if (noise_timer < 0) {
            int bit = ((noise_lfsr >> 15) ^ (noise_lfsr >> 12) ^ (noise_lfsr >> 11) ^ (noise_lfsr >> 10) ^ 1) & 1;
            noise_lfsr = ((noise_lfsr << 1) | bit) & 0xffff;
            noise_timer += 0x20000 >> nshift;
        }
        int32_t L = 0, Rr = 0;
        for (int n = 0; n < 24; n++) {
            int16_t s = voice_sample(n);
            if (!s || dbg == 1) continue;
            L += (s * vol_of(R16(n * 16 + 0))) >> 15;
            Rr += (s * vol_of(R16(n * 16 + 2))) >> 15;
        }
        int16_t cl, cr;
        cd_xa_fetch(&cl, &cr);
        if (dbg == 2) cl = cr = 0;
        if (cnt & 1) {
            L += (cl * (int16_t)R16(0x1b0)) >> 15;
            Rr += (cr * (int16_t)R16(0x1b2)) >> 15;
        }
        if (!(cnt & 0x4000) || !(cnt & 0x8000)) { L = 0; Rr = 0; }
        L = (L * vol_of(R16(0x180))) >> 14;
        Rr = (Rr * vol_of(R16(0x182))) >> 14;
        if (L > 32767) L = 32767; if (L < -32768) L = -32768;
        if (Rr > 32767) Rr = 32767; if (Rr < -32768) Rr = -32768;
        out[2 * f] = (int16_t)L; out[2 * f + 1] = (int16_t)Rr;
    }
}

void spu_init(void) { memset(spuram, 0, sizeof spuram); memset(regs, 0, sizeof regs); }

uint16_t spu_read(uint32_t addr) {
    uint32_t off = addr & 0x3fe;
    if (off < 0x180 && (off & 15) == 12) return (uint16_t)v[off >> 4].level;
    switch (off) {
    case 0x19c: return endx & 0xffff;
    case 0x19e: return endx >> 16;
    case 0x1a8: { uint16_t h; memcpy(&h, &spuram[xfer_addr & 0x7fffe], 2); xfer_addr += 2; return h; }
    case 0x1ae: {
        uint16_t c = R16(0x1aa);
        return (c & 0x3f) | ((c & 0x20) << 2);
    }
    }
    return R16(off);
}

void spu_write(uint32_t addr, uint16_t val) {
    uint32_t off = addr & 0x3fe;
    R16(off) = val;
    switch (off) {
    case 0x188: for (int i = 0; i < 16; i++) if (val & (1u << i)) key_on(i); break;
    case 0x18a: for (int i = 0; i < 8; i++) if (val & (1u << i)) key_on(16 + i); break;
    case 0x18c: for (int i = 0; i < 16; i++) if (val & (1u << i)) key_off(i); break;
    case 0x18e: for (int i = 0; i < 8; i++) if (val & (1u << i)) key_off(16 + i); break;
    case 0x19c: case 0x19e: break;
    case 0x1a6: xfer_addr = (uint32_t)val * 8; break;
    case 0x1a8: memcpy(&spuram[xfer_addr & 0x7fffe], &val, 2); xfer_addr = (xfer_addr + 2) & 0x7ffff; break;
    default:
        if (off < 0x180 && (off & 15) == 14) v[off >> 4].repeat = (uint32_t)val * 8;
        break;
    }
}

void spu_dma_write(uint32_t w) {
    memcpy(&spuram[xfer_addr & 0x7fffc], &w, 4);
    xfer_addr = (xfer_addr + 4) & 0x7ffff;
}
uint32_t spu_dma_read(void) {
    uint32_t w; memcpy(&w, &spuram[xfer_addr & 0x7fffc], 4);
    xfer_addr = (xfer_addr + 4) & 0x7ffff;
    return w;
}
