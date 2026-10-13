// Runtime interface for statically recompiled Gekko (PowerPC 750CL) code.
// Included by every generated file; keep it small and inline-friendly.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define FORCEINLINE __forceinline
#define UNLIKELY(x) (x)
#else
#define FORCEINLINE inline __attribute__((always_inline))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

struct HostThread;

struct CPU {
    u32 gpr[32];
    union FPR { double d; u64 u; } fpr[32];   // ps0 / scalar FPR
    double ps1[32];                           // paired-single slot 1
    u8 cr[8];                                 // per field: LT=8 GT=4 EQ=2 SO=1
    u8 so, ov, ca, xer_bc;
    u32 lr, ctr;
    u32 msr, fpscr;
    u32 gqr[8];
    u32 sr[16];
    u32 reserve;
    s32 budget;                               // back-edge countdown until rt_poll
    u32 pc;                                   // entry of the most recently entered guest function
    HostThread* thread;                       // owning host fiber
};

typedef void (*GuestFn)(CPU*);
struct FuncEntry { u32 addr; GuestFn fn; };
extern const FuncEntry g_func_table[];
extern const unsigned g_func_count;

// Guest memory: physical address = virtual & 0x3FFFFFFF (covers the cached
// 0x8xxxxxxx, uncached 0xCxxxxxxx and locked-cache 0xExxxxxxx mirrors).
extern u8* g_mem;

u32 mmio_read32(u32 a);
u16 mmio_read16(u32 a);
u8 mmio_read8(u32 a);
void mmio_write32(u32 a, u32 v);
void mmio_write16(u32 a, u16 v);
void mmio_write8(u32 a, u8 v);

#if defined(_MSC_VER)
#define BSWAP16(x) _byteswap_ushort(x)
#define BSWAP32(x) _byteswap_ulong(x)
#define BSWAP64(x) _byteswap_uint64(x)
#define ROTL(x, n) _rotl((x), (n))
#else
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
static FORCEINLINE u32 ROTL(u32 x, u32 n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
#endif

#define PHYS(a) ((a) & 0x3FFFFFFFu)
// 0x0C000000..0x0DFFFFFF physical = hardware registers + write-gather pipe
#define IS_MMIO(a) ((((a) >> 25) & 0x1Fu) == 6u)

static FORCEINLINE u8* host_ptr(u32 a) { return g_mem + PHYS(a); }

static FORCEINLINE u8 rd8(u32 a) { if (UNLIKELY(IS_MMIO(a))) return mmio_read8(a); return g_mem[PHYS(a)]; }
static FORCEINLINE u16 rd16(u32 a) { if (UNLIKELY(IS_MMIO(a))) return mmio_read16(a); u16 v; memcpy(&v, g_mem + PHYS(a), 2); return BSWAP16(v); }
static FORCEINLINE u32 rd32(u32 a) { if (UNLIKELY(IS_MMIO(a))) return mmio_read32(a); u32 v; memcpy(&v, g_mem + PHYS(a), 4); return BSWAP32(v); }
static FORCEINLINE u64 rd64(u32 a) { if (UNLIKELY(IS_MMIO(a))) return ((u64)mmio_read32(a) << 32) | mmio_read32(a + 4); u64 v; memcpy(&v, g_mem + PHYS(a), 8); return BSWAP64(v); }
static FORCEINLINE void wr8(u32 a, u8 v) { if (UNLIKELY(IS_MMIO(a))) { mmio_write8(a, v); return; } g_mem[PHYS(a)] = v; }
static FORCEINLINE void wr16(u32 a, u16 v) { if (UNLIKELY(IS_MMIO(a))) { mmio_write16(a, v); return; } v = BSWAP16(v); memcpy(g_mem + PHYS(a), &v, 2); }
static FORCEINLINE void wr32(u32 a, u32 v) { if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, v); return; } v = BSWAP32(v); memcpy(g_mem + PHYS(a), &v, 4); }
static FORCEINLINE void wr64(u32 a, u64 v) { if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, (u32)(v >> 32)); mmio_write32(a + 4, (u32)v); return; } v = BSWAP64(v); memcpy(g_mem + PHYS(a), &v, 8); }

