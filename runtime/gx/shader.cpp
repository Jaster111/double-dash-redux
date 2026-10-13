// TEV -> HLSL pixel shader generation.
//
// The generated shader reproduces the TEV's integer arithmetic (as documented
// by the Dolphin project): inputs a/b/c are 8-bit, d is 11-bit signed,
//   result = clamp(scale((d << 8) + bias +/- (a*(256-c') + b*c')) >> 8),  c' = c + (c >> 7)
// with the comparison modes replacing the lerp when bias == 3.
#include "gx.h"
#include "shader.h"
#include <string>

static const char* kColorIn[16] = {
    "prev.rgb", "prev.aaa", "r0.rgb", "r0.aaa", "r1.rgb", "r1.aaa", "r2.rgb", "r2.aaa",
    "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "int3(255,255,255)", "int3(128,128,128)", "kc.rgb", "int3(0,0,0)",
};
static const char* kAlphaIn[8] = {"prev.a", "r0.a", "r1.a", "r2.a", "tex.a", "ras.a", "kc.a", "0"};
static const char* kDest[4] = {"prev", "r0", "r1", "r2"};
static const char* kKonstConst[8] = {"255", "223", "191", "159", "128", "96", "64", "32"};
static const char* kComp[8] = {"false", "(%s < %s)", "(%s == %s)", "(%s <= %s)", "(%s > %s)", "(%s != %s)", "(%s >= %s)", "true"};

static std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static std::string konst_color(u32 sel) {
    if (sel < 8) return fmt("int3(%s,%s,%s)", kKonstConst[sel], kKonstConst[sel], kKonstConst[sel]);
    if (sel >= 12 && sel <= 15) return fmt("c_k[%u].rgb", sel - 12);
    static const char* comp = "rgba";
    if (sel >= 16) { u32 k = (sel - 16) & 3, c = (sel - 16) >> 2; return fmt("c_k[%u].%c%c%c", k, comp[c], comp[c], comp[c]); }
    return "int3(255,255,255)";
}
static std::string konst_alpha(u32 sel) {
    if (sel < 8) return kKonstConst[sel];
    static const char* comp = "rgba";
    if (sel >= 16) { u32 k = (sel - 16) & 3, c = (sel - 16) >> 2; return fmt("c_k[%u].%c", k, comp[c]); }
    return "255";
}

static std::string swizzle(const char* v, u32 table, const ShaderKey& k) {
    static const char* comp = "rgba";
    u32 sw = k.swap[table];
    return fmt("%s.%c%c%c%c", v, comp[sw & 3], comp[(sw >> 2) & 3], comp[(sw >> 4) & 3], comp[(sw >> 6) & 3]);
}

ShaderKey gx_shader_key() {
    ShaderKey k{};
    const u32* bp = g_gx.bp;
    u32 gen = bp[0x00];
    k.num_stages = ((gen >> 10) & 15) + 1;
    k.num_tex = gen & 15;
    k.num_ind = (gen >> 16) & 7;
    for (u32 s = 0; s < k.num_stages; s++) {
        k.color_env[s] = bp[0xC0 + 2 * s];
        k.alpha_env[s] = bp[0xC1 + 2 * s];
        u32 tref = bp[0x28 + s / 2] >> ((s & 1) * 12);
        k.order[s] = tref & 0xFFF;
        u32 ksel = bp[0xF6 + s / 2] >> ((s & 1) * 10);
        k.ksel[s] = (ksel >> 4) & 0x3FF;
        k.ind_cmd[s] = bp[0x10 + s];
    }
    for (u32 t = 0; t < 4; t++) {
        u32 a = bp[0xF6 + 2 * t], b = bp[0xF7 + 2 * t];
        k.swap[t] = (a & 3) | ((a >> 2 & 3) << 2) | ((b & 3) << 4) | ((b >> 2 & 3) << 6);
    }
    k.ind_order = bp[0x27];
    u32 ac = bp[0xF3];
    k.alpha_cmp = (ac >> 16) & 0xFF;
    u32 cm1 = bp[0x42];
    k.dst_alpha = (cm1 >> 8) & 1;
    k.fog = (bp[0xF1] >> 21) & 7;
    k.ztex = ((bp[0xF5] >> 2) & 3) ? (bp[0xF5] & 0xF) : 0;
    k.num_ind_used = k.num_ind;
    return k;
}

