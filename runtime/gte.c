/* GTE (COP2) emulation following the documented hardware algorithms. */
#include "psx.h"

static int32_t D[32];   /* data registers */
static int32_t C[32];   /* control registers */
static uint32_t FLAG;

#define VX(n) ((int16_t)(D[(n) * 2] & 0xffff))
#define VY(n) ((int16_t)(D[(n) * 2] >> 16))
#define VZ(n) ((int16_t)(D[(n) * 2 + 1]))
#define IR0 D[8]
#define IR1 D[9]
#define IR2 D[10]
#define IR3 D[11]
#define MAC0 D[24]
#define MAC1 D[25]
#define MAC2 D[26]
#define MAC3 D[27]
#define OTZ D[7]

static int16_t RT(int i, int j) { int k = i * 3 + j; int32_t w = C[k >> 1]; return (k & 1) ? (int16_t)(w >> 16) : (int16_t)w; }
static int16_t LL(int i, int j) { int k = i * 3 + j; int32_t w = C[8 + (k >> 1)]; return (k & 1) ? (int16_t)(w >> 16) : (int16_t)w; }
static int16_t LC(int i, int j) { int k = i * 3 + j; int32_t w = C[16 + (k >> 1)]; return (k & 1) ? (int16_t)(w >> 16) : (int16_t)w; }
#define TRX C[5]
#define TRY C[6]
#define TRZ C[7]
#define RBK C[13]
#define GBK C[14]
#define BBK C[15]
#define RFC C[21]
#define GFC C[22]
#define BFC C[23]
#define OFX C[24]
#define OFY C[25]
#define HREG ((uint16_t)C[26])
#define DQA ((int16_t)C[27])
#define DQB C[28]
#define ZSF3 ((int16_t)C[29])
#define ZSF4 ((int16_t)C[30])

static int64_t A(int i, int64_t v) {
    if (v > 0x7ffffffffffLL) FLAG |= 1u << (31 - i);
    else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
    return (v << 20) >> 20;
}
static int32_t LmB(int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { FLAG |= 1u << (25 - i); return lo; }
    if (v > 0x7fff) { FLAG |= 1u << (25 - i); return 0x7fff; }
    return v;
}
static int32_t LmB3_sf(int32_t v, int32_t vshifted, int lm) {
    /* IR3 saturation quirk for RTPS/RTPT: flag based on (MAC3 >> 12) */
    int32_t lo = lm ? 0 : -0x8000;
    if (vshifted < -0x8000 || vshifted > 0x7fff) FLAG |= 1u << 22;
    if (v < lo) return lo;
    if (v > 0x7fff) return 0x7fff;
    return v;
}
static uint8_t LmC(int i, int32_t v) {
    if (v < 0) { FLAG |= 1u << (21 - i); return 0; }
    if (v > 255) { FLAG |= 1u << (21 - i); return 255; }
    return (uint8_t)v;
}
static int32_t LmD(int64_t v) {
    if (v < 0) { FLAG |= 1u << 18; return 0; }
    if (v > 0xffff) { FLAG |= 1u << 18; return 0xffff; }
    return (int32_t)v;
}
static int32_t LmE(int32_t v) {
    if (v < 0) { FLAG |= 1u << 12; return 0; }
    if (v > 0x1000) { FLAG |= 1u << 12; return 0x1000; }
    return v;
}
static int32_t LmG(int i, int32_t v) {
    if (v < -0x400) { FLAG |= 1u << (14 - i); return -0x400; }
    if (v > 0x3ff) { FLAG |= 1u << (14 - i); return 0x3ff; }
    return v;
}
static int64_t F(int64_t v) {
    if (v > 0x7fffffffLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    return v;
}

static uint8_t unr_table[0x101];
static int unr_init;
static uint32_t gte_divide(uint16_t h, uint16_t sz3) {
    if (!unr_init) {
        for (int i = 0; i < 0x101; i++) {
            int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
        }
        unr_init = 1;
    }
    if (h < sz3 * 2) {
        int z = __builtin_clz((uint32_t)sz3) - 16;
        uint32_t n = (uint32_t)h << z;
        uint32_t d = (uint32_t)sz3 << z;
        uint32_t u = unr_table[(d - 0x7fc0) >> 7] + 0x101;
        d = (0x2000080 - (d * u)) >> 8;
        d = (0x0000080 + (d * u)) >> 8;
        uint64_t r = (((uint64_t)n * d) + 0x8000) >> 16;
        return r > 0x1ffff ? 0x1ffff : (uint32_t)r;
    }
    FLAG |= 1u << 17;
    return 0x1ffff;
}

static void push_sz(int32_t z) { D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = z; }
static void push_sxy(int32_t x, int32_t y) {
    D[12] = D[13]; D[13] = D[14];
    D[14] = (x & 0xffff) | ((uint32_t)y << 16);
}
static void push_color(int32_t r, int32_t g, int32_t b) {
    uint8_t cd = (uint8_t)(D[6] >> 24);
    D[20] = D[21]; D[21] = D[22];
    D[22] = LmC(0, r) | (LmC(1, g) << 8) | (LmC(2, b) << 16) | ((uint32_t)cd << 24);
}

static void rtp(int n, int sf, int lm, int last) {
    int shift = sf ? 12 : 0;
    int64_t m1 = A(1, A(1, A(1, ((int64_t)TRX << 12) + RT(0, 0) * VX(n)) + RT(0, 1) * VY(n)) + RT(0, 2) * VZ(n));
    int64_t m2 = A(2, A(2, A(2, ((int64_t)TRY << 12) + RT(1, 0) * VX(n)) + RT(1, 1) * VY(n)) + RT(1, 2) * VZ(n));
    int64_t m3 = A(3, A(3, A(3, ((int64_t)TRZ << 12) + RT(2, 0) * VX(n)) + RT(2, 1) * VY(n)) + RT(2, 2) * VZ(n));
    MAC1 = (int32_t)(m1 >> shift); MAC2 = (int32_t)(m2 >> shift); MAC3 = (int32_t)(m3 >> shift);
    IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm);
    IR3 = LmB3_sf(MAC3, (int32_t)(m3 >> 12), lm);
    push_sz(LmD(m3 >> 12));
    uint32_t q = gte_divide(HREG, (uint16_t)D[19]);
    int64_t sx = F((int64_t)q * IR1 + OFX);
    int64_t sy = F((int64_t)q * IR2 + OFY);
    push_sxy(LmG(1, (int32_t)(sx >> 16)), LmG(2, (int32_t)(sy >> 16)));
    if (last) {
        int64_t dq = F((int64_t)q * DQA + DQB);
        MAC0 = (int32_t)dq;
        IR0 = LmE((int32_t)(dq >> 12));
    }
}

