/* CD-ROM controller emulation (register level) reading raw sectors from the BIN image,
 * including XA-ADPCM audio decoding for streamed music. */
#include "psx.h"
#include <strings.h>

static FILE *bin;
static int32_t disc_sectors;

static int index_reg;
static uint8_t params[16]; static int nparams;
static uint8_t resp[16]; static int resp_len, resp_pos;
static uint8_t ie = 0x1f, iflag;   /* BIOS boot leaves all CD IRQs enabled */
static uint8_t stat_byte = 0x02;
static uint8_t mode;
static int32_t setloc_lba, cur_lba, last_lba;
static int setloc_pending;
static int reading, read_speed_cycles;
static uint8_t filter_file, filter_chan;
static int muted;
static uint8_t last_header[8];

static uint8_t sector[2352];
static int sector_ready;
static uint8_t fifo[2352]; static int fifo_len, fifo_pos;

/* interrupt queue */
typedef struct { uint8_t type; uint8_t r[16]; int len; int delay; } Pend;
static Pend q[8]; static int qn;

static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

#ifdef __EMSCRIPTEN__
extern int js_disc_sectors(void);
extern int js_disc_read(int lba, uint8_t *dst);
void cd_init(const char *path) { (void)path; disc_sectors = js_disc_sectors(); LOG(1, "disc: %d sectors\n", disc_sectors); }
static void read_raw(int32_t lba, uint8_t *dst) {
    if (lba < 0 || lba >= disc_sectors || !js_disc_read(lba, dst)) memset(dst, 0, 2352);
}
#else
void cd_init(const char *path) {
    bin = fopen(path, "rb");
    if (!bin) { fprintf(stderr, "cannot open disc image %s\n", path); exit(1); }
    fseek(bin, 0, SEEK_END);
    disc_sectors = (int32_t)(ftell(bin) / 2352);
    LOG(1, "disc: %s (%d sectors)\n", path, disc_sectors);
}

static void read_raw(int32_t lba, uint8_t *dst) {
    if (lba < 0 || lba >= disc_sectors) { memset(dst, 0, 2352); return; }
    fseek(bin, (long)lba * 2352, SEEK_SET);
    if (fread(dst, 1, 2352, bin) != 2352) memset(dst, 0, 2352);
}
#endif

static void push(uint8_t type, const uint8_t *r, int len, int delay) {
    if (qn >= 8) return;
    q[qn].type = type; memcpy(q[qn].r, r, len); q[qn].len = len; q[qn].delay = delay;
    qn++;
    if (qn == 1 && iflag == 0) sched_set(EV_CDROM, cycles + delay);
}
static void push_stat(uint8_t type, int delay) { uint8_t s = stat_byte; push(type, &s, 1, delay); }

static void deliver(Pend *p) {
    LOG(3, "CD INT%d resp[%d]=%02x ie=%02x\n", p->type, p->len, p->r[0], ie);
    iflag = p->type;
    memcpy(resp, p->r, p->len); resp_len = p->len; resp_pos = 0;
    if (iflag & ie) irq_raise(IRQ_CDROM);
}

void cd_event(void) {
    if (!qn || iflag) return;
    Pend p = q[0];
    memmove(q, q + 1, sizeof(Pend) * (size_t)(--qn));
    /* commands with side effects on completion */
    deliver(&p);
}

/* Data sectors arrive 8x faster than a real double-speed drive, so loading screens are short
 * (the game acks each sector before the next is delivered, see cd_read_event). XA-ADPCM
 * streaming keeps the real rate so audio plays at the right speed. */
#define FAST_CD_SPS 1200
static int sector_period(void) {
    if (!(mode & 0x40)) return CPU_HZ / FAST_CD_SPS;
    return (mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75;
}

static void start_reading(void) {
    if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = 0; }
    reading = 1;
    stat_byte |= 0x20;
    sched_set(EV_CDREAD, cycles + sector_period() + 20000);
}
static void stop_reading(void) {
    reading = 0;
    stat_byte &= ~0xe0u;
    sched_clear(EV_CDREAD);
}

/* ---------------------------------------------------------------- XA audio */
#define XA_BUF 65536
static int16_t xa_l[XA_BUF], xa_r[XA_BUF];
static int xa_wr, xa_rd_int; static double xa_frac;
static int xa_rate = 37800;
static int32_t xa_old[2], xa_older[2];
static uint8_t cd_vol[4] = {0x80, 0, 0, 0x80}; /* LL, LR, RL, RR */
static uint8_t cd_vol_pending[4] = {0x80, 0, 0, 0x80};

