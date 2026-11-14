/* Memory-mapped I/O: interrupt controller, DMA controller, root counters. */
#include "psx.h"

static uint32_t memctrl[16];

/* ---------------------------------------------------------------- DMA */
static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
static uint32_t dpcr = 0x07654321, dicr;
static int mdec_out_pending;

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7f, en = (dicr >> 16) & 0x7f;
    int master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    uint32_t old = dicr & 0x80000000;
    dicr = (dicr & 0x7fffffff) | (master ? 0x80000000 : 0);
    if (master && !old) irq_raise(IRQ_DMA);
}
static void dma_done(int ch) {
    dma_chcr[ch] &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

/* Data moves immediately, but completion (CHCR busy clear + IRQ) is signalled after a
 * transfer time, like the hardware: an IRQ can't fire before the code that started it ends. */
static int64_t dma_due[7] = {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX};
static void dma_resched(void) {
    int64_t t = INT64_MAX;
    for (int i = 0; i < 7; i++) if (dma_due[i] < t) t = dma_due[i];
    if (t == INT64_MAX) sched_clear(EV_DMA); else sched_set(EV_DMA, t);
}
static void dma_finish(int ch, uint32_t words) {
    static const int cost[7] = {1, 1, 1, 12, 4, 1, 1};   /* cycles per word, approx. */
    dma_due[ch] = cycles + 16 + (int64_t)words * cost[ch];
    dma_resched();
}
void dma_event(void) {
    for (int i = 0; i < 7; i++)
        if (dma_due[i] <= cycles) { dma_due[i] = INT64_MAX; dma_done(i); }
    dma_resched();
}

static void dma_gpu(void) {
    uint32_t ch = dma_chcr[2], madr = dma_madr[2] & 0x1ffffc;
    int sync = (ch >> 9) & 3;
    if (sync == 2) {          /* linked list */
        uint32_t addr = madr; int guard = 0;
        for (;;) {
            uint32_t hdr = rd32(addr);
            uint32_t n = hdr >> 24;
            for (uint32_t i = 0; i < n; i++) gpu_gp0(rd32(addr + 4 + 4 * i));
            cycles += n;
            if ((hdr & 0x800000) || ++guard > 200000) break;
            addr = hdr & 0x1ffffc;
        }
        dma_madr[2] = 0xffffff;
        dma_finish(2, 0);
        return;
    } else {                  /* block */
        uint32_t words = (dma_bcr[2] & 0xffff) * (sync == 1 ? (dma_bcr[2] >> 16) : 1);
        if (sync == 0 && !words) words = 0x10000;
        if (ch & 1) for (uint32_t i = 0; i < words; i++) gpu_gp0(rd32(madr + 4 * i));
        else for (uint32_t i = 0; i < words; i++) wr32(madr + 4 * i, gpu_read());
        dma_madr[2] = madr + 4 * words;
        dma_finish(2, words);
    }
}

static void dma_otc(void) {
    uint32_t madr = dma_madr[6] & 0x1ffffc, n = dma_bcr[6] & 0xffff;
    if (!n) n = 0x10000;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = madr - 4 * i;
        wr32(a, i == n - 1 ? 0xffffff : ((a - 4) & 0x1fffff));
    }
    dma_finish(6, n);
}

static uint32_t block_words(int c) {
    int sync = (dma_chcr[c] >> 9) & 3;
    uint32_t w = (dma_bcr[c] & 0xffff) * (sync == 1 ? ((dma_bcr[c] >> 16) ? (dma_bcr[c] >> 16) : 1) : 1);
    if (sync == 0 && !(dma_bcr[c] & 0xffff)) w = 0x10000;
    return w;
}