static void mat_vec(int m, int vsel, int tsel, int sf, int lm) {
    int shift = sf ? 12 : 0;
    int16_t vx, vy, vz;
    if (vsel < 3) { vx = VX(vsel); vy = VY(vsel); vz = VZ(vsel); }
    else { vx = (int16_t)IR1; vy = (int16_t)IR2; vz = (int16_t)IR3; }
    int64_t t[3] = {0, 0, 0};
    if (tsel == 0) { t[0] = TRX; t[1] = TRY; t[2] = TRZ; }
    else if (tsel == 1) { t[0] = RBK; t[1] = GBK; t[2] = BBK; }
    else if (tsel == 2) { t[0] = RFC; t[1] = GFC; t[2] = BFC; }
    for (int i = 0; i < 3; i++) {
        int16_t a, b, c;
        if (m == 0) { a = RT(i, 0); b = RT(i, 1); c = RT(i, 2); }
        else if (m == 1) { a = LL(i, 0); b = LL(i, 1); c = LL(i, 2); }
        else if (m == 2) { a = LC(i, 0); b = LC(i, 1); c = LC(i, 2); }
        else { a = b = c = 0; }
        int64_t s;
        if (tsel == 2) {
            /* buggy far-colour translation: only last two terms are kept */
            int64_t x = A(i + 1, (t[i] << 12) + a * vx);
            (void)LmB(i + 1, (int32_t)(x >> shift), 0);
            s = A(i + 1, A(i + 1, (int64_t)b * vy) + (int64_t)c * vz);
        } else {
            s = A(i + 1, A(i + 1, A(i + 1, (t[i] << 12) + a * vx) + (int64_t)b * vy) + (int64_t)c * vz);
        }
        D[25 + i] = (int32_t)(s >> shift);
        D[9 + i] = LmB(i + 1, D[25 + i], lm);
    }
}

/* lighting helpers */
static void light_stage(int vsel, int sf, int lm) { mat_vec(1, vsel, 3, sf, lm); }
static void color_stage(int sf, int lm) { mat_vec(2, 3, 1, sf, lm); }

