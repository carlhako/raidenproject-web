/* SDL2 front-end: video output, audio, input and frame pacing.
 * Headless test mode: PSX_HEADLESS=1, PSX_FRAMES=n (exit after n frames),
 * PSX_SHOT_EVERY=n (write shots/frame_XXXXX.png), PSX_INPUT="frame:buttons,..." scripted input. */
#include "psx.h"
#include <SDL.h>
#include <time.h>
#include <zlib.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *pads[2];
static int headless, max_frames, shot_every, turbo;
static uint64_t frame_no;
static uint32_t frame_rgba[640 * 512];
static int fb_w = 320, fb_h = 240;
static uint16_t kb_buttons = 0xffff;
static uint64_t t0_ns;

typedef struct { uint64_t frame; uint16_t mask; uint16_t hold; } ScriptEv;
static ScriptEv script[256]; static int nscript;

static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

static void parse_script(const char *s) {
    /* "120:start,200:cross,..." button names or hex masks; each press lasts 6 frames */
    static const struct { const char *n; uint16_t b; } names[] = {
        {"select", 1 << 0}, {"start", 1 << 3}, {"up", 1 << 4}, {"right", 1 << 5}, {"down", 1 << 6}, {"left", 1 << 7},
        {"l2", 1 << 8}, {"r2", 1 << 9}, {"l1", 1 << 10}, {"r1", 1 << 11}, {"triangle", 1 << 12}, {"circle", 1 << 13},
        {"cross", 1 << 14}, {"square", 1 << 15}};
    while (s && *s && nscript < 256) {
        char item[64]; int n = 0;
        while (*s && *s != ',' && n < 63) item[n++] = *s++;
        item[n] = 0; if (*s == ',') s++;
        char *colon = strchr(item, ':'); if (!colon) continue;
        *colon = 0;
        ScriptEv e = {strtoull(item, NULL, 10), 0, 6};
        char *nm = colon + 1;
        char *star = strchr(nm, '*'); if (star) { *star = 0; e.hold = (uint16_t)atoi(star + 1); }
        for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++) if (!strcmp(nm, names[k].n)) e.mask = names[k].b;
        if (!e.mask) e.mask = (uint16_t)strtoul(nm, NULL, 16);
        script[nscript++] = e;
    }
}

void platform_init(void) {
    headless = getenv("PSX_HEADLESS") != NULL;
    if (getenv("PSX_FRAMES")) max_frames = atoi(getenv("PSX_FRAMES"));
    if (getenv("PSX_SHOT_EVERY")) shot_every = atoi(getenv("PSX_SHOT_EVERY"));
    if (getenv("PSX_INPUT")) parse_script(getenv("PSX_INPUT"));
    if (getenv("PSX_TURBO")) turbo = 1;
    if (headless) { SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); SDL_setenv("SDL_AUDIODRIVER", "dummy", 1); turbo = 1; }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        if (SDL_Init(SDL_INIT_VIDEO) != 0) exit(1);
    }
    if (!headless) {
        win = SDL_CreateWindow("Raiden Project (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
        SDL_AudioSpec want = {0}, have;
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
        for (int i = 0; i < SDL_NumJoysticks() && i < 2; i++)
            if (SDL_IsGameController(i)) pads[i] = SDL_GameControllerOpen(i);
    }
    t0_ns = now_ns();
}

static void convert_frame(void) {
    GpuDisplay d; gpu_get_display(&d);
    fb_w = d.w; fb_h = d.h;
    if (fb_w > 640) fb_w = 640;
    if (fb_h > 512) fb_h = 512;
    for (int y = 0; y < fb_h; y++) {
        uint32_t *dst = &frame_rgba[y * 640];
        if (!d.enabled) { memset(dst, 0, fb_w * 4); continue; }
        int vy = (d.y + y) & 511;
        memset(dst, 0, fb_w * 4);
        if (d.depth24) {
            const uint8_t *row = (const uint8_t *)&vram[vy * 1024];
            for (int x = d.hoff; x < d.hoff + d.hvis; x++) {
                int bx = (d.x * 2 + (x - d.hoff) * 3) & 2047;
                uint8_t r = row[bx], g = row[(bx + 1) & 2047], b = row[(bx + 2) & 2047];
                dst[x] = 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
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
}

static void write_png(const char *path, const uint32_t *px, int w, int h, int stride) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    size_t rawsz = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(rawsz);
    for (int y = 0; y < h; y++) {
        raw[y * (w * 3 + 1)] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * stride + x];
            uint8_t *o = &raw[y * (w * 3 + 1) + 1 + x * 3];
            o[0] = p & 0xff; o[1] = (p >> 8) & 0xff; o[2] = (p >> 16) & 0xff;
        }
    }
    uLongf zl = compressBound(rawsz); uint8_t *z = malloc(zl);
    compress(z, &zl, raw, rawsz);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    #define CHUNK(type, data, len) do { uint8_t hdr[8]; uint32_t L = (uint32_t)(len); \
        hdr[0] = L >> 24; hdr[1] = L >> 16; hdr[2] = L >> 8; hdr[3] = L; memcpy(hdr + 4, type, 4); \
        fwrite(hdr, 1, 8, f); if (L) fwrite(data, 1, L, f); \
        uint32_t c = crc32(0, hdr + 4, 4); c = crc32(c, (const uint8_t *)(data), L); \
        uint8_t cb[4] = {c >> 24, c >> 16, c >> 8, c}; fwrite(cb, 1, 4, f); } while (0)
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    CHUNK("IHDR", ihdr, 13);
    CHUNK("IDAT", z, zl);
    CHUNK("IEND", "", 0);
    #undef CHUNK
    fclose(f); free(raw); free(z);
}