static void dma_cd(void) {
    uint32_t madr = dma_madr[3] & 0x1ffffc, w = block_words(3);
    for (uint32_t i = 0; i < w; i++) wr32(madr + 4 * i, cd_dma_read_word());
    dma_finish(3, w);
}
static void dma_spu(void) {
    uint32_t madr = dma_madr[4] & 0x1ffffc, w = block_words(4);
    if (dma_chcr[4] & 1) for (uint32_t i = 0; i < w; i++) spu_dma_write(rd32(madr + 4 * i));
    else for (uint32_t i = 0; i < w; i++) wr32(madr + 4 * i, spu_dma_read());
    dma_finish(4, w);
}
static void dma_mdec_in(void) {
    uint32_t madr = dma_madr[0] & 0x1ffffc, w = block_words(0);
    for (uint32_t i = 0; i < w; i++) mdec_dma_in(rd32(madr + 4 * i));
    dma_finish(0, w);
}
static void dma_mdec_out(void) {
    if (!mdec_out_ready()) { mdec_out_pending = 1; return; }
    mdec_out_pending = 0;
    uint32_t madr = dma_madr[1] & 0x1ffffc, w = block_words(1);
    for (uint32_t i = 0; i < w; i++) wr32(madr + 4 * i, mdec_dma_out());
    dma_finish(1, w);
}

static void dma_start(int ch) {
    LOG(3, "DMA%d start madr=%08x bcr=%08x chcr=%08x cyc=%lld\n", ch, dma_madr[ch], dma_bcr[ch], dma_chcr[ch], (long long)cycles);
    switch (ch) {
    case 0: dma_mdec_in(); if (mdec_out_pending) dma_mdec_out(); break;
    case 1: dma_mdec_out(); break;
    case 2: dma_gpu(); break;
    case 3: dma_cd(); break;
    case 4: dma_spu(); break;
    case 6: dma_otc(); break;
    default: dma_finish(ch, 0); break;
    }
}

void dma_write(uint32_t addr, uint32_t v) {
    uint32_t off = addr & 0x7f;
    if (off == 0x70) { dpcr = v; return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7f000000;
        dicr = (dicr & 0xff000000 & ~ack) | (v & 0x00ffffff);
        dicr_update();
        return;
    }
    int ch = off >> 4, r = (off >> 2) & 3;
    if (ch > 6) return;
    if (r == 0) dma_madr[ch] = v & 0xffffff;
    else if (r == 1) dma_bcr[ch] = v;
    else if (r == 2) {
        dma_chcr[ch] = v;
        int sync = (v >> 9) & 3;
        if ((v & 0x01000000) && (sync != 0 || (v & 0x10000000))) dma_start(ch);
    }
}
uint32_t dma_read(uint32_t addr) {
    uint32_t off = addr & 0x7f;
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    int ch = off >> 4, r = (off >> 2) & 3;
    if (ch > 6) return 0;
    if (r == 0) return dma_madr[ch];
    if (r == 1) return dma_bcr[ch];
    if (r == 2) return dma_chcr[ch];
    return 0;
}

/* ---------------------------------------------------------------- root counters */
typedef struct { uint32_t mode, target; int64_t base; uint32_t base_val; int paused; } Timer;
static Timer tmr[3];

static int64_t tmr_rate(int n) {
    uint32_t src = (tmr[n].mode >> 8) & 3;
    if (n == 0 && (src & 1)) return 5;                 /* dot clock (~6.7MHz) approx */
    if (n == 1 && (src & 1)) return HBLANK_CYCLES;
    if (n == 2 && (src & 2)) return 8;
    return 1;
}
static uint32_t tmr_limit(int n) { return (tmr[n].mode & 8) ? (tmr[n].target ? tmr[n].target : 0x10000) : 0x10000; }

static uint32_t tmr_value(int n) {
    if (tmr[n].paused) return tmr[n].base_val;
    int64_t ticks = (cycles - tmr[n].base) / tmr_rate(n);
    uint32_t lim = tmr_limit(n);
    uint64_t v = tmr[n].base_val + (uint64_t)ticks;
    if (lim == 0x10000 || (tmr[n].mode & 8) == 0) return (uint32_t)(v & 0xffff);
    return (uint32_t)(v % (lim + 1));
}

static void tmr_schedule(int n) {
    Timer *t = &tmr[n];
    if (!(t->mode & 0x30) || t->paused) { sched_clear(EV_TIMER0 + n); return; }
    uint32_t cur = tmr_value(n);
    uint32_t goal = (t->mode & 0x10) ? t->target : 0xffff;
    if (goal == 0) goal = 0xffff;
    uint32_t dist = goal > cur ? goal - cur : (tmr_limit(n) + 1 - cur) + goal;
    sched_set(EV_TIMER0 + n, cycles + (int64_t)dist * tmr_rate(n));
}

