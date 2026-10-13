// MMIO dispatch plus the small register blocks: PI (processor interface /
// interrupt controller) and MI (memory interface).
#include "hw.h"
#include <set>

static const MmioBlock* block_for(u32 phys, u32* off) {
    if ((phys & 0xFFFF0000u) != 0x0C000000u) return nullptr;
    u32 o = phys & 0xFFFF;
    switch (o >> 12) {
    case 0x0: *off = o & 0xFFF; return &g_cp_block;
    case 0x1: *off = o & 0xFFF; return &g_pe_block;
    case 0x2: *off = o & 0xFFF; return &g_vi_block;
    case 0x3: *off = o & 0xFFF; return &g_pi_block;
    case 0x4: *off = o & 0xFFF; return &g_mi_block;
    case 0x5: *off = o & 0xFFF; return &g_dsp_block;
    case 0x6:
        *off = o & 0x3FF;
        switch ((o >> 10) & 3) {
        case 0: return &g_di_block;
        case 1: return &g_si_block;
        case 2: return &g_exi_block;
        case 3: return &g_ai_block;
        }
    }
    return nullptr;
}

static bool is_wgpipe(u32 phys) { return (phys & 0xFFFFF000u) == 0x0C008000u; }

static void unknown(const char* what, u32 a, u32 v) {
    static std::set<u32> seen;
    if (seen.insert(a).second) LOG("mmio", "unhandled %s %08X (%08X)", what, a, v);
}

u32 mmio_read32(u32 a) {
    u32 p = PHYS(a), off;
    if (const MmioBlock* b = block_for(p, &off)) {
        if (b->read32) return b->read32(off);
        return (b->read16(off) << 16) | b->read16(off + 2);
    }
    unknown("read32", a, 0);
    return 0;
}

u16 mmio_read16(u32 a) {
    u32 p = PHYS(a), off;
    if (const MmioBlock* b = block_for(p, &off)) {
        if (b->read16) return (u16)b->read16(off);
        u32 v = b->read32(off & ~3u);
        return (off & 2) ? lo16(v) : hi16(v);
    }
    unknown("read16", a, 0);
    return 0;
}

u8 mmio_read8(u32 a) {
    u16 v = mmio_read16(a & ~1u);
    return (a & 1) ? (u8)v : (u8)(v >> 8);
}

void mmio_write32(u32 a, u32 v) {
    u32 p = PHYS(a), off;
    if (is_wgpipe(p)) { gx_fifo_write32(v); return; }
    if (const MmioBlock* b = block_for(p, &off)) {
        if (b->write32) { b->write32(off, v); return; }
        b->write16(off, hi16(v));
        b->write16(off + 2, lo16(v));
        return;
    }
    unknown("write32", a, v);
}

void mmio_write16(u32 a, u16 v) {
    u32 p = PHYS(a), off;
    if (is_wgpipe(p)) { gx_fifo_write16(v); return; }
    if (const MmioBlock* b = block_for(p, &off)) {
        if (b->write16) { b->write16(off, v); return; }
        // 16-bit write into a 32-bit-only block: read-modify-write
        u32 cur = b->read32 ? b->read32(off & ~3u) : 0;
        if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
        b->write32(off & ~3u, cur);
        return;
    }
    unknown("write16", a, v);
}

void mmio_write8(u32 a, u8 v) {
    u32 p = PHYS(a);
    if (is_wgpipe(p)) { gx_fifo_write8(v); return; }
    unknown("write8", a, v);
}

// ============================================================ PI
static u32 s_pi_cause, s_pi_mask;
static u32 s_pi_fifo_base, s_pi_fifo_end, s_pi_fifo_wp;
static u32 s_pi_reset;

void pi_set_irq(u32 bit, bool on) {
    if (on) s_pi_cause |= bit;
    else s_pi_cause &= ~bit;
}

bool pi_irq_pending() { return (s_pi_cause & s_pi_mask) != 0; }

void pi_fifo_advance(u32 bytes) {
    if (!s_pi_fifo_base) return;
    u32 wrap = s_pi_fifo_wp & 0x20000000u;
    u32 cur = (s_pi_fifo_wp & 0x03FFFFFFu) + bytes;
    if (cur >= (s_pi_fifo_end & 0x03FFFFFFu)) { cur = s_pi_fifo_base & 0x03FFFFFFu; wrap = 0x20000000u; }
    s_pi_fifo_wp = cur | wrap;
}

u32 pi_fifo_wptr() { return s_pi_fifo_wp; }

static u32 pi_read32(u32 off) {
    switch (off) {
    case 0x00: return s_pi_cause | 0x10000;   // RSWST: reset switch released
    case 0x04: return s_pi_mask;
    case 0x0C: return s_pi_fifo_base;
    case 0x10: return s_pi_fifo_end;
    case 0x14: return s_pi_fifo_wp;
    case 0x24: return s_pi_reset;
    case 0x2C: return 0x246500B1;             // Flipper revision C
    }
    LOG("pi", "read %03X", off);
    return 0;
}

static void pi_write32(u32 off, u32 v) {
    switch (off) {
    case 0x00:   // acknowledge latched causes (reset switch, PI error, HSP)
        s_pi_cause &= ~(v & (PI_IRQ_RSW | PI_IRQ_PI | PI_IRQ_HSP | PI_IRQ_DEBUG));
        return;
    case 0x04: s_pi_mask = v; return;
    case 0x0C: s_pi_fifo_base = v; return;
    case 0x10: s_pi_fifo_end = v; return;
    case 0x14: s_pi_fifo_wp = v; return;
    case 0x18: return;                        // FIFO reset
    case 0x24: s_pi_reset = v; return;
    }
    LOG("pi", "write %03X = %08X", off, v);
}

static u32 pi_read16(u32 off) { u32 v = pi_read32(off & ~3u); return (off & 2) ? lo16(v) : hi16(v); }
static void pi_write16(u32 off, u16 v) {
    u32 cur = pi_read32(off & ~3u);
    if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
    pi_write32(off & ~3u, cur);
}

const MmioBlock g_pi_block = {pi_read16, pi_write16, pi_read32, pi_write32};

// ============================================================ MI (memory protection; stored, otherwise ignored)
static u16 s_mi[0x80];
static u32 mi_read16(u32 off) { return off < 0x100 ? s_mi[off / 2] : 0; }
static void mi_write16(u32 off, u16 v) { if (off < 0x100) s_mi[off / 2] = v; }
const MmioBlock g_mi_block = {mi_read16, mi_write16, nullptr, nullptr};
