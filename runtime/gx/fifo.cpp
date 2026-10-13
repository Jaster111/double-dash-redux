// GX FIFO: write-gather pipe stream parser, CP (command processor) and PE
// (pixel engine) register blocks.
#include "gx.h"
#include <windows.h>
#include "../hw/hw.h"
#include <vector>

GxState g_gx;

static std::vector<u8> s_buf;
static size_t s_rd;

u32 pi_fifo_wptr();

// ---------------------------------------------------------------- vertex layout
static const u8 kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const u8 kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

static u32 attr_size_direct_pos(u32 vat_a) {
    u32 cnt = (vat_a & 1) ? 3 : 2, fmt = (vat_a >> 1) & 7;
    return cnt * kCompSize[fmt];
}

VtxLayout gx_vertex_layout(int v) {
    VtxLayout L;
    u32 lo = g_gx.vcd_lo, hi = g_gx.vcd_hi;
    u32 a = g_gx.vat_a[v], b = g_gx.vat_b[v], c = g_gx.vat_c[v];
    u32 off = 0;
    L.pmidx = (lo & 1) ? off++ : ~0u;
    for (int i = 0; i < 8; i++) L.tmidx[i] = (lo >> (1 + i) & 1) ? off++ : ~0u;
    auto idx_size = [](u32 mode) -> u32 { return mode == 2 ? 1 : mode == 3 ? 2 : 0; };
    // position
    u32 m = (lo >> 9) & 3;
    L.pos = m ? off : ~0u;
    off += m == 1 ? attr_size_direct_pos(a) : idx_size(m);
    // normal
    m = (lo >> 11) & 3;
    L.nrm = m ? off : ~0u;
    {
        bool nbt = (a >> 9) & 1;
        u32 fmt = (a >> 10) & 7;
        bool idx3 = (a >> 31) & 1;
        if (m == 1) off += (nbt ? 9 : 3) * kCompSize[fmt];
        else if (m >= 2) off += idx_size(m) * ((nbt && idx3) ? 3 : 1);
    }
    // colors
    for (int i = 0; i < 2; i++) {
        m = (lo >> (13 + 2 * i)) & 3;
        L.col[i] = m ? off : ~0u;
        u32 fmt = i == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        off += m == 1 ? kColorSize[fmt] : idx_size(m);
    }
    // texture coordinates
    u32 tcnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    u32 tfmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    for (int i = 0; i < 8; i++) {
        m = (hi >> (2 * i)) & 3;
        L.tex[i] = m ? off : ~0u;
        off += m == 1 ? (tcnt[i] + 1) * kCompSize[tfmt[i]] : idx_size(m);
    }
    L.size = off;
    return L;
}

// ---------------------------------------------------------------- CP register loads
static void cp_load(u8 reg, u32 v) {
    if (reg == 0x30) g_gx.matindex_a = v;
    else if (reg == 0x40) g_gx.matindex_b = v;
    else if (reg == 0x50) g_gx.vcd_lo = v;
    else if (reg == 0x60) g_gx.vcd_hi = v;
    else if ((reg & 0xF8) == 0x70) g_gx.vat_a[reg & 7] = v;
    else if ((reg & 0xF8) == 0x80) g_gx.vat_b[reg & 7] = v;
    else if ((reg & 0xF8) == 0x90) g_gx.vat_c[reg & 7] = v;
    else if ((reg & 0xF0) == 0xA0) g_gx.array_base[reg & 15] = v & 0x03FFFFFFu;
    else if ((reg & 0xF0) == 0xB0) g_gx.array_stride[reg & 15] = v & 0xFF;
    gx_on_cp(reg, v);
}

static void xf_load(u32 addr, u32 n, const u32* data) {
    for (u32 i = 0; i < n; i++) {
        u32 a = addr + i;
        if (a < 0x1100) g_gx.xfmem[a] = data[i];
    }
    gx_on_xf(addr, n, data);
}

static void xf_load_indexed(int array, u32 w) {
    u32 index = w >> 16, size = ((w >> 12) & 15) + 1, addr = w & 0xFFF;
    u32 src = g_gx.array_base[array] + index * g_gx.array_stride[array];
    u32 tmp[16];
    for (u32 i = 0; i < size; i++) tmp[i] = rd32(0x80000000u | (src + 4 * i));
    xf_load(addr, size, tmp);
}

static void bp_load(u32 w) {
    u32 reg = w >> 24, val = w & 0x00FFFFFF;
    u32 old = g_gx.bp[reg];
    // BP mask register (0xFE) applies to the next write only
    if (reg != 0xFE) {
        u32 mask = g_gx.bp[0xFE] ? g_gx.bp[0xFE] : 0x00FFFFFF;
        val = (old & ~mask) | (val & mask);
        g_gx.bp[0xFE] = 0x00FFFFFF;
    }
    g_gx.bp[reg] = val;
    switch (reg) {
    case 0x45: pe_signal_finish(); break;                       // PE_DONE
    case 0x47: pe_signal_token((u16)val, true); break;          // PE_TOKEN_INT
    case 0x48: pe_signal_token((u16)val, false); break;         // PE_TOKEN
    }
    gx_on_bp(reg, old, val);
}

// ---------------------------------------------------------------- command parser
static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

// returns bytes consumed, or 0 if the command is incomplete
static size_t run_command(const u8* p, size_t avail, int depth);

static void run_display_list(u32 addr, u32 size, int depth) {
    if (depth > 4) { LOG("gx", "display list nesting too deep"); return; }
    const u8* p = host_ptr(addr);
    size_t pos = 0;
    while (pos < size) {
        size_t n = run_command(p + pos, size - pos, depth + 1);
        if (!n) break;
        pos += n;
    }
}

