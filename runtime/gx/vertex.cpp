// GX vertex pipeline on the CPU: attribute decoding (CP), transform, lighting
// and texture coordinate generation (XF).  Produces clip-space vertices for the
// D3D11 rasterizer.
#include "gx.h"
#include "vertex.h"
#include <math.h>

static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static inline float xf_float(u32 addr) { float f; memcpy(&f, &g_gx.xfmem[addr], 4); return f; }

// ---------------------------------------------------------------- attribute decoding
static inline float read_comp(const u8* p, u32 fmt, float scale) {
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return be16(p) * scale;
    case 3: return (s16)be16(p) * scale;
    default: { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }
    }
}
static const u8 kCompSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};

static void read_color(const u8* p, u32 fmt, float out[4]) {
    u32 r, g, b, a;
    switch (fmt) {
    case 0: { u16 v = be16(p); r = (v >> 11) << 3 | (v >> 13); g = ((v >> 5) & 63) << 2 | ((v >> 9) & 3); b = (v & 31) << 3 | ((v >> 2) & 7); a = 255; break; }
    case 1: case 2: r = p[0]; g = p[1]; b = p[2]; a = 255; break;
    case 3: { u16 v = be16(p); r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; break; }
    case 4: { u32 v = (p[0] << 16) | (p[1] << 8) | p[2]; r = (v >> 18) & 63; g = (v >> 12) & 63; b = (v >> 6) & 63; a = v & 63;
              r = r << 2 | r >> 4; g = g << 2 | g >> 4; b = b << 2 | b >> 4; a = a << 2 | a >> 4; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = a / 255.0f;
}
static const u8 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

struct RawVertex {
    u32 pmidx;
    u32 tmidx[8];
    float pos[3];
    float nrm[3], bin[3], tan[3];
    bool has_nrm;
    float col[2][4];
    bool has_col[2];
    float tex[8][2];
};

struct Decoder {
    u32 vcd_lo, vcd_hi, a, b, c;
    // per attribute: mode (0 none, 1 direct, 2 idx8, 3 idx16)
    u32 pos_mode, nrm_mode, col_mode[2], tex_mode[8];
    u32 pos_cnt, pos_fmt; float pos_scale;
    bool nbt, nidx3; u32 nrm_fmt; float nrm_scale;
    u32 col_fmt[2];
    u32 tex_cnt[8], tex_fmt[8]; float tex_scale[8];
    u32 pos_cnt_n;

    void init(int vat) {
        vcd_lo = g_gx.vcd_lo; vcd_hi = g_gx.vcd_hi;
        a = g_gx.vat_a[vat]; b = g_gx.vat_b[vat]; c = g_gx.vat_c[vat];
        pos_mode = (vcd_lo >> 9) & 3;
        nrm_mode = (vcd_lo >> 11) & 3;
        col_mode[0] = (vcd_lo >> 13) & 3;
        col_mode[1] = (vcd_lo >> 15) & 3;
        for (int i = 0; i < 8; i++) tex_mode[i] = (vcd_hi >> (2 * i)) & 3;
        pos_cnt = (a & 1) ? 3 : 2;
        pos_fmt = (a >> 1) & 7;
        pos_scale = pos_fmt == 4 ? 1.0f : 1.0f / (float)(1u << ((a >> 4) & 31));
        nbt = (a >> 9) & 1;
        nrm_fmt = (a >> 10) & 7;
        nrm_scale = nrm_fmt == 1 ? 1.0f / 64 : nrm_fmt == 3 ? 1.0f / 16384 : 1.0f;
        nidx3 = (a >> 31) & 1;
        col_fmt[0] = (a >> 14) & 7;
        col_fmt[1] = (a >> 18) & 7;
        u32 tcnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
        u32 tfmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
        u32 tsh[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
        for (int i = 0; i < 8; i++) {
            tex_cnt[i] = tcnt[i] + 1;
            tex_fmt[i] = tfmt[i];
            tex_scale[i] = tfmt[i] == 4 ? 1.0f : 1.0f / (float)(1u << tsh[i]);
        }
    }

    // source pointer for an attribute: inline data or indexed array element
    const u8* src(const u8*& p, u32 mode, int array) {
        if (mode == 1) return p;
        u32 idx;
        if (mode == 2) { idx = p[0]; p += 1; }
        else { idx = be16(p); p += 2; }
        return host_ptr(0x80000000u | (g_gx.array_base[array] + idx * g_gx.array_stride[array]));
    }

    void decode(const u8* p, RawVertex& v) {
        u32 mi_a = g_gx.xfmem[0x1018], mi_b = g_gx.xfmem[0x1019];
        v.pmidx = mi_a & 63;
        v.tmidx[0] = (mi_a >> 6) & 63; v.tmidx[1] = (mi_a >> 12) & 63; v.tmidx[2] = (mi_a >> 18) & 63; v.tmidx[3] = (mi_a >> 24) & 63;
        v.tmidx[4] = mi_b & 63; v.tmidx[5] = (mi_b >> 6) & 63; v.tmidx[6] = (mi_b >> 12) & 63; v.tmidx[7] = (mi_b >> 18) & 63;
        if (vcd_lo & 1) v.pmidx = *p++ & 63;
        for (int i = 0; i < 8; i++) if (vcd_lo >> (1 + i) & 1) v.tmidx[i] = *p++ & 63;
        // position
        if (pos_mode) {
            const u8* s = src(p, pos_mode, 0);
            u32 cs = kCompSize[pos_fmt];
            v.pos[0] = read_comp(s, pos_fmt, pos_scale);
            v.pos[1] = read_comp(s + cs, pos_fmt, pos_scale);
            v.pos[2] = pos_cnt == 3 ? read_comp(s + 2 * cs, pos_fmt, pos_scale) : 0.0f;
            if (pos_mode == 1) p += pos_cnt * cs;
        } else {
            v.pos[0] = v.pos[1] = v.pos[2] = 0;
        }
        // normal (+ binormal/tangent)
        v.has_nrm = nrm_mode != 0;
        if (nrm_mode) {
            u32 cs = kCompSize[nrm_fmt];
            int nvec = nbt ? 3 : 1;
            float* dst[3] = {v.nrm, v.bin, v.tan};
            if (nrm_mode == 1) {
                for (int k = 0; k < nvec; k++)
                    for (int j = 0; j < 3; j++) dst[k][j] = read_comp(p + (k * 3 + j) * cs, nrm_fmt, nrm_scale);
                p += nvec * 3 * cs;
            } else if (nbt && nidx3) {
                for (int k = 0; k < 3; k++) {
                    const u8* s = src(p, nrm_mode, 1);
                    for (int j = 0; j < 3; j++) dst[k][j] = read_comp(s + (k * 3 + j) * cs, nrm_fmt, nrm_scale);
                }
            } else {
                const u8* s = src(p, nrm_mode, 1);
                for (int k = 0; k < nvec; k++)
                    for (int j = 0; j < 3; j++) dst[k][j] = read_comp(s + (k * 3 + j) * cs, nrm_fmt, nrm_scale);
            }
        }
        for (int i = 0; i < 2; i++) {
            v.has_col[i] = col_mode[i] != 0;
            if (col_mode[i]) {
                const u8* s = src(p, col_mode[i], 2 + i);
                read_color(s, col_fmt[i], v.col[i]);
                if (col_mode[i] == 1) p += kColorSize[col_fmt[i]];
            }
        }
        for (int i = 0; i < 8; i++) {
            if (!tex_mode[i]) { v.tex[i][0] = v.tex[i][1] = 0; continue; }
            const u8* s = src(p, tex_mode[i], 4 + i);
            u32 cs = kCompSize[tex_fmt[i]];
            v.tex[i][0] = read_comp(s, tex_fmt[i], tex_scale[i]);
            v.tex[i][1] = tex_cnt[i] == 2 ? read_comp(s + cs, tex_fmt[i], tex_scale[i]) : 0.0f;
            if (tex_mode[i] == 1) p += tex_cnt[i] * cs;
        }
    }
};

// ---------------------------------------------------------------- transform helpers
static inline void mul34(u32 base, const float in[4], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = xf_float(base + r * 4) * in[0] + xf_float(base + r * 4 + 1) * in[1] + xf_float(base + r * 4 + 2) * in[2] +
                 xf_float(base + r * 4 + 3) * in[3];
}
static inline void mul33(u32 base, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = xf_float(base + r * 3) * in[0] + xf_float(base + r * 3 + 1) * in[1] + xf_float(base + r * 3 + 2) * in[2];
}
static inline void normalize3(float v[3]) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0) { l = 1.0f / sqrtf(l); v[0] *= l; v[1] *= l; v[2] *= l; }
}
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void unpack_rgba(u32 c, float out[4]) {
    out[0] = (c >> 24) / 255.0f; out[1] = ((c >> 16) & 255) / 255.0f; out[2] = ((c >> 8) & 255) / 255.0f; out[3] = (c & 255) / 255.0f;
}

// ---------------------------------------------------------------- per-draw lighting / texgen setup
// Everything derived from XF registers is decoded once per draw; the per-vertex loops only do arithmetic.
struct LightParams {
    float col[4];
    float pos[3], dir[3];
    float a[3], k[3];
    float pos_n[3];   // normalised position (specular lights use it as the light direction for N.L)
};

// one lighting channel component set: colour (rgb) or alpha of channel 0 / 1
struct LitComp {
    bool lit, mat_vtx, amb_vtx, attn_en, spot;
    u32 diff_fn;
    float mat[4], amb[4];
    u32 nlights;
    u8 lights[8];
    int c0, c1;       // component range: 0..3 colour, 3..4 alpha
};

static void setup_light(u32 l, LightParams& L) {
    u32 base = 0x600 + l * 0x10;
    unpack_rgba(g_gx.xfmem[base + 3], L.col);
    for (int i = 0; i < 3; i++) {
        L.a[i] = xf_float(base + 4 + i);
        L.k[i] = xf_float(base + 7 + i);
        L.pos[i] = xf_float(base + 10 + i);
        L.dir[i] = xf_float(base + 13 + i);
    }
    float ln = sqrtf(L.pos[0] * L.pos[0] + L.pos[1] * L.pos[1] + L.pos[2] * L.pos[2]);
    for (int i = 0; i < 3; i++) L.pos_n[i] = ln > 0 ? L.pos[i] / ln : 0.0f;
}

static void setup_litcomp(u32 ctrl, u32 amb_reg, u32 mat_reg, bool alpha, LitComp& C, u32& lights_used) {
    C.mat_vtx = ctrl & 1;
    C.amb_vtx = (ctrl >> 6) & 1;
    C.lit = (ctrl >> 1) & 1;
    unpack_rgba(mat_reg, C.mat);
    unpack_rgba(amb_reg, C.amb);
    C.c0 = alpha ? 3 : 0;
    C.c1 = alpha ? 4 : 3;
    u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    C.diff_fn = (ctrl >> 7) & 3;
    C.attn_en = (ctrl >> 9) & 1;
    C.spot = (ctrl >> 10) & 1;
    C.nlights = 0;
    if (C.lit)
        for (u32 l = 0; l < 8; l++)
            if (mask & (1u << l)) { C.lights[C.nlights++] = (u8)l; lights_used |= 1u << l; }
}

static void light_comp(const LitComp& C, const LightParams* lights, const float* vcol, const float pos[3], const float nrm[3],
                       float* out_comp) {
    const float* mat = C.mat_vtx ? vcol : C.mat;
    if (!C.lit) {                                           // lighting disabled: material only
        for (int k = C.c0; k < C.c1; k++) out_comp[k] = mat[k];
        return;
    }
    const float* amb = C.amb_vtx ? vcol : C.amb;
    float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
    for (u32 i = 0; i < C.nlights; i++) {
        const LightParams& Lp = lights[C.lights[i]];
        float L[3] = {Lp.pos[0] - pos[0], Lp.pos[1] - pos[1], Lp.pos[2] - pos[2]};
        float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
        float dist = sqrtf(dist2);
        float attn = 1.0f;
        if (dist > 0) { L[0] /= dist; L[1] /= dist; L[2] /= dist; }
        float ndotl = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2];
        if (C.attn_en) {
            if (C.spot) {
                float cosa = L[0] * Lp.dir[0] + L[1] * Lp.dir[1] + L[2] * Lp.dir[2];
                if (cosa < 0) cosa = 0;
                float num = Lp.a[0] + Lp.a[1] * cosa + Lp.a[2] * cosa * cosa;
                if (num < 0) num = 0;
                float den = Lp.k[0] + Lp.k[1] * dist + Lp.k[2] * dist2;
                attn = den != 0 ? num / den : 0;
            } else {
                // specular: direction holds the half-angle vector
                float ndoth = nrm[0] * Lp.dir[0] + nrm[1] * Lp.dir[1] + nrm[2] * Lp.dir[2];
                float cosa = ndotl > 0 ? (ndoth > 0 ? ndoth : 0) : 0;
                float num = Lp.a[0] + Lp.a[1] * cosa + Lp.a[2] * cosa * cosa;
                if (num < 0) num = 0;
                float den = Lp.k[0] + Lp.k[1] * cosa + Lp.k[2] * cosa * cosa;
                attn = den != 0 ? num / den : 0;
                // specular lights use the light direction rather than position for N.L
                ndotl = nrm[0] * Lp.pos_n[0] + nrm[1] * Lp.pos_n[1] + nrm[2] * Lp.pos_n[2];
            }
        }
        float d = C.diff_fn == 0 ? 1.0f : C.diff_fn == 1 ? ndotl : (ndotl > 0 ? ndotl : 0);
        float sc = d * attn;
        for (int k = C.c0; k < C.c1; k++) acc[k] += sc * Lp.col[k];
    }
    for (int k = C.c0; k < C.c1; k++) out_comp[k] = clamp01(acc[k]) * mat[k];
}

