// GX (Flipper graphics) command processing state shared by the FIFO parser and
// the renderer.
#pragma once
#include "../common.h"

struct GxState {
    // CP registers
    u32 matindex_a = 0, matindex_b = 0;
    u32 vcd_lo = 0, vcd_hi = 0;
    u32 vat_a[8] = {}, vat_b[8] = {}, vat_c[8] = {};
    u32 array_base[16] = {}, array_stride[16] = {};
    // XF memory (matrices 0x000-0x4FF, normal mats 0x400-, post mats 0x500-, lights 0x600-) and registers 0x1000-
    u32 xfmem[0x1100] = {};
    // BP registers
    u32 bp[0x100] = {};
};
extern GxState g_gx;

// vertex attribute layout for one VAT (computed from VCD + VAT)
struct VtxLayout {
    u32 size;
    // offsets into the vertex (or ~0u if absent)
    u32 pmidx, tmidx[8];
    u32 pos, nrm, col[2], tex[8];
};
VtxLayout gx_vertex_layout(int vat);

// renderer hooks (gx/render.cpp)
void gx_render_init();
void gx_on_bp(u32 reg, u32 old, u32 val);
void gx_on_xf(u32 addr, u32 count, const u32* data);
void gx_on_cp(u32 reg, u32 val);
void gx_draw(u32 prim, u32 vat, u32 count, const u8* data);
void gx_on_retrace();

// PE interrupts (gx/fifo.cpp)
void pe_signal_finish();
void pe_signal_token(u16 token, bool irq);
