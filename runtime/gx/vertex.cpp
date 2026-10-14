// GX vertex pipeline on the CPU: attribute decoding (CP), transform, lighting
// and texture coordinate generation (XF).  Produces clip-space vertices for the
// D3D11 rasterizer.
//
// Hot path (tens of thousands of draws per field in splitscreen): attribute readers are specialised per
// format and picked once per vertex format, XF-derived lighting / texgen setup is cached until an XF write
// touches it, matrices are loaded once per draw (or when a per-vertex matrix index changes), and only the
// attributes the enabled stages use are decoded.
#include "gx.h"
#include "vertex.h"
#include <math.h>
#include <emmintrin.h>
#include <algorithm>

static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static inline float xf_float(u32 addr) { float f; memcpy(&f, &g_gx.xfmem[addr], 4); return f; }

// ---------------------------------------------------------------- attribute readers
static constexpr u32 comp_size(u32 fmt) { return fmt < 2 ? 1 : fmt < 4 ? 2 : 4; }
template <u32 F> static inline float read_comp(const u8* p, float scale) {
    if constexpr (F == 0) return p[0] * scale;
    else if constexpr (F == 1) return (s8)p[0] * scale;
    else if constexpr (F == 2) return be16(p) * scale;
    else if constexpr (F == 3) return (s16)be16(p) * scale;
    else { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }
}
typedef void (*CompFn)(const u8* s, float scale, float* out);
template <u32 F, u32 N> static void read_comps(const u8* s, float scale, float* out) {
    for (u32 i = 0; i < N; i++) out[i] = read_comp<F>(s + i * comp_size(F), scale);
}
template <u32 N> static CompFn comp_fn(u32 fmt) {
    switch (fmt) {
    case 0: return read_comps<0, N>;
    case 1: return read_comps<1, N>;
    case 2: return read_comps<2, N>;
    case 3: return read_comps<3, N>;
    default: return read_comps<4, N>;
    }
}

// colours are kept in SSE registers (r, g, b, a in lanes 0-3) from decoding to packing
typedef __m128 (*ColFn)(const u8* p);
static const __m128 k_inv255 = _mm_set1_ps(1.0f / 255.0f);
template <u32 F> static __m128 read_color(const u8* p) {
    if constexpr (F >= 5) {   // RGBA8
        u32 v;
        memcpy(&v, p, 4);
        __m128i z = _mm_setzero_si128(), i = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)v), z), z);
        return _mm_mul_ps(_mm_cvtepi32_ps(i), k_inv255);
    }
    u32 r, g, b, a;
    if constexpr (F == 0) { u16 v = be16(p); r = (v >> 11) << 3 | (v >> 13); g = ((v >> 5) & 63) << 2 | ((v >> 9) & 3); b = (v & 31) << 3 | ((v >> 2) & 7); a = 255; }
    else if constexpr (F == 1 || F == 2) { r = p[0]; g = p[1]; b = p[2]; a = 255; }
    else if constexpr (F == 3) { u16 v = be16(p); r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; }
    else if constexpr (F == 4) {
        u32 v = (p[0] << 16) | (p[1] << 8) | p[2];
        r = (v >> 18) & 63; g = (v >> 12) & 63; b = (v >> 6) & 63; a = v & 63;
        r = r << 2 | r >> 4; g = g << 2 | g >> 4; b = b << 2 | b >> 4; a = a << 2 | a >> 4;
    }
    return _mm_mul_ps(_mm_cvtepi32_ps(_mm_setr_epi32((int)r, (int)g, (int)b, (int)a)), k_inv255);
}
static ColFn color_fn(u32 fmt) {
    switch (fmt) {
    case 0: return read_color<0>;
    case 1: return read_color<1>;
    case 2: return read_color<2>;
    case 3: return read_color<3>;
    case 4: return read_color<4>;
    default: return read_color<5>;
    }
}