void timers_event(int n) {
    Timer *t = &tmr[n];
    if (t->mode & 0x10) t->mode |= 0x800; else t->mode |= 0x1000;
    t->mode &= ~0x400u;                        /* IRQ flag active-low */
    irq_raise(IRQ_TMR0 + n);
    if (t->mode & 8) { t->base = cycles; t->base_val = 0; }
    if (t->mode & 0x40) tmr_schedule(n);       /* repeat */
    else sched_clear(EV_TIMER0 + n);
    t->mode |= 0x400;
}

void timers_vblank(void) {
    /* timer1 sync mode 3: paused until first vblank */
    Timer *t = &tmr[1];
    if (t->paused) { t->paused = 0; t->base = cycles; tmr_schedule(1); }
    if ((t->mode & 1) && ((t->mode >> 1) & 3) == 1) { t->base = cycles; t->base_val = 0; }
}

void timers_write(uint32_t addr, uint32_t v) {
    int n = (addr >> 4) & 3, r = (addr >> 2) & 3;
    if (n > 2) return;
    Timer *t = &tmr[n];
    switch (r) {
    case 0: t->base = cycles; t->base_val = v & 0xffff; break;
    case 1:
        t->mode = (v & 0x3ff) | 0x400;
        t->base = cycles; t->base_val = 0;
        t->paused = (n == 1 && (v & 1) && ((v >> 1) & 3) == 3);
        if (n == 2 && (v & 1) && (((v >> 1) & 3) == 0 || ((v >> 1) & 3) == 3)) t->paused = 1;
        break;
    case 2: t->target = v & 0xffff; break;
    }
    tmr_schedule(n);
}
uint32_t timers_read(uint32_t addr) {
    int n = (addr >> 4) & 3, r = (addr >> 2) & 3;
    if (n > 2) return 0;
    switch (r) {
    case 0: return tmr_value(n);
    case 1: { uint32_t m = tmr[n].mode; tmr[n].mode &= ~0x1800u; return m; }
    case 2: return tmr[n].target;
    }
    return 0;
}

/* ---------------------------------------------------------------- bus */
static int warned_io;

uint32_t io_read(uint32_t addr, int size) {
    uint32_t p = addr & 0x1fffffff;
    if (p >= 0x1f801000 && p < 0x1f803000) {
        uint32_t off = p - 0x1f801000;
        if (off < 0x40) return memctrl[off >> 2];
        if (off == 0x60) return 0x00000b88;
        if ((off & ~3u) == 0x70) return i_stat >> ((off & 3) * 8);
        if ((off & ~3u) == 0x74) return i_mask >> ((off & 3) * 8);
        if (off >= 0x40 && off < 0x50) return sio_read(off, size);
        if (off >= 0x50 && off < 0x60) return 0;
        if (off >= 0x80 && off < 0x100) return dma_read(p & ~3u) >> ((p & 3) * 8);
        if (off >= 0x100 && off < 0x130) return timers_read(p & ~3u) >> ((p & 2) * 8);
        if (off >= 0x800 && off < 0x804) {
            if (size == 1) return cd_read_reg(off & 3);
            uint32_t v = 0; for (int i = 0; i < size; i++) v |= (uint32_t)cd_read_reg((off + i) & 3) << (8 * i);
            return v;
        }
        if (off == 0x810) return gpu_read();
        if (off == 0x814) return gpu_stat();
        if (off == 0x820 || off == 0x824) return mdec_read((off >> 2) & 1);
        if (off >= 0xc00 && off < 0xe00) {
            uint32_t v = spu_read(p & ~1u);
            if (size == 4) v |= (uint32_t)spu_read((p & ~1u) + 2) << 16;
            else if (p & 1) v >>= 8;
            return v;
        }
        if (off >= 0x1000) return 0;          /* expansion 2 */
    }
    if (p >= 0x1f000000 && p < 0x1f800000) return 0xffffffff;  /* expansion 1 */
    if (p >= 0x1fc00000 && p < 0x1fc80000) return 0;          /* BIOS ROM */
    if (addr == 0xfffe0130) return 0;
    if (warned_io++ < 50) {
        LOG(1, "io_read%d unmapped %08x (ra=%08x)\n", size * 8, addr, R[31]);
#ifdef TRACE_FUNCS
        { extern uint32_t last_func; LOG(1, "  in function %08x\n", last_func); }
#endif
    }
    return 0;
}

