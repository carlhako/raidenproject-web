/* PSX GPU emulation: GP0 command processing with a software rasteriser. */
#include "psx.h"

uint16_t vram[1024 * 512];

/* draw state */
static int tp_x, tp_y, tp_abr, tp_depth, dither, draw_to_disp, tex_disable;
static int rect_flip_x, rect_flip_y;
static int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static int da_x1, da_y1, da_x2, da_y2;
static int off_x, off_y;
static int mask_set, mask_check;
/* display state */
static int disp_x, disp_y, disp_enabled = 0, dma_dir;
static int h_range1 = 0x260, h_range2 = 0xc60, v_range1 = 0x23, v_range2 = 0x123;
static uint32_t disp_mode;
static int irq_flag;
static uint32_t gpuread_latch;
static int field;

/* command fifo */
static uint32_t cmd[16];
static int cmd_len, cmd_need;
static int poly_line, poly_line_gouraud;
/* transfers */
static int xfer_in, xfer_out;
static int xf_x, xf_y, xf_w, xf_h, xf_i, xf_n;

void gpu_init(void) {
    memset(vram, 0, sizeof vram);
    da_x2 = 1023; da_y2 = 511;
}

static inline int sext11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

/* ---------------------------------------------------------------- pixel pipeline */
typedef struct {
    int textured, raw, semi, gouraud;
    int clut_x, clut_y, page_x, page_y, depth, abr;
} DrawCtx;

