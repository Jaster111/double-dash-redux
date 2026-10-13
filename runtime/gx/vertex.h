#pragma once
#include "../common.h"

// vertex as consumed by the D3D11 vertex shader (pass-through)
struct GxOutVertex {
    float pos[4];       // D3D clip space
    float col0[4];
    float col1[4];
    float tex[8][3];    // s, t, q
};

struct GxFrameTransform {
    float inv_efb_w, inv_efb_h;   // 1 / EFB size in GX pixels (640 x 528)
};

void gx_process_vertices(u32 vat, u32 count, const u8* data, GxOutVertex* out, const GxFrameTransform& ft);
