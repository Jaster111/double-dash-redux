// CPU-level runtime services for recompiled code: indirect calls, special
// purpose registers, timebase/decrementer, paired-single quantization, cache
// ops and diagnostics.
#include "common.h"
#include <windows.h>
#include <vector>
#include <unordered_map>
#include <string>

// ---------------------------------------------------------------- function lookup
static const u32 LOOKUP_BASE = 0x80000000u;
static const u32 LOOKUP_END = 0x80400000u;
static std::vector<GuestFn> s_lookup;
static std::unordered_map<u32, GuestFn> s_extra;   // functions registered at run time

static void build_lookup() {
    s_lookup.assign((LOOKUP_END - LOOKUP_BASE) / 4, nullptr);
    for (unsigned i = 0; i < g_func_count; i++) {
        u32 a = g_func_table[i].addr;
        if (a >= LOOKUP_BASE && a < LOOKUP_END) s_lookup[(a - LOOKUP_BASE) / 4] = g_func_table[i].fn;
        else s_extra[a] = g_func_table[i].fn;
    }
}

GuestFn lookup_guest_fn(u32 addr) {
    if (s_lookup.empty()) build_lookup();
    if (addr >= LOOKUP_BASE && addr < LOOKUP_END && !(addr & 3)) return s_lookup[(addr - LOOKUP_BASE) / 4];
    auto it = s_extra.find(addr);
    return it == s_extra.end() ? nullptr : it->second;
}

void call_indirect(CPU* c, u32 addr) {
    GuestFn f = lookup_guest_fn(addr);
    if (!f) rt_panic("call_indirect: no recompiled function at %08X (lr=%08X)", addr, c->lr);
    f(c);
}

// ---------------------------------------------------------------- SPRs
enum {
    SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22, SPR_SRR0 = 26,
    SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269, SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287,
    SPR_HID2 = 920, SPR_WPAR = 921, SPR_DMAU = 922, SPR_DMAL = 923, SPR_HID0 = 1008, SPR_HID1 = 1009,
    SPR_L2CR = 1017,
};
static u32 s_spr[1024];
static s64 s_tb_offset;        // guest-written timebase offset
static u64 s_dec_set_tb;       // timebase at last DEC write
static u32 s_dec_value;
static bool s_dec_pending;

static u64 guest_tb() { return tb_now() + s_tb_offset; }

static void dec_event(u64) { s_dec_pending = true; }

bool dec_pending() { return s_dec_pending; }
void cpu_debug_dump() {
    u64 el = tb_now() - s_dec_set_tb;
    LOG("hang", "dec: last write %08X, %lld us ago, now %08X, pending=%d", s_dec_value,
        (long long)(el * 1000000 / TB_HZ), s_dec_value - (u32)el, (int)s_dec_pending);
}
void dec_acknowledge() { s_dec_pending = false; }

static void locked_cache_dma(u32 dmau, u32 dmal) {
    u32 mem = dmau & 0xFFFFFFE0u;
    u32 lc = dmal & 0xFFFFFFE0u;
    u32 len = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
    if (len == 0) len = 128;
    len *= 32;
    if (dmal & 0x10)  // DMA_LD: memory -> locked cache
        memcpy(host_ptr(lc), host_ptr(mem), len);
    else
        memcpy(host_ptr(mem), host_ptr(lc), len);
}

u32 rt_mfspr(CPU* c, u32 spr) {
    switch (spr) {
    case SPR_DEC: {
        u64 el = tb_now() - s_dec_set_tb;
        return s_dec_value - (u32)el;
    }
    case SPR_SRR0: case SPR_SRR1: return s_spr[spr];
    case SPR_PVR: return 0x00083214;          // Gekko
    case SPR_L2CR: return s_spr[spr] & ~1u;   // L2IP never busy
    case SPR_WPAR: return s_spr[spr] & ~1u;   // gather buffer always empty
    case SPR_DMAL: return s_spr[spr] & ~3u;   // DMA never in progress
    case SPR_TBL_R: return (u32)guest_tb();
    case SPR_TBU_R: return (u32)(guest_tb() >> 32);
    default: return s_spr[spr & 1023];
    }
}

