/* Internal interfaces between runtime modules (hardware emulation + HLE BIOS). */
#pragma once
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define CPU_HZ 33868800
#define FRAME_HZ 60
#define FRAME_CYCLES (CPU_HZ / FRAME_HZ)  /* NTSC vblank period */
#define LINES_PER_FRAME 263
#define HBLANK_CYCLES (FRAME_CYCLES / LINES_PER_FRAME)

extern int log_level;
#define LOG(lvl, ...) do { if (log_level >= (lvl)) { fprintf(stderr, __VA_ARGS__); } } while (0)
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ---- scheduler ---- */
enum { EV_VBLANK, EV_CDROM, EV_CDREAD, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_AUDIO, EV_DMA, EV_SIO, EV_COUNT };
void sched_set(int ev, int64_t when);
void sched_clear(int ev);
int64_t sched_get(int ev);

/* ---- cop0 / interrupts ---- */
extern uint32_t cop0_sr, cop0_cause, cop0_epc;
extern uint32_t i_stat, i_mask;
enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
       IRQ_TMR2 = 6, IRQ_PAD = 7, IRQ_SIO = 8, IRQ_SPU = 9 };
void irq_raise(int n);
void check_irq(void);

/* ---- BIOS HLE ---- */
void bios_init(void);
void bios_call(uint32_t table, uint32_t fn);
void bios_exception(void);
void bios_vblank(void);
void bios_deliver_event(uint32_t cls, uint32_t spec);

/* ---- hardware ---- */
void hw_init(void);
void dma_write(uint32_t addr, uint32_t v);
void dma_event(void);
uint32_t dma_read(uint32_t addr);
void timers_write(uint32_t addr, uint32_t v);
uint32_t timers_read(uint32_t addr);
void timers_event(int n);
void timers_vblank(void);

void gpu_init(void);
void gpu_gp0(uint32_t v);
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);
extern uint16_t vram[1024 * 512];
typedef struct { int x, y, w, h; int depth24; int enabled; int hoff, hvis; } GpuDisplay;
void gpu_get_display(GpuDisplay *d);

void cd_init(const char *bin_path);
uint8_t cd_read_reg(int r);
void cd_write_reg(int r, uint8_t v);
void cd_event(void);
void cd_read_event(void);
uint32_t cd_dma_read_word(void);
int cd_xa_fetch(int16_t *l, int16_t *r); /* one 44.1kHz stereo sample of CD audio */

void sio_event(void);
uint32_t sio_read(uint32_t off, int size);
void sio_write(uint32_t off, uint32_t v, int size);

void spu_init(void);
uint16_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint16_t v);
void spu_dma_write(uint32_t word);
uint32_t spu_dma_read(void);
void spu_mix(int16_t *out, int frames);

void mdec_init(void);
void mdec_write(int reg, uint32_t v);
uint32_t mdec_read(int reg);
void mdec_dma_in(uint32_t word);
uint32_t mdec_dma_out(void);
int mdec_out_ready(void);

/* ---- platform (SDL) ---- */
void platform_init(void);
void platform_frame(void);        /* present + input + pacing, called each vblank */
void platform_audio(const int16_t *samples, int frames);
uint16_t platform_pad_buttons(int port); /* active-low PSX button mask */
int platform_pad_present(int port);
extern const char *data_dir;
