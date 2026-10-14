// SI: serial interface (controller ports).  Port 1 has a standard GameCube
// controller fed from host input; the other ports are empty.
#include "hw.h"

struct PadState { u16 buttons; u8 sx, sy, cx, cy, l, r; bool connected; };
PadState host_get_pad(int port);   // host/input.cpp

enum : u32 { SIPOLL = 0x30, SICOMCSR = 0x34, SISR = 0x38, SIEXILK = 0x3C };
enum : u32 {
    COMCSR_TCINT = 1u << 31, COMCSR_TCINTMSK = 1u << 30, COMCSR_COMERR = 1u << 29, COMCSR_RDSTINT = 1u << 28,
    COMCSR_RDSTINTMSK = 1u << 27, COMCSR_TSTART = 1u,
};
enum : u32 { SR_UNRUN = 1, SR_OVRUN = 2, SR_COLL = 4, SR_NOREP = 8, SR_WRST = 0x10, SR_RDST = 0x20 };

static u32 s_out[4], s_inh[4], s_inl[4];
static u32 s_poll, s_comcsr, s_sr, s_exilk;
static u8 s_buf[128];

static u32 sr_shift(int ch) { return (3 - ch) * 8; }

static void update_irq() {
    bool rdst = false;
    for (int ch = 0; ch < 4; ch++) if (s_sr & (SR_RDST << sr_shift(ch))) rdst = true;
    if (rdst) s_comcsr |= COMCSR_RDSTINT; else s_comcsr &= ~COMCSR_RDSTINT;
    bool on = ((s_comcsr & COMCSR_TCINT) && (s_comcsr & COMCSR_TCINTMSK)) ||
              ((s_comcsr & COMCSR_RDSTINT) && (s_comcsr & COMCSR_RDSTINTMSK));
    pi_set_irq(PI_IRQ_SI, on);
}

static void pad_status(int ch, u32* hi, u32* lo) {
    PadState p = host_get_pad(ch);
    u16 b = p.buttons | 0x0080;   // PAD_USE_ORIGIN
    *hi = ((u32)b << 16) | ((u32)p.sx << 8) | p.sy;
    *lo = ((u32)p.cx << 24) | ((u32)p.cy << 16) | ((u32)p.l << 8) | p.r;
}

// Output word / poll command 0x40 0x03 MM: MM bits 0-1 are the rumble motor (0 stop, 1 on, 2 hard stop)
static void pad_command(int ch, u32 cmd) {
    if ((cmd >> 16 & 0xFF) == 0x40) host_set_rumble(ch, (cmd & 3) == 1);
}

// run one command on a device; returns response length or -1 for no device
static int device_transfer(int ch, const u8* in, int inlen, u8* out) {
    if (!host_get_pad(ch).connected) return -1;
    switch (in[0]) {
    case 0x00: case 0xFF:   // reset / identify: standard controller
        out[0] = 0x09; out[1] = 0x00; out[2] = 0x00;
        return 3;
    case 0x40: {            // direct status
        pad_command(ch, (u32)in[0] << 16 | (u32)in[1] << 8 | in[2]);
        u32 hi, lo;
        pad_status(ch, &hi, &lo);
        for (int i = 0; i < 4; i++) { out[i] = (u8)(hi >> (24 - 8 * i)); out[4 + i] = (u8)(lo >> (24 - 8 * i)); }
        return 8;
    }
    case 0x41: case 0x42: { // origin / recalibrate
        static const u8 origin[10] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F, 0x00, 0x00};
        memcpy(out, origin, 10);
        return 10;
    }
    }
    LOG("si", "unknown controller command %02X", in[0]);
    return -1;
}

static void run_transfer() {
    int ch = (s_comcsr >> 1) & 3;
    int outlen = (s_comcsr >> 16) & 0x7F; if (!outlen) outlen = 128;
    int inlen = (s_comcsr >> 8) & 0x7F; if (!inlen) inlen = 128;
    u8 resp[128] = {};
    int n = device_transfer(ch, s_buf, outlen, resp);
    s_comcsr &= ~(COMCSR_TSTART | COMCSR_COMERR);
    if (n < 0) {
        s_comcsr |= COMCSR_COMERR;
        s_sr |= SR_NOREP << sr_shift(ch);
    } else {
        memcpy(s_buf, resp, inlen < 128 ? inlen : 128);
    }
    s_comcsr |= COMCSR_TCINT;
    update_irq();
}

void si_vblank() {
    // automatic polling of enabled channels
    for (int ch = 0; ch < 4; ch++) {
        if (!(s_poll & (0x80 >> ch))) continue;
        if (!host_get_pad(ch).connected) { s_sr |= SR_NOREP << sr_shift(ch); continue; }
        pad_status(ch, &s_inh[ch], &s_inl[ch]);
        s_sr |= SR_RDST << sr_shift(ch);
    }
    update_irq();
}

void si_init() {}

static u32 si_read32(u32 off) {
    if (off < 0x30) {
        int ch = off / 12;
        switch (off % 12) {
        case 0: return s_out[ch];
        case 4:
            s_sr &= ~(SR_RDST << sr_shift(ch));
            update_irq();
            return s_inh[ch];
        case 8: return s_inl[ch];
        }
    }
    switch (off) {
    case SIPOLL: return s_poll;
    case SICOMCSR: return s_comcsr;
    case SISR: return s_sr;
    case SIEXILK: return s_exilk;
    }
    if (off >= 0x80 && off < 0x100) {
        u32 i = off - 0x80;
        return ((u32)s_buf[i] << 24) | ((u32)s_buf[i + 1] << 16) | ((u32)s_buf[i + 2] << 8) | s_buf[i + 3];
    }
    return 0;
}

static void si_write32(u32 off, u32 v) {
    if (off < 0x30) {
        int ch = off / 12;
        if (off % 12 == 0) {
            s_out[ch] = v;
            pad_command(ch, v);
        }
        return;
    }
    switch (off) {
    case SIPOLL: s_poll = v; return;
    case SICOMCSR:
        if (v & COMCSR_TCINT) s_comcsr &= ~COMCSR_TCINT;
        s_comcsr = (s_comcsr & (COMCSR_TCINT | COMCSR_COMERR | COMCSR_RDSTINT)) | (v & ~(COMCSR_TCINT | COMCSR_COMERR | COMCSR_RDSTINT));
        if (v & COMCSR_TSTART) run_transfer();
        else update_irq();
        return;
    case SISR:
        // error bits are write-1-to-clear; WR copies the output buffers
        s_sr &= ~(v & 0x0F0F0F0F);
        update_irq();
        return;
    case SIEXILK: s_exilk = v; return;
    }
    if (off >= 0x80 && off < 0x100) {
        u32 i = off - 0x80;
        s_buf[i] = (u8)(v >> 24); s_buf[i + 1] = (u8)(v >> 16); s_buf[i + 2] = (u8)(v >> 8); s_buf[i + 3] = (u8)v;
    }
}

static u32 si_read16(u32 off) { u32 v = si_read32(off & ~3u); return (off & 2) ? lo16(v) : hi16(v); }
static void si_write16(u32 off, u16 v) {
    u32 cur = si_read32(off & ~3u);
    if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
    si_write32(off & ~3u, cur);
}

const MmioBlock g_si_block = {si_read16, si_write16, si_read32, si_write32};