static int xa_fill(void) { return (xa_wr - xa_rd_int) & (XA_BUF - 1); }

static void xa_decode_sector(const uint8_t *s) {
    uint8_t ci = s[16 + 3];
    int stereo = (ci & 3) == 1;
    xa_rate = (ci & 0x0c) ? 18900 : 37800;
    int bits8 = (ci & 0x30) == 0x10;
    static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
    const uint8_t *data = s + 24;
    int16_t out[2][18 * 8 * 28];
    int nout[2] = {0, 0};
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = data + g * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t hdr = bits8 ? grp[4 + u] : grp[4 + u];
            int shift = hdr & 15; if (shift > 12) shift = 9;
            int filt = (hdr >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            for (int j = 0; j < 28; j++) {
                int32_t smp;
                if (bits8) smp = (int16_t)(grp[16 + j * 4 + u] << 8) >> shift;
                else {
                    uint8_t b = grp[16 + j * 4 + (u >> 1)];
                    int nib = (u & 1) ? (b >> 4) : (b & 15);
                    smp = (int16_t)(nib << 12) >> shift;
                }
                smp += (xa_old[ch] * K0[filt] + xa_older[ch] * K1[filt] + 32) >> 6;
                if (smp > 32767) smp = 32767; if (smp < -32768) smp = -32768;
                xa_older[ch] = xa_old[ch]; xa_old[ch] = smp;
                out[ch][nout[ch]++] = (int16_t)smp;
            }
        }
    }
    int n = nout[0];
    for (int i = 0; i < n; i++) {
        if (xa_fill() >= XA_BUF - 2) break;
        int16_t l = out[0][i], r = stereo ? out[1][i < nout[1] ? i : nout[1] - 1] : l;
        xa_l[xa_wr] = l; xa_r[xa_wr] = r;
        xa_wr = (xa_wr + 1) & (XA_BUF - 1);
    }
}

int cd_xa_fetch(int16_t *l, int16_t *r) {
    if (xa_fill() < 2 || muted) { *l = *r = 0; return 0; }
    int16_t a = xa_l[xa_rd_int], b = xa_r[xa_rd_int];
    int32_t L = (a * cd_vol[0] + b * cd_vol[2]) >> 7;
    int32_t Rr = (a * cd_vol[1] + b * cd_vol[3]) >> 7;
    if (L > 32767) L = 32767; if (L < -32768) L = -32768;
    if (Rr > 32767) Rr = 32767; if (Rr < -32768) Rr = -32768;
    *l = (int16_t)L; *r = (int16_t)Rr;
    xa_frac += (double)xa_rate / 44100.0;
    while (xa_frac >= 1.0) { xa_frac -= 1.0; xa_rd_int = (xa_rd_int + 1) & (XA_BUF - 1); if (xa_fill() == 0) break; }
    return 1;
}

/* ---------------------------------------------------------------- sector reading */
void cd_read_event(void) {
    if (!reading) return;
    if (iflag) { sched_set(EV_CDREAD, cycles + 2000); return; }  /* host hasn't acked yet */
    uint8_t raw[2352];
    read_raw(cur_lba, raw);
    memcpy(last_header, raw + 12, 4); memcpy(last_header + 4, raw + 16, 4);
    last_lba = cur_lba;
    cur_lba++;
    sched_set(EV_CDREAD, cycles + sector_period());
    uint8_t submode = raw[18];
    int is_audio = (raw[15] == 2) && (submode & 0x04);
    if ((mode & 0x40) && is_audio) {
        if (!(mode & 0x08) || (raw[16] == filter_file && raw[17] == filter_chan)) xa_decode_sector(raw);
        return;
    }
    if ((mode & 0x08) && raw[15] == 2 && (submode & 0x04)) return;
    memcpy(sector, raw, 2352);
    sector_ready = 1;
    uint8_t s = stat_byte;
    Pend p = {1, {s}, 1, 0};
    deliver(&p);
}

uint32_t cd_dma_read_word(void) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = fifo_pos < fifo_len ? fifo[fifo_pos++] : 0;
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