static void save_vram_png(const char *path) {
    static uint32_t buf[1024 * 512];
    for (int i = 0; i < 1024 * 512; i++) {
        uint16_t p = vram[i];
        uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
        buf[i] = (b << 16) | (g << 8) | r;
    }
    write_png(path, buf, 1024, 512, 1024);
}

static uint16_t script_buttons(void) {
    uint16_t pressed = 0;
    for (int i = 0; i < nscript; i++)
        if (frame_no >= script[i].frame && frame_no < script[i].frame + script[i].hold) pressed |= script[i].mask;
    return (uint16_t)~pressed;
}

static void handle_key(SDL_Keycode k, int down) {
    uint16_t bit = 0;
    switch (k) {
    case SDLK_UP: bit = 1 << 4; break;
    case SDLK_RIGHT: bit = 1 << 5; break;
    case SDLK_DOWN: bit = 1 << 6; break;
    case SDLK_LEFT: bit = 1 << 7; break;
    case SDLK_RETURN: bit = 1 << 3; break;
    case SDLK_BACKSPACE: case SDLK_RSHIFT: bit = 1 << 0; break;
    case SDLK_z: bit = 1 << 14; break;      /* Cross */
    case SDLK_x: bit = 1 << 13; break;      /* Circle */
    case SDLK_a: bit = 1 << 15; break;      /* Square */
    case SDLK_s: bit = 1 << 12; break;      /* Triangle */
    case SDLK_q: bit = 1 << 10; break;      /* L1 */
    case SDLK_w: bit = 1 << 11; break;      /* R1 */
    case SDLK_e: bit = 1 << 8; break;       /* L2 */
    case SDLK_r: bit = 1 << 9; break;       /* R2 */
    case SDLK_TAB: turbo = down; return;
    case SDLK_F12: if (down) { save_vram_png("vram.png"); fprintf(stderr, "saved vram.png\n"); } return;
    case SDLK_F11: if (down) { Uint32 fl = SDL_GetWindowFlags(win); SDL_SetWindowFullscreen(win, (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); } return;
    case SDLK_ESCAPE: if (down) exit(0); return;
    default: return;
    }
    if (down) kb_buttons &= ~bit; else kb_buttons |= bit;
}

static uint16_t gamepad_buttons(SDL_GameController *gc) {
    uint16_t b = 0xffff;
    if (!gc) return b;
    static const struct { int sdl; uint16_t bit; } m[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 1 << 4}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1 << 5},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1 << 6}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1 << 7},
        {SDL_CONTROLLER_BUTTON_START, 1 << 3}, {SDL_CONTROLLER_BUTTON_BACK, 1 << 0},
        {SDL_CONTROLLER_BUTTON_A, 1 << 14}, {SDL_CONTROLLER_BUTTON_B, 1 << 13},
        {SDL_CONTROLLER_BUTTON_X, 1 << 15}, {SDL_CONTROLLER_BUTTON_Y, 1 << 12},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1 << 10}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1 << 11}};
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if (SDL_GameControllerGetButton(gc, m[i].sdl)) b &= ~m[i].bit;
    int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax < -16000) b &= ~(1 << 7); if (ax > 16000) b &= ~(1 << 5);
    if (ay < -16000) b &= ~(1 << 4); if (ay > 16000) b &= ~(1 << 6);
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b &= ~(1 << 8);
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b &= ~(1 << 9);
    return b;
}

