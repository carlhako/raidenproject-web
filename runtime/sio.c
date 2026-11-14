/* SIO0 (controller port) emulation with a digital pad on port 1.
 * Memory cards are handled by the HLE BIOS, so card traffic on SIO gets no device. */
#include "psx.h"

static uint16_t sio_mode, sio_ctrl, sio_baud;
static uint32_t stat_bits;            /* bit1 RX ready, bit7 /ACK level, bit9 IRQ */
static uint8_t rx_fifo[8]; static int rx_n;
static int busy;                      /* byte transfer in flight */
static uint8_t pend_rx; static int pend_ack;
static int pad_step;
static uint16_t pad_latched;

#define BYTE_CYCLES 1088              /* 8 bits at 250 kHz */
#define ACK_DELAY   340

static void pad_exchange(uint8_t tx, uint8_t *rx, int *ack) {
    int port = (sio_ctrl >> 13) & 1;
    *rx = 0xff; *ack = 0;
    if (!(sio_ctrl & 2)) { pad_step = 0; return; }
    if (!platform_pad_present(port)) return;
    uint16_t buttons = platform_pad_buttons(port);
    switch (pad_step) {
    case 0:
        if (tx == 0x01) { LOG(4, "pad poll port %d buttons %04x\n", port, buttons); *rx = 0xff; *ack = 1; pad_step = 1; pad_latched = buttons; }
        break;
    case 1: *rx = 0x41; *ack = 1; pad_step = 2; break;   /* digital pad id, any command */
    case 2: *rx = 0x5a; *ack = 1; pad_step = 3; break;
    case 3: *rx = pad_latched & 0xff; *ack = 1; pad_step = 4; break;
    case 4: *rx = pad_latched >> 8; *ack = 0; pad_step = 5; break;
    default: break;
    }
}

void sio_event(void) {
    busy = 0;
    if (rx_n < 8) rx_fifo[rx_n++] = pend_rx;
    stat_bits |= 2;
    if (pend_ack) {
        stat_bits |= 0x80;
        if (sio_ctrl & 0x1000) { stat_bits |= 0x200; irq_raise(IRQ_PAD); }
    }
}

uint32_t sio_read(uint32_t off, int size) {
    switch (off) {
    case 0x40: {
        uint8_t v = 0xff;
        if (rx_n) { v = rx_fifo[0]; memmove(rx_fifo, rx_fifo + 1, (size_t)--rx_n); }
        if (!rx_n) stat_bits &= ~2u;
        return size == 1 ? v : (uint32_t)v | 0xffffff00u;
    }
    case 0x44: {
        uint32_t s = stat_bits | 1 | (busy ? 0 : 4);
        return s;
    }
    case 0x48: return sio_mode;
    case 0x4a: return sio_ctrl;
    case 0x4e: return sio_baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int size) {
    (void)size;
    switch (off) {
    case 0x40: {
        int ack;
        stat_bits &= ~0x80u;
        pad_exchange((uint8_t)v, &pend_rx, &ack);
        pend_ack = ack;
        busy = 1;
        sched_set(EV_SIO, cycles + BYTE_CYCLES + (ack ? ACK_DELAY : 0));
        break;
    }
    case 0x48: sio_mode = (uint16_t)v; break;
    case 0x4a:
        sio_ctrl = (uint16_t)v;
        if (v & 0x10) stat_bits &= ~(0x200u | 0x38u);
        if (v & 0x40) { stat_bits = 0; rx_n = 0; pad_step = 0; sio_ctrl = 0; busy = 0; }
        if (!(v & 2)) pad_step = 0;
        break;
    case 0x4e: sio_baud = (uint16_t)v; break;
    }
}
