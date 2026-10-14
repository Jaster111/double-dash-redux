#pragma once
#include "../common.h"

// vertex as consumed by the D3D11 vertex shader (pass-through).  Only the first `ntex` texture
// coordinates are stored (the pixel shader's texgen count, shared by all draws of a batch), so
// vertices are gx_out_stride(ntex) bytes apart.
struct GxOutVertex {
    float pos[4];       // D3D clip space
    u32 col0, col1;     // RGBA8 (R in the low byte)
    float tex[8][3];    // s, t, q
};
static inline u32 gx_out_stride(u32 ntex) { return 24 + 12 * ntex; }
static inline GxOutVertex* gx_out_vertex(u8* base, u32 stride, u32 i) { return (GxOutVertex*)(base + (size_t)i * stride); }

struct GxFrameTransform {
    float inv_efb_w, inv_efb_h;   // 1 / EFB size in GX pixels (640 x 528)
};

void gx_process_vertices(u32 vat, u32 count, const u8* data, u8* out, u32 ntex, const GxFrameTransform& ft);
void gx_vertex_on_xf(u32 addr, u32 count);   // XF write: drops the cached lighting / texgen setup