static size_t run_command(const u8* p, size_t avail, int depth) {
    u8 cmd = p[0];
    switch (cmd) {
    case 0x00: return 1;                                       // NOP
    case 0x08:                                                 // LOAD_CP_REG
        if (avail < 6) return 0;
        cp_load(p[1], be32(p + 2));
        return 6;
    case 0x10: {                                               // LOAD_XF_REG
        if (avail < 5) return 0;
        u32 h = be32(p + 1);
        u32 n = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
        if (avail < 5 + 4 * n) return 0;
        u32 tmp[16];
        for (u32 i = 0; i < n; i++) tmp[i] = be32(p + 5 + 4 * i);
        xf_load(addr, n, tmp);
        return 5 + 4 * n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:                // LOAD_INDX_A..D
        if (avail < 5) return 0;
        xf_load_indexed(12 + ((cmd - 0x20) >> 3), be32(p + 1));
        return 5;
    case 0x40:                                                 // CALL_DL
        if (avail < 9) return 0;
        run_display_list(be32(p + 1) & 0x03FFFFE0u, be32(p + 5), depth);
        return 9;
    case 0x44: return 1;
    case 0x48: return 1;                                       // INVL_VC
    case 0x61:                                                 // LOAD_BP_REG
        if (avail < 5) return 0;
        bp_load(be32(p + 1));
        return 5;
    }
    if (cmd & 0x80) {                                          // draw primitive
        if (avail < 3) return 0;
        u32 vat = cmd & 7, prim = (cmd >> 3) & 0xF;
        u32 count = be16(p + 1);
        VtxLayout L = gx_vertex_layout(vat);
        size_t need = 3 + (size_t)count * L.size;
        if (avail < need) return 0;
        if (count) gx_draw(prim, vat, count, p + 3);
        return need;
    }
    static int warned;
    if (warned++ < 20) LOG("gx", "unknown FIFO command %02X", cmd);
    return 1;
}

u64 g_prof_fifo_ticks;   // profiling: QPC ticks spent parsing the FIFO (includes drawing)
static void pump() {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    struct Done { LARGE_INTEGER& a; LARGE_INTEGER& b; ~Done() { QueryPerformanceCounter(&b); g_prof_fifo_ticks += b.QuadPart - a.QuadPart; } } done{t0, t1};
    while (s_rd < s_buf.size()) {
        size_t n = run_command(&s_buf[s_rd], s_buf.size() - s_rd, 0);
        if (!n) break;
        s_rd += n;
    }
    if (s_rd > 65536 || s_rd == s_buf.size()) {
        s_buf.erase(s_buf.begin(), s_buf.begin() + s_rd);
        s_rd = 0;
    }
}

void gx_fifo_write8(u8 v) { s_buf.push_back(v); pi_fifo_advance(1); pump(); }
void gx_fifo_write16(u16 v) { s_buf.push_back((u8)(v >> 8)); s_buf.push_back((u8)v); pi_fifo_advance(2); pump(); }
void gx_fifo_write32(u32 v) {
    s_buf.push_back((u8)(v >> 24)); s_buf.push_back((u8)(v >> 16)); s_buf.push_back((u8)(v >> 8)); s_buf.push_back((u8)v);
    pi_fifo_advance(4);
    pump();
}

void gx_init() { gx_render_init(); }
void gx_vi_retrace() { gx_on_retrace(); }

// ============================================================ PE registers (0x0C001000)
static u16 s_pe[0x80];
static u16 s_pe_isr;      // bit0 token enable, bit1 finish enable, bit2 token status, bit3 finish status
static u16 s_pe_token;

static void pe_update_irq() {
    pi_set_irq(PI_IRQ_PE_TOKEN, (s_pe_isr & 4) && (s_pe_isr & 1));
    pi_set_irq(PI_IRQ_PE_FINISH, (s_pe_isr & 8) && (s_pe_isr & 2));
}
void pe_signal_finish() { s_pe_isr |= 8; pe_update_irq(); }
void pe_signal_token(u16 t, bool irq) {
    s_pe_token = t;
    if (irq) { s_pe_isr |= 4; pe_update_irq(); }
}

static u32 pe_read16(u32 off) {
    if (off == 0x0A) return s_pe_isr;
    if (off == 0x0E) return s_pe_token;
    return off < 0x100 ? s_pe[off / 2] : 0;
}
static void pe_write16(u32 off, u16 v) {
    if (off == 0x0A) {
        s_pe_isr = (s_pe_isr & 0xC & ~(v & 0xC)) | (v & 3);
        pe_update_irq();
        return;
    }
    if (off == 0x0E) { s_pe_token = v; return; }
    if (off < 0x100) s_pe[off / 2] = v;
}
const MmioBlock g_pe_block = {pe_read16, pe_write16, nullptr, nullptr};

// ============================================================ CP registers (0x0C000000)
static u16 s_cp[0x40];
static u32 cp_read16(u32 off) {
    switch (off) {
    case 0x00: return 0x000C;                      // read idle | command idle, FIFO never over/underflows
    case 0x34: case 0x36: return 0;                // read/write distance
    case 0x38: return (u16)(pi_fifo_wptr() >> 16); // GP write pointer follows the CPU FIFO
    case 0x3A: return (u16)pi_fifo_wptr();
    case 0x3C: return (u16)(pi_fifo_wptr() >> 16); // read pointer == write pointer (drained)
    case 0x3E: return (u16)pi_fifo_wptr();
    }
    return off < 0x80 ? s_cp[off / 2] : 0;
}
static void cp_write16(u32 off, u16 v) { if (off < 0x80) s_cp[off / 2] = v; }
const MmioBlock g_cp_block = {cp_read16, cp_write16, nullptr, nullptr};