/* ---------------------------------------------------------------- commands */
static void command(uint8_t c) {
    LOG(2, "CD cmd %02x params[%d] %02x %02x %02x\n", c, nparams, params[0], params[1], params[2]);
    const int D1 = 25000, D2 = 60000;
    switch (c) {
    case 0x01: push_stat(3, D1); break;
    case 0x02:
        setloc_lba = (unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2]) - 150;
        setloc_pending = 1;
        push_stat(3, D1); break;
    case 0x03: stat_byte |= 0x02; push_stat(3, D1); break;   /* Play: no CD-DA tracks */
    case 0x06: case 0x1b:
        stat_byte |= 0x02;
        push_stat(3, D1);
        start_reading();
        break;
    case 0x07: stat_byte |= 0x02; push_stat(3, D1); push_stat(2, D2); break;
    case 0x08: stop_reading(); push_stat(3, D1); stat_byte &= ~0x02u; push_stat(2, D2); break;
    case 0x09: {
        push_stat(3, D1);
        stop_reading();
        push_stat(2, D2);
        break;
    }
    case 0x0a:
        stop_reading(); mode = 0x20; stat_byte = 0x02;
        push_stat(3, D1); push_stat(2, D2); break;
    case 0x0b: muted = 1; push_stat(3, D1); break;
    case 0x0c: muted = 0; push_stat(3, D1); break;
    case 0x0d: filter_file = params[0]; filter_chan = params[1]; push_stat(3, D1); break;
    case 0x0e: mode = params[0]; push_stat(3, D1); break;
    case 0x0f: { uint8_t r[5] = {stat_byte, mode, 0, filter_file, filter_chan}; push(3, r, 5, D1); break; }
    case 0x10: push(3, last_header, 8, D1); break;
    case 0x11: {
        int32_t l = last_lba + 150;
        int32_t rel = last_lba;
        uint8_t r[8] = {1, 1, bcd(rel / 4500), bcd((rel / 75) % 60), bcd(rel % 75),
                        bcd(l / 4500), bcd((l / 75) % 60), bcd(l % 75)};
        push(3, r, 8, D1); break;
    }
    case 0x13: { uint8_t r[3] = {stat_byte, 1, 1}; push(3, r, 3, D1); break; }
    case 0x14: {
        int32_t l = (params[0] == 0) ? disc_sectors + 150 : 150;
        uint8_t r[3] = {stat_byte, bcd(l / 4500), bcd((l / 75) % 60)};
        push(3, r, 3, D1); break;
    }
    case 0x15: case 0x16:
        stop_reading();
        if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = 0; }
        last_lba = cur_lba;
        push_stat(3, D1); push_stat(2, D2); break;
    case 0x19:
        if (params[0] == 0x20) { uint8_t r[4] = {0x94, 0x09, 0x19, 0xc0}; push(3, r, 4, D1); }
        else push_stat(3, D1);
        break;
    case 0x1a: {
        push_stat(3, D1);
        uint8_t r[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E'};
        push(2, r, 8, D2); break;
    }
    case 0x1e: push_stat(3, D1); push_stat(2, D2); break;
    default: {
        LOG(1, "CD: unknown command %02x\n", c);
        uint8_t r[2] = {(uint8_t)(stat_byte | 1), 0x40};
        push(5, r, 2, D1); break;
    }
    }
    nparams = 0;
}

static uint8_t cd_read_reg_(int r);
uint8_t cd_read_reg(int r) {
    uint8_t v = cd_read_reg_(r);
    LOG(4, "CD rd %d.%d -> %02x\n", r, index_reg, v);
    return v;
}
static uint8_t cd_read_reg_(int r) {
    switch (r) {
    case 0: {
        uint8_t s = (uint8_t)index_reg;
        if (nparams == 0) s |= 0x08;
        if (nparams < 16) s |= 0x10;
        if (resp_pos < resp_len) s |= 0x20;
        if (fifo_pos < fifo_len) s |= 0x40;
        return s;
    }
    case 1: return resp_pos < resp_len ? resp[resp_pos++] : 0;
    case 2: return fifo_pos < fifo_len ? fifo[fifo_pos++] : 0;
    case 3:
        if (index_reg & 1) return iflag | 0xe0;
        return ie | 0xe0;
    }
    return 0;
}

