/* High-level emulation of the PSX BIOS kernel services used by the game. */
#include "psx.h"
#include <setjmp.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <ctype.h>
#include <strings.h>

/* ---------------------------------------------------------------- events */
#define MAX_EVENTS 32
typedef struct { uint32_t cls, spec, mode, func, status; } Event;
enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000 };
static Event events[MAX_EVENTS];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < MAX_EVENTS; i++) if (events[i].status == EV_FREE) {
        events[i] = (Event){cls, spec, mode, func, EV_DISABLED};
        LOG(2, "OpenEvent(%08x,%04x,%04x,%08x) -> %d\n", cls, spec, mode, func, i);
        return 0xf1000000 | i;
    }
    return 0xffffffff;
}
static Event *ev_get(uint32_t h) {
    uint32_t i = h & 0xffff;
    if ((h & 0xff000000) != 0xf1000000 || i >= MAX_EVENTS) return NULL;
    return &events[i];
}

static void call_guest(uint32_t fn) {
    /* call a guest function from native code, preserving the caller-visible context */
    uint32_t save_ra = R[31];
    R[31] = 0;
    call_addr(fn);
    R[31] = save_ra;
}

void bios_deliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        Event *e = &events[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x1000) {        /* EvMdINTR: call function, stay busy */
            if (e->func) {
                uint32_t save[32]; memcpy(save, R, sizeof save);
                call_guest(e->func);
                memcpy(R, save, sizeof save);
            }
        } else {
            e->status = EV_READY;
        }
    }
}

/* ---------------------------------------------------------------- exceptions */
static jmp_buf exc_jb;
static int exc_active;
static uint32_t custom_exit;          /* guest address of jmp_buf, 0 = default */
static uint32_t int_chain[4];         /* SysEnqIntRP queues */
static int clear_rcnt[4] = {1, 1, 1, 1};
static int clear_pad = 1;
#define EXC_STACK 0x8000c000u

void bios_exception(void) {
    uint32_t saveR[32], sHI = HI, sLO = LO, sSR = cop0_sr;
    memcpy(saveR, R, sizeof saveR);
    cop0_epc = PC;
    cop0_cause = 0x400;                      /* interrupt pending, ExcCode 0 */
    cop0_sr = (cop0_sr & ~0x3fu) | ((cop0_sr << 2) & 0x3f);

    if (setjmp(exc_jb) == 0) {
        exc_active = 1;
        R[29] = EXC_STACK;
        uint32_t pend = i_stat & i_mask;
        /* root counter events (BIOS prio-1 handlers) */
        static const int rc_irq[4] = {IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_VBLANK};
        for (int n = 0; n < 4; n++) {
            if (pend & (1u << rc_irq[n])) {
                bios_deliver_event(0xf2000000 + n, 0x0002);
                if (clear_rcnt[n]) i_stat &= ~(1u << rc_irq[n]);
            }
        }
        if ((pend & 1) && clear_pad) i_stat &= ~1u;
        /* SysEnqIntRP chains */
        for (int prio = 0; prio < 4; prio++) {
            for (uint32_t node = int_chain[prio]; node; node = rd32(node)) {
                uint32_t f1 = rd32(node + 8), f2 = rd32(node + 4);
                if (!f1) continue;
                R[29] = EXC_STACK;
                call_guest(f1);
                if (R[2] && f2) { R[4] = R[2]; R[29] = EXC_STACK; call_guest(f2); }
            }
        }
        if (custom_exit) {
            uint32_t jb = custom_exit;
            R[31] = rd32(jb + 0);  R[29] = rd32(jb + 4);  R[30] = rd32(jb + 8);
            for (int i = 0; i < 8; i++) R[16 + i] = rd32(jb + 12 + 4 * i);
            R[28] = rd32(jb + 44);
            R[2] = 1;
            call_addr(R[31]);
        }
    }
    exc_active = 0;
    memcpy(R, saveR, sizeof saveR);
    HI = sHI; LO = sLO; cop0_sr = sSR;
}

static void return_from_exception(void) {
    if (exc_active) longjmp(exc_jb, 1);
    LOG(1, "ReturnFromException outside exception\n");
}