// ---------------------------------------------------------------- vertex format plan (per VCD + VAT)
struct Attr {
    u32 mode;          // 0 none, 1 direct, 2 idx8, 3 idx16
    u32 off;           // offset in the vertex
    int array;
};
struct Plan {
    u32 key[5];
    bool valid;
    u32 vsize;
    int pmidx_off, tmidx_off[8];   // per-vertex matrix indices (-1: from the XF registers)
    Attr pos; CompFn pos_fn; float pos_scale;
    Attr nrm; CompFn nrm_fn; float nrm_scale; bool nbt, nidx3; u32 nrm_cs;
    Attr col[2]; ColFn col_fn[2];
    Attr tex[8]; CompFn tex_fn[8]; float tex_scale[8];
};
static Plan s_plans[8];

static const Plan& get_plan(u32 vat) {
    Plan& P = s_plans[vat];
    u32 a = g_gx.vat_a[vat], b = g_gx.vat_b[vat], c = g_gx.vat_c[vat];
    u32 key[5] = {g_gx.vcd_lo, g_gx.vcd_hi, a, b, c};
    if (P.valid && !memcmp(P.key, key, sizeof key)) return P;
    memcpy(P.key, key, sizeof key);
    P.valid = true;
    const VtxLayout& L = gx_vertex_layout(vat);
    u32 lo = key[0], hi = key[1];
    P.vsize = L.size;
    P.pmidx_off = (lo & 1) ? (int)L.pmidx : -1;
    for (int i = 0; i < 8; i++) P.tmidx_off[i] = (lo >> (1 + i) & 1) ? (int)L.tmidx[i] : -1;
    P.pos = {(lo >> 9) & 3, L.pos, 0};
    u32 pos_fmt = (a >> 1) & 7;
    P.pos_fn = (a & 1) ? comp_fn<3>(pos_fmt) : comp_fn<2>(pos_fmt);
    P.pos_scale = pos_fmt == 4 ? 1.0f : 1.0f / (float)(1u << ((a >> 4) & 31));
    P.nrm = {(lo >> 11) & 3, L.nrm, 1};
    P.nbt = (a >> 9) & 1;
    P.nidx3 = (a >> 31) & 1;
    u32 nrm_fmt = (a >> 10) & 7;
    P.nrm_cs = comp_size(nrm_fmt);
    P.nrm_scale = nrm_fmt == 1 ? 1.0f / 64 : nrm_fmt == 3 ? 1.0f / 16384 : 1.0f;
    P.nrm_fn = (P.nbt && !(P.nidx3 && P.nrm.mode >= 2)) ? comp_fn<9>(nrm_fmt) : comp_fn<3>(nrm_fmt);
    for (int i = 0; i < 2; i++) {
        P.col[i] = {(lo >> (13 + 2 * i)) & 3, L.col[i], 2 + i};
        P.col_fn[i] = color_fn((a >> (14 + 4 * i)) & 7);
    }
    u32 tcnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    u32 tfmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    u32 tsh[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (int i = 0; i < 8; i++) {
        P.tex[i] = {(hi >> (2 * i)) & 3, L.tex[i], 4 + i};
        P.tex_fn[i] = tcnt[i] ? comp_fn<2>(tfmt[i]) : comp_fn<1>(tfmt[i]);
        P.tex_scale[i] = tfmt[i] == 4 ? 1.0f : 1.0f / (float)(1u << tsh[i]);
    }
    return P;
}

// attribute source for one draw: inline data or an indexed array element (array base resolved once)
struct Src {
    u32 mode, off, stride;
    const u8* base;
    void init(const Attr& a) {
        mode = a.mode; off = a.off;
        if (mode >= 2) {
            stride = g_gx.array_stride[a.array];
            base = host_ptr(0x80000000u | g_gx.array_base[a.array]);
        }
    }
    // index-th element pointer for the vertex at v
    inline const u8* ptr(const u8* v) const {
        const u8* p = v + off;
        if (mode == 1) return p;
        u32 idx = mode == 2 ? p[0] : be16(p);
        return base + (size_t)idx * stride;
    }
};

// ---------------------------------------------------------------- transform helpers
static inline void load_mtx(u32 base, u32 n, float* out) { memcpy(out, &g_gx.xfmem[base], n * 4); }
static inline void mul34(const float* m, const float in[3], float out[3]) {   // in[3] = 1
    for (int r = 0; r < 3; r++) out[r] = m[r * 4] * in[0] + m[r * 4 + 1] * in[1] + m[r * 4 + 2] * in[2] + m[r * 4 + 3];
}
static inline void mul34w(const float* m, const float in[4], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 4] * in[0] + m[r * 4 + 1] * in[1] + m[r * 4 + 2] * in[2] + m[r * 4 + 3] * in[3];
}
static inline void mul33(const float* m, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
}
static inline void normalize3(float v[3]) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0) { l = 1.0f / sqrtf(l); v[0] *= l; v[1] *= l; v[2] *= l; }
}
// normals for lighting: reciprocal square root estimate + one Newton step (~23 bits)
static inline void normalize3_fast(float v[3]) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0) {
        float r = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(l)));
        r = r * (1.5f - 0.5f * l * r * r);
        v[0] *= r; v[1] *= r; v[2] *= r;
    }
}