// fast variants: base register is r1/r2/r13, which never points at MMIO
static FORCEINLINE u8 rd8f(u32 a) { return g_mem[PHYS(a)]; }
static FORCEINLINE u16 rd16f(u32 a) { u16 v; memcpy(&v, g_mem + PHYS(a), 2); return BSWAP16(v); }
static FORCEINLINE u32 rd32f(u32 a) { u32 v; memcpy(&v, g_mem + PHYS(a), 4); return BSWAP32(v); }
static FORCEINLINE void wr8f(u32 a, u8 v) { g_mem[PHYS(a)] = v; }
static FORCEINLINE void wr16f(u32 a, u16 v) { v = BSWAP16(v); memcpy(g_mem + PHYS(a), &v, 2); }
static FORCEINLINE void wr32f(u32 a, u32 v) { v = BSWAP32(v); memcpy(g_mem + PHYS(a), &v, 4); }

static FORCEINLINE float rdf32(u32 a) { u32 v = rd32(a); float f; memcpy(&f, &v, 4); return f; }
static FORCEINLINE void wrf32(u32 a, float f) { u32 v; memcpy(&v, &f, 4); wr32(a, v); }

// ---------------------------------------------------------------- register macros used by generated code
#define R(n) (c->gpr[n])
#define F(n) (c->fpr[n].d)
#define FU(n) (c->fpr[n].u)
#define P1(n) (c->ps1[n])

#define RD8(a) rd8(a)
#define RD16(a) rd16(a)
#define RD16S(a) ((u32)(s32)(s16)rd16(a))
#define RD32(a) rd32(a)
#define RD64(a) rd64(a)
#define RD16BR(a) ((u32)BSWAP16(rd16(a)))
#define RD32BR(a) BSWAP32(rd32(a))
#define RD8F(a) rd8f(a)
#define RD16F(a) rd16f(a)
#define RD16SF(a) ((u32)(s32)(s16)rd16f(a))
#define RD32F(a) rd32f(a)
#define WR8(a, v) wr8((a), (u8)(v))
#define WR16(a, v) wr16((a), (u16)(v))
#define WR32(a, v) wr32((a), (v))
#define WR64(a, v) wr64((a), (v))
#define WR16BR(a, v) wr16((a), BSWAP16((u16)(v)))
#define WR32BR(a, v) wr32((a), BSWAP32(v))
#define WR8F(a, v) wr8f((a), (u8)(v))
#define WR16F(a, v) wr16f((a), (u16)(v))
#define WR32F(a, v) wr32f((a), (v))
#define RDF32(a) rdf32(a)
#define WRF32(a, f) wrf32((a), (f))

#define SET_CR0(v) (c->cr[0] = (u8)(((s32)(v) < 0 ? 8 : (s32)(v) > 0 ? 4 : 2) | c->so))
#define CMPS(f, a, b) (c->cr[f] = (u8)(((s32)(a) < (s32)(b) ? 8 : (s32)(a) > (s32)(b) ? 4 : 2) | c->so))
#define CMPU(f, a, b) (c->cr[f] = (u8)(((u32)(a) < (u32)(b) ? 8 : (u32)(a) > (u32)(b) ? 4 : 2) | c->so))
#define CRBIT(bi) ((c->cr[(bi) >> 2] >> (3 - ((bi) & 3))) & 1)
#define SETCRBIT(bi, v) (c->cr[(bi) >> 2] = (u8)((c->cr[(bi) >> 2] & ~(1u << (3 - ((bi) & 3)))) | (((v) & 1) << (3 - ((bi) & 3)))))

static FORCEINLINE u8 fcmp_bits(double a, double b) {
    if (a != a || b != b) return 1;
    return a < b ? 8 : a > b ? 4 : 2;
}
#define FCMP(f, a, b) (c->cr[f] = fcmp_bits((a), (b)))

void rt_poll(CPU* c);
#define POLL() do { if (--c->budget < 0) rt_poll(c); } while (0)

// Calls.  With DD_SPCHECK every call verifies the callee restored r1 (debugging aid
// for recompiler / context-switch bugs: an unbalanced return corrupts the caller).
void rt_sp_mismatch(CPU* c, u32 ret, u32 sp_before);
#ifdef DD_SPCHECK
#define CALL(f, ret) do { u32 sp_ = c->gpr[1]; c->lr = (ret); f(c); if (c->gpr[1] != sp_) rt_sp_mismatch(c, (ret), sp_); } while (0)
#define CALL_CTR(ret) do { u32 sp_ = c->gpr[1]; c->lr = (ret); call_indirect(c, c->ctr); if (c->gpr[1] != sp_) rt_sp_mismatch(c, (ret), sp_); } while (0)
#else
#define CALL(f, ret) do { c->lr = (ret); f(c); } while (0)
#define CALL_CTR(ret) do { c->lr = (ret); call_indirect(c, c->ctr); } while (0)
#endif

