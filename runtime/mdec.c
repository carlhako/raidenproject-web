/* MDEC (macroblock decoder) emulation: RLE -> IDCT -> YUV->RGB. */
#include "psx.h"
#include <math.h>

static uint32_t cur_cmd;
static int remaining;          /* parameter words still expected */
static uint32_t *inbuf; static int in_n, in_cap;
static uint8_t qt_y[64], qt_c[64];
static int16_t scale[64];
static uint32_t *outbuf; static int out_n, out_pos, out_cap;
static uint32_t ctrl;

static const int zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static int zagzig[64];

void mdec_init(void) {
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = i;
    in_cap = 1 << 16; inbuf = malloc(in_cap * 4);
    out_cap = 1 << 18; outbuf = malloc(out_cap * 4);
}

static inline int s10(int v) { return (v << 22) >> 22; }

static void out_word(uint32_t w) {
    if (out_n >= out_cap) { out_cap *= 2; outbuf = realloc(outbuf, out_cap * 4); }
    outbuf[out_n++] = w;
}

/* decode one 8x8 block from the halfword stream; returns 0 at end of data */
static int rl_block(const uint16_t *src, int nhw, int *pos, const uint8_t *qt, double *blk) {
    int coef[64]; memset(coef, 0, sizeof coef);
    while (*pos < nhw && src[*pos] == 0xfe00) (*pos)++;
    if (*pos >= nhw) return 0;
    uint16_t n = src[(*pos)++];
    int k = 0;
    int qs = (n >> 10) & 63;
    int val = s10(n & 0x3ff) * qt[0];
    for (;;) {
        if (qs == 0) val = s10(n & 0x3ff) * 2;
        if (val < -0x400) val = -0x400; if (val > 0x3ff) val = 0x3ff;
        if (qs > 0) coef[zagzig[k]] = val; else coef[k] = val;
        if (*pos >= nhw) break;
        n = src[(*pos)++];
        if (n == 0xfe00) break;
        k += ((n >> 10) & 63) + 1;
        if (k > 63) break;
        val = (s10(n & 0x3ff) * qt[k] * qs + 4) / 8;
    }
    /* 2-pass IDCT using the game's scale table */
    double tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int z = 0; z < 8; z++) s += coef[y + z * 8] * (double)scale[x + z * 8];
            tmp[x + y * 8] = s / 65536.0;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int z = 0; z < 8; z++) s += tmp[y + z * 8] * (double)scale[x + z * 8];
            blk[x + y * 8] = s / 65536.0;
        }
    return 1;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void decode_macroblocks(void) {
    int depth = (cur_cmd >> 27) & 3, sgn = (cur_cmd >> 26) & 1, b15 = (cur_cmd >> 25) & 1;
    const uint16_t *src = (const uint16_t *)inbuf;
    int nhw = in_n * 2, pos = 0;
    out_n = out_pos = 0;
    if (depth >= 2) {
        for (;;) {
            double cr[64], cb[64], y[4][64];
            if (!rl_block(src, nhw, &pos, qt_c, cr)) break;
            if (!rl_block(src, nhw, &pos, qt_c, cb)) break;
            int ok = 1;
            for (int i = 0; i < 4 && ok; i++) ok = rl_block(src, nhw, &pos, qt_y, y[i]);
            if (!ok) break;
            uint8_t rgb[16][16][3];
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++) {
                    int bi = (py >= 8) * 2 + (px >= 8);
                    double Y = y[bi][(px & 7) + (py & 7) * 8];
                    double Cr = cr[(px >> 1) + (py >> 1) * 8], Cb = cb[(px >> 1) + (py >> 1) * 8];
                    int R = clampi((int)lround(Y + 1.402 * Cr), -128, 127);
                    int G = clampi((int)lround(Y - 0.3437 * Cb - 0.7143 * Cr), -128, 127);
                    int B = clampi((int)lround(Y + 1.772 * Cb), -128, 127);
                    if (!sgn) { R ^= 0x80; G ^= 0x80; B ^= 0x80; }
                    rgb[py][px][0] = (uint8_t)R; rgb[py][px][1] = (uint8_t)G; rgb[py][px][2] = (uint8_t)B;
                }
            if (depth == 2) {
                const uint8_t *b = &rgb[0][0][0];
                for (int i = 0; i < 768; i += 4) out_word(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24));
            } else {
                for (int py = 0; py < 16; py++)
                    for (int px = 0; px < 16; px += 2) {
                        uint32_t w = 0;
                        for (int k = 0; k < 2; k++) {
                            uint8_t *c = rgb[py][px + k];
                            uint32_t p = (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10) | (b15 << 15);
                            w |= p << (16 * k);
                        }
                        out_word(w);
                    }
            }
        }
    } else {
        for (;;) {
            double yb[64];
            if (!rl_block(src, nhw, &pos, qt_y, yb)) break;
            uint8_t px[64];
            for (int i = 0; i < 64; i++) { int v = clampi((int)lround(yb[i]), -128, 127); if (!sgn) v ^= 0x80; px[i] = (uint8_t)v; }
            if (depth == 1) for (int i = 0; i < 64; i += 4) out_word(px[i] | (px[i + 1] << 8) | (px[i + 2] << 16) | ((uint32_t)px[i + 3] << 24));
            else for (int i = 0; i < 64; i += 8) {
                uint32_t w = 0; for (int k = 0; k < 8; k++) w |= (uint32_t)(px[i + k] >> 4) << (4 * k);
                out_word(w);
            }
        }
    }
}

static void param_word(uint32_t v) {
    if (in_n >= in_cap) { in_cap *= 2; inbuf = realloc(inbuf, in_cap * 4); }
    inbuf[in_n++] = v;
    if (--remaining > 0) return;
    int c = cur_cmd >> 29;
    if (c == 1) decode_macroblocks();
    else if (c == 2) {
        uint8_t *b = (uint8_t *)inbuf;
        memcpy(qt_y, b, 64);
        if (cur_cmd & 1) memcpy(qt_c, b + 64, 64);
    } else if (c == 3) {
        memcpy(scale, inbuf, 128);
    }
    cur_cmd = 0;
}

void mdec_dma_in(uint32_t w) {
    if (remaining > 0) { param_word(w); return; }
    cur_cmd = w; in_n = 0;
    int c = w >> 29;
    if (c == 1) remaining = w & 0xffff;
    else if (c == 2) remaining = (w & 1) ? 32 : 16;
    else if (c == 3) remaining = 32;
    else remaining = 0;
    if (remaining == 0) cur_cmd = 0;
}

void mdec_write(int reg, uint32_t v) {
    if (reg == 0) mdec_dma_in(v);
    else {
        ctrl = v;
        if (v & 0x80000000u) { remaining = 0; cur_cmd = 0; in_n = 0; out_n = out_pos = 0; }
    }
}
uint32_t mdec_dma_out(void) { return out_pos < out_n ? outbuf[out_pos++] : 0; }
int mdec_out_ready(void) { return remaining == 0 && out_pos < out_n; }
uint32_t mdec_read(int reg) {
    if (reg == 0) return mdec_dma_out();
    uint32_t s = 0;
    if (out_pos >= out_n) s |= 1u << 31;
    if (remaining > 0) s |= 1u << 29;
    if (ctrl & (1u << 30)) s |= 1u << 28;
    if ((ctrl & (1u << 29)) && out_pos < out_n) s |= 1u << 27;
    s |= ((cur_cmd >> 25) & 0xf) << 23;
    s |= (uint32_t)((remaining - 1) & 0xffff);
    return s;
}