static __m128 unpack_rgba(u32 c) {   // XF colour register: RGBA8 with R in the high byte
    return _mm_mul_ps(_mm_cvtepi32_ps(_mm_setr_epi32((int)(c >> 24), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255))), k_inv255);
}
// [0,1] -> RGBA8, R in the low byte
static inline u32 pack_rgba8(__m128 c) {
    __m128i i = _mm_cvtps_epi32(_mm_mul_ps(c, _mm_set1_ps(255.0f)));
    i = _mm_packs_epi32(i, i);
    i = _mm_packus_epi16(i, i);
    return (u32)_mm_cvtsi128_si32(i);
}

// ---------------------------------------------------------------- lighting / texgen setup (cached)
struct LightParams {
    __m128 col;
    float pos[3], dir[3];
    float a[3], k[3];
    float pos_n[3];   // normalised position (specular lights use it as the light direction for N.L)
};

// one lighting channel component set: colour (rgb), alpha, or both when their controls are equal
struct LitComp {
    __m128 mat, amb;
    __m128 mask;      // lanes written: rgb (colour), a (alpha) or all four when colour and alpha share the control
    bool lit, mat_vtx, amb_vtx, attn_en, spot;
    u32 diff_fn;
    u32 nlights;
    u8 lights[8];
};

static void setup_light(u32 l, LightParams& L) {
    u32 base = 0x600 + l * 0x10;
    L.col = unpack_rgba(g_gx.xfmem[base + 3]);
    for (int i = 0; i < 3; i++) {
        L.a[i] = xf_float(base + 4 + i);
        L.k[i] = xf_float(base + 7 + i);
        L.pos[i] = xf_float(base + 10 + i);
        L.dir[i] = xf_float(base + 13 + i);
    }
    float ln = sqrtf(L.pos[0] * L.pos[0] + L.pos[1] * L.pos[1] + L.pos[2] * L.pos[2]);
    for (int i = 0; i < 3; i++) L.pos_n[i] = ln > 0 ? L.pos[i] / ln : 0.0f;
}

static void setup_litcomp(u32 ctrl, u32 amb_reg, u32 mat_reg, int c0, int c1, LitComp& C, u32& lights_used) {
    C.mat_vtx = ctrl & 1;
    C.amb_vtx = (ctrl >> 6) & 1;
    C.lit = (ctrl >> 1) & 1;
    C.mat = unpack_rgba(mat_reg);
    C.amb = unpack_rgba(amb_reg);
    C.mask = _mm_castsi128_ps(_mm_setr_epi32(c0 <= 0 && c1 > 0 ? -1 : 0, c0 <= 1 && c1 > 1 ? -1 : 0, c0 <= 2 && c1 > 2 ? -1 : 0,
                                             c0 <= 3 && c1 > 3 ? -1 : 0));
    u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    C.diff_fn = (ctrl >> 7) & 3;
    C.attn_en = (ctrl >> 9) & 1;
    C.spot = (ctrl >> 10) & 1;
    C.nlights = 0;
    if (C.lit)
        for (u32 l = 0; l < 8; l++)
            if (mask & (1u << l)) { C.lights[C.nlights++] = (u8)l; lights_used |= 1u << l; }
}