void io_write(uint32_t addr, uint32_t v, int size) {
    uint32_t p = addr & 0x1fffffff;
    if (p >= 0x1f801000 && p < 0x1f803000) {
        uint32_t off = p - 0x1f801000;
        if (off < 0x40) { memctrl[off >> 2] = v; return; }
        if (off == 0x60) return;
        if ((off & ~3u) == 0x70) { uint32_t sh = (off & 3) * 8; i_stat &= (v << sh) | ~((size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu) << sh); return; }
        if ((off & ~3u) == 0x74) { uint32_t sh = (off & 3) * 8, m = (size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu) << sh;
            i_mask = ((i_mask & ~m) | ((v << sh) & m)) & 0x7ff; check_irq(); return; }
        if (off >= 0x40 && off < 0x50) { sio_write(off, v, size); return; }
        if (off >= 0x50 && off < 0x60) return;
        if (off >= 0x80 && off < 0x100) {
            if (size == 4) { dma_write(p, v); return; }
            uint32_t al = p & ~3u, sh = (p & 3) * 8;
            uint32_t mask = (size == 1 ? 0xffu : 0xffffu) << sh;
            uint32_t nv = v << sh;
            if (al == 0x1f8010f4) {
                /* DICR: only the bytes written change; flag bits ack only if written */
                uint32_t cur = dicr & 0x00ffffff;
                uint32_t merged = (cur & ~mask) | (nv & mask & 0x00ffffff);
                merged |= nv & mask & 0x7f000000;          /* acks */
                dma_write(al, merged);
            } else {
                uint32_t cur = dma_read(al);
                dma_write(al, (cur & ~mask) | (nv & mask));
            }
            return;
        }
        if (off >= 0x100 && off < 0x130) { timers_write(p, v); return; }
        if (off >= 0x800 && off < 0x804) {
            for (int i = 0; i < size; i++) cd_write_reg((off + i) & 3, (v >> (8 * i)) & 0xff);
            return;
        }
        if (off == 0x810) { gpu_gp0(v); return; }
        if (off == 0x814) { gpu_gp1(v); return; }
        if (off == 0x820 || off == 0x824) { mdec_write((off >> 2) & 1, v); return; }
        if (off >= 0xc00 && off < 0xe00) {
            if (size == 4) { spu_write(p, v & 0xffff); spu_write(p + 2, v >> 16); }
            else spu_write(p & ~1u, v & 0xffff);
            return;
        }
        if (off >= 0x1000) {
            if (p == 0x1f802041) LOG(2, "POST %x\n", v);
            return;
        }
    }
    if (addr == 0xfffe0130) return;
    if (p >= 0x1f000000 && p < 0x1f800000) return;
    if (warned_io++ < 50) {
        LOG(1, "io_write%d unmapped %08x = %08x (ra=%08x)\n", size * 8, addr, v, R[31]);
#ifdef TRACE_FUNCS
        { extern uint32_t last_func; LOG(1, "  in function %08x\n", last_func); }
#endif
    }
}

void hw_init(void) {
    gpu_init();
    spu_init();
    mdec_init();
    for (int i = 0; i < 3; i++) tmr[i].mode = 0x400;
}

/* Exec(): forget in-flight DMA and stale interrupt flags of the previous program */
void hw_reset_for_exec(void) {
    for (int i = 0; i < 7; i++) { dma_due[i] = INT64_MAX; dma_chcr[i] &= ~0x11000000u; }
    dma_resched();
    dicr &= 0x00ffffff; dicr_update();
    i_stat = 0;
}
