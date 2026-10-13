// GX renderer: Direct3D 11 backend.
//
// Vertices are transformed on the CPU (vertex.cpp); TEV is emulated by
// generated pixel shaders (shader.cpp).  The EFB is a render target at
// EFB_SCALE x the native 640x528.  EFB copies stay on the GPU: texture copies
// are kept in a cache keyed by their destination address and substituted when
// the game samples that address; display (XFB) copies are presented at the
// next VI retrace.
#include "gx.h"
#include "vertex.h"
#include "texture.h"
#include "shader.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <intrin.h>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <vector>
#include <string>

HWND host_window();
u32 vi_xfb_address();
double vi_field_ms();

static const int EFB_W = 640, EFB_H = 528;
static int EFB_SCALE = 3;

static ID3D11Device* s_dev;
static ID3D11DeviceContext* s_ctx;
static IDXGISwapChain* s_swap;
static ID3D11RenderTargetView* s_backbuffer_rtv;
static ID3D11Texture2D* s_efb;
static ID3D11RenderTargetView* s_efb_rtv;
static ID3D11ShaderResourceView* s_efb_srv;
static ID3D11Texture2D* s_efb_copy_src;            // snapshot of the EFB for copies
static ID3D11ShaderResourceView* s_efb_copy_src_srv;
static ID3D11Texture2D* s_depth;
static ID3D11DepthStencilView* s_dsv;
static ID3D11ShaderResourceView* s_depth_srv;
static ID3D11VertexShader* s_vs;
static ID3D11InputLayout* s_layout;
static ID3D11Buffer* s_vb;
static ID3D11Buffer* s_ib;
static ID3D11Buffer* s_ps_cb;

// pixel shader constants (cbuffer PS in shader.cpp)
struct PSConstants {
    s32 reg[4][4];
    s32 k[4][4];
    s32 aref[4];
    float texdim[8][4];
    float fog0[4], fog1[4];
    s32 ind[6][4];
    float indscale[2][4];
    s32 cdst[4];
    s32 zbias[4];
    s32 fogi[4];
    float fogrange[3][4];
};
static_assert(sizeof(PSConstants) % 16 == 0, "cbuffer size");

static const u32 VB_VERTS = 65536 * 4;
static u32 s_vb_pos;
static bool s_ok;

static ID3D11VertexShader* s_blit_vs;
static ID3D11PixelShader* s_blit_ps;
static ID3D11PixelShader* s_copy_ps;
static ID3D11Buffer* s_copy_cb;
static ID3D11SamplerState* s_linear_clamp;
static ID3D11SamplerState* s_point_clamp;

static u32 s_frame;
static u32 s_draws_this_field;
static u32 s_fields;
// DD_DBG_FLAGS bits (renderer debugging): 1 no culling, 2 no depth test, 4 no depth writes, 8 clear depth to 1.0,
// 0x40 no fog, 0x80 fog colour magenta (shows where fog is applied)
static const u32 dbg_flags = getenv("DD_DBG_FLAGS") ? (u32)strtoul(getenv("DD_DBG_FLAGS"), nullptr, 0) : 0;
// DD_DBG_FIELD: field number for the per-draw / copy debug dumps and depth probes (0 = all off)
static const u32 dbg_field = getenv("DD_DBG_FIELD") ? (u32)atoi(getenv("DD_DBG_FIELD")) : 0;
static inline bool dbg_window(u32 f) { return dbg_field && f + 6 >= dbg_field && f <= dbg_field; }

// ---------------------------------------------------------------- helpers
static ID3DBlob* compile(const std::string& src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &err);
    if (FAILED(hr)) {
        LOG("gx", "shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        FILE* f = fopen("bad_shader.hlsl", "w");
        if (f) { fputs(src.c_str(), f); fclose(f); }
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

static const char* kVS = R"(
struct VSIn { float4 pos : POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
  float3 tc0 : TEXCOORD0; float3 tc1 : TEXCOORD1; float3 tc2 : TEXCOORD2; float3 tc3 : TEXCOORD3;
  float3 tc4 : TEXCOORD4; float3 tc5 : TEXCOORD5; float3 tc6 : TEXCOORD6; float3 tc7 : TEXCOORD7; };
struct VSOut { float4 pos : SV_Position; float4 col0 : COLOR0; float4 col1 : COLOR1;
  float3 tc0 : TEXCOORD0; float3 tc1 : TEXCOORD1; float3 tc2 : TEXCOORD2; float3 tc3 : TEXCOORD3;
  float3 tc4 : TEXCOORD4; float3 tc5 : TEXCOORD5; float3 tc6 : TEXCOORD6; float3 tc7 : TEXCOORD7; };
VSOut main(VSIn i) { VSOut o; o.pos = i.pos; o.col0 = i.col0; o.col1 = i.col1;
  o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.tc3 = i.tc3; o.tc4 = i.tc4; o.tc5 = i.tc5; o.tc6 = i.tc6; o.tc7 = i.tc7; return o; }
)";

// full-screen triangle; uv rect in cbuffer
static const char* kBlitVS = R"(
cbuffer C : register(b0) { float4 rect; float4 p0; float4 p1; float4 p2; };
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = rect.xy + t * 0.5 * rect.zw * 2.0; return o; }
)";
// depth-only clear of a scissored rectangle: z from cbuffer p0.x, no colour output
static const char* kDepthClearVS = R"(
cbuffer C : register(b0) { float4 rect; float4 p0; float4 p1; float4 p2; };
float4 main(uint id : SV_VertexID) : SV_Position { float2 t = float2((id << 1) & 2, id & 2);
  return float4(t * float2(2, -2) + float2(-1, 1), p0.x, 1); }
)";
static const char* kNullPS = "void main() {}";
static ID3D11VertexShader* s_zclear_vs;
static ID3D11PixelShader* s_null_ps;
static ID3D11DepthStencilState* s_zclear_ds;

static const char* kBlitPS = R"(
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(t0.Sample(s0, uv).rgb, 1); }
)";
// EFB -> texture copy with GX format conversion.  p0.x = mode, p0.y = half scale, p0.zw = texel size
static const char* kCopyPS = R"(
cbuffer C : register(b0) { float4 rect; float4 p0; float4 p1; float4 p2; };
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float4 fetch(float2 uv) {
  if (p0.y > 0.5) {
    float2 d = p0.zw * 0.5;
    return (t0.Sample(s0, uv + float2(-d.x, -d.y)) + t0.Sample(s0, uv + float2(d.x, -d.y)) +
            t0.Sample(s0, uv + float2(-d.x, d.y)) + t0.Sample(s0, uv + float2(d.x, d.y))) * 0.25;
  }
  return t0.Sample(s0, uv);
}
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 c = fetch(uv);
  int m = (int)p0.x;
  if (p1.y > 0.5) {
    // depth source: split Z24 into bytes (R = 23..16, G = 15..8, B = 7..0) and use the colour mappings;
    // Z16 (RA8) stores I = middle byte, A = high byte
    uint z = (uint)min(round(saturate(c.r) * 16777216.0), 16777215.0);
    float hi = (z >> 16) / 255.0, mid = ((z >> 8) & 255) / 255.0, lo = (z & 255) / 255.0;
    c = float4(hi, mid, lo, 1.0);
    if (m == 3) return float4(mid, mid, mid, hi);
  }
  float y = dot(c.rgb, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
  if (m == 0) return float4(y, y, y, y);                 // I4 / I8
  if (m == 1) return float4(y, y, y, c.a);               // IA4 / IA8
  if (m == 2) return float4(c.r, c.r, c.r, c.r);         // R4 / R8
  if (m == 3) return float4(c.r, c.r, c.r, c.a);         // RA4 / RA8
  if (m == 4) return float4(c.rgb, 1);                   // RGB565
  if (m == 5) return c;                                  // RGB5A3 / RGBA8
  if (m == 6) return float4(c.a, c.a, c.a, c.a);         // A8
  if (m == 7) return float4(c.g, c.g, c.g, c.g);         // G8
  if (m == 8) return float4(c.b, c.b, c.b, c.b);         // B8
  if (m == 9) return float4(c.r, c.r, c.r, c.g);         // RG8
  if (m == 10) return float4(c.g, c.g, c.g, c.b);        // GB8
  return c;
}
)";

struct BlitCB { float rect[4]; float p0[4]; float p1[4]; float p2[4]; };

static bool create_rt(int w, int h, DXGI_FORMAT fmt, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv,
                      ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(s_dev->CreateTexture2D(&d, nullptr, tex))) return false;
    if (rtv && FAILED(s_dev->CreateRenderTargetView(*tex, nullptr, rtv))) return false;
    if (srv && FAILED(s_dev->CreateShaderResourceView(*tex, nullptr, srv))) return false;
    return true;
}

static void create_backbuffer_view() {
    ID3D11Texture2D* bb = nullptr;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    s_dev->CreateRenderTargetView(bb, nullptr, &s_backbuffer_rtv);
    bb->Release();
}

// ---------------------------------------------------------------- init
static void batch_init();
static void shader_cache_load();