static inline uint16_t tex_fetch(const DrawCtx *c, int u, int v) {
    u &= 0xff; v &= 0xff;
    u = (u & ~(tw_mask_x * 8)) | ((tw_off_x & tw_mask_x) * 8);
    v = (v & ~(tw_mask_y * 8)) | ((tw_off_y & tw_mask_y) * 8);
    int ty = (c->page_y + v) & 511;
    switch (c->depth) {
    case 0: {
        uint16_t w = vram[ty * 1024 + ((c->page_x + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 0xf;
        return vram[c->clut_y * 1024 + ((c->clut_x + idx) & 1023)];
    }
    case 1: {
        uint16_t w = vram[ty * 1024 + ((c->page_x + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 0xff;
        return vram[c->clut_y * 1024 + ((c->clut_x + idx) & 1023)];
    }
    default:
        return vram[ty * 1024 + ((c->page_x + u) & 1023)];
    }
}

static inline void plot(const DrawCtx *c, int x, int y, int r, int g, int b, int u, int v) {
    uint16_t *dst = &vram[y * 1024 + x];
    if (mask_check && (*dst & 0x8000)) return;
    int pr, pg, pb, stp;
    if (c->textured) {
        uint16_t t = tex_fetch(c, u, v);
        if (t == 0) return;
        stp = t >> 15;
        pr = t & 31; pg = (t >> 5) & 31; pb = (t >> 10) & 31;
        if (!c->raw) {
            pr = (pr * r) >> 7; pg = (pg * g) >> 7; pb = (pb * b) >> 7;
            if (pr > 31) pr = 31; if (pg > 31) pg = 31; if (pb > 31) pb = 31;
        }
    } else {
        stp = 1;
        pr = r >> 3; pg = g >> 3; pb = b >> 3;
    }
    if (c->semi && stp) {
        int br = *dst & 31, bg = (*dst >> 5) & 31, bb = (*dst >> 10) & 31;
        switch (c->abr) {
        case 0: pr = (br + pr) >> 1; pg = (bg + pg) >> 1; pb = (bb + pb) >> 1; break;
        case 1: pr += br; pg += bg; pb += bb; break;
        case 2: pr = br - pr; pg = bg - pg; pb = bb - pb; break;
        case 3: pr = br + (pr >> 2); pg = bg + (pg >> 2); pb = bb + (pb >> 2); break;
        }
        if (pr < 0) pr = 0; if (pg < 0) pg = 0; if (pb < 0) pb = 0;
        if (pr > 31) pr = 31; if (pg > 31) pg = 31; if (pb > 31) pb = 31;
    }
    *dst = (uint16_t)(pr | (pg << 5) | (pb << 10) | ((c->textured ? stp : 0) << 15) | (mask_set << 15));
}

/* ---------------------------------------------------------------- triangles */
typedef struct { int x, y, r, g, b, u, v; } Vtx;

static void draw_tri(const DrawCtx *c, Vtx a, Vtx b, Vtx d) {
    /* reject oversized primitives like the hardware */
    int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
    if (b.x < minx) minx = b.x; if (b.x > maxx) maxx = b.x;
    if (d.x < minx) minx = d.x; if (d.x > maxx) maxx = d.x;
    if (b.y < miny) miny = b.y; if (b.y > maxy) maxy = b.y;
    if (d.y < miny) miny = d.y; if (d.y > maxy) maxy = d.y;
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    int64_t area = (int64_t)(b.x - a.x) * (d.y - a.y) - (int64_t)(b.y - a.y) * (d.x - a.x);
    if (area == 0) return;
    if (area < 0) { Vtx t = b; b = d; d = t; area = -area; }
    if (minx < da_x1) minx = da_x1; if (maxx > da_x2) maxx = da_x2;
    if (miny < da_y1) miny = da_y1; if (maxy > da_y2) maxy = da_y2;
    if (minx > maxx || miny > maxy) return;
    /* edge functions: E(p) = (v1-v0) x (p-v0); top-left fill rule */
    #define EDGE(v0, v1, px, py) ((int64_t)((v1).x - (v0).x) * ((py) - (v0).y) - (int64_t)((v1).y - (v0).y) * ((px) - (v0).x))
    #define TOPLEFT(v0, v1) (((v0).y == (v1).y && (v1).x < (v0).x) || ((v1).y < (v0).y))
    int bias0 = TOPLEFT(b, d) ? 0 : -1;
    int bias1 = TOPLEFT(d, a) ? 0 : -1;
    int bias2 = TOPLEFT(a, b) ? 0 : -1;
    double inv = 1.0 / (double)area;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int64_t w0 = EDGE(b, d, x, y) + bias0;
            int64_t w1 = EDGE(d, a, x, y) + bias1;
            int64_t w2 = EDGE(a, b, x, y) + bias2;
            if ((w0 | w1 | w2) < 0) continue;
            double l0 = (double)(w0 - bias0) * inv, l1 = (double)(w1 - bias1) * inv, l2 = (double)(w2 - bias2) * inv;
            int r = a.r, g = a.g, bl = a.b, u = 0, v = 0;
            if (c->gouraud) {
                r = (int)(a.r * l0 + b.r * l1 + d.r * l2 + 0.5);
                g = (int)(a.g * l0 + b.g * l1 + d.g * l2 + 0.5);
                bl = (int)(a.b * l0 + b.b * l1 + d.b * l2 + 0.5);
            }
            if (c->textured) {
                u = (int)(a.u * l0 + b.u * l1 + d.u * l2 + 0.5);
                v = (int)(a.v * l0 + b.v * l1 + d.v * l2 + 0.5);
            }
            plot(c, x, y, r, g, bl, u, v);
        }
    }
    #undef EDGE
    #undef TOPLEFT
}

static void cmd_polygon(void) {
    uint32_t op = cmd[0] >> 24;
    int gouraud = (op >> 4) & 1, quad = (op >> 3) & 1, textured = (op >> 2) & 1;
    DrawCtx c = {0};
    c.textured = textured && !tex_disable; c.raw = op & 1; c.semi = (op >> 1) & 1; c.gouraud = gouraud;
    c.abr = tp_abr; c.depth = tp_depth; c.page_x = tp_x; c.page_y = tp_y;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    int k = 0;
    uint32_t col = cmd[k++] & 0xffffff;
    for (int i = 0; i < nv; i++) {
        if (i > 0 && gouraud) col = cmd[k++] & 0xffffff;
        uint32_t xy = cmd[k++];
        v[i].x = sext11(xy) + off_x; v[i].y = sext11(xy >> 16) + off_y;
        v[i].r = col & 0xff; v[i].g = (col >> 8) & 0xff; v[i].b = (col >> 16) & 0xff;
        v[i].u = v[i].v = 0;
        if (textured) {
            uint32_t uv = cmd[k++];
            v[i].u = uv & 0xff; v[i].v = (uv >> 8) & 0xff;
            if (i == 0) { c.clut_x = ((uv >> 16) & 0x3f) * 16; c.clut_y = (uv >> 22) & 0x1ff; }
            if (i == 1) {
                uint32_t tp = uv >> 16;
                tp_x = (tp & 15) * 64; tp_y = ((tp >> 4) & 1) * 256; tp_abr = (tp >> 5) & 3;
                tp_depth = (tp >> 7) & 3; if (tp_depth == 3) tp_depth = 2;
                tex_disable = 0;
                c.page_x = tp_x; c.page_y = tp_y; c.abr = tp_abr; c.depth = tp_depth;
            }
        }
    }
    if (!gouraud) for (int i = 1; i < nv; i++) { v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b; }
    draw_tri(&c, v[0], v[1], v[2]);
    if (quad) draw_tri(&c, v[1], v[2], v[3]);
}

/* ---------------------------------------------------------------- rectangles */
static void cmd_rect(void) {
    uint32_t op = cmd[0] >> 24;
    int size = (op >> 3) & 3, textured = (op >> 2) & 1;
    DrawCtx c = {0};
    c.textured = textured && !tex_disable; c.raw = op & 1; c.semi = (op >> 1) & 1;
    c.abr = tp_abr; c.depth = tp_depth; c.page_x = tp_x; c.page_y = tp_y;
    int k = 1;
    uint32_t xy = cmd[k++];
    int x0 = sext11(xy) + off_x, y0 = sext11(xy >> 16) + off_y;
    int u0 = 0, v0 = 0;
    if (textured) {
        uint32_t uv = cmd[k++];
        u0 = uv & 0xff; v0 = (uv >> 8) & 0xff;
        c.clut_x = ((uv >> 16) & 0x3f) * 16; c.clut_y = (uv >> 22) & 0x1ff;
    }
    int w, h;
    switch (size) {
    case 0: { uint32_t wh = cmd[k++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; break; }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    int r = cmd[0] & 0xff, g = (cmd[0] >> 8) & 0xff, b = (cmd[0] >> 16) & 0xff;
    for (int y = 0; y < h; y++) {
        int py = y0 + y;
        if (py < da_y1 || py > da_y2) continue;
        int tv = rect_flip_y ? v0 - y : v0 + y;
        for (int x = 0; x < w; x++) {
            int px = x0 + x;
            if (px < da_x1 || px > da_x2) continue;
            int tu = rect_flip_x ? u0 - x : u0 + x;
            plot(&c, px, py, r, g, b, tu, tv);
        }
    }
}

/* ---------------------------------------------------------------- lines */
static void draw_line(int semi, Vtx a, Vtx b, int gouraud) {
    DrawCtx c = {0};
    c.semi = semi; c.abr = tp_abr;
    int dx = b.x - a.x, dy = b.y - a.y;
    if (abs(dx) >= 1024 || abs(dy) >= 512) return;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (dx * i + (dx >= 0 ? n / 2 : -n / 2)) / n : a.x;
        int y = n ? a.y + (dy * i + (dy >= 0 ? n / 2 : -n / 2)) / n : a.y;
        if (x < da_x1 || x > da_x2 || y < da_y1 || y > da_y2) continue;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) { r = a.r + (b.r - a.r) * i / n; g = a.g + (b.g - a.g) * i / n; bl = a.b + (b.b - a.b) * i / n; }
        plot(&c, x, y, r, g, bl, 0, 0);
    }
}

static Vtx line_prev;
static uint32_t line_col;
static void cmd_line(void) {
    uint32_t op = cmd[0] >> 24;
    int gouraud = (op >> 4) & 1;
    Vtx v[2];
    int k = 0;
    uint32_t col = cmd[k++] & 0xffffff;
    for (int i = 0; i < 2; i++) {
        if (i > 0 && gouraud) col = cmd[k++] & 0xffffff;
        uint32_t xy = cmd[k++];
        v[i].x = sext11(xy) + off_x; v[i].y = sext11(xy >> 16) + off_y;
        v[i].r = col & 0xff; v[i].g = (col >> 8) & 0xff; v[i].b = (col >> 16) & 0xff;
    }
    draw_line((op >> 1) & 1, v[0], v[1], gouraud);
    line_prev = v[1]; line_col = col;
}

/* ---------------------------------------------------------------- vram ops */
static void cmd_fill(void) {
    uint32_t c = cmd[0];
    uint16_t px = (uint16_t)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
    int x0 = cmd[1] & 0x3f0, y0 = (cmd[1] >> 16) & 0x1ff;
    int w = ((cmd[2] & 0x3ff) + 15) & ~15, h = (cmd[2] >> 16) & 0x1ff;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            vram[((y0 + y) & 511) * 1024 + ((x0 + x) & 1023)] = px;
}
static void cmd_copy(void) {
    int sx = cmd[1] & 0x3ff, sy = (cmd[1] >> 16) & 0x1ff;
    int dx = cmd[2] & 0x3ff, dy = (cmd[2] >> 16) & 0x1ff;
    int w = cmd[3] & 0x3ff, h = (cmd[3] >> 16) & 0x1ff;
    if (!w) w = 0x400; if (!h) h = 0x200;
    static uint16_t line[1024];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) line[x] = vram[((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
        for (int x = 0; x < w; x++) {
            uint16_t *d = &vram[((dy + y) & 511) * 1024 + ((dx + x) & 1023)];
            if (mask_check && (*d & 0x8000)) continue;
            *d = line[x] | (mask_set << 15);
        }
    }
}

static void xfer_setup(void) {
    xf_x = cmd[1] & 0x3ff; xf_y = (cmd[1] >> 16) & 0x1ff;
    xf_w = cmd[2] & 0x3ff; xf_h = (cmd[2] >> 16) & 0x1ff;
    if (!xf_w) xf_w = 0x400; if (!xf_h) xf_h = 0x200;
    xf_i = 0; xf_n = xf_w * xf_h;
}
static void xfer_pixel(uint16_t p) {
    int x = xf_x + xf_i % xf_w, y = xf_y + xf_i / xf_w;
    uint16_t *d = &vram[(y & 511) * 1024 + (x & 1023)];
    if (!(mask_check && (*d & 0x8000))) *d = p | (mask_set << 15);
    xf_i++;
}

/* ---------------------------------------------------------------- GP0 */
static int cmd_length(uint32_t op) {
    if (op >= 0x20 && op < 0x40) {
        int g = (op >> 4) & 1, q = (op >> 3) & 1, t = (op >> 2) & 1;
        int nv = q ? 4 : 3;
        return 1 + nv * (1 + t) + (g ? nv - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) { int g = (op >> 4) & 1; return g ? 4 : 3; }
    if (op >= 0x60 && op < 0x80) {
        int sz = (op >> 3) & 3, t = (op >> 2) & 1;
        return 2 + t + (sz == 0);
    }
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xa0) return 4;
    if (op >= 0xa0 && op < 0xe0) return 3;
    return 1;
}

static void poly_line_word(uint32_t v) {
    if ((v & 0xf000f000) == 0x50005000) { poly_line = 0; cmd_len = 0; return; }
    cmd[cmd_len++] = v;
    int need = poly_line_gouraud ? 2 : 1;
    if (cmd_len < need) return;
    Vtx b;
    uint32_t col = line_col, xy;
    if (poly_line_gouraud) { col = cmd[0] & 0xffffff; xy = cmd[1]; } else xy = cmd[0];
    b.x = sext11(xy) + off_x; b.y = sext11(xy >> 16) + off_y;
    b.r = col & 0xff; b.g = (col >> 8) & 0xff; b.b = (col >> 16) & 0xff;
    draw_line(0, line_prev, b, poly_line_gouraud);
    line_prev = b; line_col = col;
    cmd_len = 0;
}

void gpu_gp0(uint32_t v) {
    if (xfer_in) {
        xfer_pixel(v & 0xffff);
        if (xf_i < xf_n) xfer_pixel(v >> 16);
        if (xf_i >= xf_n) xfer_in = 0;
        return;
    }
    if (poly_line) { poly_line_word(v); return; }
    if (cmd_len == 0) {
        uint32_t op = v >> 24;
        cmd_need = cmd_length(op);
    }
    cmd[cmd_len++] = v;
    if (cmd_len < cmd_need) return;
    cmd_len = 0;
    uint32_t op = cmd[0] >> 24;
    if (op >= 0x20 && op < 0x40) cmd_polygon();
    else if (op >= 0x40 && op < 0x60) {
        cmd_line();
        if (op & 8) { poly_line = 1; poly_line_gouraud = (op >> 4) & 1; }
    }
    else if (op >= 0x60 && op < 0x80) cmd_rect();
    else if (op == 0x02) cmd_fill();
    else if (op >= 0x80 && op < 0xa0) cmd_copy();
    else if (op >= 0xa0 && op < 0xc0) { xfer_setup(); xfer_in = 1; }
    else if (op >= 0xc0 && op < 0xe0) { xfer_setup(); xfer_out = 1; }
    else switch (op) {
    case 0x00: case 0x01: break;
    case 0x1f: irq_flag = 1; irq_raise(IRQ_GPU); break;
    case 0xe1:
        tp_x = (v & 15) * 64; tp_y = ((v >> 4) & 1) * 256; tp_abr = (v >> 5) & 3;
        tp_depth = (v >> 7) & 3; if (tp_depth == 3) tp_depth = 2;
        dither = (v >> 9) & 1; draw_to_disp = (v >> 10) & 1; tex_disable = (v >> 11) & 1;
        rect_flip_x = (v >> 12) & 1; rect_flip_y = (v >> 13) & 1;
        break;
    case 0xe2: tw_mask_x = v & 31; tw_mask_y = (v >> 5) & 31; tw_off_x = (v >> 10) & 31; tw_off_y = (v >> 15) & 31; break;
    case 0xe3: da_x1 = v & 0x3ff; da_y1 = (v >> 10) & 0x1ff; break;
    case 0xe4: da_x2 = v & 0x3ff; da_y2 = (v >> 10) & 0x1ff; break;
    case 0xe5: off_x = sext11(v); off_y = sext11(v >> 11); break;
    case 0xe6: mask_set = v & 1; mask_check = (v >> 1) & 1; break;
    default: LOG(2, "GP0 unknown %08x\n", v); break;
    }
}

uint32_t gpu_read(void) {
    if (xfer_out) {
        uint32_t lo = 0, hi = 0;
        int x = xf_x + xf_i % xf_w, y = xf_y + xf_i / xf_w;
        lo = vram[(y & 511) * 1024 + (x & 1023)]; xf_i++;
        x = xf_x + xf_i % xf_w; y = xf_y + xf_i / xf_w;
        hi = vram[(y & 511) * 1024 + (x & 1023)]; xf_i++;
        if (xf_i >= xf_n) xfer_out = 0;
        return lo | (hi << 16);
    }
    return gpuread_latch;
}

void gpu_gp1(uint32_t v) {
    uint32_t op = (v >> 24) & 0x3f;
    switch (op) {
    case 0x00:
        cmd_len = 0; xfer_in = xfer_out = poly_line = 0; irq_flag = 0; disp_enabled = 0; dma_dir = 0;
        tp_x = tp_y = tp_abr = tp_depth = dither = draw_to_disp = tex_disable = 0;
        rect_flip_x = rect_flip_y = 0; tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0;
        da_x1 = da_y1 = 0; da_x2 = 1023; da_y2 = 511; off_x = off_y = 0; mask_set = mask_check = 0;
        disp_x = disp_y = 0; disp_mode = 0;
        break;
    case 0x01: cmd_len = 0; xfer_in = poly_line = 0; break;
    case 0x02: irq_flag = 0; break;
    case 0x03: disp_enabled = !(v & 1); break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05: disp_x = v & 0x3fe; disp_y = (v >> 10) & 0x1ff; break;
    case 0x06: h_range1 = v & 0xfff; h_range2 = (v >> 12) & 0xfff; break;
    case 0x07: v_range1 = v & 0x3ff; v_range2 = (v >> 10) & 0x3ff; break;
    case 0x08: disp_mode = v & 0xff; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        switch (v & 7) {
        case 2: gpuread_latch = tw_mask_x | (tw_mask_y << 5) | (tw_off_x << 10) | (tw_off_y << 15); break;
        case 3: gpuread_latch = da_x1 | (da_y1 << 10); break;
        case 4: gpuread_latch = da_x2 | (da_y2 << 10); break;
        case 5: gpuread_latch = (off_x & 0x7ff) | ((off_y & 0x7ff) << 11); break;
        case 7: gpuread_latch = 2; break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_stat(void) {
    uint32_t s = 0;
    s |= (tp_x / 64) | ((tp_y / 256) << 4) | (tp_abr << 5) | (tp_depth << 7);
    s |= dither << 9 | draw_to_disp << 10 | mask_set << 11 | mask_check << 12;
    s |= tex_disable << 15;
    s |= ((disp_mode >> 6) & 1) << 16;
    s |= (disp_mode & 3) << 17;
    s |= ((disp_mode >> 2) & 1) << 19;
    s |= ((disp_mode >> 3) & 1) << 20;
    s |= ((disp_mode >> 4) & 1) << 21;
    s |= ((disp_mode >> 5) & 1) << 22;
    s |= (!disp_enabled) << 23;
    s |= irq_flag << 24;
    s |= 1u << 26 | 1u << 28;
    if (xfer_out) s |= 1u << 27;
    s |= (uint32_t)dma_dir << 29;
    switch (dma_dir) { case 1: s |= 1u << 25; break; case 2: s |= 1u << 25; break; case 3: if (xfer_out) s |= 1u << 25; break; }
    int interlaced = (disp_mode >> 5) & 1;
    int odd = interlaced ? field : (int)((cycles / HBLANK_CYCLES) & 1);
    s |= (uint32_t)odd << 31;
    if (interlaced && field) s |= 1u << 13;
    return s;
}

void gpu_vblank(void) { field ^= 1; }

void gpu_get_display(GpuDisplay *d) {
    static const int widths[4] = {256, 320, 512, 640};
    d->w = (disp_mode & 0x40) ? 368 : widths[disp_mode & 3];
    int lines = v_range2 - v_range1;
    if (lines <= 0 || lines > 300) lines = 240;
    if ((disp_mode & 0x24) == 0x24) lines *= 2;
    d->h = lines;
    d->x = disp_x; d->y = disp_y;
    d->depth24 = (disp_mode >> 4) & 1;
    d->enabled = disp_enabled;
    /* GP1(06h) horizontal display range: only x1..x2 (GPU clocks) is drawn, the rest of the line is blank */
    int div = (disp_mode & 0x40) ? 7 : (int[]){10, 8, 5, 4}[disp_mode & 3];
    int hoff = (h_range1 - 0x260) / div, hvis = (h_range2 - h_range1) / div;
    if (hoff < 0) hoff = 0;
    if (hvis <= 0 || hvis > d->w) hvis = d->w;
    if (hoff + hvis > d->w) hvis = d->w - hoff;
    d->hoff = hoff; d->hvis = hvis;
}