/* ---------------------------------------------------------------- pads */
static uint32_t pad_buf[2], pad_len[2];
static int pad_started;

static uint32_t old_pad_dest;
static uint32_t old_pad_word(void) {
    uint32_t b1 = platform_pad_present(0) ? platform_pad_buttons(0) : 0xffff;
    uint32_t b2 = platform_pad_present(1) ? platform_pad_buttons(1) : 0xffff;
    /* libetc layout: byte0 (select..left) in bits 15..8, byte1 (L2..square) in bits 7..0 */
    b1 = ((b1 & 0xff) << 8) | ((b1 >> 8) & 0xff);
    b2 = ((b2 & 0xff) << 8) | ((b2 >> 8) & 0xff);
    return b1 | (b2 << 16);
}
void bios_vblank(void) {
    if (!pad_started) return;
    if (old_pad_dest) wr32(old_pad_dest, old_pad_word());
    for (int p = 0; p < 2; p++) {
        if (!pad_buf[p]) continue;
        uint16_t b = platform_pad_buttons(p);
        if (!platform_pad_present(p)) {  /* nothing connected */
            wr8(pad_buf[p], 0xff);
            continue;
        }
        wr8(pad_buf[p] + 0, 0x00);
        wr8(pad_buf[p] + 1, 0x41);       /* digital pad */
        wr8(pad_buf[p] + 2, b & 0xff);
        wr8(pad_buf[p] + 3, b >> 8);
    }
}

/* ---------------------------------------------------------------- memory card files */
#define SWCARD 0xf4000001
#define HWCARD 0xf0000011
#define EVSP_IOE 0x0004
#define EVSP_ERROR 0x8000
#define EVSP_TIMEOUT 0x0100
#define EVSP_NEW 0x2000

typedef struct { FILE *f; int used; } HostFile;
static HostFile files[16];

static void read_guest_str(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char c = (char)rd8(a + i); if (!c) break; out[i] = c; }
    out[i] = 0;
}
static int card_path(const char *name, char *out, size_t sz, int *port) {
    /* "bu00:NAME" -> <data>/memcard/bu00/NAME */
    if (strncmp(name, "bu", 2) || !name[2] || !name[3] || name[4] != ':') return -1;
    *port = (name[2] - '0') * 16 + (name[3] - '0');
    char dir[512];
    snprintf(dir, sizeof dir, "%s/memcard", data_dir); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/memcard/bu%c%c", data_dir, name[2], name[3]); mkdir(dir, 0755);
    snprintf(out, sz, "%s/%s", dir, name + 5);
    return 0;
}
static void card_done(int ok) {
    bios_deliver_event(SWCARD, ok ? EVSP_IOE : EVSP_ERROR);
    bios_deliver_event(HWCARD, ok ? EVSP_IOE : EVSP_ERROR);
}

static int32_t file_open(uint32_t name_a, uint32_t mode) {
    char name[64], path[600]; int port;
    read_guest_str(name_a, name, sizeof name);
    LOG(1, "open(\"%s\", %x)\n", name, mode);
    if (card_path(name, path, sizeof path, &port) < 0 || port != 0) return -1;
    FILE *f = NULL;
    if (mode & 0x200) {
        struct stat st;
        if (stat(path, &st) == 0) return -1;     /* exists */
        f = fopen(path, "w+b");
        if (!f) return -1;
        uint32_t blocks = mode >> 16; if (!blocks) blocks = 1;
        static uint8_t zero[8192];
        for (uint32_t i = 0; i < blocks; i++) fwrite(zero, 1, 8192, f);
        fseek(f, 0, SEEK_SET);
    } else {
        f = fopen(path, (mode & 2) ? "r+b" : "rb");
        if (!f) return -1;
    }
    for (int i = 2; i < 16; i++) if (!files[i].used) { files[i].f = f; files[i].used = 1; return i; }
    fclose(f); return -1;
}