/* Port 1: keyboard + first gamepad + scripted input.  Port 2: second gamepad if present. */
uint16_t platform_pad_buttons(int port) {
    if (port == 0) return kb_buttons & script_buttons() & gamepad_buttons(pads[0]);
    if (port == 1 && pads[1]) return gamepad_buttons(pads[1]);
    return 0xffff;
}

int platform_pad_present(int port) { return port == 0 || pads[1] != NULL; }

static FILE *wav;
static uint32_t wav_bytes;
static void wav_header(FILE *f, uint32_t n) {
    uint8_t h[44]; uint32_t v;
    memcpy(h, "RIFF", 4); v = 36 + n; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); uint16_t s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2);
    v = 44100; memcpy(h + 24, &v, 4); v = 44100 * 4; memcpy(h + 28, &v, 4); s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &n, 4);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}
static void wav_close(void) { if (wav) { wav_header(wav, wav_bytes); fclose(wav); wav = NULL; } }

void platform_audio(const int16_t *samples, int frames) {
    if (getenv("PSX_WAV")) {
        if (!wav) { wav = fopen(getenv("PSX_WAV"), "wb"); if (wav) { wav_header(wav, 0); atexit(wav_close); } }
        if (wav) { fwrite(samples, 4, (size_t)frames, wav); wav_bytes += (uint32_t)frames * 4; }
    }
    if (!audio_dev || turbo) return;
    Uint32 queued = SDL_GetQueuedAudioSize(audio_dev);
    if (queued > 44100 * 4 / 5) return;          /* >200ms queued: drop to resync */
    SDL_QueueAudio(audio_dev, samples, (Uint32)frames * 4);
}

void platform_frame(void) {
    frame_no++;
    convert_frame();
    if (!headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) exit(0);
            if (e.type == SDL_KEYDOWN && !e.key.repeat) handle_key(e.key.keysym.sym, 1);
            if (e.type == SDL_KEYUP) handle_key(e.key.keysym.sym, 0);
            if (e.type == SDL_CONTROLLERDEVICEADDED && !pads[0]) pads[0] = SDL_GameControllerOpen(e.cdevice.which);
        }
        SDL_UpdateTexture(tex, NULL, frame_rgba, 640 * 4);
        int ww, wh; SDL_GetRendererOutputSize(ren, &ww, &wh);
        int dw = ww, dh = ww * 3 / 4;
        if (dh > wh) { dh = wh; dw = wh * 4 / 3; }
        SDL_Rect src = {0, 0, fb_w, fb_h}, dst = {(ww - dw) / 2, (wh - dh) / 2, dw, dh};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, &dst);
        SDL_RenderPresent(ren);
    }
    static const char *shot_list; static int shot_list_init;
    if (!shot_list_init) { shot_list_init = 1; shot_list = getenv("PSX_SHOT_FRAMES"); }
    int listed = 0;
    if (shot_list) {                           /* PSX_SHOT_FRAMES="600,1000,..." saves exactly those frames */
        for (const char *q = shot_list; *q;) {
            if ((uint64_t)strtoull(q, (char **)&q, 10) == frame_no) listed = 1;
            while (*q == ',') q++;
            if (*q && (*q < '0' || *q > '9')) break;
        }
    }
    if (listed || (shot_every && frame_no % shot_every == 0)) {
        char p[256]; snprintf(p, sizeof p, "shots/frame_%05llu.png", (unsigned long long)frame_no);
        write_png(p, frame_rgba, fb_w, fb_h, 640);
        if (getenv("PSX_VRAM_SHOTS")) { snprintf(p, sizeof p, "shots/vram_%05llu.png", (unsigned long long)frame_no); save_vram_png(p); }
    }
    if (getenv("PSX_DUMPRAM") && frame_no == strtoull(getenv("PSX_DUMPRAM"), NULL, 10)) {
        FILE *f = fopen("shots/ram.bin", "wb"); if (f) { fwrite(ram, 1, RAM_SIZE, f); fclose(f); }
        fprintf(stderr, "dumped RAM at frame %llu\n", (unsigned long long)frame_no);
    }
    if (max_frames && frame_no >= (uint64_t)max_frames) {
        fprintf(stderr, "frame limit reached (%llu frames)\n", (unsigned long long)frame_no);
        exit(0);
    }
    if (!turbo) {
        uint64_t target = t0_ns + frame_no * 16666667ull;    /* 60 Hz NTSC */
        uint64_t now = now_ns();
        if (now < target) {
            uint64_t d = target - now;
            struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
            nanosleep(&ts, NULL);
        } else if (now - target > 100000000ull) {
            t0_ns = now - frame_no * 16666667ull;            /* fell behind: resync */
        }
    }
}