void rt_mtspr(CPU* c, u32 spr, u32 v) {
    switch (spr) {
    case SPR_DEC: {
        s_dec_set_tb = tb_now();
        s_dec_value = v;
        s_dec_pending = false;
        ev_cancel(dec_event);
        if ((s32)v >= 0) ev_schedule(s_dec_set_tb + (u64)v + 1, dec_event, 0);
        else s_dec_pending = true;
        return;
    }
    case SPR_TBL_W: {
        LOG("cpu", "mttbl %08X", v);
        u64 cur = guest_tb();
        s_tb_offset += (s64)(((cur & 0xFFFFFFFF00000000ull) | v) - cur);
        return;
    }
    case SPR_TBU_W: {
        LOG("cpu", "mttbu %08X", v);
        u64 cur = guest_tb();
        s_tb_offset += (s64)((((u64)v << 32) | (cur & 0xFFFFFFFFull)) - cur);
        return;
    }
    case SPR_DMAL:
        s_spr[spr] = v;
        if (v & 2) locked_cache_dma(s_spr[SPR_DMAU], v);
        return;
    default:
        s_spr[spr & 1023] = v;
    }
}

u32 rt_mftb(CPU* c, u32 spr) {
    u32 v = rt_mfspr(c, spr);
    static int n;
    if (n < 0) { n++; LOG("cpu", "mftb %u -> %08X (off %lld)", spr, v, (long long)s_tb_offset); }
    return v;
}

void rt_mtmsr(CPU* c, u32 v) {
    u32 old = c->msr;
    c->msr = v;
    if (!(old & 0x8000) && (v & 0x8000)) os_check_interrupts(c);
}

void rt_dcbz(u32 ea) { memset(host_ptr(ea & ~31u), 0, 32); }
void rt_sc(CPU* c) {}
// rfi outside the exception path is used by RealMode()-style trampolines:
// "jump to SRR0 with MSR = SRR1".  The generated code returns right after this
// call, so jumping is modelled as a tail call; an rfi whose target is the
// pending return address simply returns.
GuestFn lookup_guest_fn(u32 addr);
void rt_rfi(CPU* c) {
    u32 target = s_spr[SPR_SRR0];
    c->msr = s_spr[SPR_SRR1];
    if ((target | 0x80000000u) == (c->lr | 0x80000000u)) return;
    GuestFn f = lookup_guest_fn(target | 0x80000000u);
    if (!f) rt_panic("rfi to unknown target %08X (lr=%08X)", target, c->lr);
    f(c);
}
void rt_sp_mismatch(CPU* c, u32 ret, u32 sp_before) {
    static int n;
    if (n++ < 40)
        LOG("spcheck", "call returning to %08X: r1 %08X -> %08X (last entered fn %08X, lr=%08X)", ret, sp_before,
            c->gpr[1], c->pc, c->lr);
}
void rt_trap(CPU* c, u32 pc) { LOG("cpu", "trap at %08X (lr=%08X)", pc, c->lr); }

void rt_unimplemented(CPU* c, u32 pc, u32 insn) { rt_panic("unimplemented instruction %08X at %08X", insn, pc); }
void rt_fatal_fallthrough(CPU* c, u32 pc) { rt_panic("execution fell off the end of a function at %08X", pc); }

// ---------------------------------------------------------------- paired-single quantization
static double ldexp_fast(double v, int e) { return ldexp(v, e); }

static int gqr_scale(u32 six) { return (int)((six ^ 0x20) - 0x20); }   // 6-bit signed

void psq_load(CPU* c, u32 ea, int rd, int w, int qi) {
    u32 g = c->gqr[qi];
    u32 type = (g >> 16) & 7;
    int scale = gqr_scale((g >> 24) & 0x3F);
    double v[2];
    int n = w ? 1 : 2;
    for (int k = 0; k < n; k++) {
        switch (type) {
        case 4: v[k] = ldexp_fast((double)rd8(ea + k), -scale); break;
        case 5: v[k] = ldexp_fast((double)rd16(ea + 2 * k), -scale); break;
        case 6: v[k] = ldexp_fast((double)(s8)rd8(ea + k), -scale); break;
        case 7: v[k] = ldexp_fast((double)(s16)rd16(ea + 2 * k), -scale); break;
        default: v[k] = (double)rdf32(ea + 4 * k); break;
        }
    }
    c->fpr[rd].d = v[0];
    c->ps1[rd] = w ? 1.0 : v[1];
}