typedef struct { char pat[64]; char names[16][24]; int n, pos; } DirScan;
static DirScan scan;
static uint32_t fill_dirent(uint32_t de) {
    while (scan.pos < scan.n) {
        const char *nm = scan.names[scan.pos++];
        char path[600]; int port;
        char full[64]; snprintf(full, sizeof full, "bu00:%s", nm);
        card_path(full, path, sizeof path, &port);
        struct stat st; if (stat(path, &st)) continue;
        for (int i = 0; i < 20; i++) wr8(de + i, i < (int)strlen(nm) ? (uint8_t)nm[i] : 0);
        wr32(de + 20, 0x50);
        wr32(de + 24, (uint32_t)st.st_size);
        wr32(de + 28, 0); wr32(de + 32, 0); wr32(de + 36, 0);
        return de;
    }
    return 0;
}
static uint32_t file_first(uint32_t name_a, uint32_t de) {
    char name[64], path[600]; int port;
    read_guest_str(name_a, name, sizeof name);
    LOG(1, "firstfile(\"%s\")\n", name);
    if (card_path(name, path, sizeof path, &port) < 0 || port != 0) return 0;
    snprintf(scan.pat, sizeof scan.pat, "%s", name + 5);
    if (!scan.pat[0]) strcpy(scan.pat, "*");
    for (char *c = scan.pat; *c; c++) if (*c == '?') *c = '?';
    scan.n = scan.pos = 0;
    char dir[600]; snprintf(dir, sizeof dir, "%s/memcard/bu00", data_dir);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && scan.n < 15) {
            if (e->d_name[0] == '.') continue;
            if (fnmatch(scan.pat, e->d_name, 0) == 0) {
                snprintf(scan.names[scan.n++], 24, "%.20s", e->d_name);
            }
        }
        closedir(d);
    }
    return fill_dirent(de);
}

/* ---------------------------------------------------------------- dispatcher */
static uint32_t heap_base, heap_size;

void bios_init(void) {
    memset(events, 0, sizeof events);
    /* fake kernel function tables (A0 @0x200, B0 @0x874, C0 @0x674): libcard reads them and
     * patches kernel code in place.  Point every entry at a zeroed scratch area. */
    for (int i = 0; i < 0xc0; i++) wr32(0x80000200 + 4 * i, 0x80001000);
    for (int i = 0; i < 0x60; i++) wr32(0x80000874 + 4 * i, 0x80001000);
    for (int i = 0; i < 0x20; i++) wr32(0x80000674 + 4 * i, 0x80001000);
}

/* ---- CD-ROM file access through the BIOS (cd, Load, firstfile on "cdrom:") ---- */
int cd_resolve(const char *cwd, const char *path, int32_t *lba, uint32_t *size, int *is_dir);
int cd_read_user(int32_t lba, uint32_t offset_sectors, uint8_t *dst);
static char cd_cwd[128] = "\\";

/* Load(path, struct EXEC*): read a PS-X EXE into RAM and fill the EXEC block */
static uint32_t bios_load_exe(uint32_t path_a, uint32_t ex) {
    char path[128]; read_guest_str(path_a, path, sizeof path);
    int32_t lba; uint32_t size; int isdir;
    /* debugging aid: PSX_LOAD_REMAP="PSJJ2H.EXE;1=PSJJ2V.EXE;1" loads another executable instead */
    const char *rm = getenv("PSX_LOAD_REMAP");
    if (rm) {
        const char *eq = strchr(rm, '=');
        if (eq && strstr(path, "PSJJ") && !strncmp(strrchr(path, ':') ? strrchr(path, ':') + 1 : path, rm, (size_t)(eq - rm))) snprintf(path, sizeof path, "%s", eq + 1);
    }
    if (!cd_resolve(cd_cwd, path, &lba, &size, &isdir) || isdir) { LOG(1, "Load(\"%s\") not found\n", path); return 0; }
    uint8_t hdr[2048];
    cd_read_user(lba, 0, hdr);
    if (memcmp(hdr, "PS-X EXE", 8)) { LOG(1, "Load(\"%s\"): not a PS-X EXE\n", path); return 0; }
    uint32_t taddr, tsize;
    memcpy(&taddr, hdr + 0x18, 4); memcpy(&tsize, hdr + 0x1c, 4);
    for (int i = 0; i < 15; i++) { uint32_t v; memcpy(&v, hdr + 0x10 + 4 * i, 4); wr32(ex + 4 * i, v); }
    uint8_t buf[2048];
    for (uint32_t off = 0; off < tsize; off += 2048) {
        cd_read_user(lba + 1 + (int32_t)(off / 2048), 0, buf);
        uint32_t n = tsize - off < 2048 ? tsize - off : 2048;
        memcpy(ram_ptr(taddr + off), buf, n);
    }
    LOG(1, "Load(\"%s\"): %u bytes at %08x, pc0=%08x\n", path, tsize, taddr, rd32(ex));
    return 1;
}

