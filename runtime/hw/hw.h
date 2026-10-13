// Flipper hardware register blocks (physical 0x0C000000..).
#pragma once
#include "../common.h"

// Each block handles 16- and 32-bit accesses at an offset inside its window.
struct MmioBlock {
    u32 (*read16)(u32 off);
    void (*write16)(u32 off, u16 v);
    u32 (*read32)(u32 off);              // optional: nullptr -> two 16-bit reads
    void (*write32)(u32 off, u32 v);     // optional: nullptr -> two 16-bit writes
};

// register blocks
extern const MmioBlock g_cp_block, g_pe_block, g_vi_block, g_pi_block, g_mi_block, g_dsp_block,
    g_di_block, g_si_block, g_exi_block, g_ai_block;

// write-gather pipe (0x0C008000): GX command stream
void gx_fifo_write8(u8 v);
void gx_fifo_write16(u16 v);
void gx_fifo_write32(u32 v);

// PI CPU FIFO pointers, advanced by write-gather pipe writes
void pi_fifo_advance(u32 bytes);

// device init / per-frame hooks
void vi_init();
void si_init();
void si_vblank();
void di_init();
void exi_init();
void dsp_init();
void ai_init();
void gx_init();

// helpers
static inline u16 hi16(u32 v) { return (u16)(v >> 16); }
static inline u16 lo16(u32 v) { return (u16)v; }
static inline void set_hi16(u32& r, u16 v) { r = (r & 0x0000FFFFu) | ((u32)v << 16); }
static inline void set_lo16(u32& r, u16 v) { r = (r & 0xFFFF0000u) | v; }