template <typename T>
static T quant(double v, int scale, double lo, double hi) {
    double s = ldexp(v, scale);
    if (!(s >= lo)) s = lo;   // also catches NaN
    if (s > hi) s = hi;
    return (T)(s64)s;
}

void psq_store(CPU* c, u32 ea, int rs, int w, int qi) {
    u32 g = c->gqr[qi];
    u32 type = g & 7;
    int scale = gqr_scale((g >> 8) & 0x3F);
    double v[2] = {c->fpr[rs].d, c->ps1[rs]};
    int n = w ? 1 : 2;
    for (int k = 0; k < n; k++) {
        switch (type) {
        case 4: wr8(ea + k, quant<u8>(v[k], scale, 0, 255)); break;
        case 5: wr16(ea + 2 * k, quant<u16>(v[k], scale, 0, 65535)); break;
        case 6: wr8(ea + k, (u8)quant<s8>(v[k], scale, -128, 127)); break;
        case 7: wr16(ea + 2 * k, (u16)quant<s16>(v[k], scale, -32768, 32767)); break;
        default: wrf32(ea + 4 * k, (float)v[k]); break;
        }
    }
}

// ---------------------------------------------------------------- logging / crash diagnostics
static FILE* s_logfile;

void rt_log(const char* cat, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!s_logfile) s_logfile = fopen("doubledash.log", "w");
    fprintf(stderr, "[%s] %s\n", cat, buf);
    if (s_logfile) { fprintf(s_logfile, "[%s] %s\n", cat, buf); fflush(s_logfile); }
}

static void dump_cpu(CPU* c) {
    if (!c) return;
    rt_log("cpu", "lr=%08X ctr=%08X msr=%08X cr=%08X", c->lr, c->ctr, c->msr, ppc_get_cr(c));
    for (int i = 0; i < 32; i += 4)
        rt_log("cpu", "r%-2d %08X %08X %08X %08X", i, c->gpr[i], c->gpr[i + 1], c->gpr[i + 2], c->gpr[i + 3]);
    // guest stack backchain: each frame holds the caller's SP at 0 and the saved LR at +4 of the caller frame
    u32 sp = c->gpr[1];
    for (int d = 0; d < 24 && sp >= 0x80000000u && sp < 0x81800000u && !(sp & 3); d++) {
        u32 next = rd32(sp);
        if (next < 0x80000000u || next >= 0x81800000u) break;
        rt_log("cpu", "  frame %2d sp=%08X lr=%08X", d, next, rd32(next + 4));
        sp = next;
    }
}

void rt_panic(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("PANIC", "%s", buf);
    dump_cpu(os_current_cpu());
    if (getenv("DD_DUMP_RAM")) {   // write main RAM for offline inspection
        if (FILE* f = fopen("crash_ram.bin", "wb")) { fwrite(g_mem, 1, 0x01800000, f); fclose(f); }
    }
    MessageBoxA(nullptr, buf, "Double Dash runtime panic", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    u64 fault = code == EXCEPTION_ACCESS_VIOLATION ? (u64)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    u64 rip = (u64)ep->ContextRecord->Rip;
    HMODULE mod = GetModuleHandleA(nullptr);
    rt_log("CRASH", "exception %08lX at host rip=%llX (image+%llX) fault=%llX", code, rip, rip - (u64)mod, fault);
    if (fault >= (u64)g_mem && fault < (u64)g_mem + 0x40000000ull)
        rt_log("CRASH", "guest physical address %08llX", fault - (u64)g_mem);
    rt_panic("host exception %08lX (see doubledash.log)", code);
}

void cpu_init() {
    build_lookup();
    AddVectoredExceptionHandler(1, crash_handler);
}