std::string gx_shader_source(const ShaderKey& k) {
    std::string s;
    s += "cbuffer PS : register(b0) {\n"
         "  int4 c_reg[4]; int4 c_k[4]; int4 c_aref; float4 c_texdim[8];\n"
         "  float4 c_fog0; float4 c_fog1; int4 c_ind[6]; float4 c_indscale[2]; int4 c_cdst; int4 c_zbias;\n"
         "};\n";
    for (int i = 0; i < 8; i++) s += fmt("Texture2D t%d : register(t%d); SamplerState s%d : register(s%d);\n", i, i, i, i);
    s += "struct PSIn { float4 pos : SV_Position; float4 col0 : COLOR0; float4 col1 : COLOR1;";
    for (int i = 0; i < 8; i++) s += fmt(" float3 tc%d : TEXCOORD%d;", i, i);
    s += " };\n";
    s += "int4 smp(Texture2D t, SamplerState ss, float2 uv) { return int4(round(t.Sample(ss, uv) * 255.0)); }\n";
    // constant destination alpha replaces only the stored alpha: blending keeps using the TEV alpha, which
    // goes out as the second (dual-source) colour
    s += "float4 main(PSIn i";
    if (k.ztex) s += ", out float o_depth : SV_Depth";
    if (k.dst_alpha) s += ", out float4 o_src1 : SV_Target1";
    s += ") : SV_Target0 {\n";
    s += "  int4 prev = c_reg[0], r0 = c_reg[1], r1 = c_reg[2], r2 = c_reg[3];\n";
    s += "  int4 rc0 = int4(round(saturate(i.col0) * 255.0)), rc1 = int4(round(saturate(i.col1) * 255.0));\n";
    s += "  int4 tex = int4(255,255,255,255), ras = int4(0,0,0,0), kc = int4(255,255,255,255);\n";
    // texture coordinates (projected)
    for (u32 t = 0; t < k.num_tex; t++) s += fmt("  float2 uv%u = i.tc%u.xy / (i.tc%u.z == 0.0 ? 1.0 : i.tc%u.z);\n", t, t, t, t);
    // indirect texture lookups
    for (u32 n = 0; n < k.num_ind; n++) {
        u32 map = (k.ind_order >> (6 * n)) & 7, coord = (k.ind_order >> (6 * n + 3)) & 7;
        if (coord < k.num_tex)
            s += fmt("  int4 ind%u = smp(t%u, s%u, uv%u * c_indscale[%u].%s);\n", n, map, map, coord, n / 2,
                     (n & 1) ? "zw" : "xy");
        else
            s += fmt("  int4 ind%u = int4(0,0,0,0);\n", n);
    }
    for (u32 st = 0; st < k.num_stages; st++) {
        u32 o = k.order[st];
        u32 map = o & 7, coord = (o >> 3) & 7, en = (o >> 6) & 1, chan = (o >> 7) & 7;
        u32 ce = k.color_env[st], ae = k.alpha_env[st];
        u32 rswap = ae & 3, tswap = (ae >> 2) & 3;
        s += fmt("  // stage %u\n  {\n", st);
        // texture coordinate, possibly offset by an indirect stage
        u32 ic = k.ind_cmd[st];
        u32 ibt = ic & 3, ifmt = (ic >> 2) & 3, ibias = (ic >> 4) & 7, imtx = (ic >> 9) & 15, iws = (ic >> 13) & 7,
            iwt = (ic >> 16) & 7, iadd_prev = (ic >> 20) & 1;
        (void)ifmt; (void)iadd_prev;
        std::string uv = coord < k.num_tex ? fmt("uv%u", coord) : "float2(0,0)";
        bool indirect = ibt < k.num_ind && (imtx != 0 || iws || iwt) && k.num_ind > 0;
        if (indirect && en) {
            s += fmt("    int3 ic = ind%u.abg;\n", ibt);   // GX uses alpha, blue, green
            if (ibias & 1) s += "    ic.x -= 128;\n";
            if (ibias & 2) s += "    ic.y -= 128;\n";
            if (ibias & 4) s += "    ic.z -= 128;\n";
            u32 m = imtx & 3;
            if (m >= 1 && m <= 3 && !(imtx & 12)) {
                u32 mi = m - 1;
                s += fmt("    float2 ioff = float2(dot(float3(c_ind[%u].xyz), float3(ic)), dot(float3(c_ind[%u].xyz), float3(ic))) * "
                         "exp2(float(c_ind[%u].w)) / 1024.0;\n", mi * 2, mi * 2 + 1, mi * 2);
                s += fmt("    float2 tuv = %s + ioff * c_texdim[%u].zw;\n", uv.c_str(), map);
            } else {
                s += fmt("    float2 tuv = %s;\n", uv.c_str());
            }
        } else {
            s += fmt("    float2 tuv = %s;\n", uv.c_str());
        }
        if (en) s += fmt("    tex = %s;\n", swizzle(fmt("smp(t%u, s%u, tuv)", map, map).c_str(), tswap, k).c_str());
        if (chan == 0) s += fmt("    ras = %s;\n", swizzle("rc0", rswap, k).c_str());
        else if (chan == 1) s += fmt("    ras = %s;\n", swizzle("rc1", rswap, k).c_str());
        else s += "    ras = int4(0,0,0,0);\n";
        u32 kcs = k.ksel[st] & 31, kas = (k.ksel[st] >> 5) & 31;
        s += fmt("    kc = int4(%s, %s);\n", konst_color(kcs).c_str(), konst_alpha(kas).c_str());
        // colour combiner
        {
            u32 d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
            u32 bias = (ce >> 16) & 3, sub = (ce >> 18) & 1, clamp = (ce >> 19) & 1, shift = (ce >> 20) & 3, dest = (ce >> 22) & 3;
            s += fmt("    int3 ca = %s & 255, cb = %s & 255, cc = %s & 255, cd = %s;\n", kColorIn[a], kColorIn[b], kColorIn[c], kColorIn[d]);
            if (bias != 3) {
                const char* biasv = bias == 1 ? " + 32768" : bias == 2 ? " - 32768" : "";
                s += fmt("    int3 cl = (ca << 8) + (cb - ca) * (cc + (cc >> 7));\n");
                s += fmt("    int3 cr = (cd << 8)%s %s cl%s;\n", biasv, sub ? "-" : "+", shift == 3 ? (sub ? " + 127" : " + 128") : "");
                const char* sc = shift == 1 ? " << 1" : shift == 2 ? " << 2" : shift == 3 ? " >> 1" : "";
                s += fmt("    cr = (cr%s) >> 8;\n", sc);
            } else {
                u32 mode = (shift << 1) | sub;
                switch (mode) {
                case 0: s += "    int3 cr = cd + ((ca.r > cb.r) ? cc : int3(0,0,0));\n"; break;
                case 1: s += "    int3 cr = cd + ((ca.r == cb.r) ? cc : int3(0,0,0));\n"; break;
                case 2: s += "    int3 cr = cd + (((ca.r | (ca.g << 8)) > (cb.r | (cb.g << 8))) ? cc : int3(0,0,0));\n"; break;
                case 3: s += "    int3 cr = cd + (((ca.r | (ca.g << 8)) == (cb.r | (cb.g << 8))) ? cc : int3(0,0,0));\n"; break;
                case 4: s += "    int3 cr = cd + (((ca.r | (ca.g << 8) | (ca.b << 16)) > (cb.r | (cb.g << 8) | (cb.b << 16))) ? cc : int3(0,0,0));\n"; break;
                case 5: s += "    int3 cr = cd + (((ca.r | (ca.g << 8) | (ca.b << 16)) == (cb.r | (cb.g << 8) | (cb.b << 16))) ? cc : int3(0,0,0));\n"; break;
                case 6: s += "    int3 cr = cd + int3(ca.r > cb.r ? cc.r : 0, ca.g > cb.g ? cc.g : 0, ca.b > cb.b ? cc.b : 0);\n"; break;
                default: s += "    int3 cr = cd + int3(ca.r == cb.r ? cc.r : 0, ca.g == cb.g ? cc.g : 0, ca.b == cb.b ? cc.b : 0);\n"; break;
                }
            }
            s += clamp ? "    cr = clamp(cr, 0, 255);\n" : "    cr = clamp(cr, -1024, 1023);\n";
            s += fmt("    int3 cres = cr;\n");
            // alpha combiner reads inputs before the colour result is written
            u32 ad = (ae >> 4) & 7, acc = (ae >> 7) & 7, ab = (ae >> 10) & 7, aa = (ae >> 13) & 7;
            u32 abias = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ashift = (ae >> 20) & 3, adest = (ae >> 22) & 3;
            s += fmt("    int aa_ = %s & 255, ab_ = %s & 255, ac_ = %s & 255, ad_ = %s;\n", kAlphaIn[aa], kAlphaIn[ab], kAlphaIn[acc], kAlphaIn[ad]);
            if (abias != 3) {
                const char* biasv = abias == 1 ? " + 32768" : abias == 2 ? " - 32768" : "";
                s += "    int al = (aa_ << 8) + (ab_ - aa_) * (ac_ + (ac_ >> 7));\n";
                s += fmt("    int ar = (ad_ << 8)%s %s al%s;\n", biasv, asub ? "-" : "+", ashift == 3 ? (asub ? " + 127" : " + 128") : "");
                const char* sc = ashift == 1 ? " << 1" : ashift == 2 ? " << 2" : ashift == 3 ? " >> 1" : "";
                s += fmt("    ar = (ar%s) >> 8;\n", sc);
            } else {
                u32 mode = (ashift << 1) | asub;
                switch (mode) {
                case 0: s += "    int ar = ad_ + ((aa_ > ab_) ? ac_ : 0);\n"; break;   // R8 compare uses red: approximate with alpha
                case 1: s += "    int ar = ad_ + ((aa_ == ab_) ? ac_ : 0);\n"; break;
                case 6: s += "    int ar = ad_ + ((aa_ > ab_) ? ac_ : 0);\n"; break;
                case 7: s += "    int ar = ad_ + ((aa_ == ab_) ? ac_ : 0);\n"; break;
                default: s += "    int ar = ad_ + ((aa_ > ab_) ? ac_ : 0);\n"; break;
                }
            }
            s += aclamp ? "    ar = clamp(ar, 0, 255);\n" : "    ar = clamp(ar, -1024, 1023);\n";
            s += fmt("    %s.rgb = cres;\n", kDest[dest]);
            s += fmt("    %s.a = ar;\n", kDest[adest]);
        }
        s += "  }\n";
    }
    s += "  int4 outc = prev & 255;\n";
    // alpha test
    {
        u32 c0 = k.alpha_cmp & 7, c1 = (k.alpha_cmp >> 3) & 7, logic = (k.alpha_cmp >> 6) & 3;
        bool trivially_pass = (c0 == 7 && (logic == 1 || c1 == 7)) || (c1 == 7 && logic == 1);
        if (!trivially_pass) {
            std::string a = fmt(kComp[c0], "outc.a", "c_aref.x"), b = fmt(kComp[c1], "outc.a", "c_aref.y");
            const char* lop = logic == 0 ? "&&" : logic == 1 ? "||" : logic == 2 ? "!=" : "==";
            s += fmt("  if (!((%s) %s (%s))) discard;\n", a.c_str(), lop, b.c_str());
        }
    }
    if (k.dst_alpha) s += "  o_src1 = float4(outc) / 255.0;\n  outc.a = c_cdst.x;\n";
    if (k.ztex) {
        // z texture: depth from the last stage's texel (U8: a, U16: r + a*256, U24: r<<16 | g<<8 | b) plus
        // bias, replacing or added to the rasterised depth
        u32 type = k.ztex & 3, op = (k.ztex >> 2) & 3;
        const char* zt = type == 0 ? "(tex.a & 255)" : type == 1 ? "((tex.r & 255) + ((tex.a & 255) << 8))"
                                                     : "(((tex.r & 255) << 16) | ((tex.g & 255) << 8) | (tex.b & 255))";
        s += fmt("  int zt = %s + c_zbias.x;\n", zt);
        if (op == 1) s += "  zt += int(i.pos.z * 16777216.0);\n";
        s += "  o_depth = float(zt & 0xFFFFFF) / 16777216.0;\n";
    }
    s += "  return float4(outc) / 255.0;\n}\n";
    return s;
}