// ambient + the enabled lights, clamped to [0,1]
static inline __m128 light_sum(const LitComp& C, const LightParams* lights, __m128 acc, const float pos[3], const float nrm[3]) {
    for (u32 i = 0; i < C.nlights; i++) {
        const LightParams& Lp = lights[C.lights[i]];
        float L[3] = {Lp.pos[0] - pos[0], Lp.pos[1] - pos[1], Lp.pos[2] - pos[2]};
        float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
        float dist = sqrtf(dist2);
        float attn = 1.0f;
        if (dist > 0) { float r = 1.0f / dist; L[0] *= r; L[1] *= r; L[2] *= r; }
        float ndotl = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2];
        if (C.attn_en) {
            if (C.spot) {
                // clamps written as !(x > 0) so a NaN becomes 0 as well, like max(0, x) on a GPU: the game leaves
                // the direction of some spot-attenuated lights unset (a = 1,0,0, so it does not matter) and heap
                // garbage there made cosa NaN, the whole channel NaN and the karts / Lakitu render black
                float cosa = L[0] * Lp.dir[0] + L[1] * Lp.dir[1] + L[2] * Lp.dir[2];
                if (!(cosa > 0)) cosa = 0;
                float num = Lp.a[0] + Lp.a[1] * cosa + Lp.a[2] * cosa * cosa;
                if (!(num > 0)) num = 0;
                float den = Lp.k[0] + Lp.k[1] * dist + Lp.k[2] * dist2;
                attn = den != 0 ? num / den : 0;
            } else {
                // specular: direction holds the half-angle vector
                float ndoth = nrm[0] * Lp.dir[0] + nrm[1] * Lp.dir[1] + nrm[2] * Lp.dir[2];
                float cosa = ndotl > 0 ? (ndoth > 0 ? ndoth : 0) : 0;
                float num = Lp.a[0] + Lp.a[1] * cosa + Lp.a[2] * cosa * cosa;
                if (!(num > 0)) num = 0;
                float den = Lp.k[0] + Lp.k[1] * cosa + Lp.k[2] * cosa * cosa;
                attn = den != 0 ? num / den : 0;
                // specular lights use the light direction rather than position for N.L
                ndotl = nrm[0] * Lp.pos_n[0] + nrm[1] * Lp.pos_n[1] + nrm[2] * Lp.pos_n[2];
            }
        }
        float d = C.diff_fn == 0 ? 1.0f : C.diff_fn == 1 ? ndotl : (ndotl > 0 ? ndotl : 0);
        float sc = d * attn;
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(sc), Lp.col));
    }
    return _mm_min_ps(_mm_max_ps(acc, _mm_setzero_ps()), _mm_set1_ps(1.0f));
}

// DD_DBG_FLAGS 0x100: lit channels output white (tells lighting problems from TEV / texture ones)
static const bool s_dbg_unlit = getenv("DD_DBG_FLAGS") && (strtoul(getenv("DD_DBG_FLAGS"), nullptr, 0) & 0x100);

// the channel colour `prev` with C's lanes replaced by its lighting result
static inline __m128 light_comp(const LitComp& C, const LightParams* lights, __m128 vcol, const float pos[3], const float nrm[3],
                                __m128 prev) {
    __m128 mat = C.mat_vtx ? vcol : C.mat;
    __m128 r = mat;                                         // lighting disabled: material only
    if (C.lit) r = s_dbg_unlit ? _mm_set1_ps(1.0f) : _mm_mul_ps(light_sum(C, lights, C.amb_vtx ? vcol : C.amb, pos, nrm), mat);
    return _mm_or_ps(_mm_and_ps(C.mask, r), _mm_andnot_ps(C.mask, prev));
}

struct TexGen {
    bool stq, abc1, post_norm;
    u32 type, row, srcgen, post_base;
};

// Everything derived from XF registers other than the matrix indices: decoded when an XF write touched it.
struct XfSetup {
    float proj[6];
    bool ortho;
    float kx, bx, ky, by, kz, bz;
    u32 numchans, numtex;
    bool dualtex;
    LitComp lit[2][2];     // [channel][colour (or both), alpha]
    u32 nlit[2];           // LitComps used per channel (1 when colour and alpha share the control)
    LightParams lights[8];
    bool any_lit;          // a lit channel: transformed normals needed
    bool raw_nrm;          // a texgen reads the normal / binormal / tangent
    u32 tex_rows;          // texture coordinate attributes read by texgens (bit per attribute)
    TexGen tg[8];
};
static XfSetup s_xf;
static bool s_xf_valid;

