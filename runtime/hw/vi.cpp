// VI: video interface.  Generates field timing (retrace interrupts via the
// display-interrupt registers DI0..DI3) and exposes the external framebuffer
// address for presentation.
#include "hw.h"

static u16 s_regs[0x80];             // register file, 16-bit view, index = off/2
static u64 s_field_start;
static u32 s_field;                  // field counter
static bool s_started;
static void start_timing();

void host_present_field(u32 xfb_addr, u32 stride_bytes, u32 height);   // host/video.cpp
void gx_vi_retrace();                                                  // gx: end-of-field hooks
void dbg_on_field();                                                   // host/debug.cpp
void patches_on_field();                                               // host/patches.cpp

static bool is_pal() { return ((s_regs[0x02 / 2] >> 8) & 3) == 1; }

// Half-lines per field from the vertical timing (VTR equalisation / active lines, VTO / VTE pre- and
// post-blanking of the odd and even field, averaged), as Dolphin does: 625 for PAL 50 Hz, 525 for
// NTSC and EuRGB60 (PAL colour with 60 Hz timing, so the format bits alone cannot tell the rate).
static u32 half_lines_per_field() {
    u32 equ = s_regs[0x00 / 2] & 0xF, acv = (s_regs[0x00 / 2] >> 4) & 0x3FF;
    u32 prb_o = s_regs[0x0E / 2] & 0x3FF, psb_o = s_regs[0x0C / 2] & 0x3FF;
    u32 prb_e = s_regs[0x12 / 2] & 0x3FF, psb_e = s_regs[0x10 / 2] & 0x3FF;
    u32 odd = 3 * equ + prb_o + 2 * acv + psb_o, even = 3 * equ + prb_e + 2 * acv + psb_e;
    return (odd + even) / 2;
}

static u64 field_ticks() {
    u32 hlw = s_regs[0x06 / 2] & 0x1FF;   // HTR0: half-line width in VI clock samples
    u32 hl = half_lines_per_field();
    if (hlw && hl >= 400 && hl <= 700) {
        double clk = (s_regs[0x6C / 2] & 1) ? 54e6 : 27e6;
        return (u64)(TB_HZ * 2.0 * hlw * hl / clk);
    }
    return is_pal() ? TB_HZ / 50 : (u64)(TB_HZ / 59.94);
}
static u32 lines_per_field() {
    u32 hl = half_lines_per_field();
    return hl >= 400 && hl <= 700 ? hl / 2 : is_pal() ? 312 : 262;
}

static u32 reg32(u32 off) { return ((u32)s_regs[off / 2] << 16) | s_regs[off / 2 + 1]; }

static void update_irq() {
    bool on = false;
    for (int i = 0; i < 4; i++) {
        u16 h = s_regs[(0x30 + 4 * i) / 2];
        if ((h & 0x8000) && (h & 0x1000)) on = true;
    }
    pi_set_irq(PI_IRQ_VI, on);
}

static void field_event(u64) {
    u64 now = tb_now();
    static u64 s_logged_ticks;
    if (field_ticks() != s_logged_ticks) {
        s_logged_ticks = field_ticks();
        LOG("vi", "field rate %.3f Hz (%u half-lines per field, %s)", (double)TB_HZ / s_logged_ticks, half_lines_per_field(),
            is_pal() ? "PAL" : "NTSC/MPAL");
    }
    // keep a steady cadence even if we were late
    s_field_start += field_ticks();
    if (now - s_field_start > field_ticks() * 4) s_field_start = now;
    s_field++;
    for (int i = 0; i < 4; i++) {
        u16& h = s_regs[(0x30 + 4 * i) / 2];
        if (h & 0x1000) h |= 0x8000;
    }
    update_irq();
    si_vblank();
    gx_vi_retrace();
    patches_on_field();
    dbg_on_field();
    u32 tfbl = reg32(0x1C);
    u32 addr = (tfbl & 0x00FFFFFF) << ((tfbl & 0x10000000) ? 5 : 0);
    u32 stride = (s_regs[0x48 / 2] & 0xFF) * 16;   // HSW: words-per-line * 16 bytes... see VIConfigure
    u32 acv = (s_regs[0x00 / 2] >> 4) & 0x3FF;     // active video lines per field
    host_present_field(addr, stride ? stride : 1280, acv * 2);
    ev_schedule(s_field_start + field_ticks(), field_event, 0);
}

void vi_init() {
    memset(s_regs, 0, sizeof s_regs);
    s_regs[0x6E / 2] = 0;      // VISEL: no component cable
    s_regs[0x02 / 2] = 0x0101; // DCR: enabled, PAL format, as left by a PAL IPL
    // retrace interrupts as programmed by the IPL: DI0 near the end of the field, DI1 at line 1
    s_regs[0x30 / 2] = 0x1000 | 313; s_regs[0x32 / 2] = 430;
    s_regs[0x34 / 2] = 0x1000 | 1;   s_regs[0x36 / 2] = 1;
    start_timing();            // the video clock runs from power-on
}

static void start_timing() {
    if (s_started) return;
    s_started = true;
    s_field_start = tb_now();
    ev_schedule(s_field_start + field_ticks(), field_event, 0);
}

static u32 vi_read16(u32 off) {
    if (off >= 0x100) return 0;
    switch (off) {
    case 0x2C: {   // DPV: vertical beam position (1-based line)
        u64 el = tb_now() - s_field_start;
        u32 line = (u32)(el * lines_per_field() / field_ticks()) + 1;
        if (line > lines_per_field()) line = lines_per_field();
        if (s_field & 1) line += lines_per_field();
        return line;
    }
    case 0x2E: return 1;   // DPH
    }
    return s_regs[off / 2];
}

static void vi_write16(u32 off, u16 v) {
    if (off >= 0x100) return;
    switch (off) {
    case 0x30: case 0x34: case 0x38: case 0x3C: {
        u16& h = s_regs[off / 2];
        // the status bit can only be cleared by software
        h = (v & 0x7FFF) | (h & v & 0x8000);
        update_irq();
        return;
    }
    case 0x02:
        s_regs[off / 2] = v;
        if (v & 1) start_timing();
        return;
    }
    s_regs[off / 2] = v;
}

const MmioBlock g_vi_block = {vi_read16, vi_write16, nullptr, nullptr};

u32 vi_field_count() { return s_field; }
double vi_field_ms() { return field_ticks() * 1000.0 / TB_HZ; }

// physical address of the frame buffer the VI is scanning out
u32 vi_xfb_address() {
    u32 tfbl = reg32(0x1C);
    return ((tfbl & 0x00FFFFFF) << ((tfbl & 0x10000000) ? 5 : 0)) & 0x01FFFFFF;
}
