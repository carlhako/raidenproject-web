/* CPU state, dispatcher, cop0, scheduler and program start-up. */
#include "psx.h"
#include <stdarg.h>
#include <signal.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#include <execinfo.h>
#else
extern void js_fatal(const char *msg);
#endif
#include <unistd.h>

uint32_t R[32];
uint32_t HI, LO, PC;
uint8_t ram[RAM_SIZE] __attribute__((aligned(64)));
uint8_t scratch[0x400] __attribute__((aligned(64)));
int64_t cycles;
int64_t next_event;
int log_level = 1;
const char *data_dir = ".";
uint32_t watch_phys = 0xffffffff;
#ifdef TRACE_FUNCS
static uint32_t trace_addr = 0xffffffff; static int trace_init;
uint32_t last_func;
static uint32_t ring[128], ring_sp[128], ring_ra[128]; static unsigned ring_n;
void trace_entry(uint32_t addr) {
    last_func = addr; ring_sp[ring_n & 127] = R[29]; ring_ra[ring_n & 127] = R[31]; ring[ring_n++ & 127] = addr;
    if (!trace_init) { trace_init = 1; if (getenv("PSX_TRACE")) trace_addr = (uint32_t)strtoul(getenv("PSX_TRACE"), NULL, 16); }
    if (addr != trace_addr) return;
    fprintf(stderr, "TRACE %08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x cyc=%lld\n", addr, R[4], R[5], R[6], R[7], R[31], (long long)cycles);
    if (getenv("PSX_TRACE_RAM")) {
        FILE *f = fopen("shots/ram_trace.bin", "wb");
        if (f) { fwrite(ram, 1, RAM_SIZE, f); fwrite(R, 4, 32, f); fwrite(scratch, 1, 0x400, f); fclose(f); }
        fprintf(stderr, "dumped RAM+regs at trace point\n");
        exit(0);
    }
    if (getenv("PSX_TRACE_DUMP")) {
        uint32_t base = (uint32_t)strtoul(getenv("PSX_TRACE_DUMP"), NULL, 16);
        uint32_t ptr = base < 32 ? R[base] : base;
        fprintf(stderr, "  [%08x]:", ptr);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", rd32(ptr + 4 * i));
        fprintf(stderr, "\n");
    }
}
#endif
void watch_hit(uint32_t a, uint32_t v, int size) {
    fprintf(stderr, "WATCH: wr%d [%08x] = %08x (ra=%08x, PC~, cycles=%lld)\n", size * 8, a, v, R[31], (long long)cycles);
    if (getenv("PSX_WATCH_FATAL")) fatal("watch");
}

uint32_t cop0_sr = 0x40000401 | 0x400, cop0_cause, cop0_epc;

void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n  regs: ");
    for (int i = 0; i < 32; i++) fprintf(stderr, "r%d=%08x%s", i, R[i], (i % 8 == 7) ? "\n        " : " ");
    fprintf(stderr, "\n");
    if (getenv("PSX_DUMP_ON_FATAL")) {
        FILE *f = fopen("shots/ram_fatal.bin", "wb");
        if (f) { fwrite(ram, 1, RAM_SIZE, f); fwrite(R, 4, 32, f); fwrite(scratch, 1, 0x400, f); fclose(f); fprintf(stderr, "dumped RAM+regs to shots/ram_fatal.bin\n"); }
    }
#ifdef TRACE_FUNCS
    fprintf(stderr, "  last functions entered (oldest first):");
    for (unsigned i = 0; i < 128 && i < ring_n; i++) { unsigned k = (ring_n - (ring_n < 128 ? ring_n : 128) + i) & 127; fprintf(stderr, " %08x@%x<%x", ring[k], ring_sp[k] & 0xfffff, ring_ra[k] & 0xfffff); }
    fprintf(stderr, "\n");
#endif
#ifdef __EMSCRIPTEN__
    { char m[256]; va_list ap2; va_start(ap2, fmt); vsnprintf(m, sizeof m, fmt, ap2); va_end(ap2); js_fatal(m); }
    __builtin_trap();
#else
    void *bt[64]; int n = backtrace(bt, 64); backtrace_symbols_fd(bt, n, 2);
    fflush(stderr);
    _exit(1);