void gx_vertex_on_xf(u32 addr, u32 count) {
    u32 end = addr + count;
    if (end <= 0x600) return;                       // matrices: loaded per draw
    if (addr >= 0x1018 && end <= 0x101A) return;    // matrix indices: read per draw
    s_xf_valid = false;
}

static void setup_xf(const GxFrameTransform& ft) {
    XfSetup& S = s_xf;
    const u32* xf = g_gx.xfmem;
    for (int i = 0; i < 6; i++) memcpy(&S.proj[i], &xf[0x1020 + i], 4);
    S.ortho = xf[0x1026] != 0;
    float vsx = xf_float(0x101A), vsy = xf_float(0x101B), vsz = xf_float(0x101C);
    float vox = xf_float(0x101D), voy = xf_float(0x101E), voz = xf_float(0x101F);
    // GX viewport -> EFB window coordinates, expressed in D3D clip space (keeps w for perspective):
    //   x = ((cx * vsx + (vox - 342) * cw) / EFB_W) * 2 - cw,  y = cw - ((cy * vsy + (voy - 342) * cw) / EFB_H) * 2
    S.kx = vsx * ft.inv_efb_w * 2.0f; S.bx = (vox - 342.0f) * ft.inv_efb_w * 2.0f - 1.0f;
    S.ky = vsy * ft.inv_efb_h * 2.0f; S.by = 1.0f - (voy - 342.0f) * ft.inv_efb_h * 2.0f;
    S.kz = vsz * (1.0f / 16777216.0f); S.bz = voz * (1.0f / 16777216.0f);
    S.numchans = std::min(xf[0x1009] & 3, 2u);
    S.numtex = std::min(xf[0x103F] & 15, 8u);
    S.dualtex = xf[0x1012] & 1;

    u32 lights_used = 0;
    S.any_lit = false;
    for (u32 ch = 0; ch < S.numchans; ch++) {
        u32 cc = xf[0x100E + ch], ac = xf[0x1010 + ch];
        if (cc == ac) {
            setup_litcomp(cc, xf[0x100A + ch], xf[0x100C + ch], 0, 4, S.lit[ch][0], lights_used);
            S.nlit[ch] = 1;
        } else {
            setup_litcomp(cc, xf[0x100A + ch], xf[0x100C + ch], 0, 3, S.lit[ch][0], lights_used);
            setup_litcomp(ac, xf[0x100A + ch], xf[0x100C + ch], 3, 4, S.lit[ch][1], lights_used);
            S.nlit[ch] = 2;
        }
        for (u32 i = 0; i < S.nlit[ch]; i++) S.any_lit |= S.lit[ch][i].lit;
    }
    for (u32 l = 0; l < 8; l++) if (lights_used & (1u << l)) setup_light(l, S.lights[l]);

    S.raw_nrm = false;
    S.tex_rows = 0;
    for (u32 t = 0; t < S.numtex; t++) {
        u32 info = xf[0x1040 + t];
        TexGen& g = S.tg[t];
        g.stq = (info >> 1) & 1;
        g.abc1 = (info >> 2) & 1;
        g.type = (info >> 4) & 7;
        g.row = (info >> 7) & 31;
        g.srcgen = (info >> 12) & 7;
        u32 post = xf[0x1050 + t];
        g.post_norm = (post & 0x100) != 0;
        g.post_base = 0x500 + (post & 63) * 4;
        if (g.row == 1 || g.row == 3 || g.row == 4) S.raw_nrm = true;
        if (g.row >= 5 && g.row <= 12) S.tex_rows |= 1u << (g.row - 5);
    }
    s_xf_valid = true;
}