void gx_render_init() {
    HWND wnd = host_window();
    if (const char* sc = getenv("DD_EFB_SCALE")) EFB_SCALE = atoi(sc) > 0 ? atoi(sc) : 3;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd,
                                               &s_swap, &s_dev, nullptr, &s_ctx);
    if (FAILED(hr)) { LOG("gx", "D3D11 init failed (%08lX): rendering disabled", hr); return; }
    {   // queue at most one frame ahead of the GPU: the game is CPU-paced, more only adds input latency
        IDXGIDevice1* dxgi = nullptr;
        if (SUCCEEDED(s_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi))) {
            dxgi->SetMaximumFrameLatency(1);
            dxgi->Release();
        }
    }
    create_backbuffer_view();
    int w = EFB_W * EFB_SCALE, h = EFB_H * EFB_SCALE;
    create_rt(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, &s_efb, &s_efb_rtv, &s_efb_srv);
    create_rt(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, &s_efb_copy_src, nullptr, &s_efb_copy_src_srv);
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        s_dev->CreateTexture2D(&d, nullptr, &s_depth);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        s_dev->CreateDepthStencilView(s_depth, &dv, &s_dsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        s_dev->CreateShaderResourceView(s_depth, &sv, &s_depth_srv);
    }
    ID3DBlob* vs = compile(kVS, "main", "vs_5_0");
    s_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
    D3D11_INPUT_ELEMENT_DESC il[11] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    for (int i = 0; i < 8; i++)
        il[3 + i] = {"TEXCOORD", (UINT)i, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)(48 + 12 * i), D3D11_INPUT_PER_VERTEX_DATA, 0};
    s_dev->CreateInputLayout(il, 11, vs->GetBufferPointer(), vs->GetBufferSize(), &s_layout);
    vs->Release();
    ID3DBlob* b = compile(kBlitVS, "main", "vs_5_0");
    s_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_blit_vs);
    b->Release();
    b = compile(kBlitPS, "main", "ps_5_0");
    s_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_blit_ps);
    b->Release();
    b = compile(kDepthClearVS, "main", "vs_5_0");
    s_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_zclear_vs);
    b->Release();
    b = compile(kNullPS, "main", "ps_5_0");
    s_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_null_ps);
    b->Release();
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = TRUE; d.DepthFunc = D3D11_COMPARISON_ALWAYS; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        s_dev->CreateDepthStencilState(&d, &s_zclear_ds);
    }
    b = compile(kCopyPS, "main", "ps_5_0");
    s_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_copy_ps);
    b->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = VB_VERTS * sizeof(GxOutVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s_dev->CreateBuffer(&bd, nullptr, &s_vb);
    bd.ByteWidth = VB_VERTS * 3 * sizeof(u32);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    s_dev->CreateBuffer(&bd, nullptr, &s_ib);
    batch_init();
    // constant buffers are updated with UpdateSubresource: DEFAULT usage, no CPU access
    bd.ByteWidth = sizeof(PSConstants); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DEFAULT; bd.CPUAccessFlags = 0;
    s_dev->CreateBuffer(&bd, nullptr, &s_ps_cb);
    bd.ByteWidth = sizeof(BlitCB);
    s_dev->CreateBuffer(&bd, nullptr, &s_copy_cb);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    s_dev->CreateSamplerState(&smp, &s_linear_clamp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_dev->CreateSamplerState(&smp, &s_point_clamp);
    float black[4] = {0, 0, 0, 1};
    s_ctx->ClearRenderTargetView(s_efb_rtv, black);
    s_ctx->ClearDepthStencilView(s_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    s_ok = true;
    LOG("gx", "D3D11 renderer ready, EFB %dx%d", w, h);
    shader_cache_load();
}

// ---------------------------------------------------------------- state objects
static std::unordered_map<ShaderKey, ID3D11PixelShader*, ShaderKeyHash> s_ps_cache;
static std::unordered_map<u64, ID3D11BlendState*> s_blend_cache;
static std::unordered_map<u32, ID3D11DepthStencilState*> s_depth_cache;
static std::unordered_map<u32, ID3D11RasterizerState*> s_rast_cache;
static std::unordered_map<u64, ID3D11SamplerState*> s_sampler_cache;

// hitch diagnostics: work done since the last game frame that can stall it
static u32 s_hitch_shaders, s_hitch_textures;
static double s_hitch_shader_ms, s_hitch_texture_ms;
static u32 s_repeats;   // VI fields that showed the same game frame as the field before, since the last report
static double qpc_ms() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ---------------------------------------------------------------- pixel shader disk cache
// Compiling a TEV shader takes milliseconds, so a new effect would stall its first frame.  Compiled
// bytecode is appended to cache/shaders.bin and loaded at startup.  Each record carries a hash of the
// generated HLSL: records whose source no longer matches (shader generator changed) are ignored.
static const char* SHADER_CACHE = "cache/shaders.bin";
static const u32 SHADER_CACHE_MAGIC = 0x43534444;   // "DDSC"
static FILE* s_shader_cache_out;
static u64 hash_mem(const u8* p, size_t n);

static void shader_cache_load() {
    if (getenv("DD_NO_SHADER_CACHE")) return;
    CreateDirectoryA("cache", nullptr);
    u32 loaded = 0, stale = 0;
    double t0 = qpc_ms();
    struct Rec { ShaderKey k; u64 h; std::vector<u8> code; };
    std::vector<Rec> valid;
    if (FILE* f = fopen(SHADER_CACHE, "rb")) {
        u32 magic = 0, key_size = 0;
        if (fread(&magic, 4, 1, f) == 1 && fread(&key_size, 4, 1, f) == 1 && magic == SHADER_CACHE_MAGIC &&
            key_size == sizeof(ShaderKey)) {
            for (;;) {
                ShaderKey k;
                u64 src_hash;
                u32 n;
                if (fread(&k, sizeof k, 1, f) != 1 || fread(&src_hash, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > (1u << 20))
                    break;
                std::vector<u8> code(n);
                if (fread(code.data(), 1, n, f) != n) break;
                std::string src = gx_shader_source(k);
                if (hash_mem((const u8*)src.data(), src.size()) != src_hash || s_ps_cache.count(k)) { stale++; continue; }
                ID3D11PixelShader* ps = nullptr;
                if (FAILED(s_dev->CreatePixelShader(code.data(), n, nullptr, &ps))) { stale++; continue; }
                s_ps_cache[k] = ps;
                loaded++;
                valid.push_back({k, src_hash, std::move(code)});
            }
        }
        fclose(f);
    }
    // a missing / foreign file, or outdated records: start a fresh file with the valid records
    s_shader_cache_out = fopen(SHADER_CACHE, "ab");
    if (s_shader_cache_out && (stale || ftell(s_shader_cache_out) == 0 || !loaded)) {
        fclose(s_shader_cache_out);
        s_shader_cache_out = fopen(SHADER_CACHE, "wb");
        if (s_shader_cache_out) {
            fwrite(&SHADER_CACHE_MAGIC, 4, 1, s_shader_cache_out);
            u32 ks = sizeof(ShaderKey);
            fwrite(&ks, 4, 1, s_shader_cache_out);
            for (const Rec& r : valid) {
                u32 n = (u32)r.code.size();
                fwrite(&r.k, sizeof r.k, 1, s_shader_cache_out);
                fwrite(&r.h, 8, 1, s_shader_cache_out);
                fwrite(&n, 4, 1, s_shader_cache_out);
                fwrite(r.code.data(), 1, n, s_shader_cache_out);
            }
            fflush(s_shader_cache_out);
        }
    }
    LOG("gx", "shader cache: %u shaders loaded in %.0f ms%s", loaded, qpc_ms() - t0,
        stale ? ", outdated records dropped" : "");
}

static void shader_cache_store(const ShaderKey& k, const std::string& src, ID3DBlob* b) {
    if (!s_shader_cache_out) return;
    u64 h = hash_mem((const u8*)src.data(), src.size());
    u32 n = (u32)b->GetBufferSize();
    fwrite(&k, sizeof k, 1, s_shader_cache_out);
    fwrite(&h, 8, 1, s_shader_cache_out);
    fwrite(&n, 4, 1, s_shader_cache_out);
    fwrite(b->GetBufferPointer(), 1, n, s_shader_cache_out);
    fflush(s_shader_cache_out);
}

static ID3D11PixelShader* get_ps(const ShaderKey& k) {
    auto it = s_ps_cache.find(k);
    if (it != s_ps_cache.end()) return it->second;
    double t0 = qpc_ms();
    struct Timed { double t0; ~Timed() { s_hitch_shaders++; s_hitch_shader_ms += qpc_ms() - t0; } } timed{t0};
    std::string src = gx_shader_source(k);
    ID3D11PixelShader* ps = nullptr;
    if (ID3DBlob* b = compile(src, "main", "ps_5_0")) {
        if (SUCCEEDED(s_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps)))
            shader_cache_store(k, src, b);
        b->Release();
    }
    s_ps_cache[k] = ps;
    return ps;
}

static bool efb_has_alpha() { return (g_gx.bp[0x43] & 7) == 1; }   // RGBA6_Z24

static ID3D11BlendState* get_blend() {
    u32 cm0 = g_gx.bp[0x41];
    bool alpha_wr = (cm0 >> 4) & 1 && efb_has_alpha();
    bool const_alpha = (g_gx.bp[0x42] >> 8) & 1;   // the shader then writes the TEV alpha as SV_Target1
    u64 key = cm0 | ((u64)alpha_wr << 32) | ((u64)efb_has_alpha() << 33) | ((u64)const_alpha << 34);
    auto it = s_blend_cache.find(key);
    if (it != s_blend_cache.end()) return it->second;
    bool dst_alpha = efb_has_alpha();
    static const D3D11_BLEND src_tab[8] = {D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
                                           D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
    static const D3D11_BLEND dst_tab[8] = {D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
                                           D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
    auto fix = [&](D3D11_BLEND b) {
        if (!dst_alpha && b == D3D11_BLEND_DEST_ALPHA) return D3D11_BLEND_ONE;
        if (!dst_alpha && b == D3D11_BLEND_INV_DEST_ALPHA) return D3D11_BLEND_ZERO;
        return b;
    };
    auto to_alpha = [](D3D11_BLEND b) {
        switch (b) {
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return b;
        }
    };
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    bool blend = cm0 & 1, logic = (cm0 >> 1) & 1, sub = (cm0 >> 11) & 1;
    rt.RenderTargetWriteMask = (((cm0 >> 3) & 1) ? 7 : 0) | (alpha_wr ? 8 : 0);
    if (blend || sub) {
        rt.BlendEnable = TRUE;
        if (sub) {
            rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
            rt.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
        } else {
            rt.SrcBlend = fix(src_tab[(cm0 >> 8) & 7]);
            rt.DestBlend = fix(dst_tab[(cm0 >> 5) & 7]);
            rt.BlendOp = D3D11_BLEND_OP_ADD;
        }
        rt.SrcBlendAlpha = to_alpha(rt.SrcBlend);
        rt.DestBlendAlpha = to_alpha(rt.DestBlend);
        rt.BlendOpAlpha = rt.BlendOp;
        if (const_alpha) {
            // blend factors use the TEV alpha (source 1); the stored alpha is the constant from source 0
            auto src1 = [](D3D11_BLEND b) {
                return b == D3D11_BLEND_SRC_ALPHA ? D3D11_BLEND_SRC1_ALPHA : b == D3D11_BLEND_INV_SRC_ALPHA ? D3D11_BLEND_INV_SRC1_ALPHA : b;
            };
            rt.SrcBlend = src1(rt.SrcBlend);
            rt.DestBlend = src1(rt.DestBlend);
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
    } else if (logic) {
        u32 op = (cm0 >> 12) & 15;
        rt.BlendEnable = TRUE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        switch (op) {
        case 0: rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO; break;   // clear
        case 5: rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE; break;    // noop
        case 15: rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE; break;    // set (approx.)
        case 7: case 6: rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE; break;   // or/xor (approx.)
        case 12: rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_INV_SRC_COLOR; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                 rt.SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; break;   // inverse copy
        case 10: rt.SrcBlend = D3D11_BLEND_ZERO; rt.DestBlend = D3D11_BLEND_INV_DEST_COLOR;
                 rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA; break;   // invert
        default: rt.BlendEnable = FALSE; break;   // copy and the rest
        }
    }
    ID3D11BlendState* st = nullptr;
    s_dev->CreateBlendState(&bd, &st);
    s_blend_cache[key] = st;
    return st;
}

static ID3D11DepthStencilState* get_depth(bool no_write = false) {
    u32 z = g_gx.bp[0x40] & 0x1F;
    if (no_write) z &= ~0x10u;
    auto it = s_depth_cache.find(z);
    if (it != s_depth_cache.end()) return it->second;
    static const D3D11_COMPARISON_FUNC f[8] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL,
                                               D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
                                               D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS};
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = (z & 1) ? TRUE : FALSE;
    d.DepthFunc = f[(z >> 1) & 7];
    d.DepthWriteMask = (z & 0x10) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    if (!d.DepthEnable) { d.DepthEnable = (z & 0x10) ? TRUE : FALSE; d.DepthFunc = D3D11_COMPARISON_ALWAYS; }
    ID3D11DepthStencilState* st = nullptr;
    s_dev->CreateDepthStencilState(&d, &st);
    s_depth_cache[z] = st;
    return st;
}

static ID3D11DepthStencilState* get_depth_always() {
    static ID3D11DepthStencilState* st;
    if (!st) {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE;
        s_dev->CreateDepthStencilState(&d, &st);
    }
    return st;
}

static ID3D11RasterizerState* get_rast(u32 cull) {
    auto it = s_rast_cache.find(cull);
    if (it != s_rast_cache.end()) return it->second;
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    r.FrontCounterClockwise = FALSE;
    r.DepthClipEnable = TRUE;
    r.ScissorEnable = TRUE;
    ID3D11RasterizerState* st = nullptr;
    s_dev->CreateRasterizerState(&r, &st);
    s_rast_cache[cull] = st;
    return st;
}

static ID3D11SamplerState* get_sampler(u32 mode0, u32 mode1) {
    u64 key = mode0 | ((u64)mode1 << 32);
    auto it = s_sampler_cache.find(key);
    if (it != s_sampler_cache.end()) return it->second;
    static const D3D11_TEXTURE_ADDRESS_MODE wrap[4] = {D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
                                                       D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_WRAP};
    D3D11_SAMPLER_DESC s = {};
    s.AddressU = wrap[mode0 & 3];
    s.AddressV = wrap[(mode0 >> 2) & 3];
    s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    bool mag_lin = (mode0 >> 4) & 1;
    u32 minf = (mode0 >> 5) & 7;
    bool min_lin = minf >= 4;
    bool mip_lin = minf == 2 || minf == 6;
    bool mip = minf != 0 && minf != 4;
    int f = (min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0);
    s.Filter = (D3D11_FILTER)f;
    s.MipLODBias = (float)(s8)(((mode0 >> 9) & 0xFF)) / 32.0f;
    s.MinLOD = mip ? (mode1 & 0xFF) / 16.0f : 0.0f;
    s.MaxLOD = mip ? ((mode1 >> 8) & 0xFF) / 16.0f : 0.0f;
    s.MaxAnisotropy = 1;
    ID3D11SamplerState* st = nullptr;
    s_dev->CreateSamplerState(&s, &st);
    s_sampler_cache[key] = st;
    return st;
}

// ---------------------------------------------------------------- texture cache
struct TexEntry {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    u64 hash = 0;
    u32 validated_frame = ~0u;
    u32 last_used = 0;
};
struct EfbCopy {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    u32 w = 0, h = 0;       // in GX texels
    u32 fmt = 0;
    bool depth = false;     // copied from the depth buffer
    u64 mem_hash = 0;       // RAM contents at copy time: a CPU overwrite invalidates the copy
    u32 bytes = 0;
    u32 validated_frame = ~0u;   // mem_hash last checked in this game frame
    u32 frame = 0;               // XFB copies: game frame number
    bool stale = false;          // the CPU overwrote the copy's RAM: use the texture data instead
};
static std::unordered_map<u64, TexEntry> s_tex_cache;
static std::unordered_map<u32, EfbCopy> s_efb_copies;   // texture copies by physical address
static std::unordered_map<u32, EfbCopy> s_xfb_copies;   // display copies by physical address

static u64 hash_mem(const u8* p, size_t n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 v;
        memcpy(&v, p + i, 8);
        h ^= v * 0xC2B2AE3D27D4EB4Full;
        h = (h << 31 | h >> 33) * 0x9E3779B97F4A7C15ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static u32 tex_reg(u32 base_lo, u32 base_hi, u32 i) { return g_gx.bp[i < 4 ? base_lo + i : base_hi + (i - 4)]; }

static ID3D11ShaderResourceView* get_texture(u32 map, u32* out_w, u32* out_h) {
    u32 img0 = tex_reg(0x88, 0xA8, map), img3 = tex_reg(0x94, 0xB4, map), mode0 = tex_reg(0x80, 0xA0, map),
        mode1 = tex_reg(0x84, 0xA4, map), tlut = tex_reg(0x98, 0xB8, map);
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    u32 addr = (img3 & 0x00FFFFFF) << 5;
    *out_w = w; *out_h = h;
    if (addr >= 0x01800000) return nullptr;
    // EFB copy substitution
    auto ec = s_efb_copies.find(addr);
    if (ec != s_efb_copies.end()) {
        EfbCopy& c = ec->second;
        if (c.w == w && c.h == h && !c.stale) {
            if (c.validated_frame != s_frame) {   // check for a CPU overwrite once per frame
                c.validated_frame = s_frame;
                c.stale = hash_mem(host_ptr(addr), c.bytes) != c.mem_hash;
            }
            if (!c.stale) {
                static int logged;
                if (logged < 40 && s_fields > 3000) {
                    logged++;
                    LOG("gxtex", "EFB copy %08X (%ux%u copyfmt=%u depth=%d) sampled as texfmt=%u", addr, w, h, c.fmt, (int)c.depth, fmt);
                }
                return c.srv;
            }
        }
    }
    u32 minf = (mode0 >> 5) & 7;
    u32 levels = 1;
    if (minf != 0 && minf != 4) {
        u32 maxlod = ((mode1 >> 8) & 0xFF) / 16;
        while (levels <= maxlod && (w >> levels) && (h >> levels)) levels++;
    }
    u32 total = 0;
    for (u32 l = 0; l < levels; l++) total += gx_tex_size_bytes(fmt, std::max(1u, w >> l), std::max(1u, h >> l));
    if (addr + total > 0x01800000) return nullptr;
    const u8* tl = nullptr;
    u32 tlfmt = (tlut >> 10) & 3;
    u64 tlhash = 0;
    if (gx_tex_is_paletted(fmt)) {
        tl = g_tmem + ((tlut & 0x3FF) << 9);
        u32 n = fmt == GX_TF_C4 ? 16 : fmt == GX_TF_C8 ? 256 : 16384;
        tlhash = hash_mem(tl, n * 2) ^ tlfmt;
    }
    u64 key = ((u64)addr << 32) ^ ((u64)fmt << 28) ^ ((u64)w << 14) ^ h ^ ((u64)levels << 24) ^ (tlhash * 31);
    TexEntry& e = s_tex_cache[key];
    e.last_used = s_frame;
    if (e.srv && e.validated_frame == s_frame) return e.srv;
    u64 hh = hash_mem(host_ptr(addr), total) ^ tlhash;
    e.validated_frame = s_frame;
    if (e.srv && hh == e.hash) return e.srv;
    if (e.srv) { e.srv->Release(); e.tex->Release(); e.srv = nullptr; e.tex = nullptr; }
    e.hash = hh;
    struct Timed { double t0 = qpc_ms(); ~Timed() { s_hitch_textures++; s_hitch_texture_ms += qpc_ms() - t0; } } timed;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = levels; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::vector<u32>> data(levels);
    std::vector<D3D11_SUBRESOURCE_DATA> sub(levels);
    u32 off = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        data[l].resize((size_t)lw * lh);
        gx_decode_texture(fmt, lw, lh, host_ptr(addr + off), tl, tlfmt, data[l].data());
        sub[l].pSysMem = data[l].data();
        sub[l].SysMemPitch = lw * 4;
        off += gx_tex_size_bytes(fmt, lw, lh);
    }
    if (FAILED(s_dev->CreateTexture2D(&d, sub.data(), &e.tex))) return nullptr;
    s_dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
    return e.srv;
}

// ---------------------------------------------------------------- per-draw state

// BP fog A / C registers: 11-bit mantissa, 8-bit exponent, sign (a float with the low mantissa bits cut)
static float fog_float(u32 v) {
    u32 bits = (((v >> 19) & 1) << 31) | (((v >> 11) & 0xFF) << 23) | ((v & 0x7FF) << 12);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// TEV colour registers and konstants are written through BP 0xE0..0xE7
static s32 s_tev_reg[4][4], s_tev_k[4][4];
static inline s32 s11(u32 v) { return (s32)(v << 21) >> 21; }

void gx_on_bp(u32 reg, u32 old, u32 val);

// GX scissor (BP 0x20/0x21, offset 0x59) in EFB render-target pixels
static D3D11_RECT scissor_rect() {
    u32 tl = g_gx.bp[0x20], br = g_gx.bp[0x21], off = g_gx.bp[0x59];
    int xo = (int)(off & 0x3FF) * 2, yo = (int)((off >> 10) & 0x3FF) * 2;
    int x0 = (int)((tl >> 12) & 0x7FF) - xo, y0 = (int)(tl & 0x7FF) - yo;
    int x1 = (int)((br >> 12) & 0x7FF) - xo + 1, y1 = (int)(br & 0x7FF) - yo + 1;
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(EFB_W, x1); y1 = std::min(EFB_H, y1);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return D3D11_RECT{x0 * EFB_SCALE, y0 * EFB_SCALE, x1 * EFB_SCALE, y1 * EFB_SCALE};
}

static std::vector<GxOutVertex> s_verts;   // scratch for lines / points (expanded into quads)

// ---------------------------------------------------------------- draw batching
// Consecutive draws with identical pixel-pipeline state are merged into one DrawIndexed.  Vertices are
// transformed on the CPU, so only BP registers (and the viewport, for range fog) affect that state:
// gx_on_bp / gx_on_xf clear s_state_valid, and the state is recomputed (and compared) only then.
struct GxDrawState {
    ID3D11PixelShader* ps;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* rast;
    ID3D11ShaderResourceView* srv[8];
    ID3D11SamplerState* smp[8];
    D3D11_RECT scissor;
    PSConstants cb;
};
static const u32 BATCH_VERTS = 65536;
static GxOutVertex* s_bverts;            // [BATCH_VERTS]
static u32* s_bidx;                      // [BATCH_VERTS * 3]
static u32 s_bn_verts, s_bn_idx;
static GxDrawState s_batch;                // state of the draws in the open batch
static bool s_state_valid;               // s_batch matches the current GX registers
static bool s_state_tri;                 // ... for triangles (true) or expanded lines/points
static bool s_bind_dirty = true;         // something other than gx_draw changed the D3D pipeline bindings
static GxDrawState s_bound;                // what the last batch bound
static u32 s_batches_this_field;

static void batch_init() {
    s_bverts = new GxOutVertex[BATCH_VERTS];
    s_bidx = new u32[BATCH_VERTS * 3];
}

static u32 build_indices(u32 prim, u32 n, u32 base, u32* out) {
    u32* o = out;
    switch (prim) {
    case 0: case 1:   // quads
        for (u32 i = 0; i + 3 < n; i += 4) {
            u32 a = base + i;
            o[0] = a; o[1] = a + 1; o[2] = a + 2; o[3] = a; o[4] = a + 2; o[5] = a + 3; o += 6;
        }
        break;
    case 2:           // triangles
        for (u32 i = 0; i + 2 < n; i += 3) { o[0] = base + i; o[1] = base + i + 1; o[2] = base + i + 2; o += 3; }
        break;
    case 3:           // strip
        for (u32 i = 0; i + 2 < n; i++) {
            u32 a = base + i;
            if (i & 1) { o[0] = a + 1; o[1] = a; } else { o[0] = a; o[1] = a + 1; }
            o[2] = a + 2; o += 3;
        }
        break;
    case 4:           // fan
        for (u32 i = 1; i + 1 < n; i++) { o[0] = base; o[1] = base + i; o[2] = base + i + 1; o += 3; }
        break;
    }
    return (u32)(o - out);
}

static void bind_state(const GxDrawState& st) {
    bool all = s_bind_dirty;
    if (all) {
        UINT stride = sizeof(GxOutVertex), voff = 0;
        s_ctx->IASetVertexBuffers(0, 1, &s_vb, &stride, &voff);
        s_ctx->IASetIndexBuffer(s_ib, DXGI_FORMAT_R32_UINT, 0);
        s_ctx->IASetInputLayout(s_layout);
        s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        s_ctx->VSSetShader(s_vs, nullptr, 0);
        s_ctx->PSSetConstantBuffers(0, 1, &s_ps_cb);
        s_ctx->OMSetRenderTargets(1, &s_efb_rtv, s_dsv);
        D3D11_VIEWPORT vp = {0, 0, (float)(EFB_W * EFB_SCALE), (float)(EFB_H * EFB_SCALE), 0.0f, 1.0f};
        s_ctx->RSSetViewports(1, &vp);
        s_bind_dirty = false;
    }
    static bool s_cb_valid;
    if (!s_cb_valid || memcmp(&st.cb, &s_bound.cb, sizeof st.cb) != 0) {   // most batches share constants
        s_ctx->UpdateSubresource(s_ps_cb, 0, nullptr, &st.cb, 0, 0);
        s_cb_valid = true;
    }
    if (all || st.ps != s_bound.ps) s_ctx->PSSetShader(st.ps, nullptr, 0);
    if (all || memcmp(st.srv, s_bound.srv, sizeof st.srv)) s_ctx->PSSetShaderResources(0, 8, st.srv);
    if (all || memcmp(st.smp, s_bound.smp, sizeof st.smp)) s_ctx->PSSetSamplers(0, 8, st.smp);
    if (all || st.blend != s_bound.blend) {
        float bf[4] = {0, 0, 0, 0};
        s_ctx->OMSetBlendState(st.blend, bf, 0xFFFFFFFF);
    }
    if (all || st.depth != s_bound.depth) s_ctx->OMSetDepthStencilState(st.depth, 0);
    if (all || st.rast != s_bound.rast) s_ctx->RSSetState(st.rast);
    if (all || memcmp(&st.scissor, &s_bound.scissor, sizeof st.scissor)) s_ctx->RSSetScissorRects(1, &st.scissor);
    memcpy(&s_bound, &st, sizeof st);
}

// submit the open batch
static void flush_batch() {
    if (!s_bn_idx) { s_bn_verts = 0; return; }
    D3D11_MAPPED_SUBRESOURCE m;
    // vertex and index rings: append without synchronisation, discard only when they wrap
    if (s_vb_pos + s_bn_verts > VB_VERTS) s_vb_pos = 0;
    D3D11_MAP mode = s_vb_pos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
    static u32 s_ib_pos;
    const u32 ib_cap = VB_VERTS * 3;
    D3D11_MAP imode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (s_ib_pos + s_bn_idx > ib_cap) { s_ib_pos = 0; imode = D3D11_MAP_WRITE_DISCARD; }
    if (SUCCEEDED(s_ctx->Map(s_vb, 0, mode, 0, &m))) {
        memcpy((GxOutVertex*)m.pData + s_vb_pos, s_bverts, s_bn_verts * sizeof(GxOutVertex));
        s_ctx->Unmap(s_vb, 0);
        if (SUCCEEDED(s_ctx->Map(s_ib, 0, imode, 0, &m))) {
            memcpy((u32*)m.pData + s_ib_pos, s_bidx, s_bn_idx * 4);
            s_ctx->Unmap(s_ib, 0);
            bind_state(s_batch);
            s_ctx->DrawIndexed(s_bn_idx, s_ib_pos, (INT)s_vb_pos);
            s_batches_this_field++;
            s_ib_pos += s_bn_idx;
        }
        s_vb_pos += s_bn_verts;
    }
    s_bn_verts = s_bn_idx = 0;
}

// lines and points become screen-aligned quads
static void expand_lines_points(u32 prim, std::vector<GxOutVertex>& v) {
    u32 lp = g_gx.bp[0x22];
    float lw = (lp & 0xFF) / 6.0f, ps = ((lp >> 8) & 0xFF) / 6.0f;
    std::vector<GxOutVertex> out;
    auto offset = [&](const GxOutVertex& a, float dx, float dy) {
        GxOutVertex r = a;
        r.pos[0] += dx * 2.0f / EFB_W * a.pos[3];
        r.pos[1] -= dy * 2.0f / EFB_H * a.pos[3];
        return r;
    };
    if (prim == 7) {
        float h = std::max(ps, 1.0f) * 0.5f;
        for (auto& p : v) {
            out.push_back(offset(p, -h, -h)); out.push_back(offset(p, h, -h));
            out.push_back(offset(p, h, h)); out.push_back(offset(p, -h, h));
        }
    } else {
        std::vector<std::pair<u32, u32>> segs;
        if (prim == 5) for (u32 i = 0; i + 1 < v.size(); i += 2) segs.push_back({i, i + 1});
        else for (u32 i = 0; i + 1 < v.size(); i++) segs.push_back({i, i + 1});
        float h = std::max(lw, 1.0f) * 0.5f;
        for (auto& s : segs) {
            const GxOutVertex &a = v[s.first], &b = v[s.second];
            float ax = a.pos[0] / a.pos[3] * EFB_W / 2, ay = -a.pos[1] / a.pos[3] * EFB_H / 2;
            float bx = b.pos[0] / b.pos[3] * EFB_W / 2, by = -b.pos[1] / b.pos[3] * EFB_H / 2;
            float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
            float nx = l > 0 ? -dy / l * h : 0, ny = l > 0 ? dx / l * h : h;
            out.push_back(offset(a, nx, ny)); out.push_back(offset(b, nx, ny));
            out.push_back(offset(b, -nx, -ny)); out.push_back(offset(a, -nx, -ny));
        }
    }
    v.swap(out);
}

// Debug: watch the centre depth sample and report every transition between "far" and "near".
static float xf_f(u32 a) { float f; memcpy(&f, &g_gx.xfmem[a], 4); return f; }
static bool s_probe_draws;   // per-draw depth probing armed (after a copy+clear in the debug window)
static const GxOutVertex* s_last_draw_verts;   // vertices of the most recent draw (debug dumps)
static void depth_probe(const char* what, u32 prim, u32 count) {
    flush_batch();
    static ID3D11Texture2D* probe;
    static float last = 1.0f;
    static int reports;
    if (reports > 30) return;
    if (!probe) {   // depth resources can only be copied whole (no CopySubresourceRegion boxes)
        D3D11_TEXTURE2D_DESC d;
        s_depth->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        s_dev->CreateTexture2D(&d, nullptr, &probe);
        if (!probe) return;
    }
    u32 cx = EFB_W * EFB_SCALE / 2, cy = 224 * EFB_SCALE;
    s_ctx->CopyResource(probe, s_depth);
    D3D11_MAPPED_SUBRESOURCE pm;
    if (FAILED(s_ctx->Map(probe, 0, D3D11_MAP_READ, 0, &pm))) return;
    float z;
    memcpy(&z, (u8*)pm.pData + (size_t)cy * pm.RowPitch + cx * 4, 4);
    s_ctx->Unmap(probe, 0);
    if ((z < 0.9f) != (last < 0.9f)) {
        reports++;
        LOG("gxprobe", "field %u after %s #%u: centre depth %.5f -> %.5f (prim=%u n=%u zmode=%02X cm0=%04X pe=%02X ortho=%u)",
            s_fields, what, s_draws_this_field, last, z, prim, count, g_gx.bp[0x40] & 0xFF, g_gx.bp[0x41] & 0xFFFF,
            g_gx.bp[0x43] & 0xFF, g_gx.xfmem[0x1026]);
        if (s_probe_draws && z < 0.9f && !strcmp(what, "draw")) {
            s_probe_draws = false;
            for (u32 i = 0; i < count && i < 24; i++)
                LOG("gxprobe", "  v%u pos=(%.3f %.3f %.5f %.3f)", i, s_last_draw_verts[i].pos[0], s_last_draw_verts[i].pos[1],
                    s_last_draw_verts[i].pos[2], s_last_draw_verts[i].pos[3]);
            LOG("gxprobe", "  vcd=%08X/%08X genmode=%08X proj %g %g %g %g %g %g vp %g %g %g %g %g %g", g_gx.vcd_lo, g_gx.vcd_hi, g_gx.bp[0],
                xf_f(0x1020), xf_f(0x1021), xf_f(0x1022), xf_f(0x1023), xf_f(0x1024), xf_f(0x1025),
                xf_f(0x101A), xf_f(0x101B), xf_f(0x101C), xf_f(0x101D), xf_f(0x101E), xf_f(0x101F));
            LOG("gxprobe", "  ztex1(F4)=%06X ztex2(F5)=%06X tev stages=%u order0=%06X tex0 img0=%06X img3=%06X pe43=%06X", g_gx.bp[0xF4] & 0xFFFFFF,
                g_gx.bp[0xF5] & 0xFFFFFF, ((g_gx.bp[0] >> 10) & 15) + 1, g_gx.bp[0x28] & 0xFFFFFF, g_gx.bp[0x88] & 0xFFFFFF,
                g_gx.bp[0x94] & 0xFFFFFF, g_gx.bp[0x43] & 0xFFFFFF);
            u32 mi = g_gx.xfmem[0x1018] & 63, b = mi * 4;
            LOG("gxprobe", "  matidx=%u cp30=%08X vat_a[0..7]=%08X %08X %08X %08X %08X %08X %08X %08X", mi, g_gx.matindex_a,
                g_gx.vat_a[0], g_gx.vat_a[1], g_gx.vat_a[2], g_gx.vat_a[3], g_gx.vat_a[4], g_gx.vat_a[5], g_gx.vat_a[6], g_gx.vat_a[7]);
            for (u32 r = 0; r < 3; r++)
                LOG("gxprobe", "  mtx row%u: %g %g %g %g", r, xf_f(b + r * 4), xf_f(b + r * 4 + 1), xf_f(b + r * 4 + 2), xf_f(b + r * 4 + 3));
        }
    }
    last = z;
}

// profiling counters (__rdtsc ticks; converted to ms in gx_on_retrace)
u64 g_prof_draw_ticks, g_prof_xform_ticks;
struct ProfScope { u64& acc; u64 t0; ProfScope(u64& a) : acc(a), t0(__rdtsc()) {}
    ~ProfScope() { acc += __rdtsc() - t0; } };

// pixel-pipeline state for the current GX registers; false if the draw cannot be rendered
static bool compute_state(GxDrawState& st, bool tri, u32 cull) {
    memset(&st, 0, sizeof st);   // memcmp'd: padding must be zero too
    static ShaderKey s_last_key;
    static ID3D11PixelShader* s_last_ps;
    ShaderKey key = gx_shader_key();
    if (s_last_ps && key == s_last_key) st.ps = s_last_ps;
    else { st.ps = get_ps(key); s_last_key = key; s_last_ps = st.ps; }
    if (!st.ps) return false;
    PSConstants& cb = st.cb;
    memcpy(cb.reg, s_tev_reg, sizeof cb.reg);
    memcpy(cb.k, s_tev_k, sizeof cb.k);
    u32 ac = g_gx.bp[0xF3];
    cb.aref[0] = ac & 0xFF; cb.aref[1] = (ac >> 8) & 0xFF;
    cb.cdst[0] = g_gx.bp[0x42] & 0xFF;
    cb.zbias[0] = (s32)(g_gx.bp[0xF4] & 0xFFFFFF);
    if (key.fog) {
        const u32* bp = g_gx.bp;
        cb.fog0[0] = fog_float(bp[0xEE]);
        cb.fog0[1] = fog_float(bp[0xF1]);
        cb.fogi[0] = (s32)(bp[0xEF] & 0xFFFFFF);
        cb.fogi[1] = (s32)(bp[0xF0] & 0x1F);
        u32 col = bp[0xF2];
        if (dbg_flags & 0x80) col = 0xFF00FF;
        cb.fog1[0] = (float)((col >> 16) & 0xFF); cb.fog1[1] = (float)((col >> 8) & 0xFF); cb.fog1[2] = (float)(col & 0xFF);
        // log the first few distinct fog setups (which scenes use fog)
        static u32 s_fog_logged, s_fog_last[5];
        u32 cur[5] = {key.fog, bp[0xEE], bp[0xEF], bp[0xF1], col};
        if (s_fog_logged < 16 && memcmp(cur, s_fog_last, sizeof cur)) {
            s_fog_logged++;
            memcpy(s_fog_last, cur, sizeof cur);
            LOG("gx", "field %u: fog mode %02X A=%g B=%u>>%u C=%g color=%06X range=%03X", s_fields, key.fog, cb.fog0[0],
                cb.fogi[0], cb.fogi[1], cb.fog0[1], col & 0xFFFFFF, bp[0xE8] & 0x7FF);
        }
        if (key.fog & 16) {
            // range adjustment: screen-space centre (BP 0xE8, offset by 342 like the viewport) and a table
            // of 10 x 12-bit k coefficients (BP 0xE9..0xED), 1.0 = 256
            float wd2 = 2.0f * xf_f(0x101A);
            if (wd2 == 0.0f) wd2 = (float)EFB_W;
            cb.fog0[2] = ((float)((s32)(bp[0xE8] & 0x3FF) - 342) / wd2) * 2.0f - 1.0f;
            cb.fog0[3] = wd2 * EFB_SCALE;
            for (int i = 0; i < 10; i++) {
                u32 r = bp[0xE9 + i / 2] >> (12 * (i & 1));
                cb.fogrange[i / 4][i % 4] = (float)(r & 0xFFF) / 256.0f * 4.0f;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        u32 a = g_gx.bp[0x06 + 3 * i], b = g_gx.bp[0x07 + 3 * i], c = g_gx.bp[0x08 + 3 * i];
        s32 scale = (s32)(((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 3) << 4)) - 17;
        cb.ind[i * 2][0] = s11(a & 0x7FF); cb.ind[i * 2 + 1][0] = s11((a >> 11) & 0x7FF);
        cb.ind[i * 2][1] = s11(b & 0x7FF); cb.ind[i * 2 + 1][1] = s11((b >> 11) & 0x7FF);
        cb.ind[i * 2][2] = s11(c & 0x7FF); cb.ind[i * 2 + 1][2] = s11((c >> 11) & 0x7FF);
        cb.ind[i * 2][3] = cb.ind[i * 2 + 1][3] = scale + 10;
    }
    for (int i = 0; i < 2; i++) {
        u32 ss = g_gx.bp[0x25 + i];
        for (int j = 0; j < 2; j++) {
            cb.indscale[i][2 * j] = 1.0f / (float)(1 << ((ss >> (8 * j)) & 15));
            cb.indscale[i][2 * j + 1] = 1.0f / (float)(1 << ((ss >> (8 * j + 4)) & 15));
        }
    }
    // textures used by this shader
    u32 used = 0;
    for (u32 s = 0; s < key.num_stages; s++) if ((key.order[s] >> 6) & 1) used |= 1u << (key.order[s] & 7);
    for (u32 n = 0; n < key.num_ind; n++) used |= 1u << ((key.ind_order >> (6 * n)) & 7);
    for (u32 t = 0; t < 8; t++) {
        if (!(used & (1u << t))) continue;
        u32 tw = 1, th = 1;
        st.srv[t] = get_texture(t, &tw, &th);
        st.smp[t] = get_sampler(tex_reg(0x80, 0xA0, t), tex_reg(0x84, 0xA4, t));
        cb.texdim[t][0] = (float)tw; cb.texdim[t][1] = (float)th;
        cb.texdim[t][2] = 1.0f / tw; cb.texdim[t][3] = 1.0f / th;
    }
    st.blend = get_blend();
    // DD_DBG_FLAGS: 1 = disable culling, 2 = disable depth test, 4 = never write depth (renderer debugging)
    st.depth = (dbg_flags & 2) ? get_depth_always() : get_depth((dbg_flags & 4) != 0);
    st.rast = get_rast(tri && !(dbg_flags & 1) ? cull : 0);
    st.scissor = scissor_rect();
    return true;
}

void gx_draw(u32 prim, u32 vat, u32 count, const u8* data) {
    ProfScope prof_(g_prof_draw_ticks);
    s_draws_this_field++;
    if (!s_ok || count == 0) return;
    u32 cull = (g_gx.bp[0x00] >> 14) & 3;
    bool tri = prim <= 4;
    if (tri && cull == 3) return;
    u32 nout = tri ? count : count * 4;   // lines / points: at most one quad per vertex
    if (nout > BATCH_VERTS) return;
    if (!s_state_valid || tri != s_state_tri) {
        GxDrawState st;
        if (!compute_state(st, tri, cull)) return;
        if (memcmp(&st, &s_batch, sizeof st) != 0) {
            flush_batch();
            memcpy(&s_batch, &st, sizeof st);
        }
        s_state_valid = true;
        s_state_tri = tri;
    }
    if (s_bn_verts + nout > BATCH_VERTS) flush_batch();
    GxOutVertex* V = s_bverts + s_bn_verts;
    GxFrameTransform ft;
    ft.inv_efb_w = 1.0f / EFB_W;
    ft.inv_efb_h = 1.0f / EFB_H;
    if (!tri) s_verts.resize(count);
    { ProfScope px_(g_prof_xform_ticks); gx_process_vertices(vat, count, data, tri ? V : s_verts.data(), ft); }
    if (!tri) {
        expand_lines_points(prim, s_verts);
        prim = 0;
        count = (u32)s_verts.size();
        if (count == 0) return;
        memcpy(V, s_verts.data(), count * sizeof(GxOutVertex));
    }
    s_last_draw_verts = V;
    if (dbg_field && s_fields == dbg_field && s_draws_this_field <= 40) {
        auto xff = [](u32 a) { float f; memcpy(&f, &g_gx.xfmem[a], 4); return f; };
        LOG("gxdbg", "draw prim=%u vat=%u n=%u vcd=%08X/%08X vat=%08X/%08X/%08X genmode=%08X", prim, vat, count, g_gx.vcd_lo,
            g_gx.vcd_hi, g_gx.vat_a[vat], g_gx.vat_b[vat], g_gx.vat_c[vat], g_gx.bp[0]);
        LOG("gxdbg", "  vp %.1f %.1f %.1f | %.1f %.1f %.1f  proj %.3f %.3f %.3f %.3f %.3f %.3f type=%u",
            xff(0x101A), xff(0x101B), xff(0x101C), xff(0x101D), xff(0x101E), xff(0x101F), xff(0x1020), xff(0x1021),
            xff(0x1022), xff(0x1023), xff(0x1024), xff(0x1025), g_gx.xfmem[0x1026]);
        for (u32 i = 0; i < std::min(count, 3u); i++)
            LOG("gxdbg", "  v%u clip=(%.2f %.2f %.3f %.2f) col0=(%.2f %.2f %.2f %.2f) tc0=(%.3f %.3f %.3f)", i, V[i].pos[0],
                V[i].pos[1], V[i].pos[2], V[i].pos[3], V[i].col0[0], V[i].col0[1],
                V[i].col0[2], V[i].col0[3], V[i].tex[0][0], V[i].tex[0][1], V[i].tex[0][2]);
        LOG("gxdbg", "  zmode=%08X cmode0=%08X scis=%08X/%08X off=%08X tev0=%08X/%08X tref=%08X", g_gx.bp[0x40], g_gx.bp[0x41],
            g_gx.bp[0x20], g_gx.bp[0x21], g_gx.bp[0x59], g_gx.bp[0xC0], g_gx.bp[0xC1], g_gx.bp[0x28]);
    }
    if (dbg_field && s_fields == dbg_field) {   // whole-field visibility statistics
        static u32 st_draws, st_behind, st_zout, st_xyout, st_persp, st_ztest;
        st_draws++;
        if (!g_gx.xfmem[0x1026]) st_persp++;
        if (g_gx.bp[0x40] & 1) st_ztest++;
        bool behind = true, zout = true, xyout = true;
        for (u32 i = 0; i < count; i++) {
            const float* p = V[i].pos;
            if (p[3] > 0) behind = false;
            if (p[2] >= 0 && p[2] <= p[3]) zout = false;
            if (fabsf(p[0]) <= fabsf(p[3]) && fabsf(p[1]) <= fabsf(p[3])) xyout = false;
        }
        st_behind += behind; st_zout += zout; st_xyout += xyout;
        bool big = false;
        for (u32 i = 0; i + 1 < count && !big; i++) {
            const float* p = V[i].pos;
            const float* q = V[i + 1].pos;
            if (p[3] > 0 && q[3] > 0 && fabsf(p[0] / p[3] - q[0] / q[3]) > 0.8f) big = true;
        }
        bool nearz = false;
        if (g_gx.bp[0x40] & 0x10)
            for (u32 i = 0; i < count; i++) {
                const float* p = V[i].pos;
                if (p[3] > 0 && p[2] / p[3] < 0.9f) nearz = true;
            }
        static u32 st_near;
        if (nearz && st_near++ >= 40) nearz = false;
        if (big || nearz || ((g_gx.bp[0x40] & 0x10) && st_draws <= 3)) {   // screen-covering / near-depth draws
            float zmin = 9, zmax = -9, xmin = 9, xmax = -9, ymin = 9, ymax = -9;
            for (u32 i = 0; i < count; i++) {
                const float* p = V[i].pos;
                if (p[3] <= 0) continue;
                zmin = std::min(zmin, p[2] / p[3]); zmax = std::max(zmax, p[2] / p[3]);
                xmin = std::min(xmin, p[0] / p[3]); xmax = std::max(xmax, p[0] / p[3]);
                ymin = std::min(ymin, p[1] / p[3]); ymax = std::max(ymax, p[1] / p[3]);
            }
            LOG("gxz", "#%u big=%d near=%d zmode=%02X persp=%d n=%u z=[%.5f %.5f] x=[%.2f %.2f] y=[%.2f %.2f] vp z %.0f/%.0f proj4/5 %g %g cm0=%04X",
                st_draws, (int)big, (int)nearz, g_gx.bp[0x40] & 0xFF, !g_gx.xfmem[0x1026], count, zmin, zmax, xmin, xmax, ymin, ymax,
                [] { float f; memcpy(&f, &g_gx.xfmem[0x101C], 4); return f; }(),
                [] { float f; memcpy(&f, &g_gx.xfmem[0x101F], 4); return f; }(),
                [] { float f; memcpy(&f, &g_gx.xfmem[0x1024], 4); return f; }(),
                [] { float f; memcpy(&f, &g_gx.xfmem[0x1025], 4); return f; }(), g_gx.bp[0x41] & 0xFFFF);
            if (big)
                LOG("gxz", "   stages=%u tev0=%06X/%06X tev1=%06X/%06X order=%06X ksel=%06X k0=%d,%d,%d,%d reg0=%d,%d,%d,%d img0=%06X img3=%06X",
                    ((g_gx.bp[0] >> 10) & 15) + 1, g_gx.bp[0xC0] & 0xFFFFFF, g_gx.bp[0xC1] & 0xFFFFFF, g_gx.bp[0xC2] & 0xFFFFFF,
                    g_gx.bp[0xC3] & 0xFFFFFF, g_gx.bp[0x28] & 0xFFFFFF, g_gx.bp[0xF6] & 0xFFFFFF, s_tev_k[0][0], s_tev_k[0][1],
                    s_tev_k[0][2], s_tev_k[0][3], s_tev_reg[1][0], s_tev_reg[1][1], s_tev_reg[1][2], s_tev_reg[1][3],
                    g_gx.bp[0x88] & 0xFFFFFF, g_gx.bp[0x94] & 0xFFFFFF);
        }
        if (st_draws % 500 == 0 || s_draws_this_field == 1)
            LOG("gxstat", "draws=%u persp=%u ztest=%u all-behind=%u z-out=%u xy-out=%u zclear=%06X", st_draws, st_persp,
                st_ztest, st_behind, st_zout, st_xyout, g_gx.bp[0x51] & 0xFFFFFF);
    }
    if (tri && (dbg_flags & 0x30) && g_gx.xfmem[0x1026] && (g_gx.bp[0x41] & 1) && count == 4) {
        // 0x10: skip 1-stage blended screen quads, 0x20: skip multi-stage ones (post-effect debugging)
        u32 stages = ((g_gx.bp[0] >> 10) & 15) + 1;
        if (((dbg_flags & 0x10) && stages == 1) || ((dbg_flags & 0x20) && stages > 1)) {
            const float* a = V[0].pos;
            const float* b = V[2].pos;
            if (fabsf(a[0] - b[0]) > 1.5f) return;
        }
    }
    u32* I = s_bidx + s_bn_idx;
    u32 nidx = build_indices(prim, count, s_bn_verts, I);
    {   // debug: large alpha-blended layers anywhere in the race (screen area of triangles in front of the camera)
        static const bool area_dbg = getenv("DD_DBG_AREA") != nullptr;
        static int area_logged;
        if (area_dbg && (g_gx.bp[0x41] & 1) && area_logged < 60 && s_fields > 3000) {
            double area = 0;
            for (u32 t = 0; t + 2 < nidx; t += 3) {
                const float* a = V[I[t] - s_bn_verts].pos;
                const float* b = V[I[t + 1] - s_bn_verts].pos;
                const float* d = V[I[t + 2] - s_bn_verts].pos;
                if (a[3] <= 0 || b[3] <= 0 || d[3] <= 0) continue;
                auto cl = [](float v) { return v < -1 ? -1.0f : v > 1 ? 1.0f : v; };
                float ax = cl(a[0] / a[3]), ay = cl(a[1] / a[3]), bx = cl(b[0] / b[3]), by = cl(b[1] / b[3]), dx = cl(d[0] / d[3]), dy = cl(d[1] / d[3]);
                area += fabs((bx - ax) * (dy - ay) - (dx - ax) * (by - ay)) * 0.5;
            }
            if (area > 0.8) {
                area_logged++;
                LOG("gxarea", "field %u draw %u area=%.2f n=%u persp=%d zmode=%02X cm0=%04X stages=%u tev0=%06X/%06X order=%06X k0=%d,%d,%d,%d reg1=%d,%d,%d,%d img0=%06X fog=%06X",
                    s_fields, s_draws_this_field, area, count, !g_gx.xfmem[0x1026], g_gx.bp[0x40] & 0xFF, g_gx.bp[0x41] & 0xFFFF,
                    ((g_gx.bp[0] >> 10) & 15) + 1, g_gx.bp[0xC0] & 0xFFFFFF, g_gx.bp[0xC1] & 0xFFFFFF, g_gx.bp[0x28] & 0xFFFFFF,
                    s_tev_k[0][0], s_tev_k[0][1], s_tev_k[0][2], s_tev_k[0][3], s_tev_reg[1][0], s_tev_reg[1][1], s_tev_reg[1][2],
                    s_tev_reg[1][3], g_gx.bp[0x88] & 0xFFFFFF, g_gx.bp[0xF1] & 0xFFFFFF);
            }
        }
    }
    s_bn_verts += count;
    s_bn_idx += nidx;
    if (s_probe_draws) depth_probe("draw", prim, count);
}

// ---------------------------------------------------------------- EFB copies
static void blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, int dw, int dh, const BlitCB& cbv,
                 ID3D11PixelShader* ps, ID3D11SamplerState* smp) {
    s_ctx->UpdateSubresource(s_copy_cb, 0, nullptr, &cbv, 0, 0);
    ID3D11ShaderResourceView* none[8] = {};
    s_ctx->PSSetShaderResources(0, 8, none);
    s_ctx->OMSetRenderTargets(1, &dst, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)dw, (float)dh, 0, 1};
    s_ctx->RSSetViewports(1, &vp);
    D3D11_RECT r = {0, 0, dw, dh};
    s_ctx->RSSetScissorRects(1, &r);
    s_ctx->RSSetState(get_rast(0));
    s_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    s_ctx->OMSetDepthStencilState(nullptr, 0);
    s_ctx->IASetInputLayout(nullptr);
    s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_ctx->VSSetShader(s_blit_vs, nullptr, 0);
    s_ctx->VSSetConstantBuffers(0, 1, &s_copy_cb);
    s_ctx->PSSetShader(ps, nullptr, 0);
    s_ctx->PSSetConstantBuffers(0, 1, &s_copy_cb);
    s_ctx->PSSetShaderResources(0, 1, &src);
    s_ctx->PSSetSamplers(0, 1, &smp);
    s_ctx->Draw(3, 0);
    s_ctx->PSSetShaderResources(0, 8, none);
    s_bind_dirty = true;
}

static void release_copy(EfbCopy& c) {
    if (c.srv) c.srv->Release();
    if (c.tex) c.tex->Release();
    c.srv = nullptr; c.tex = nullptr;
}

// Clear depth inside `r` only (a copy-with-clear of a sub-rectangle, e.g. a render-to-texture
// strip, must not wipe the depth of the rest of the frame).
static void clear_depth_rect(const D3D11_RECT& r, float z) {
    if (dbg_flags & 8) z = 1.0f;
    if (r.left <= 0 && r.top <= 0 && r.right >= EFB_W * EFB_SCALE && r.bottom >= EFB_H * EFB_SCALE) {
        s_ctx->ClearDepthStencilView(s_dsv, D3D11_CLEAR_DEPTH, z, 0);
        return;
    }
    BlitCB cbv = {};
    cbv.p0[0] = z;
    s_ctx->UpdateSubresource(s_copy_cb, 0, nullptr, &cbv, 0, 0);
    s_ctx->OMSetRenderTargets(0, nullptr, s_dsv);
    D3D11_VIEWPORT vp = {0, 0, (float)(EFB_W * EFB_SCALE), (float)(EFB_H * EFB_SCALE), 0, 1};
    s_ctx->RSSetViewports(1, &vp);
    s_ctx->RSSetScissorRects(1, &r);
    s_ctx->RSSetState(get_rast(0));
    s_ctx->OMSetDepthStencilState(s_zclear_ds, 0);
    s_ctx->IASetInputLayout(nullptr);
    s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_ctx->VSSetShader(s_zclear_vs, nullptr, 0);
    s_ctx->VSSetConstantBuffers(0, 1, &s_copy_cb);
    s_ctx->PSSetShader(s_null_ps, nullptr, 0);
    s_ctx->Draw(3, 0);
    s_bind_dirty = true;
}

static void efb_clear_rect(u32 x, u32 y, u32 w, u32 h) {
    u32 ar = g_gx.bp[0x4F], gb = g_gx.bp[0x50], z = g_gx.bp[0x51] & 0xFFFFFF;
    float col[4] = {(ar & 255) / 255.0f, ((gb >> 8) & 255) / 255.0f, (gb & 255) / 255.0f, ((ar >> 8) & 255) / 255.0f};
    u32 cm0 = g_gx.bp[0x41];
    bool color_upd = (cm0 >> 3) & 1, alpha_upd = (cm0 >> 4) & 1, z_upd = (g_gx.bp[0x40] >> 4) & 1;
    if (!efb_has_alpha()) col[3] = 1.0f;
    D3D11_RECT r = {(LONG)(x * EFB_SCALE), (LONG)(y * EFB_SCALE), (LONG)((x + w) * EFB_SCALE), (LONG)((y + h) * EFB_SCALE)};
    ID3D11DeviceContext1* c1 = nullptr;
    s_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1);
    if (c1) {
        if (color_upd || alpha_upd) c1->ClearView(s_efb_rtv, col, &r, 1);
        if (z_upd) clear_depth_rect(r, z / 16777216.0f);
        c1->Release();
    } else {
        s_ctx->ClearRenderTargetView(s_efb_rtv, col);
        s_ctx->ClearDepthStencilView(s_dsv, D3D11_CLEAR_DEPTH, z / 16777216.0f, 0);
    }
}

static void save_png(ID3D11Texture2D* tex, const char* path);
static bool s_png_alpha;

// Debug: read EFB colour (RGBA) at a few points.
static void efb_sample_log(const char* what) {
    flush_batch();
    static ID3D11Texture2D* st;
    if (!st) {
        D3D11_TEXTURE2D_DESC d;
        s_efb->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        s_dev->CreateTexture2D(&d, nullptr, &st);
        if (!st) return;
    }
    s_ctx->CopyResource(st, s_efb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(s_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return;
    const u32 pts[4][2] = {{304, 112}, {304, 224}, {304, 400}, {100, 300}};
    char buf[256]; int n = 0;
    for (auto& p : pts) {
        const u8* px = (const u8*)m.pData + (size_t)(p[1] * EFB_SCALE) * m.RowPitch + (size_t)(p[0] * EFB_SCALE) * 4;
        n += snprintf(buf + n, sizeof buf - n, " (%u,%u)=%02X%02X%02X/a%u", p[0], p[1], px[0], px[1], px[2], px[3]);
    }
    s_ctx->Unmap(st, 0);
    LOG("gxefb", "field %u draw %u %s:%s dstalpha=%06X", s_fields, s_draws_this_field, what, buf, g_gx.bp[0x42] & 0xFFFFFF);
}

static void efb_copy(u32 val) {
    if (!s_ok) return;
    flush_batch();
    u32 tl = g_gx.bp[0x49], sz = g_gx.bp[0x4A];
    u32 sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, sw = (sz & 0x3FF) + 1, sh = ((sz >> 10) & 0x3FF) + 1;
    u32 dst = (g_gx.bp[0x4B] & 0x00FFFFFF) << 5;
    bool to_xfb = (val >> 14) & 1, clear = (val >> 11) & 1, half = (val >> 9) & 1, intensity = (val >> 15) & 1;
    u32 tpf = (val >> 3) & 15;
    u32 fmt = (tpf >> 1) | ((tpf & 1) << 3);
    bool depth_src = (g_gx.bp[0x43] & 7) == 3;
    static int logged;
    if (logged < 12 || dbg_window(s_fields)) {
        logged++;
        LOG("gxdbg", "field %u draw#%u EFB copy val=%06X src=(%u,%u %ux%u) dst=%08X stride=%u xfb=%d clear=%d fmt=%u pe_ctl=%06X yscale=%06X zclear=%06X zupd=%d",
            s_fields, s_draws_this_field, val, sx, sy, sw, sh, dst, g_gx.bp[0x4D], to_xfb, clear, fmt, g_gx.bp[0x43], g_gx.bp[0x4E], g_gx.bp[0x51] & 0xFFFFFF, (int)((g_gx.bp[0x40] >> 4) & 1));
    }
    if (dbg_field && s_fields == dbg_field) {   // dump colour + depth as they are at each copy of the debug field
        static int nd;
        char path[64];
        CreateDirectoryA("shots", nullptr);
        snprintf(path, sizeof path, "shots/dbg_color_%d.png", nd);
        save_png(s_efb, path);
        snprintf(path, sizeof path, "shots/dbg_depth_%d.png", nd);
        save_png(s_depth, path);
        snprintf(path, sizeof path, "shots/dbg_alpha_%d.png", nd++);
        s_png_alpha = true;
        save_png(s_efb, path);
        s_png_alpha = false;
    }
    // snapshot the EFB region source
    s_ctx->CopyResource(s_efb_copy_src, s_efb);
    ID3D11ShaderResourceView* src = depth_src ? s_depth_srv : s_efb_copy_src_srv;
    float W = (float)(EFB_W * EFB_SCALE), H = (float)(EFB_H * EFB_SCALE);
    BlitCB cbv = {};
    cbv.rect[0] = sx * EFB_SCALE / W; cbv.rect[1] = sy * EFB_SCALE / H;
    cbv.rect[2] = sw * EFB_SCALE / W; cbv.rect[3] = sh * EFB_SCALE / H;
    if (to_xfb) {
        EfbCopy& c = s_xfb_copies[dst];
        u32 ow = sw * EFB_SCALE, oh = sh * EFB_SCALE;
        if (!c.tex || c.w != sw || c.h != sh) {
            release_copy(c);
            ID3D11RenderTargetView* rtv = nullptr;
            create_rt(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, &c.tex, &rtv, &c.srv);
            if (rtv) rtv->Release();
            c.w = sw; c.h = sh;
        }
        // display copies need no conversion: copy the region straight from the EFB
        D3D11_BOX box = {sx * EFB_SCALE, sy * EFB_SCALE, 0, (sx + sw) * EFB_SCALE, (sy + sh) * EFB_SCALE, 1};
        s_ctx->CopySubresourceRegion(c.tex, 0, 0, 0, 0, s_efb, 0, &box);
        s_frame++;
        c.frame = s_frame;
        // DD_DBG_SLOW=ms: busy-wait this long per game frame (simulates a slower host when testing timing behaviour)
        static const double slow_ms = getenv("DD_DBG_SLOW") ? atof(getenv("DD_DBG_SLOW")) : 0.0;
        if (slow_ms > 0) for (double t0 = qpc_ms(); qpc_ms() - t0 < slow_ms;) YieldProcessor();
    } else {
        u32 tw = half ? std::max(1u, sw / 2) : sw, th = half ? std::max(1u, sh / 2) : sh;
        int mode = 5;
        if (intensity && !depth_src) mode = (fmt == 2 || fmt == 3) ? 1 : 0;
        else switch (fmt) {
            case 0: case 1: case 8: mode = 2; break;
            case 2: case 3: mode = 3; break;
            case 4: mode = 4; break;
            case 5: case 6: mode = 5; break;
            case 7: mode = 6; break;
            case 9: mode = 7; break;
            case 10: mode = 8; break;
            case 11: mode = 9; break;
            case 12: mode = 10; break;
            }
        EfbCopy& c = s_efb_copies[dst];
        u32 ow = tw * EFB_SCALE, oh = th * EFB_SCALE;
        if (!c.tex || c.w != tw || c.h != th) {
            release_copy(c);
            ID3D11RenderTargetView* rtv = nullptr;
            create_rt(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, &c.tex, &rtv, &c.srv);
            if (rtv) rtv->Release();
            c.w = tw; c.h = th;
        }
        c.fmt = fmt;
        c.depth = depth_src;
        // GX texture formats that the copy produces (for the RAM-overwrite check)
        static const u32 tex_fmt_for_copy[16] = {GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3,
                                                 GX_TF_RGBA8, GX_TF_I8, GX_TF_I8, GX_TF_I8, GX_TF_I8, GX_TF_IA8,
                                                 GX_TF_IA8, GX_TF_I8, GX_TF_I8, GX_TF_I8};
        c.bytes = std::min(gx_tex_size_bytes(tex_fmt_for_copy[fmt & 15], tw, th), 0x01800000u - std::min(dst, 0x01800000u));
        c.mem_hash = hash_mem(host_ptr(dst), c.bytes);
        c.validated_frame = s_frame;
        c.stale = false;
        ID3D11RenderTargetView* rtv = nullptr;
        s_dev->CreateRenderTargetView(c.tex, nullptr, &rtv);
        cbv.p0[0] = (float)mode;
        cbv.p1[1] = depth_src ? 1.0f : 0.0f;
        cbv.p0[1] = half ? 1.0f : 0.0f;
        cbv.p0[2] = 1.0f / W * EFB_SCALE;
        cbv.p0[3] = 1.0f / H * EFB_SCALE;
        blit(src, rtv, ow, oh, cbv, s_copy_ps, half ? s_linear_clamp : s_point_clamp);
        rtv->Release();
    }
    if (clear) efb_clear_rect(sx, sy, sw, sh);
    if (dbg_window(s_fields)) {
        if (!to_xfb && sw > 100) efb_sample_log("after copy");
        depth_probe(clear ? "copy+clear" : "copy", val, to_xfb);
        if (clear && to_xfb) s_probe_draws = true;
    }
}

// ---------------------------------------------------------------- register hooks
static void load_tlut(u32 v) {
    u32 src = (g_gx.bp[0x64] & 0x00FFFFFF) << 5;
    u32 tmem = (v & 0x3FF) << 9;
    u32 n = ((v >> 10) & 0x7FF) * 32;
    if (src + n <= 0x01800000 && tmem + n <= sizeof g_tmem) memcpy(g_tmem + tmem, host_ptr(src), n);
}

void gx_on_bp(u32 reg, u32 old, u32 val) {
    // any change can affect the pixel pipeline; TLUT loads and copies act even when the value repeats
    if (val != old || reg == 0x52 || reg == 0x65) s_state_valid = false;
    switch (reg) {
    case 0x52: efb_copy(val); break;
    case 0x65: load_tlut(val); break;
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: {
        int i = (reg - 0xE0) / 2;
        s32* d = (val >> 23) & 1 ? s_tev_k[i] : s_tev_reg[i];
        d[0] = s11(val & 0x7FF); d[3] = s11((val >> 12) & 0x7FF);
        break;
    }
    case 0xE1: case 0xE3: case 0xE5: case 0xE7: {
        int i = (reg - 0xE1) / 2;
        s32* d = (val >> 23) & 1 ? s_tev_k[i] : s_tev_reg[i];
        d[2] = s11(val & 0x7FF); d[1] = s11((val >> 12) & 0x7FF);
        break;
    }
    }
}
void gx_on_xf(u32 addr, u32 count, const u32* data) {
    if (addr <= 0x101A && addr + count > 0x101A) s_state_valid = false;   // viewport width: range fog constants
}
void gx_on_cp(u32 reg, u32 val) {}

// ---------------------------------------------------------------- presentation
static u32 s_last_presented_frame = ~0u;

static void depth_probe(const char* what, u32 prim, u32 count);
void gx_on_retrace() {
    if (dbg_field && s_fields < dbg_field && s_fields + 1500 > dbg_field) depth_probe("field end", 0, 0);   // once per field: find when depth goes bad
    if (++s_fields % 250 == 0) {
        static u64 last_ms = GetTickCount64();
        static u32 last_frame;
        u64 now = GetTickCount64();
        double secs = (double)(now - last_ms + 1) / 1000.0;
        LOG("gx", "field %u: %.1f fields/s, %.1f game frames/s, %u hitches, %u draws (%u batches) in last field, %zu shaders, %zu textures",
            s_fields, 250.0 / secs, (s_frame - last_frame) / secs, s_repeats, s_draws_this_field, s_batches_this_field, s_ps_cache.size(),
            s_tex_cache.size());
        s_repeats = 0;
        last_ms = now;
        last_frame = s_frame;
        extern u64 g_prof_fifo_ticks;
        // the counters are in TSC ticks: calibrate against the elapsed wall-clock time
        static u64 last_tsc = __rdtsc();
        u64 tsc = __rdtsc();
        double ms = secs * 1000.0 / (double)(tsc - last_tsc + 1);
        last_tsc = tsc;
        LOG("prof", "last %.1fs: fifo %.0f ms (draw %.0f ms, of which vertex xform %.0f ms)", secs, g_prof_fifo_ticks * ms,
            g_prof_draw_ticks * ms, g_prof_xform_ticks * ms);
        g_prof_fifo_ticks = g_prof_draw_ticks = g_prof_xform_ticks = 0;
    }
    s_draws_this_field = 0;
    s_batches_this_field = 0;
}

// ---------------------------------------------------------------- screenshots (debugging aid)
// s_png_alpha: save_png writes the alpha channel as grey instead of colour
static void save_png(ID3D11Texture2D* tex, const char* path) {
    flush_batch();
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    bool depth = d.Format == DXGI_FORMAT_R32_TYPELESS;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(s_dev->CreateTexture2D(&d, nullptr, &st))) return;
    s_ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(s_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return; }
    static bool com;
    if (!com) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); com = true; }
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (f) {
        IWICStream* stream = nullptr;
        f->CreateStream(&stream);
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
        stream->InitializeFromFilename(wpath, GENERIC_WRITE);
        IWICBitmapEncoder* enc = nullptr;
        f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(stream, WICBitmapEncoderNoCache);
        IWICBitmapFrameEncode* fr = nullptr;
        enc->CreateNewFrame(&fr, nullptr);
        fr->Initialize(nullptr);
        fr->SetSize(d.Width, d.Height);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
        fr->SetPixelFormat(&pf);   // the encoder may substitute BGRA (it does on Windows 10)
        bool bgra = pf != GUID_WICPixelFormat32bppRGBA;
        // force opaque alpha so the PNG is viewable
        std::vector<u8> rows((size_t)d.Width * d.Height * 4);
        for (u32 y = 0; y < d.Height; y++) {
            memcpy(&rows[(size_t)y * d.Width * 4], (u8*)m.pData + (size_t)y * m.RowPitch, d.Width * 4);
            for (u32 x = 0; x < d.Width; x++) {
                u8* px = &rows[((size_t)y * d.Width + x) * 4];
                if (depth) {   // depth buffer: nearer = brighter, stretched so 0.99..1.0 is visible
                    float z;
                    memcpy(&z, px, 4);
                    float v = (1.0f - z) * 255.0f * 40.0f;
                    px[0] = px[1] = px[2] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
                } else if (s_png_alpha) {
                    px[0] = px[1] = px[2] = px[3];
                } else if (bgra) std::swap(px[0], px[2]);
                px[3] = 255;
            }
        }
        fr->WritePixels(d.Height, d.Width * 4, (UINT)rows.size(), rows.data());
        fr->Commit();
        enc->Commit();
        fr->Release(); enc->Release(); stream->Release(); f->Release();
    }
    s_ctx->Unmap(st, 0);
    st->Release();
}

u32 gx_game_frames() { return s_frame; }

static std::string s_shot_request;
void gx_request_shot(const char* path) { s_shot_request = path; }
bool gx_shot_pending() { return !s_shot_request.empty(); }

static void maybe_screenshot(ID3D11Texture2D* tex) {
    if (!s_shot_request.empty()) {
        save_png(tex, s_shot_request.c_str());
        s_shot_request.clear();
    }
    static int every = getenv("DD_SHOT_EVERY") ? atoi(getenv("DD_SHOT_EVERY")) : 0;
    static int n;
    if (every <= 0 || (s_fields % every) != 0) return;
    char path[64];
    snprintf(path, sizeof path, "shots/shot_%03d.png", n);
    CreateDirectoryA("shots", nullptr);
    save_png(tex, path);
    snprintf(path, sizeof path, "shots/efb_%03d.png", n++);
    save_png(s_efb_copy_src, path);
}

bool render_present_efb() {
    if (!s_ok) return false;
    flush_batch();
    s_bind_dirty = true;
    u32 xfb = vi_xfb_address();
    auto it = s_xfb_copies.find(xfb);
    if (it == s_xfb_copies.end() || !it->second.srv) {
        // nothing copied to this XFB yet: show black
        float black[4] = {0, 0, 0, 1};
        s_ctx->ClearRenderTargetView(s_backbuffer_rtv, black);
        s_swap->Present(0, 0);
        return true;
    }
    RECT rc;
    GetClientRect(host_window(), &rc);
    int ww = std::max(1L, rc.right), wh = std::max(1L, rc.bottom);
    // resize the swap chain with the window
    DXGI_SWAP_CHAIN_DESC sd;
    s_swap->GetDesc(&sd);
    if ((int)sd.BufferDesc.Width != ww || (int)sd.BufferDesc.Height != wh) {
        s_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        s_backbuffer_rtv->Release();
        s_swap->ResizeBuffers(0, ww, wh, DXGI_FORMAT_UNKNOWN, 0);
        create_backbuffer_view();
    }
    float black[4] = {0, 0, 0, 1};
    s_ctx->ClearRenderTargetView(s_backbuffer_rtv, black);
    // 4:3 letterbox
    int vw = ww, vh = ww * 3 / 4;
    if (vh > wh) { vh = wh; vw = wh * 4 / 3; }
    BlitCB cbv = {};
    cbv.rect[0] = 0; cbv.rect[1] = 0; cbv.rect[2] = 1; cbv.rect[3] = 1;
    ID3D11ShaderResourceView* none[8] = {};
    s_ctx->PSSetShaderResources(0, 8, none);
    s_ctx->UpdateSubresource(s_copy_cb, 0, nullptr, &cbv, 0, 0);
    s_ctx->OMSetRenderTargets(1, &s_backbuffer_rtv, nullptr);
    D3D11_VIEWPORT vp = {(float)(ww - vw) / 2, (float)(wh - vh) / 2, (float)vw, (float)vh, 0, 1};
    s_ctx->RSSetViewports(1, &vp);
    D3D11_RECT r = {0, 0, ww, wh};
    s_ctx->RSSetScissorRects(1, &r);
    s_ctx->RSSetState(get_rast(0));
    s_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    s_ctx->OMSetDepthStencilState(nullptr, 0);
    s_ctx->IASetInputLayout(nullptr);
    s_ctx->VSSetShader(s_blit_vs, nullptr, 0);
    s_ctx->VSSetConstantBuffers(0, 1, &s_copy_cb);
    s_ctx->PSSetShader(s_blit_ps, nullptr, 0);
    s_ctx->PSSetShaderResources(0, 1, &it->second.srv);
    s_ctx->PSSetSamplers(0, 1, &s_linear_clamp);
    s_ctx->Draw(3, 0);
    s_ctx->PSSetShaderResources(0, 8, none);
    maybe_screenshot(it->second.tex);
    {   // stutter diagnostics: a field that repeats the previous frame while the game renders every field
        static u32 last_shown, streak;   // streak: consecutive fields that each showed a new frame
        u32 shown = it->second.frame;
        if (shown == last_shown) {
            if (streak >= 25) {   // the game was running at full rate: this is a visible hitch
                s_repeats++;
                static int logged;
                if (logged++ < 100 || s_hitch_shader_ms + s_hitch_texture_ms > 5)
                    LOG("hitch", "field %u repeated game frame %u: %u shaders compiled in %.1f ms, %u textures decoded in %.1f ms",
                        s_fields, shown, s_hitch_shaders, s_hitch_shader_ms, s_hitch_textures, s_hitch_texture_ms);
            }
            streak = 0;
        } else {
            streak++;
        }
        last_shown = shown;
        s_hitch_shaders = s_hitch_textures = 0;
        s_hitch_shader_ms = s_hitch_texture_ms = 0;
    }
    s_swap->Present(0, 0);
    return true;
}