/* guest printf: args come from a1..a3 then the stack (o32 varargs) */
static void guest_printf(uint32_t fmt, uint32_t argn_first) {
    uint32_t args[4] = {R[4], R[5], R[6], R[7]};
    int ai = (int)argn_first;
    #define NEXTARG() (ai < 4 ? args[ai++] : rd32(R[29] + 16 + 4 * (ai++ - 4)))
    char out[1024]; size_t n = 0;
    for (uint32_t p = fmt; rd8(p) && n < sizeof out - 16; p++) {
        char c = (char)rd8(p);
        if (c != '%') { out[n++] = c; continue; }
        char spec[16]; int k = 0; spec[k++] = '%';
        for (p++; k < 14; p++) {
            c = (char)rd8(p); spec[k++] = c;
            if (strchr("diouxXcsp%", c) || !c) break;
        }
        spec[k] = 0;
        char t[256];
        if (c == '%') n += (size_t)snprintf(out + n, 8, "%%");
        else if (c == 's') {
            char sb[200]; read_guest_str(NEXTARG(), sb, sizeof sb);
            snprintf(t, sizeof t, spec, sb); n += (size_t)snprintf(out + n, sizeof out - n, "%s", t);
        } else if (c == 'c' || strchr("diouxXp", c)) {
            snprintf(t, sizeof t, spec, NEXTARG()); n += (size_t)snprintf(out + n, sizeof out - n, "%s", t);
        } else break;
    }
    out[n] = 0;
    LOG(1, "[guest printf] %s", out);
    #undef NEXTARG
}

void bios_reset_for_exec(void) {
    /* a new program image replaces the old one: drop kernel state that points into it */
    memset(events, 0, sizeof events);
    memset(int_chain, 0, sizeof int_chain);
    custom_exit = 0; exc_active = 0; pad_started = 0; old_pad_dest = 0;
    memset(pad_buf, 0, sizeof pad_buf);
}

