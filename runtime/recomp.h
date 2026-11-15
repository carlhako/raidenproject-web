/* Shared runtime interface for the statically recompiled PSX code. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef void (*FuncPtr)(void);
typedef struct { uint32_t addr; FuncPtr fn; } FuncEntry;

/* One statically recompiled PS-X EXE (the game loads several from disc). */
typedef struct { const char *name; uint32_t entry, base, size, crc; const FuncEntry *table; int count; } Program;
extern const Program prog_scus, prog_jj2h, prog_jj2v, prog_jj3;
extern const Program *cur_prog;
extern unsigned exec_epoch;   /* bumped by every Exec(): native frames of the previous program are gone */

/* ---- CPU state ---- */
extern uint32_t R[32];
extern uint32_t HI, LO, PC;

/* ---- memory ---- */
#define RAM_SIZE 0x200000
extern uint8_t ram[RAM_SIZE];
extern uint8_t scratch[0x400];

uint32_t io_read(uint32_t addr, int size);
void io_write(uint32_t addr, uint32_t val, int size);

static inline uint32_t rd32(uint32_t a) {
    uint32_t p = a & 0x1fffffff;
    if (p < 0x800000) { uint32_t v; memcpy(&v, ram + (p & 0x1ffffc), 4); return v; }
    if ((p & 0xfffffc00) == 0x1f800000) { uint32_t v; memcpy(&v, scratch + (p & 0x3fc), 4); return v; }
    return io_read(a, 4);
}
static inline uint32_t rd16(uint32_t a) {
    uint32_t p = a & 0x1fffffff;
    if (p < 0x800000) { uint16_t v; memcpy(&v, ram + (p & 0x1ffffe), 2); return v; }
    if ((p & 0xfffffc00) == 0x1f800000) { uint16_t v; memcpy(&v, scratch + (p & 0x3fe), 2); return v; }
    return io_read(a, 2) & 0xffff;
}
static inline uint32_t rd8(uint32_t a) {
    uint32_t p = a & 0x1fffffff;
    if (p < 0x800000) return ram[p & 0x1fffff];
    if ((p & 0xfffffc00) == 0x1f800000) return scratch[p & 0x3ff];
    return io_read(a, 1) & 0xff;
}
extern uint32_t watch_phys;
void watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(p, a, v, sz) do { if (__builtin_expect(((p) & 0x1ffffc) == watch_phys, 0)) watch_hit(a, v, sz); } while (0)
static inline void wr32(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffff;
    WATCH(p, a, v, 4);
    if (p < 0x800000) { memcpy(ram + (p & 0x1ffffc), &v, 4); return; }
    if ((p & 0xfffffc00) == 0x1f800000) { memcpy(scratch + (p & 0x3fc), &v, 4); return; }
    io_write(a, v, 4);
}
static inline void wr16(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffff;
    uint16_t h = (uint16_t)v;
    WATCH(p, a, v, 2);
    if (p < 0x800000) { memcpy(ram + (p & 0x1ffffe), &h, 2); return; }
    if ((p & 0xfffffc00) == 0x1f800000) { memcpy(scratch + (p & 0x3fe), &h, 2); return; }
    io_write(a, v & 0xffff, 2);
}
static inline void wr8(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffff;
    WATCH(p, a, v, 1);
    if (p < 0x800000) { ram[p & 0x1fffff] = (uint8_t)v; return; }
    if ((p & 0xfffffc00) == 0x1f800000) { scratch[p & 0x3ff] = (uint8_t)v; return; }
    io_write(a, v & 0xff, 1);
}
/* direct RAM pointer for native helpers (BIOS/DMA) */
static inline uint8_t *ram_ptr(uint32_t a) { return ram + (a & 0x1fffff); }

