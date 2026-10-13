#pragma once
#include "../common.h"
#include <vector>

enum : u32 {
    GX_TF_I4 = 0, GX_TF_I8 = 1, GX_TF_IA4 = 2, GX_TF_IA8 = 3, GX_TF_RGB565 = 4, GX_TF_RGB5A3 = 5, GX_TF_RGBA8 = 6,
    GX_TF_C4 = 8, GX_TF_C8 = 9, GX_TF_C14X2 = 10, GX_TF_CMPR = 14,
};

extern u8 g_tmem[1024 * 1024];

void gx_tex_block(u32 fmt, u32* bw, u32* bh, u32* bytes);
u32 gx_tex_size_bytes(u32 fmt, u32 w, u32 h);
void gx_decode_texture(u32 fmt, u32 w, u32 h, const u8* src, const u8* tlut, u32 tlut_fmt, u32* out);
static inline bool gx_tex_is_paletted(u32 fmt) { return fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2; }