struct TexGen {
    bool stq, abc1, post_norm;
    u32 type, row, srcgen, post_base;
};

// ---------------------------------------------------------------- main entry
// Decode `count` vertices and transform them into `out` (clip space for D3D).
void gx_process_vertices(u32 vat, u32 count, const u8* data, GxOutVertex* out, const GxFrameTransform& ft) {
    Decoder dec;
    dec.init(vat);
    const u32 vsize = gx_vertex_layout(vat).size;
    const u32* xf = g_gx.xfmem;
    float proj[7];
    for (int i = 0; i < 7; i++) memcpy(&proj[i], &xf[0x1020 + i], 4);
    bool ortho = xf[0x1026] != 0;
    float vsx = xf_float(0x101A), vsy = xf_float(0x101B), vsz = xf_float(0x101C);
    float vox = xf_float(0x101D), voy = xf_float(0x101E), voz = xf_float(0x101F);
    u32 numchans = xf[0x1009] & 3;
    u32 numtex = xf[0x103F] & 15;
    if (numtex > 8) numtex = 8;
    bool dualtex = xf[0x1012] & 1;
    // GX viewport -> EFB window coordinates, expressed in D3D clip space (keeps w for perspective):
    //   x = ((cx * vsx + (vox - 342) * cw) / EFB_W) * 2 - cw,  y = cw - ((cy * vsy + (voy - 342) * cw) / EFB_H) * 2
    const float kx = vsx * ft.inv_efb_w * 2.0f, bx = (vox - 342.0f) * ft.inv_efb_w * 2.0f - 1.0f;
    const float ky = vsy * ft.inv_efb_h * 2.0f, by = 1.0f - (voy - 342.0f) * ft.inv_efb_h * 2.0f;
    const float kz = vsz * (1.0f / 16777216.0f), bz = voz * (1.0f / 16777216.0f);

    LitComp lit[2][2];   // [channel][colour, alpha]
    LightParams lights[8];
    u32 lights_used = 0;
    for (u32 ch = 0; ch < numchans && ch < 2; ch++) {
        setup_litcomp(xf[0x100E + ch], xf[0x100A + ch], xf[0x100C + ch], false, lit[ch][0], lights_used);
        setup_litcomp(xf[0x1010 + ch], xf[0x100A + ch], xf[0x100C + ch], true, lit[ch][1], lights_used);
    }
    for (u32 l = 0; l < 8; l++) if (lights_used & (1u << l)) setup_light(l, lights[l]);

    TexGen tg[8];
    for (u32 t = 0; t < numtex; t++) {
        u32 info = xf[0x1040 + t];
        tg[t].stq = (info >> 1) & 1;
        tg[t].abc1 = (info >> 2) & 1;
        tg[t].type = (info >> 4) & 7;
        tg[t].row = (info >> 7) & 31;
        tg[t].srcgen = (info >> 12) & 7;
        u32 post = xf[0x1050 + t];
        tg[t].post_norm = (post & 0x100) != 0;
        tg[t].post_base = 0x500 + (post & 63) * 4;
    }

    for (u32 n = 0; n < count; n++) {
        RawVertex v;
        dec.decode(data + (size_t)n * vsize, v);
        GxOutVertex& o = out[n];
        // position
        float p4[4] = {v.pos[0], v.pos[1], v.pos[2], 1.0f};
        float ep[3];
        mul34(v.pmidx * 4, p4, ep);
        float en[3] = {0, 0, 1};
        if (v.has_nrm) {
            mul33(0x400 + (v.pmidx & 31) * 3, v.nrm, en);
            normalize3(en);
        }
        float cx, cy, cz, cw;
        if (!ortho) {
            cx = proj[0] * ep[0] + proj[1] * ep[2];
            cy = proj[2] * ep[1] + proj[3] * ep[2];
            cz = proj[4] * ep[2] + proj[5];
            cw = -ep[2];
        } else {
            cx = proj[0] * ep[0] + proj[1];
            cy = proj[2] * ep[1] + proj[3];
            cz = proj[4] * ep[2] + proj[5];
            cw = 1.0f;
        }
        o.pos[0] = cx * kx + bx * cw;
        o.pos[1] = by * cw - cy * ky;
        o.pos[2] = cz * kz + bz * cw;
        o.pos[3] = cw;
        // colors
        float vc[2][4];
        for (int i = 0; i < 2; i++) {
            if (v.has_col[i]) memcpy(vc[i], v.col[i], sizeof vc[i]);
            else if (i == 1 && v.has_col[0]) memcpy(vc[1], v.col[0], sizeof vc[1]);   // GX reuses color0
            else { vc[i][0] = vc[i][1] = vc[i][2] = vc[i][3] = 1.0f; }
        }
        float outc[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
        for (u32 ch = 0; ch < 2; ch++) {
            if (ch >= numchans) { memcpy(outc[ch], vc[ch], sizeof outc[ch]); continue; }
            light_comp(lit[ch][0], lights, vc[ch], ep, en, outc[ch]);
            light_comp(lit[ch][1], lights, vc[ch], ep, en, outc[ch]);
        }
        memcpy(o.col0, outc[0], sizeof o.col0);
        memcpy(o.col1, outc[1], sizeof o.col1);
        // texture coordinate generation
        for (u32 t = 0; t < numtex; t++) {
            const TexGen& g = tg[t];
            float in[4] = {0, 0, 1, 1};
            switch (g.row) {
            case 0: in[0] = v.pos[0]; in[1] = v.pos[1]; in[2] = v.pos[2]; break;
            case 1: if (v.has_nrm) { in[0] = v.nrm[0]; in[1] = v.nrm[1]; in[2] = v.nrm[2]; } else { in[0] = in[1] = 0; in[2] = 1; } break;
            case 2: break;
            case 3: if (dec.nbt) { in[0] = v.tan[0]; in[1] = v.tan[1]; in[2] = v.tan[2]; } else { in[0] = in[1] = in[2] = 0; } break;
            case 4: if (dec.nbt) { in[0] = v.bin[0]; in[1] = v.bin[1]; in[2] = v.bin[2]; } else { in[0] = in[1] = in[2] = 0; } break;
            default: if (g.row >= 5 && g.row <= 12) { in[0] = v.tex[g.row - 5][0]; in[1] = v.tex[g.row - 5][1]; } break;
            }
            if (!g.abc1) in[2] = 1.0f;
            float r[3] = {0, 0, 1};
            if (g.type == 0) {
                u32 mb = v.tmidx[t] * 4;
                if (g.stq) mul34(mb, in, r);
                else {
                    for (int k = 0; k < 2; k++)
                        r[k] = xf_float(mb + k * 4) * in[0] + xf_float(mb + k * 4 + 1) * in[1] + xf_float(mb + k * 4 + 2) * in[2] +
                               xf_float(mb + k * 4 + 3) * in[3];
                    r[2] = 1.0f;
                }
            } else if (g.type == 1) {
                // emboss: offset a previously generated coordinate (approximation: copy source)
                if (g.srcgen < t) { r[0] = o.tex[g.srcgen][0]; r[1] = o.tex[g.srcgen][1]; r[2] = o.tex[g.srcgen][2]; }
            } else {
                // color texgen: s,t from the lit channel colour
                const float* cc = g.type == 2 ? o.col0 : o.col1;
                r[0] = cc[0]; r[1] = cc[1]; r[2] = 1.0f;
            }
            if (dualtex && g.type == 0) {
                if (g.post_norm) normalize3(r);
                float in2[4] = {r[0], r[1], r[2], 1.0f};
                mul34(g.post_base, in2, r);
            }
            o.tex[t][0] = r[0]; o.tex[t][1] = r[1]; o.tex[t][2] = r[2];
        }
        for (u32 t = numtex; t < 8; t++) { o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1; }
    }
}