void exec_program(uint32_t pc, uint32_t gp, uint32_t base, uint32_t size, uint32_t sp);
void bios_call(uint32_t table, uint32_t fn) {
    uint32_t a0 = R[4], a1 = R[5], a2 = R[6], a3 = R[7];
    LOG(3, "BIOS %02X:%02X(%08x,%08x,%08x,%08x) ra=%08x\n", table, fn, a0, a1, a2, a3, R[31]);
    uint32_t ret = 0;
    if (table == 0xa0) {
        switch (fn) {
        case 0x39: heap_base = a0; heap_size = a1; break;      /* InitHeap */
        case 0x13: {                                             /* setjmp(buf) */
            wr32(a0 + 0, R[31]); wr32(a0 + 4, R[29]); wr32(a0 + 8, R[30]);
            for (int i = 0; i < 8; i++) wr32(a0 + 12 + 4 * i, R[16 + i]);
            wr32(a0 + 44, R[28]); ret = 0; break;
        }
        case 0x14: {                                             /* longjmp(buf, val): resume after the setjmp call */
            R[31] = rd32(a0 + 0); R[29] = rd32(a0 + 4); R[30] = rd32(a0 + 8);
            for (int i = 0; i < 8; i++) R[16 + i] = rd32(a0 + 12 + 4 * i);
            R[28] = rd32(a0 + 44); R[2] = a1 ? a1 : 1;
            call_addr(R[31]); return;
        }
        case 0x0e: ret = (int32_t)a0 < 0 ? -a0 : a0; break;      /* abs */
        case 0x0f: ret = (int32_t)a0 < 0 ? -a0 : a0; break;      /* labs */
        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1b:
        case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: {
            /* libc string/memory routines, done natively on guest memory */
            switch (fn) {
            case 0x15: { uint32_t e = a0; while (rd8(e)) e++; uint32_t i = 0; do wr8(e + i, rd8(a1 + i)); while (rd8(a1 + i++)); ret = a0; break; }
            case 0x16: { uint32_t e = a0; while (rd8(e)) e++; uint32_t i = 0; while (i < a2 && rd8(a1 + i)) { wr8(e + i, rd8(a1 + i)); i++; } wr8(e + i, 0); ret = a0; break; }
            case 0x17: { uint32_t i = 0; for (;; i++) { int c1 = rd8(a0 + i), c2 = rd8(a1 + i); if (c1 != c2) { ret = (uint32_t)(c1 - c2); break; } if (!c1) { ret = 0; break; } } break; }
            case 0x18: { ret = 0; for (uint32_t i = 0; i < a2; i++) { int c1 = rd8(a0 + i), c2 = rd8(a1 + i); if (c1 != c2) { ret = (uint32_t)(c1 - c2); break; } if (!c1) break; } break; }
            case 0x19: { uint32_t i = 0; do wr8(a0 + i, rd8(a1 + i)); while (rd8(a1 + i++)); ret = a0; break; }
            case 0x1a: { uint32_t i = 0; for (; i < a2 && rd8(a1 + i); i++) wr8(a0 + i, rd8(a1 + i)); for (; i < a2; i++) wr8(a0 + i, 0); ret = a0; break; }
            case 0x1b: { uint32_t n = 0; while (rd8(a0 + n)) n++; ret = n; break; }
            case 0x25: ret = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0; break;
            case 0x26: ret = (a0 >= 'A' && a0 <= 'Z') ? a0 + 32 : a0; break;
            case 0x27: case 0x2a: case 0x2c: {                       /* bcopy(src,dst,n) / memcpy / memmove */
                uint32_t dst = fn == 0x27 ? a1 : a0, src = fn == 0x27 ? a0 : a1, n = a2;
                memmove(ram_ptr(dst), ram_ptr(src), n); ret = fn == 0x27 ? 0 : a0; break;
            }
            case 0x28: memset(ram_ptr(a0), 0, a1); break;            /* bzero */
            case 0x29: case 0x2d: { ret = 0; for (uint32_t i = 0; i < a2; i++) { int c1 = rd8(a0 + i), c2 = rd8(a1 + i); if (c1 != c2) { ret = (uint32_t)(c1 - c2); break; } } break; }
            case 0x2b: memset(ram_ptr(a0), (int)(a1 & 0xff), a2); ret = a0; break;
            case 0x2e: { ret = 0; for (uint32_t i = 0; i < a2; i++) if (rd8(a0 + i) == (a1 & 0xff)) { ret = a0 + i; break; } break; }
            }
            break;
        }
        case 0x2f: { static uint32_t seed = 1; seed = seed * 0x41c64e6d + 0x3039; ret = (seed >> 16) & 0x7fff; break; }   /* rand */
        case 0x30: break;                                        /* srand */
        case 0x3f: guest_printf(a0, 1); break;                   /* printf */
        case 0x3c: LOG(1, "%c", (int)(a0 & 0xff)); break;        /* putchar */
        case 0x3e: { char sb[256]; read_guest_str(a0, sb, sizeof sb); LOG(1, "%s\n", sb); break; } /* puts */
        case 0xad: ret = 0; break;                               /* set_card_auto_format */
        case 0x42: ret = bios_load_exe(a0, a1); break;           /* Load */
        case 0x51: {                                             /* LoadExec(path, stack, stack_offset) */
            uint32_t ex = 0x80000400;
            if (!bios_load_exe(a0, ex)) { ret = 0; break; }
            exec_program(rd32(ex), rd32(ex + 4), rd32(ex + 8), rd32(ex + 12), a1 + a2);
            break;
        }
        case 0x44: break;                                        /* FlushCache */
        case 0x43: {                                             /* Exec(struct EXEC*, argc, argv) */
            uint32_t sp = rd32(a0 + 0x20) ? rd32(a0 + 0x20) + rd32(a0 + 0x24) : 0;   /* s_addr + s_size */
            if (rd32(a0 + 0x1c)) memset(ram_ptr(rd32(a0 + 0x18)), 0, rd32(a0 + 0x1c));  /* clear bss */
            exec_program(rd32(a0 + 0x0), rd32(a0 + 0x4), rd32(a0 + 0x8), rd32(a0 + 0xc), sp);
            break;
        }
        case 0x49: gpu_gp0(a0); break;                           /* GPU_cw */
        case 0x70: ret = 1; break;                               /* _bu_init */
        case 0x71: case 0x72: break;                             /* _96_init / _96_remove */
        case 0xab:                                               /* _card_info(port) */
            ret = 1;
            bios_deliver_event(SWCARD, (a0 & 0x10) ? EVSP_TIMEOUT : EVSP_IOE);
            bios_deliver_event(HWCARD, (a0 & 0x10) ? EVSP_TIMEOUT : EVSP_IOE);
            break;
        case 0xac:                                               /* _card_load(port) */
            ret = 1;
            bios_deliver_event(SWCARD, (a0 & 0x10) ? EVSP_TIMEOUT : EVSP_IOE);
            break;
        default: fatal("unimplemented BIOS A0:%02X (ra=%08x)", fn, R[31]);
        }
    } else if (table == 0xb0) {
        switch (fn) {
        case 0x07: bios_deliver_event(a0, a1); ret = 1; break;   /* DeliverEvent */
        case 0x08: ret = open_event(a0, a1, a2, a3); break;      /* OpenEvent */
        case 0x09: { Event *e = ev_get(a0); if (e) e->status = EV_FREE; ret = 1; break; }
        case 0x0a: {                                             /* WaitEvent */
            Event *e = ev_get(a0);
            if (!e) { ret = 0; break; }
            int guard = 0;
            check_irq();
            while (e->status != EV_READY && guard++ < 1000) { cycles = next_event; run_events(); }
            if (e->status != EV_READY) LOG(1, "WaitEvent(%08x) timed out\n", a0);
            if (e->status == EV_READY) e->status = EV_ENABLED;
            ret = 1; break;
        }
        case 0x0b: {                                             /* TestEvent */
            Event *e = ev_get(a0);
            if (e && e->status == EV_READY) { e->status = EV_ENABLED; ret = 1; } else ret = 0;
            break;
        }
        case 0x0c: { Event *e = ev_get(a0); if (e && e->status != EV_FREE) e->status = EV_ENABLED; ret = 1; break; }
        case 0x0d: { Event *e = ev_get(a0); if (e && e->status != EV_FREE) e->status = EV_DISABLED; ret = 1; break; }
        case 0x12: pad_buf[0] = a0; pad_len[0] = a1; pad_buf[1] = a2; pad_len[1] = a3; ret = 2; break; /* InitPAD */
        case 0x13: pad_started = 1; cop0_sr |= 0x401; ret = 1; break;  /* StartPAD */
        case 0x14: pad_started = 0; ret = 1; break;              /* StopPAD */
        case 0x15: old_pad_dest = a1; pad_started = 1; ret = 1; break;   /* OutdatedPadInitAndStart(type, dest) */
        case 0x16: ret = old_pad_word(); break;   /* PAD_dr */
        case 0x17: return_from_exception(); return;
        case 0x18: custom_exit = 0; break;                       /* SetDefaultExitFromException */
        case 0x19: custom_exit = a0; LOG(2, "SetCustomExitFromException(%08x) ra=%08x\n", a0, rd32(a0)); break;
        case 0x32: ret = (uint32_t)file_open(a0, a1); break;     /* open */
        case 0x33: {                                             /* lseek */
            if (a0 < 16 && files[a0].used) {
                fseek(files[a0].f, (long)(int32_t)a1, a2 == 0 ? SEEK_SET : a2 == 1 ? SEEK_CUR : SEEK_END);
                ret = (uint32_t)ftell(files[a0].f);
            } else ret = 0xffffffff;
            break;
        }
        case 0x34: {                                             /* read */
            if (a0 < 16 && files[a0].used) {
                size_t n = fread(ram_ptr(a1), 1, a2, files[a0].f);
                ret = (uint32_t)n; card_done(1);
            } else if (a0 < 16) { ret = 0xffffffff; card_done(0); }
            else ret = 0xffffffff;
            break;
        }
        case 0x35: {                                             /* write */
            if (a0 == 1) { fwrite(ram_ptr(a1), 1, a2, stdout); fflush(stdout); ret = a2; }
            else if (a0 < 16 && files[a0].used) {
                size_t n = fwrite(ram_ptr(a1), 1, a2, files[a0].f); fflush(files[a0].f);
                ret = (uint32_t)n; card_done(1);
            } else ret = 0xffffffff;
            break;
        }
        case 0x36: if (a0 < 16 && files[a0].used) { fclose(files[a0].f); files[a0].used = 0; ret = a0; } else ret = 0xffffffff; break;
        case 0x40: {                                             /* cd(path) */
            char p[128]; read_guest_str(a0, p, sizeof p);
            int32_t l; uint32_t sz; int d;
            if (cd_resolve(cd_cwd, p, &l, &sz, &d) && d) {
                const char *q = !strncasecmp(p, "cdrom:", 6) ? p + 6 : p;
                if (*q == '\\') snprintf(cd_cwd, sizeof cd_cwd, "%s", q); else { size_t n = strlen(cd_cwd); snprintf(cd_cwd + n, sizeof cd_cwd - n, "\\%s", q); }
                ret = 1;
            } else ret = 0;
            LOG(1, "cd(\"%s\") -> %u (cwd %s)\n", p, ret, cd_cwd);
            break;
        }
        case 0x41: ret = 1; break;                               /* format */
        case 0x42: ret = file_first(a0, a1); break;              /* firstfile */
        case 0x43: ret = fill_dirent(a0); break;                 /* nextfile */
        case 0x45: {                                             /* delete */
            char name[64], path[600]; int port;
            read_guest_str(a0, name, sizeof name);
            ret = (card_path(name, path, sizeof path, &port) == 0 && remove(path) == 0) ? 1 : 0;
            break;
        }
        case 0x4a: case 0x4b: case 0x4c: ret = 1; break;         /* InitCARD/StartCARD/StopCARD */
        case 0x4e: ret = 1; card_done(1); break;                 /* _card_write */
        case 0x50: break;                                        /* _new_card */
        case 0x56: ret = 0x80000674; break;                      /* GetC0Table */
        case 0x57: ret = 0x80000874; break;                      /* GetB0Table */
        case 0x5b: clear_pad = a0 != 0; break;                   /* ChangeClearPad */
        default: fatal("unimplemented BIOS B0:%02X (ra=%08x)", fn, R[31]);
        }
    } else {
        switch (fn) {
        case 0x02: {                                             /* SysEnqIntRP */
            uint32_t p = a0 & 3;
            wr32(a1, int_chain[p]); int_chain[p] = a1; ret = 1;
            LOG(2, "SysEnqIntRP(%u, %08x) f1=%08x f2=%08x\n", p, a1, rd32(a1 + 8), rd32(a1 + 4));
            break;
        }
        case 0x03: {                                             /* SysDeqIntRP */
            uint32_t p = a0 & 3;
            if (int_chain[p] == a1) int_chain[p] = rd32(a1);
            else for (uint32_t n = int_chain[p]; n; n = rd32(n))
                if (rd32(n) == a1) { wr32(n, rd32(a1)); break; }
            ret = 1; break;
        }
        case 0x0a: ret = (uint32_t)clear_rcnt[a0 & 3]; clear_rcnt[a0 & 3] = a1 != 0; break;
        default: fatal("unimplemented BIOS C0:%02X (ra=%08x)", fn, R[31]);
        }
    }
    R[2] = ret;
}