// ---------------------------------------------------------------- main entry
// Decode `count` vertices and transform them into `out` (clip space for D3D), gx_out_stride(ntex) bytes apart.
void gx_process_vertices(u32 vat, u32 count, const u8* data, u8* out, u32 ntex, const GxFrameTransform& ft) {
    const Plan& P = get_plan(vat);
    if (!s_xf_valid) setup_xf(ft);
    const XfSetup& S = s_xf;
    const u32 stride = gx_out_stride(ntex);
    const u32 ngen = std::min(S.numtex, ntex);   // texgens the pixel shader reads
    const u32 mi_a = g_gx.xfmem[0x1018], mi_b = g_gx.xfmem[0x1019];
    const u32 def_tm[8] = {(mi_a >> 6) & 63, (mi_a >> 12) & 63, (mi_a >> 18) & 63, (mi_a >> 24) & 63,
                           mi_b & 63, (mi_b >> 6) & 63, (mi_b >> 12) & 63, (mi_b >> 18) & 63};

    // attributes this draw needs
    const bool want_nrm = P.nrm.mode && (S.any_lit || S.raw_nrm);
    const bool xf_nrm = P.nrm.mode && S.any_lit;
    Src pos_src, nrm_src, col_src[2], tex_src[8];
    pos_src.init(P.pos);
    nrm_src.init(P.nrm);
    for (int i = 0; i < 2; i++) col_src[i].init(P.col[i]);
    u32 tex_need = 0;
    for (u32 i = 0; i < 8; i++)
        if (P.tex[i].mode && (S.tex_rows & (1u << i))) { tex_need |= 1u << i; tex_src[i].init(P.tex[i]); }
    const u32 nidx_size = P.nrm.mode == 3 ? 2 : 1;

    // matrices: per draw, reloaded when a per-vertex index changes
    float M[12], N[9], TM[8][12], PM[8][12];
    u32 cur_pm = ~0u, cur_tm[8];
    auto load_pos = [&](u32 pm) {
        cur_pm = pm;
        load_mtx(pm * 4, 12, M);
        if (xf_nrm) load_mtx(0x400 + (pm & 31) * 3, 9, N);
    };
    if (P.pmidx_off < 0) load_pos(mi_a & 63);
    for (u32 t = 0; t < ngen; t++) {
        cur_tm[t] = ~0u;
        if (S.tg[t].type != 0) continue;
        if (P.tmidx_off[t] < 0) { cur_tm[t] = def_tm[t]; load_mtx(def_tm[t] * 4, 12, TM[t]); }
        if (S.dualtex) load_mtx(S.tg[t].post_base, 12, PM[t]);
    }

    u8 tex_list[8];
    u32 ntex_list = 0;
    for (u32 i = 0; i < 8; i++) if (tex_need & (1u << i)) tex_list[ntex_list++] = (u8)i;

    for (u32 n = 0; n < count; n++) {
        const u8* vp = data + (size_t)n * P.vsize;
        GxOutVertex& o = *gx_out_vertex(out, stride, n);
        if (P.pmidx_off >= 0) { u32 pm = vp[P.pmidx_off] & 63; if (pm != cur_pm) load_pos(pm); }
        // position
        float pos[3] = {0, 0, 0};
        if (P.pos.mode) P.pos_fn(pos_src.ptr(vp), P.pos_scale, pos);
        float ep[3];
        mul34(M, pos, ep);
        float cx, cy, cz, cw;
        if (!S.ortho) {
            cx = S.proj[0] * ep[0] + S.proj[1] * ep[2];
            cy = S.proj[2] * ep[1] + S.proj[3] * ep[2];
            cz = S.proj[4] * ep[2] + S.proj[5];
            cw = -ep[2];
        } else {
            cx = S.proj[0] * ep[0] + S.proj[1];
            cy = S.proj[2] * ep[1] + S.proj[3];
            cz = S.proj[4] * ep[2] + S.proj[5];
            cw = 1.0f;
        }
        o.pos[0] = cx * S.kx + S.bx * cw;
        o.pos[1] = S.by * cw - cy * S.ky;
        o.pos[2] = cz * S.kz + S.bz * cw;
        o.pos[3] = cw;
        // normal (+ binormal / tangent)
        float nrm[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        float en[3] = {0, 0, 1};
        if (want_nrm) {
            if (P.nbt && P.nidx3 && P.nrm.mode >= 2) {
                const u8* ip = vp + P.nrm.off;
                for (u32 k = 0; k < 3; k++, ip += nidx_size) {
                    u32 idx = nidx_size == 1 ? ip[0] : be16(ip);
                    P.nrm_fn(nrm_src.base + (size_t)idx * nrm_src.stride + k * 3 * P.nrm_cs, P.nrm_scale, nrm + 3 * k);
                }
            } else {
                P.nrm_fn(nrm_src.ptr(vp), P.nrm_scale, nrm);
            }
            if (xf_nrm) { mul33(N, nrm, en); normalize3_fast(en); }
        }
        // colours
        const __m128 one = _mm_set1_ps(1.0f);
        __m128 vc0 = P.col[0].mode ? P.col_fn[0](col_src[0].ptr(vp)) : one;
        __m128 vc1 = P.col[1].mode ? P.col_fn[1](col_src[1].ptr(vp)) : P.col[0].mode ? vc0 : one;   // GX reuses color0
        const __m128 vc[2] = {vc0, vc1};
        __m128 outc[2] = {vc0, vc1};   // channels beyond numchans pass the vertex colour through
        for (u32 ch = 0; ch < S.numchans; ch++)
            for (u32 i = 0; i < S.nlit[ch]; i++) outc[ch] = light_comp(S.lit[ch][i], S.lights, vc[ch], ep, en, outc[ch]);
        o.col0 = pack_rgba8(outc[0]);
        o.col1 = pack_rgba8(outc[1]);
        // texture coordinate generation
        float tc[8][2];
        for (u32 j = 0; j < ntex_list; j++) {
            u32 i = tex_list[j];
            tc[i][1] = 0.0f;
            P.tex_fn[i](tex_src[i].ptr(vp), P.tex_scale[i], tc[i]);
        }
        for (u32 t = 0; t < ngen; t++) {
            const TexGen& g = S.tg[t];
            float in[4] = {0, 0, 1, 1};
            switch (g.row) {
            case 0: in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; break;
            case 1: if (P.nrm.mode) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; } else { in[0] = in[1] = 0; in[2] = 1; } break;
            case 2: break;
            case 3: if (P.nrm.mode && P.nbt) { in[0] = nrm[6]; in[1] = nrm[7]; in[2] = nrm[8]; } else { in[0] = in[1] = in[2] = 0; } break;
            case 4: if (P.nrm.mode && P.nbt) { in[0] = nrm[3]; in[1] = nrm[4]; in[2] = nrm[5]; } else { in[0] = in[1] = in[2] = 0; } break;
            default:
                if (g.row >= 5 && g.row <= 12) {
                    u32 a = g.row - 5;
                    if (tex_need & (1u << a)) { in[0] = tc[a][0]; in[1] = tc[a][1]; } else { in[0] = in[1] = 0; }
                }
                break;
            }
            if (!g.abc1) in[2] = 1.0f;
            float r[3] = {0, 0, 1};
            if (g.type == 0) {
                if (P.tmidx_off[t] >= 0) {
                    u32 tm = vp[P.tmidx_off[t]] & 63;
                    if (tm != cur_tm[t]) { cur_tm[t] = tm; load_mtx(tm * 4, 12, TM[t]); }
                }
                const float* m = TM[t];
                if (g.stq) mul34w(m, in, r);
                else {
                    for (int k = 0; k < 2; k++) r[k] = m[k * 4] * in[0] + m[k * 4 + 1] * in[1] + m[k * 4 + 2] * in[2] + m[k * 4 + 3] * in[3];
                    r[2] = 1.0f;
                }
                if (S.dualtex) {
                    if (g.post_norm) normalize3(r);
                    float r2[3] = {r[0], r[1], r[2]};
                    mul34(PM[t], r2, r);
                }
            } else if (g.type == 1) {
                // emboss: offset a previously generated coordinate (approximation: copy source)
                if (g.srcgen < t) { r[0] = o.tex[g.srcgen][0]; r[1] = o.tex[g.srcgen][1]; r[2] = o.tex[g.srcgen][2]; }
            } else {
                // color texgen: s,t from the lit channel colour
                float cc[4];
                _mm_storeu_ps(cc, outc[g.type == 2 ? 0 : 1]);
                r[0] = cc[0]; r[1] = cc[1]; r[2] = 1.0f;
            }
            o.tex[t][0] = r[0]; o.tex[t][1] = r[1]; o.tex[t][2] = r[2];
        }
        for (u32 t = ngen; t < ntex; t++) { o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1; }
    }
}