static void depth_cue_from_mac(int sf, int lm) {
    int shift = sf ? 12 : 0;
    int32_t fc[3] = {RFC, GFC, BFC};
    int32_t m[3] = {MAC1, MAC2, MAC3};
    int32_t ir[3];
    for (int i = 0; i < 3; i++) {
        int64_t v = A(i + 1, ((int64_t)fc[i] << 12) - ((int64_t)m[i] << (sf ? 0 : 0)));
        ir[i] = LmB(i + 1, (int32_t)(v >> shift), 0);
    }
    for (int i = 0; i < 3; i++) {
        int64_t v = A(i + 1, (int64_t)ir[i] * IR0 + m[i]);
        D[25 + i] = (int32_t)(v >> shift);
    }
    for (int i = 0; i < 3; i++) D[9 + i] = LmB(i + 1, D[25 + i], lm);
    push_color(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void rgb_times_ir(void) {
    uint32_t rgb = (uint32_t)D[6];
    MAC1 = (int32_t)A(1, (int64_t)((rgb & 0xff) * IR1) << 4);
    MAC2 = (int32_t)A(2, (int64_t)(((rgb >> 8) & 0xff) * IR2) << 4);
    MAC3 = (int32_t)A(3, (int64_t)(((rgb >> 16) & 0xff) * IR3) << 4);
}

static void finish_color(int sf, int lm) {
    int shift = sf ? 12 : 0;
    MAC1 >>= shift; MAC2 >>= shift; MAC3 >>= shift;
    IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm);
    push_color(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void ncds(int n, int sf, int lm) {
    light_stage(n, sf, lm); color_stage(sf, lm);
    rgb_times_ir();
    depth_cue_from_mac(sf, lm);
}
static void nccs(int n, int sf, int lm) {
    light_stage(n, sf, lm); color_stage(sf, lm);
    rgb_times_ir();
    finish_color(sf, lm);
}
static void ncs(int n, int sf, int lm) {
    light_stage(n, sf, lm); color_stage(sf, lm);
    push_color(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

void gte_command(uint32_t cmd) {
    int op = cmd & 0x3f;
    int sf = (cmd >> 19) & 1, lm = (cmd >> 10) & 1;
    FLAG = 0;
    switch (op) {
    case 0x01: rtp(0, sf, lm, 1); break;
    case 0x30: rtp(0, sf, lm, 0); rtp(1, sf, lm, 0); rtp(2, sf, lm, 1); break;
    case 0x06: {
        int32_t x0 = (int16_t)D[12], y0 = (int16_t)(D[12] >> 16);
        int32_t x1 = (int16_t)D[13], y1 = (int16_t)(D[13] >> 16);
        int32_t x2 = (int16_t)D[14], y2 = (int16_t)(D[14] >> 16);
        MAC0 = (int32_t)F((int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1);
        break;
    }
    case 0x0c: {
        int shift = sf ? 12 : 0;
        int32_t d1 = RT(0, 0), d2 = RT(1, 1), d3 = RT(2, 2);
        MAC1 = (int32_t)(A(1, (int64_t)IR3 * d2 - (int64_t)IR2 * d3) >> shift);
        MAC2 = (int32_t)(A(2, (int64_t)IR1 * d3 - (int64_t)IR3 * d1) >> shift);
        MAC3 = (int32_t)(A(3, (int64_t)IR2 * d1 - (int64_t)IR1 * d2) >> shift);
        IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm);
        break;
    }
    case 0x10: case 0x2a: {      /* DPCS / DPCT */
        int iters = op == 0x2a ? 3 : 1;
        for (int k = 0; k < iters; k++) {
            uint32_t rgb = (uint32_t)(op == 0x2a ? D[20] : D[6]);
            MAC1 = (int32_t)((rgb & 0xff) << 16); MAC2 = (int32_t)(((rgb >> 8) & 0xff) << 16); MAC3 = (int32_t)(((rgb >> 16) & 0xff) << 16);
            depth_cue_from_mac(sf, lm);
        }
        break;
    }
    case 0x11: {                 /* INTPL */
        MAC1 = IR1 << 12; MAC2 = IR2 << 12; MAC3 = IR3 << 12;
        depth_cue_from_mac(sf, lm);
        break;
    }
    case 0x12: {                 /* MVMVA */
        int m = (cmd >> 17) & 3, v = (cmd >> 15) & 3, t = (cmd >> 13) & 3;
        mat_vec(m, v, t, sf, lm);
        break;
    }
    case 0x13: ncds(0, sf, lm); break;
    case 0x16: ncds(0, sf, lm); ncds(1, sf, lm); ncds(2, sf, lm); break;
    case 0x14: color_stage(sf, lm); rgb_times_ir(); depth_cue_from_mac(sf, lm); break;   /* CDP */
    case 0x1b: nccs(0, sf, lm); break;
    case 0x3f: nccs(0, sf, lm); nccs(1, sf, lm); nccs(2, sf, lm); break;
    case 0x1c: color_stage(sf, lm); rgb_times_ir(); finish_color(sf, lm); break;           /* CC */
    case 0x1e: ncs(0, sf, lm); break;
    case 0x20: ncs(0, sf, lm); ncs(1, sf, lm); ncs(2, sf, lm); break;
    case 0x28: {                 /* SQR */
        int shift = sf ? 12 : 0;
        MAC1 = (int32_t)(A(1, (int64_t)IR1 * IR1) >> shift);
        MAC2 = (int32_t)(A(2, (int64_t)IR2 * IR2) >> shift);
        MAC3 = (int32_t)(A(3, (int64_t)IR3 * IR3) >> shift);
        IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm);
        break;
    }
    case 0x29: rgb_times_ir(); depth_cue_from_mac(sf, lm); break;   /* DCPL */
    case 0x2d: {
        int64_t v = F((int64_t)ZSF3 * (D[17] + D[18] + D[19]));
        MAC0 = (int32_t)v; OTZ = LmD(v >> 12);
        break;
    }
    case 0x2e: {
        int64_t v = F((int64_t)ZSF4 * (D[16] + D[17] + D[18] + D[19]));
        MAC0 = (int32_t)v; OTZ = LmD(v >> 12);
        break;
    }
    case 0x3d: {                 /* GPF */
        int shift = sf ? 12 : 0;
        MAC1 = (int32_t)(A(1, (int64_t)IR1 * IR0) >> shift);
        MAC2 = (int32_t)(A(2, (int64_t)IR2 * IR0) >> shift);
        MAC3 = (int32_t)(A(3, (int64_t)IR3 * IR0) >> shift);
        IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm);
        push_color(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
        break;
    }
    case 0x3e: {                 /* GPL */
        int shift = sf ? 12 : 0;
        MAC1 = (int32_t)(A(1, ((int64_t)MAC1 << shift) + (int64_t)IR1 * IR0) >> shift);
        MAC2 = (int32_t)(A(2, ((int64_t)MAC2 << shift) + (int64_t)IR2 * IR0) >> shift);
        MAC3 = (int32_t)(A(3, ((int64_t)MAC3 << shift) + (int64_t)IR3 * IR0) >> shift);
        IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm);
        push_color(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
        break;
    }
    default:
        LOG(1, "GTE: unimplemented command %02x\n", op);
        break;
    }
    if (FLAG & 0x7f87e000) FLAG |= 0x80000000u;
}

uint32_t gte_read_data(int r) {
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (uint32_t)(int32_t)(int16_t)D[r];
    case 7: case 16: case 17: case 18: case 19:
        return (uint32_t)(uint16_t)D[r];
    case 15: return (uint32_t)D[14];
    case 28: case 29: {
        int r5 = IR1 >> 7, g5 = IR2 >> 7, b5 = IR3 >> 7;
        if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
        if (g5 < 0) g5 = 0; if (g5 > 31) g5 = 31;
        if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
        return (uint32_t)(r5 | (g5 << 5) | (b5 << 10));
    }
    case 31: {
        int32_t v = D[30];
        uint32_t x = (uint32_t)(v < 0 ? ~v : v);
        return x == 0 ? 32 : (uint32_t)__builtin_clz(x);
    }
    default: return (uint32_t)D[r];
    }
}

void gte_write_data(int r, uint32_t v) {
    switch (r) {
    case 15: D[12] = D[13]; D[13] = D[14]; D[14] = (int32_t)v; D[15] = (int32_t)v; break;
    case 28:
        D[28] = (int32_t)(v & 0x7fff);
        IR1 = (int32_t)((v & 31) << 7); IR2 = (int32_t)(((v >> 5) & 31) << 7); IR3 = (int32_t)(((v >> 10) & 31) << 7);
        break;
    case 29: case 31: break;
    default: D[r] = (int32_t)v; break;
    }
}

uint32_t gte_read_ctrl(int r) {
    if (r == 31) return FLAG;
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (uint32_t)(int32_t)(int16_t)C[r];
    }
    return (uint32_t)C[r];
}
void gte_write_ctrl(int r, uint32_t v) {
    if (r == 31) { FLAG = v & 0x7ffff000; if (FLAG & 0x7f87e000) FLAG |= 0x80000000u; return; }
    C[r] = (int32_t)v;
}
