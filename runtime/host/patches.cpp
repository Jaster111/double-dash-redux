// Game patches from runtime/patches/NAME.gecko (Gecko codes, Dolphin GameSettings format).  tools/recomp
// compiles their writes into code as function variants that run while g_patch_on[n] is set; the remaining
// codes (data writes, pointer codes, conditionals: build/recomp/patches.cpp) run here every VI field, as
// Dolphin's code handler runs them every frame.
//   widescreen   settings.ini video.widescreen (DD_WIDESCREEN): 16:9; the renderer presents 16:9 as well
// A patch is chosen at boot: switching one off again would need the values it overwrote.
#include "../common.h"
#include "../ppc_rt.h"

extern unsigned char g_patch_on[];
extern const unsigned g_patch_count;
extern const char* const g_patch_names[];
extern const unsigned* const g_patch_codes[];
extern const unsigned g_patch_code_words[];

bool g_widescreen;
bool g_movie_drawn;   // a THP video frame was drawn since the last XFB copy (gx/render.cpp presents that frame 4:3)

// THPGXYuv2RgbDraw (GM4P01 0x801B0A28, hle_functions.txt) draws a decoded THP frame over the whole screen; the
// widescreen code does not touch it, so in 16:9 the 4:3 video would be stretched.  (The Lakitu scene before the
// video is 3D and follows the widescreen aspect.)
void orig_801B0A28(CPU* c);
void HLE_THPGXYuv2RgbDraw(CPU* c) {
    g_movie_drawn = true;
    orig_801B0A28(c);
}

// ShimmerScreen::draw (GM4P01 0x801AB6C0): blends a half-resolution copy of the scene over the far depth range
// (the courses' distance blur; with an indirect map also the heat shimmer).  settings.ini video.depth_blur = 0
// (DD_BLUR=0) skips it.
void orig_801AB6C0(CPU* c);
void HLE_ShimmerScreen_draw(CPU* c) {
    static const bool on = cfg_bool("video.depth_blur", true, "DD_BLUR");
    if (on) orig_801AB6C0(c);
}

void patches_init() {
    for (unsigned n = 0; n < g_patch_count; n++) {
        bool on = false;
        if (!strcmp(g_patch_names[n], "widescreen")) on = g_widescreen = cfg_bool("video.widescreen", false, "DD_WIDESCREEN");
        g_patch_on[n] = on;
        if (on) LOG("patch", "%s on", g_patch_names[n]);
    }
}

static bool in_mem1(u32 a, u32 n) { return a >= 0x80000000u && a + n <= 0x81800000u && a + n > a; }

// the subset of the Gecko code types that the patches use: 00/02/04 writes, 06 string write, 20-26 32-bit
// compares, 48 load po, DE po range check, E0 full terminator, E2 endif / else, F0 end
static void run_codes(const u32* c, u32 words) {
    u32 ba = 0x80000000, po = 0x80000000;
    int skip = 0;   // > 0: inside that many false conditionals
    for (u32 i = 0; i + 1 < words;) {
        u32 a = c[i], v = c[i + 1];
        u32 t = (a >> 24) & 0xFE, base = (a >> 24) & 0x10 ? po : ba;
        u32 addr = base + (a & 0x01FFFFFF);
        u32 next = i + 2;
        if (t == 0x06 || t == 0x16) next += ((v + 7) / 8) * 2;
        switch (t & 0xEE) {   // the po bit (0x10) only selects the base
        case 0x00: case 0x02: case 0x04:
            if (skip) break;
            if (t & 0x0E) {
                u32 count = (t & 0x0E) == 0x04 ? 1 : (v >> 16) + 1;
                for (u32 k = 0; k < count; k++) {
                    if ((t & 0x0E) == 0x02 && in_mem1(addr + 2 * k, 2)) wr16(addr + 2 * k, (u16)v);
                    if ((t & 0x0E) == 0x04 && in_mem1(addr, 4)) wr32(addr, v);
                }
            } else {
                for (u32 k = 0; k <= (v >> 16); k++)
                    if (in_mem1(addr + k, 1)) wr8(addr + k, (u8)v);
            }
            break;
        case 0x06:
            if (skip || !in_mem1(addr, v)) break;
            for (u32 k = 0; k < v; k++) wr8(addr + k, (u8)(c[i + 2 + k / 4] >> (24 - 8 * (k % 4))));
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: {   // 32-bit if ==, !=, >, <  (address bit 0: endif first)
            if (a & 1) { if (skip) skip--; }
            if (skip) { skip++; break; }
            u32 m = in_mem1(addr & ~1u, 4) ? rd32(addr & ~1u) : 0;
            bool ok = t == 0x20 ? m == v : t == 0x22 ? m != v : t == 0x24 ? m > v : m < v;
            if (!ok) skip = 1;
            break;
        }
        default:
            switch (t) {
            case 0x48: case 0x58: {   // 48TY0000 XXXXXXXX: po = [XXXXXXXX (+ ba, Y = 1: + po, when T = 1)]
                if (skip) break;
                u32 p = v + (a & 0x00100000 ? (a & 0x00010000 ? po : ba) : 0);
                po = in_mem1(p, 4) ? rd32(p) : 0;
                break;
            }
            case 0xDE:   // DE000000 LLLLHHHH: if the high halfword of po is in [LLLL, HHHH)
                if (skip) { skip++; break; }
                if (!((po >> 16) >= (v >> 16) && (po >> 16) < (v & 0xFFFF))) skip = 1;
                break;
            case 0xE2: {   // E2T000VV XXXXYYYY: VV endifs (T = 1: else), then ba / po reset
                u32 n = a & 0xFF;
                if (a & 0x00100000) {   // else: flip the innermost
                    if (skip == 1) skip = 0;
                    else if (skip == 0) skip = 1;
                } else
                    skip = skip > (int)n ? skip - (int)n : 0;
                if (v >> 16) ba = v & 0xFFFF0000;
                if (v & 0xFFFF) po = v << 16;
                break;
            }
            case 0xE0:
                skip = 0;
                ba = v >> 16 ? v & 0xFFFF0000 : 0x80000000;
                po = v & 0xFFFF ? v << 16 : 0x80000000;
                break;
            case 0xF0:
                return;
            default: {
                static bool warned;
                if (!warned) { warned = true; LOG("patch", "unsupported Gecko code %08X %08X", a, v); }
                break;
            }
            }
        }
        i = next;
    }
}

// called once per VI field (hw/vi.cpp)
void patches_on_field() {
    for (unsigned n = 0; n < g_patch_count; n++)
        if (g_patch_on[n]) run_codes(g_patch_codes[n], g_patch_code_words[n]);
}
