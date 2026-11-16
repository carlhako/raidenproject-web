/* Browser front end: the wasm module runs in a Web Worker and never returns to the event
 * loop, so every host interaction (video, audio, pad input, pacing, netplay lock-step) goes
 * through JS imports defined in web/psx-lib.js. */
#include "psx.h"
#include <emscripten.h>

extern void js_frame(const uint32_t *px, int w, int h, int stride);
extern void js_audio(const int16_t *samples, int frames);
extern int js_pad(int port);            /* low 16 bits = active-low mask, bit 16 = pad present */
extern void js_init(void);

static uint32_t frame_rgba[640 * 512];
static uint16_t pad_cur[2] = {0xffff, 0xffff};
static int pad_pres[2] = {1, 0};

static void latch_pads(void) {
    for (int p = 0; p < 2; p++) {
        int v = js_pad(p);
        pad_cur[p] = (uint16_t)v;
        pad_pres[p] = (v >> 16) & 1;
    }
}

void platform_init(void) { js_init(); latch_pads(); }

static void convert_frame(int *pw, int *ph) {
    GpuDisplay d; gpu_get_display(&d);
    int fb_w = d.w > 640 ? 640 : d.w, fb_h = d.h > 512 ? 512 : d.h;
    for (int y = 0; y < fb_h; y++) {
        uint32_t *dst = &frame_rgba[y * 640];
        memset(dst, 0, (size_t)fb_w * 4);
        if (!d.enabled) continue;
        int vy = (d.y + y) & 511;
        if (d.depth24) {
            const uint8_t *row = (const uint8_t *)&vram[vy * 1024];
            for (int x = d.hoff; x < d.hoff + d.hvis; x++) {
                int bx = (d.x * 2 + (x - d.hoff) * 3) & 2047;
                dst[x] = 0xff000000u | ((uint32_t)row[(bx + 2) & 2047] << 16) | ((uint32_t)row[(bx + 1) & 2047] << 8) | row[bx];
            }
        } else {
            for (int x = d.hoff; x < d.hoff + d.hvis; x++) {
                uint16_t p = vram[vy * 1024 + ((d.x + x - d.hoff) & 1023)];
                uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                r |= r >> 5; g |= g >> 5; b |= b >> 5;
                dst[x] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
        }
    }
    *pw = fb_w; *ph = fb_h;
}

void platform_frame(void) {
    int w, h;
    convert_frame(&w, &h);
    js_frame(frame_rgba, w, h, 640);   /* publishes the frame, paces, and waits for remote input */
    latch_pads();
}

/* The empty asm after the call keeps clang from emitting `return_call` into the JS import:
 * Firefox crashed on that wasm->JS tail call (the stack ended in run_events with no frame above). */
void platform_audio(const int16_t *samples, int frames) { js_audio(samples, frames); __asm__ volatile("" ::: "memory"); }
uint16_t platform_pad_buttons(int port) { return port < 2 ? pad_cur[port] : 0xffff; }
int platform_pad_present(int port) { return port < 2 && pad_pres[port]; }

/* Deterministic digest of emulated state, used by the netplay desync check. */
EMSCRIPTEN_KEEPALIVE uint32_t psx_state_hash(void) {
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)ram;
    for (size_t i = 0; i < RAM_SIZE / 4; i++) h = (h ^ w[i]) * 16777619u;
    for (int i = 0; i < 32; i++) h = (h ^ R[i]) * 16777619u;
    h = (h ^ HI) * 16777619u; h = (h ^ LO) * 16777619u;
    h = (h ^ (uint32_t)cycles) * 16777619u;
    return h;
}