// ---------------------------------------------------------------- integer helpers
static FORCEINLINE u32 ppc_divw(u32 a, u32 b) {
    s32 sa = (s32)a, sb = (s32)b;
    if (sb == 0 || (sa == (s32)0x80000000 && sb == -1)) return sa < 0 ? 0xFFFFFFFFu : 0;
    return (u32)(sa / sb);
}
static FORCEINLINE u32 ppc_divwu(u32 a, u32 b) { return b ? a / b : 0; }
static FORCEINLINE u32 ppc_cntlzw(u32 v) {
#if defined(_MSC_VER)
    unsigned long i;
    return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
    return v ? __builtin_clz(v) : 32;
#endif
}
static FORCEINLINE u32 ppc_sraw(CPU* c, u32 s, u32 b) {
    s32 v = (s32)s;
    if (b & 0x20) { c->ca = v < 0; return (u32)(v >> 31); }
    u32 n = b & 31;
    if (n == 0) { c->ca = 0; return s; }
    c->ca = (v < 0) && (s & ((1u << n) - 1));
    return (u32)(v >> n);
}

static FORCEINLINE u32 ppc_get_cr(CPU* c) {
    u32 v = 0;
    for (int i = 0; i < 8; i++) v |= (u32)(c->cr[i] & 0xF) << (28 - 4 * i);
    return v;
}
static FORCEINLINE void ppc_set_cr(CPU* c, u32 v, u32 crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80 >> i)) c->cr[i] = (u8)((v >> (28 - 4 * i)) & 0xF);
}
static FORCEINLINE u32 ppc_get_xer(CPU* c) {
    return ((u32)c->so << 31) | ((u32)c->ov << 30) | ((u32)c->ca << 29) | c->xer_bc;
}
static FORCEINLINE void ppc_set_xer(CPU* c, u32 v) {
    c->so = (v >> 31) & 1; c->ov = (v >> 30) & 1; c->ca = (v >> 29) & 1; c->xer_bc = (u8)(v & 0x7F);
}

// ---------------------------------------------------------------- floating point helpers
// Gekko rounds the C operand of single-precision multiplies to 25 bits.
static FORCEINLINE double RC25(double d) {
    u64 i; memcpy(&i, &d, 8);
    i = (i & 0xFFFFFFFFF8000000ull) + (i & 0x8000000ull);
    memcpy(&d, &i, 8);
    return d;
}
static FORCEINLINE double ppc_frsqrte(double b) { return 1.0 / sqrt(b); }
static FORCEINLINE u32 ppc_fctiwz(double b) {
    if (b != b) return 0x80000000u;
    if (b >= 2147483648.0) return 0x7FFFFFFFu;
    if (b < -2147483648.0) return 0x80000000u;
    return (u32)(s32)b;
}
static FORCEINLINE u32 ppc_fctiw(double b) {
    if (b != b) return 0x80000000u;
    double r = nearbyint(b);
    if (r >= 2147483648.0) return 0x7FFFFFFFu;
    if (r < -2147483648.0) return 0x80000000u;
    return (u32)(s32)r;
}
static FORCEINLINE void ppc_mtfsf(CPU* c, u32 fm, u32 v) {
    u32 m = 0;
    for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
    c->fpscr = (c->fpscr & ~m) | (v & m);
}

void psq_load(CPU* c, u32 ea, int rd, int w, int qi);
void psq_store(CPU* c, u32 ea, int rs, int w, int qi);

// ---------------------------------------------------------------- runtime services
void call_indirect(CPU* c, u32 addr);
u32 rt_mfspr(CPU* c, u32 spr);
void rt_mtspr(CPU* c, u32 spr, u32 v);
u32 rt_mftb(CPU* c, u32 spr);
void rt_mtmsr(CPU* c, u32 v);
void rt_dcbz(u32 ea);
void rt_sc(CPU* c);
void rt_rfi(CPU* c);
void rt_trap(CPU* c, u32 pc);
[[noreturn]] void rt_unimplemented(CPU* c, u32 pc, u32 insn);
[[noreturn]] void rt_fatal_fallthrough(CPU* c, u32 pc);
