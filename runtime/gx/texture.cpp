// GX texture formats -> RGBA8 (memory order R, G, B, A).
#include "gx.h"
#include "texture.h"

u8 g_tmem[1024 * 1024];   // texture memory (TLUTs and preloaded textures)

static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }
static inline u32 from565(u16 v) {
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return rgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
}
static inline u32 from5a3(u16 v) {
    if (v & 0x8000) {
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    }
    u32 a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return rgba(r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1);
}
static inline u32 fromIA8(u16 v) { u32 i = v & 255, a = v >> 8; return rgba(i, i, i, a); }

static u32 tlut_entry(const u8* tlut, u32 fmt, u32 idx) {
    u16 v = be16(tlut + idx * 2);
    switch (fmt) {
    case 0: return fromIA8(v);
    case 1: return from565(v);
    default: return from5a3(v);
    }
}

u32 gx_tex_size_bytes(u32 fmt, u32 w, u32 h) {
    u32 bw, bh, bb;
    gx_tex_block(fmt, &bw, &bh, &bb);
    u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    return nx * ny * bb;
}

void gx_tex_block(u32 fmt, u32* bw, u32* bh, u32* bytes) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: *bw = 8; *bh = 8; *bytes = 32; return;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: *bw = 8; *bh = 4; *bytes = 32; return;
    case GX_TF_RGBA8: *bw = 4; *bh = 4; *bytes = 64; return;
    default: *bw = 4; *bh = 4; *bytes = 32; return;   // IA8, RGB565, RGB5A3, C14X2
    }
}

static void decode_cmpr_sub(const u8* p, u32* out, u32 stride) {
    u16 c0 = be16(p), c1 = be16(p + 2);
    u32 pal[4];
    pal[0] = from565(c0);
    pal[1] = from565(c1);
    auto ch = [](u32 c, int s) { return (c >> s) & 255; };
    if (c0 > c1) {
        pal[2] = rgba((2 * ch(pal[0], 0) + ch(pal[1], 0)) / 3, (2 * ch(pal[0], 8) + ch(pal[1], 8)) / 3,
                      (2 * ch(pal[0], 16) + ch(pal[1], 16)) / 3, 255);
        pal[3] = rgba((ch(pal[0], 0) + 2 * ch(pal[1], 0)) / 3, (ch(pal[0], 8) + 2 * ch(pal[1], 8)) / 3,
                      (ch(pal[0], 16) + 2 * ch(pal[1], 16)) / 3, 255);
    } else {
        pal[2] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
        pal[3] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 0);
    }
    for (int y = 0; y < 4; y++) {
        u8 bits = p[4 + y];
        for (int x = 0; x < 4; x++) out[y * stride + x] = pal[(bits >> (6 - 2 * x)) & 3];
    }
}

// Decode a w x h texture.  `out` receives w*h RGBA8 texels.
void gx_decode_texture(u32 fmt, u32 w, u32 h, const u8* src, const u8* tlut, u32 tlut_fmt, u32* out) {
    u32 bw, bh, bb;
    gx_tex_block(fmt, &bw, &bh, &bb);
    u32 nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    u32 pw = nbx * bw;   // padded width for the scratch image
    static thread_local std::vector<u32> tmp;
    tmp.resize((size_t)pw * nby * bh);
    u32* img = tmp.data();
    for (u32 by = 0; by < nby; by++) {
        for (u32 bx = 0; bx < nbx; bx++) {
            const u8* b = src + ((size_t)by * nbx + bx) * bb;
            u32* o = img + (size_t)by * bh * pw + bx * bw;
            switch (fmt) {
            case GX_TF_I4:
                for (u32 y = 0; y < 8; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u8 v = b[y * 4 + x / 2];
                        u32 i = (x & 1) ? (v & 15) : (v >> 4);
                        i *= 17;
                        o[y * pw + x] = rgba(i, i, i, i);
                    }
                break;
            case GX_TF_I8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) { u32 i = b[y * 8 + x]; o[y * pw + x] = rgba(i, i, i, i); }
                break;
            case GX_TF_IA4:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u8 v = b[y * 8 + x];
                        u32 i = (v & 15) * 17, a = (v >> 4) * 17;
                        o[y * pw + x] = rgba(i, i, i, a);
                    }
                break;
            case GX_TF_IA8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) o[y * pw + x] = fromIA8(be16(b + (y * 4 + x) * 2));
                break;
            case GX_TF_RGB565:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) o[y * pw + x] = from565(be16(b + (y * 4 + x) * 2));
                break;
            case GX_TF_RGB5A3:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) o[y * pw + x] = from5a3(be16(b + (y * 4 + x) * 2));
                break;
            case GX_TF_RGBA8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) {
                        u32 k = (y * 4 + x) * 2;
                        o[y * pw + x] = rgba(b[k + 1], b[32 + k], b[32 + k + 1], b[k]);
                    }
                break;
            case GX_TF_C4:
                for (u32 y = 0; y < 8; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u8 v = b[y * 4 + x / 2];
                        u32 i = (x & 1) ? (v & 15) : (v >> 4);
                        o[y * pw + x] = tlut_entry(tlut, tlut_fmt, i);
                    }
                break;
            case GX_TF_C8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) o[y * pw + x] = tlut_entry(tlut, tlut_fmt, b[y * 8 + x]);
                break;
            case GX_TF_C14X2:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) o[y * pw + x] = tlut_entry(tlut, tlut_fmt, be16(b + (y * 4 + x) * 2) & 0x3FFF);
                break;
            case GX_TF_CMPR:
                decode_cmpr_sub(b, o, pw);
                decode_cmpr_sub(b + 8, o + 4, pw);
                decode_cmpr_sub(b + 16, o + 4 * pw, pw);
                decode_cmpr_sub(b + 24, o + 4 * pw + 4, pw);
                break;
            default:
                for (u32 y = 0; y < bh; y++)
                    for (u32 x = 0; x < bw; x++) o[y * pw + x] = rgba(255, 0, 255, 255);
                break;
            }
        }
    }
    for (u32 y = 0; y < h; y++) memcpy(out + (size_t)y * w, img + (size_t)y * pw, w * 4);
}
