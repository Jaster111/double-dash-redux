#pragma once
#include "../common.h"
#include <string>

// Everything that changes the generated pixel shader's code (constants live in the cbuffer).
struct ShaderKey {
    u32 num_stages, num_tex, num_ind, num_ind_used;
    u32 color_env[16], alpha_env[16], order[16], ksel[16], ind_cmd[16];
    u32 swap[4];
    u32 ind_order;
    u32 alpha_cmp;
    u32 dst_alpha;
    u32 fog;
    u32 ztex;        // BP 0xF5: type (bits 0-1) | op (bits 2-3); 0 when z-texturing is off
    bool operator==(const ShaderKey& o) const { return memcmp(this, &o, sizeof *this) == 0; }
};

struct ShaderKeyHash {
    size_t operator()(const ShaderKey& k) const {
        const u32* p = (const u32*)&k;
        u64 h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof k / 4; i++) { h ^= p[i]; h *= 1099511628211ull; }
        return (size_t)h;
    }
};

ShaderKey gx_shader_key();
std::string gx_shader_source(const ShaderKey& k);