#endif
}

/* ---------------------------------------------------------------- dispatcher */
void interp_run(uint32_t pc);
FuncPtr func_lookup(uint32_t addr);
const Program *cur_prog;
static const Program *const programs[] = {&prog_scus, &prog_jj2h, &prog_jj2v, &prog_jj3};
static FuncPtr lookup(uint32_t addr) {
    const FuncEntry *func_table = cur_prog->table;
    int lo = 0, hi = cur_prog->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t a = func_table[mid].addr;
        if (a == addr) return func_table[mid].fn;
        if (a < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

FuncPtr func_lookup(uint32_t addr) {
    FuncPtr f = lookup(addr | 0x80000000);
    return f ? f : lookup(addr);
}

static void interp_fallback(uint32_t addr, const char *why) {
    uint32_t p = addr & 0x1fffffff;
    if (p < 0x10000 || p >= RAM_SIZE) fatal("%s to invalid address %08x (ra=%08x)", why, addr, R[31]);
    static uint32_t seen[512]; static int nseen; static const Program *seen_prog;
    if (seen_prog != cur_prog) { seen_prog = cur_prog; nseen = 0; }
    int known = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == addr) known = 1;
    if (!known) {
        if (nseen < 512) seen[nseen++] = addr;
        LOG(1, "note: [%s] %s %08x not recompiled, interpreting (add to analysis/extra_%s.txt)\n", cur_prog->name, why, addr, cur_prog->name);
    }
    interp_run(addr);
}

void call_addr(uint32_t addr) {
    uint32_t p = addr & 0x1fffffff;
    if (p == 0xa0 || p == 0xb0 || p == 0xc0) { bios_call(p, R[9] & 0xff); return; }
    FuncPtr f = func_lookup(addr);
    static int force = -1;
    /* self-tests of the interpreter: INDIRECT routes indirect calls through it, ALL runs the whole
     * game in it (the reference for checking the recompiler; very slow) */
    if (force < 0) force = getenv("PSX_INTERP_ALL") ? 2 : getenv("PSX_INTERP_INDIRECT") != NULL;
    if (!f || force) { if (f) interp_run(addr); else interp_fallback(addr, "call"); return; }
    f();
}
void jump_addr(uint32_t addr) { call_addr(addr); }

void op_badjump(uint32_t addr) { interp_fallback(addr, "jump"); }
void op_unimpl(uint32_t pc, uint32_t word) { fatal("unimplemented instruction %08x at %08x", word, pc); }
void op_break(uint32_t pc, uint32_t code) {
    LOG(1, "break %u at %08x (ra=%08x)\n", code, pc, R[31]);
    if (code != 6 && code != 7) fatal("break %u at %08x", code, pc);
}

void op_syscall(uint32_t pc) {
    (void)pc;
    switch (R[4]) {
    case 0: break;
    case 1: /* EnterCriticalSection */
        R[2] = (cop0_sr & 0x401) == 0x401;
        cop0_sr &= ~0x401u;
        break;
    case 2: /* ExitCriticalSection */
        cop0_sr |= 0x401;
        check_irq();
        break;
    default:
        LOG(1, "syscall(%u) at %08x ignored\n", R[4], pc);
    }
}

uint32_t cop0_read(int r) {
    switch (r) {
    case 12: return cop0_sr;
    case 13: return cop0_cause;
    case 14: return cop0_epc;
    case 15: return 0x00000002; /* PRId */
    default: return 0;
    }
}
void cop0_write(int r, uint32_t v) {
    if (r == 12) { cop0_sr = v; check_irq(); }
    else if (r == 13) cop0_cause = (cop0_cause & ~0x300u) | (v & 0x300);
}
void cop0_rfe(void) { cop0_sr = (cop0_sr & ~0x0fu) | ((cop0_sr >> 2) & 0x0f); }

/* ---------------------------------------------------------------- scheduler */
static int64_t ev_time[EV_COUNT];

static void recompute_next(void) {
    int64_t n = INT64_MAX;
    for (int i = 0; i < EV_COUNT; i++) if (ev_time[i] < n) n = ev_time[i];
    next_event = n;
}
void sched_set(int ev, int64_t when) { ev_time[ev] = when; if (when < next_event) next_event = when; }
void sched_clear(int ev) { ev_time[ev] = INT64_MAX; recompute_next(); }
int64_t sched_get(int ev) { return ev_time[ev]; }

static int in_events;
void run_events(void) {
    if (in_events) return;
    in_events = 1;
    for (;;) {
        int best = -1; int64_t bt = INT64_MAX;
        for (int i = 0; i < EV_COUNT; i++) if (ev_time[i] < bt) { bt = ev_time[i]; best = i; }
        if (best < 0 || bt > cycles) break;
        ev_time[best] = INT64_MAX;
        switch (best) {
        case EV_VBLANK:
            sched_set(EV_VBLANK, bt + FRAME_CYCLES);
            timers_vblank();
            gpu_vblank();
            bios_vblank();
            irq_raise(IRQ_VBLANK);
#ifdef TRACE_FUNCS
            { extern uint32_t last_func; LOG(5, "vblank last_func=%08x ra=%08x sp=%08x\n", last_func, R[31], R[29]); }
#else
            LOG(5, "vblank PC=%08x ra=%08x sp=%08x\n", PC, R[31], R[29]);
#endif
            platform_frame();
            break;
        case EV_CDROM: cd_event(); break;
        case EV_CDREAD: cd_read_event(); break;
        case EV_SIO: sio_event(); break;
        case EV_DMA: dma_event(); break;
        case EV_TIMER0: timers_event(0); break;
        case EV_TIMER1: timers_event(1); break;
        case EV_TIMER2: timers_event(2); break;
        case EV_AUDIO: {
            /* mix one chunk of audio (735 frames ~ 1/60 s at 44.1kHz -> use 1/100 s chunks) */
            static int16_t buf[441 * 2];
            spu_mix(buf, 441);
            platform_audio(buf, 441);
            sched_set(EV_AUDIO, bt + CPU_HZ / 100);
            break;
        }
        default: break;
        }
    }
    recompute_next();
    in_events = 0;
    check_irq();
}

/* ---------------------------------------------------------------- interrupts */
uint32_t i_stat, i_mask;
void irq_raise(int n) { i_stat |= 1u << n; if (i_mask & (1u << n)) next_event = cycles; /* deliver at next check */ }

static int in_exception;
void check_irq(void) {
    if (in_exception) return;
    if ((cop0_sr & 0x401) != 0x401) return;
    if (!(i_stat & i_mask)) return;
    in_exception = 1;
    bios_exception();
    in_exception = 0;
}

/* ---------------------------------------------------------------- start-up */
static char *exe_path;
void bios_boot_raiden2(void);
#include <setjmp.h>
static jmp_buf exec_jb;
static int exec_pending;
unsigned exec_epoch;
static uint32_t exec_sp, exec_gp;
void bios_reset_for_exec(void);
void cd_reset_for_exec(void);
void hw_reset_for_exec(void);

static const Program *find_program(uint32_t pc, uint32_t crc) {
    for (unsigned i = 0; i < sizeof programs / sizeof programs[0]; i++)
        if (programs[i]->entry == pc && programs[i]->crc == crc) return programs[i];
    return NULL;
}
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xffffffffu;
    while (n--) c = t[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

/* HLE of Exec(): the loaded image is already in RAM; switch to its recompiled twin and restart. */
void exec_program(uint32_t pc, uint32_t gp, uint32_t base, uint32_t size, uint32_t sp) {
    const Program *p = find_program(pc, crc32_buf(ram_ptr(base), size));
    if (!p) fatal("Exec: no recompiled program for entry %08x base %08x size %x", pc, base, size);
    LOG(1, "Exec -> %s (entry %08x) at frame ~%lld\n", p->name, pc, (long long)(cycles / FRAME_CYCLES));
    cur_prog = p;
    exec_gp = gp; exec_sp = sp;
    exec_pending = 1; exec_epoch++;
    longjmp(exec_jb, 1);
}

static void load_exe(void) {
    FILE *f = fopen(exe_path, "rb");
    if (!f) fatal("cannot open %s (run `make` to extract it from the disc image)", exe_path);
    uint8_t hdr[0x800];
    if (fread(hdr, 1, 0x800, f) != 0x800) fatal("short exe");
    uint32_t pc, gp, addr, size, sp;
    memcpy(&pc, hdr + 0x10, 4); memcpy(&gp, hdr + 0x14, 4);
    memcpy(&addr, hdr + 0x18, 4); memcpy(&size, hdr + 0x1c, 4); memcpy(&sp, hdr + 0x30, 4);
    if (fread(ram_ptr(addr), 1, size, f) != size) fatal("short exe body");
    fclose(f);
    R[28] = gp;
    R[29] = R[30] = sp ? sp : 0x801ffff0;
    cur_prog = find_program(pc, crc32_buf(ram_ptr(addr), size));
    if (!cur_prog) fatal("%s does not match the recompiled boot executable (wrong disc/version?)", exe_path);
}

static void segv(int sig) { fatal("signal %d", sig); }

static void *game_thread(void *arg) {
    (void)arg;
    platform_init();
    hw_init();
    bios_init();
    load_exe();
    for (int i = 0; i < EV_COUNT; i++) ev_time[i] = INT64_MAX;
    sched_set(EV_VBLANK, FRAME_CYCLES);
    sched_set(EV_AUDIO, CPU_HZ / 100);
    recompute_next();
    setjmp(exec_jb);
    if (exec_pending) {
        exec_pending = 0; in_events = 0; in_exception = 0;
        cd_reset_for_exec(); hw_reset_for_exec();
        bios_reset_for_exec();
        R[28] = exec_gp; R[29] = R[30] = exec_sp ? exec_sp : 0x801fff00;
        R[31] = 0;
    }
    LOG(1, "starting recompiled program %s at %08x\n", cur_prog->name, cur_prog->entry);
    call_addr(cur_prog->entry);
    LOG(1, "game returned from main\n");
    exit(0);
}

#ifdef __EMSCRIPTEN__
extern void bios_boot_raiden2(void);
int main(void) {
    exe_path = (char *)"/extracted/PSJJ3.EXE";
    bios_boot_raiden2();
    cd_init(NULL);
    game_thread(NULL);
    return 0;
}
#else
int main(int argc, char **argv) {
    const char *bin = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) log_level++;
        else if (!strcmp(argv[i], "-q")) log_level = 0;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch_phys = (uint32_t)strtoul(argv[++i], NULL, 16) & 0x1ffffc;
        else bin = argv[i];
    }
    static char pbuf[1024], ebuf[1024], dbuf[1024];
    if (!strcmp(data_dir, ".")) {
        /* default: the directory containing the executable */
        ssize_t n = readlink("/proc/self/exe", dbuf, sizeof dbuf - 1);
        if (n > 0) { dbuf[n] = 0; char *sl = strrchr(dbuf, '/'); if (sl) { *sl = 0; data_dir = dbuf; } }
    }
    if (!bin) { snprintf(pbuf, sizeof pbuf, "%s/disc/Raiden Project, The (USA).bin", data_dir); bin = pbuf; }
    snprintf(ebuf, sizeof ebuf, "%s/extracted/SCUS_944.02", data_dir);
    exe_path = ebuf;
    if (getenv("PSX_MOD")) mod_mode = atoi(getenv("PSX_MOD"));
    if (getenv("PSX_RAIDEN2")) { snprintf(ebuf, sizeof ebuf, "%s/extracted/JJ3X/PSJJ3.EXE", data_dir); bios_boot_raiden2(); }
    signal(SIGSEGV, segv); signal(SIGBUS, segv); signal(SIGFPE, segv);
    cd_init(bin);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)1 << 30);
    pthread_t th;
    if (pthread_create(&th, &at, game_thread, NULL)) fatal("pthread_create");
    pthread_join(th, NULL);
    return 0;
}
#endif

#ifdef CHECK_SP
void sp_mismatch(uint32_t callee, uint32_t site, uint32_t before, uint32_t after) {
    int d = (int)(before - after); if (d > 0x2000 || d < -0x2000) return; static int n; if (n++ < 40) fprintf(stderr, "SP MISMATCH: call %08x at %08x: sp %08x -> %08x (frame %d)\n", callee, site, before, after, (int)(before - after));
}
#endif