/* ---- timing / events ---- */
extern int64_t cycles;       /* virtual CPU clock (33.8688 MHz) */
extern int64_t next_event;
void run_events(void);
#ifndef CPI
#define CPI 2
#endif
#define CYC(n) (cycles += (n) * CPI)
#ifdef TRACE_FUNCS
void trace_entry(uint32_t addr);
#define FENTRY(a) trace_entry(a)
#else
#define FENTRY(a) ((void)0)
#endif
#ifdef CHECK_SP
void sp_mismatch(uint32_t callee, uint32_t site, uint32_t before, uint32_t after);
#define CALL_CHECK(fn, callee, site) do { uint32_t _sp = R[29]; fn(); if (R[29] != _sp) sp_mismatch(callee, site, _sp, R[29]); } while (0)
#define CALL_CHECK_IND(t, site) do { uint32_t _sp = R[29]; call_addr(t); if (R[29] != _sp) sp_mismatch(t, site, _sp, R[29]); } while (0)
#else
#define CALL_CHECK(fn, callee, site) fn()
#define CALL_CHECK_IND(t, site) call_addr(t)
#endif
#define CHECK_EVENTS() do { if (cycles >= next_event) run_events(); } while (0)

/* ---- tail calls: generated code relies on these not growing the stack ---- */
#if defined(__clang__)
#define TAILCALL(e) do { __attribute__((musttail)) return e; } while (0)
#else
#define TAILCALL(e) do { e; return; } while (0)
#endif

/* ---- helpers called by generated code ---- */
void call_addr(uint32_t addr);
void jump_addr(uint32_t addr);
/* `jr rs` that is not a return/switch: a tail call, or a call with a hand-made $ra (the callee
 * returns to $ra, which can be an address inside the calling function). */
#define JR_CALL(t) do { call_addr(t); if (R[31] != ra_in) call_addr(R[31]); return; } while (0)   /* R[31] is where the callee returned to */
void op_syscall(uint32_t pc);
void op_break(uint32_t pc, uint32_t code);
void op_unimpl(uint32_t pc, uint32_t word);
void op_badjump(uint32_t addr);
uint32_t cop0_read(int r);
void cop0_write(int r, uint32_t v);
void cop0_rfe(void);

static inline void op_div(uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { HI = a; LO = n >= 0 ? 0xffffffffu : 1u; }
    else if (n == INT32_MIN && d == -1) { HI = 0; LO = 0x80000000u; }
    else { LO = (uint32_t)(n / d); HI = (uint32_t)(n % d); }
}
static inline void op_divu(uint32_t a, uint32_t b) {
    if (b == 0) { HI = a; LO = 0xffffffffu; }
    else { LO = a / b; HI = a % b; }
}
static inline uint32_t op_lwl(uint32_t a, uint32_t old) {
    uint32_t w = rd32(a & ~3u); int sh = a & 3;
    switch (sh) {
    case 0: return (old & 0x00ffffff) | (w << 24);
    case 1: return (old & 0x0000ffff) | (w << 16);
    case 2: return (old & 0x000000ff) | (w << 8);
    default: return w;
    }
}
static inline uint32_t op_lwr(uint32_t a, uint32_t old) {
    uint32_t w = rd32(a & ~3u); int sh = a & 3;
    switch (sh) {
    case 0: return w;
    case 1: return (old & 0xff000000) | (w >> 8);
    case 2: return (old & 0xffff0000) | (w >> 16);
    default: return (old & 0xffffff00) | (w >> 24);
    }
}
static inline void op_swl(uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = rd32(al); int sh = a & 3;
    switch (sh) {
    case 0: w = (w & 0xffffff00) | (v >> 24); break;
    case 1: w = (w & 0xffff0000) | (v >> 16); break;
    case 2: w = (w & 0xff000000) | (v >> 8); break;
    default: w = v; break;
    }
    wr32(al, w);
}
static inline void op_swr(uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = rd32(al); int sh = a & 3;
    switch (sh) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000ff) | (v << 8); break;
    case 2: w = (w & 0x0000ffff) | (v << 16); break;
    default: w = (w & 0x00ffffff) | (v << 24); break;
    }
    wr32(al, w);
}

/* ---- GTE ---- */
uint32_t gte_read_data(int r);
void gte_write_data(int r, uint32_t v);
uint32_t gte_read_ctrl(int r);
void gte_write_ctrl(int r, uint32_t v);
void gte_command(uint32_t cmd);