void cd_write_reg(int r, uint8_t v) {
    LOG(4, "CD wr %d.%d <- %02x\n", r, index_reg, v);
    if (r == 0) { index_reg = v & 3; return; }
    switch (r * 4 + index_reg) {
    case 4: command(v); break;
    case 5: case 6: break;                         /* sound map */
    case 7: cd_vol_pending[3] = v; break;          /* R->R */
    case 8: if (nparams < 16) params[nparams++] = v; break;
    case 9: ie = v & 0x1f; break;
    case 10: cd_vol_pending[0] = v; break;         /* L->L */
    case 11: cd_vol_pending[2] = v; break;         /* R->L */
    case 12:                                       /* request register */
        if (v & 0x80) {
            if (sector_ready) {        /* loads once per newly read sector */
                if (mode & 0x20) { memcpy(fifo, sector + 12, 0x924); fifo_len = 0x924; }
                else { memcpy(fifo, sector + 24, 0x800); fifo_len = 0x800; }
                fifo_pos = 0;
                sector_ready = 0;
            }
        } else { fifo_len = fifo_pos = 0; }
        break;
    case 13:                                       /* interrupt flag ack */
        iflag &= ~(v & 0x1f);
        if (v & 0x40) nparams = 0;
        if (iflag == 0 && qn) sched_set(EV_CDROM, cycles + q[0].delay / 4 + 1000);
        break;
    case 14: cd_vol_pending[1] = v; break;         /* L->R */
    case 15: if (v & 0x20) memcpy(cd_vol, cd_vol_pending, 4); muted = (v & 1) ? 1 : muted; break;
    }
}

/* ---------------------------------------------------------------- ISO9660 lookup (for BIOS file calls: cd/Load/firstfile) */
static int iso_find(int32_t dir_lba, uint32_t dir_size, const char *name, size_t nlen, int32_t *lba, uint32_t *size, int *is_dir) {
    uint8_t sec[2352];
    for (uint32_t off = 0; off < dir_size; off += 2048) {
        read_raw(dir_lba + (int32_t)(off / 2048), sec);
        const uint8_t *d = sec + 24;
        for (int p = 0; p < 2048;) {
            int ln = d[p];
            if (!ln) break;
            int nl = d[p + 32];
            const uint8_t *nm = d + p + 33;
            size_t cmp = 0;
            while (cmp < (size_t)nl && nm[cmp] != ';') cmp++;
            if (nl > 1 && cmp == nlen) {
                size_t i = 0;
                for (; i < nlen; i++) if ((nm[i] & ~0x20) != (name[i] & ~0x20) && nm[i] != name[i]) break;
                if (i == nlen) {
                    memcpy(lba, d + p + 2, 4); memcpy(size, d + p + 10, 4);
                    *is_dir = d[p + 25] & 2;
                    return 1;
                }
            }
            p += ln;
        }
    }
    return 0;
}

/* resolve "cdrom:\DIR\FILE;1" (or relative to `cwd`) to a start sector and size */
int cd_resolve(const char *cwd, const char *path, int32_t *lba, uint32_t *size, int *is_dir) {
    if (!strncasecmp(path, "cdrom:", 6)) path += 6;
    char full[256];
    if (path[0] == '\\' || path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s\\%s", cwd, path);
    uint8_t sec[2352];
    read_raw(16, sec);
    int32_t cur_lba; uint32_t cur_size;
    memcpy(&cur_lba, sec + 24 + 156 + 2, 4); memcpy(&cur_size, sec + 24 + 156 + 10, 4);
    int dir = 1;
    const char *p = full;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        size_t n = (size_t)(e - p);
        for (size_t k = 0; k < n; k++) if (p[k] == ';') { n = k; break; }
        if (!dir) return 0;
        if (!iso_find(cur_lba, cur_size, p, n, &cur_lba, &cur_size, &dir)) return 0;
        p = e;
    }
    *lba = cur_lba; *size = cur_size; *is_dir = dir;
    return 1;
}

int cd_read_user(int32_t lba, uint32_t offset_sectors, uint8_t *dst) {
    uint8_t sec[2352];
    read_raw(lba + (int32_t)offset_sectors, sec);
    memcpy(dst, sec + 24, 2048);
    return 2048;
}

/* a new program replaces the old one mid-flight: drop pending CD commands, interrupts and reads */
void cd_reset_for_exec(void) {
    qn = 0; iflag = 0; resp_len = resp_pos = 0; nparams = 0;
    fifo_len = fifo_pos = 0; sector_ready = 0;
    stop_reading();
    sched_clear(EV_CDROM);
}
