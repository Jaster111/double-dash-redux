// Dolphin OS threading on host fibers, plus exception delivery.
//
// The guest's own scheduler (OSThread.c: SelectThread, run queues, sleeping,
// mutexes, message queues) runs unmodified.  Only the two primitives that move
// execution between contexts are replaced:
//
//   OSSaveContext(ctx)   setjmp-like; we run the original register save and
//                        record that `ctx` belongs to the calling fiber.
//   OSLoadContext(ctx)   resume whatever `ctx` describes:
//                        - a context saved by another fiber  -> switch to it
//                        - a fresh context (OSInitContext)   -> new fiber
//                        - a context saved by an exception   -> unwind to the
//                          point where that exception was delivered (longjmp)
//
// Every guest thread therefore owns one fiber and one CPU register set, and the
// registers the guest scheduler stores in OSContext are only informational.
//
// Exceptions (decrementer, external interrupts) are delivered at safe points
// (loop back-edges via rt_poll, and when MSR[EE] becomes set).  Delivery
// emulates OSExceptionVector: save the registers into OSCurrentContext, mark it
// OS_CONTEXT_STATE_EXC, and call the handler from the exception table at
// 0x80003000 with r3 = exception number, r4 = context.  Handlers end with
// OSLoadContext(context), which lands back here.
#include "common.h"
#include "funcs.h"
#include <windows.h>
#include <setjmp.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// OSContext layout
enum : u32 {
    CTX_GPR = 0x000, CTX_CR = 0x080, CTX_LR = 0x084, CTX_CTR = 0x088, CTX_XER = 0x08C, CTX_FPR = 0x090,
    CTX_FPSCR = 0x194, CTX_SRR0 = 0x198, CTX_SRR1 = 0x19C, CTX_MODE = 0x1A0, CTX_STATE = 0x1A2,
    CTX_GQR = 0x1A4, CTX_PSF = 0x1C8,
};
enum : u16 { OS_CONTEXT_STATE_FPSAVED = 1, OS_CONTEXT_STATE_EXC = 2 };

static const u32 OS_CURRENT_CONTEXT_PHYS = 0x800000C0;
static const u32 OS_CURRENT_CONTEXT = 0x800000D4;
static const u32 OS_CURRENT_THREAD = 0x800000E4;
static const u32 OS_EXCEPTION_TABLE = 0x80003000;
static const s32 POLL_BUDGET = 2000;

struct Delivery {
    jmp_buf jb;
    CPU saved;
    u32 ctx;
};

struct HostThread {
    void* fiber = nullptr;
    CPU cpu{};
    std::vector<Delivery*> deliveries;
    int resume_kind = 0;
    u32 entry = 0;
    u32 exit_lr = 0;
    u32 ctx = 0;
};

enum { KIND_SAVED = 1, KIND_DELIVERY = 2, KIND_FRESH = 3 };
struct Owner { HostThread* t; int kind; };

static std::unordered_map<u32, Owner> s_ctx;
static std::vector<HostThread*> s_all;
static u32 s_exc_count[16];
static HostThread* s_cur;
static std::vector<HostThread*> s_dead;

CPU* os_current_cpu() { return s_cur ? &s_cur->cpu : nullptr; }

// ---------------------------------------------------------------- fibers
static void switch_to(HostThread* t, int kind) {
    HostThread* me = s_cur;
    t->resume_kind = kind;
    s_cur = t;
    SwitchToFiber(t->fiber);
    // back on `me`
    s_cur = me;
    for (HostThread* d : s_dead)
        if (d != me) { DeleteFiber(d->fiber); delete d; }
    s_dead.erase(std::remove_if(s_dead.begin(), s_dead.end(), [&](HostThread* d) { return d != me; }), s_dead.end());
}

[[noreturn]] static void resume_delivery(HostThread* me) {
    if (me->deliveries.empty()) rt_panic("resume_delivery with no pending delivery");
    longjmp(me->deliveries.back()->jb, 1);
}

// called on a fiber right after it has been switched back to
static void after_switch(HostThread* me) {
    if (me->resume_kind == KIND_DELIVERY) resume_delivery(me);
}

static void WINAPI fiber_main(void* p) {
    HostThread* t = (HostThread*)p;
    s_cur = t;
    CPU* c = &t->cpu;
    call_indirect(c, t->entry);
    // the thread function returned into its link register (OSExitThread)
    c->lr = 0;
    call_indirect(c, t->exit_lr);
    rt_panic("guest thread exit routine %08X returned", t->exit_lr);
}

static HostThread* new_thread_from_ctx(u32 ctx) {
    HostThread* t = new HostThread;
    CPU* c = &t->cpu;
    for (int i = 0; i < 32; i++) {
        c->gpr[i] = rd32(ctx + CTX_GPR + 4 * i);
        c->fpr[i].u = rd64(ctx + CTX_FPR + 8 * i);
        u64 ps1 = rd64(ctx + CTX_PSF + 8 * i);
        memcpy(&c->ps1[i], &ps1, 8);
    }
    ppc_set_cr(c, rd32(ctx + CTX_CR), 0xFF);
    c->lr = rd32(ctx + CTX_LR);
    c->ctr = rd32(ctx + CTX_CTR);
    ppc_set_xer(c, rd32(ctx + CTX_XER));
    c->fpscr = rd32(ctx + CTX_FPSCR);
    c->msr = rd32(ctx + CTX_SRR1);
    for (int i = 0; i < 8; i++) c->gqr[i] = rd32(ctx + CTX_GQR + 4 * i);
    c->budget = POLL_BUDGET;
    c->thread = t;
    t->entry = rd32(ctx + CTX_SRR0);
    t->exit_lr = c->lr;
    t->ctx = ctx;
    t->fiber = CreateFiberEx(256 * 1024, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, fiber_main, t);
    s_all.push_back(t);
    if (!t->fiber) rt_panic("CreateFiberEx failed");
    LOG("os", "new thread: ctx=%08X entry=%08X sp=%08X arg=%08X", ctx, t->entry, c->gpr[1], c->gpr[3]);
    return t;
}

CPU* os_create_boot_thread() {
    HostThread* t = new HostThread;
    t->fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    t->cpu.thread = t;
    t->cpu.budget = POLL_BUDGET;
    s_cur = t;
    s_all.push_back(t);
    return &t->cpu;
}

// ---------------------------------------------------------------- context primitives
void HLE_OSSaveContext(CPU* c) {
    u32 ctx = c->gpr[3];
    orig_800E795C(c);                       // original register save, returns r3 = 0
    s_ctx[ctx] = {c->thread, KIND_SAVED};
}

void HLE_OSLoadContext(CPU* c) {
    u32 ctx = c->gpr[3];
    HostThread* me = c->thread;
    auto it = s_ctx.find(ctx);
    if (it == s_ctx.end() || it->second.kind == KIND_FRESH) {
        HostThread* t = (it == s_ctx.end() || !it->second.t) ? new_thread_from_ctx(ctx) : it->second.t;
        s_ctx[ctx] = {t, KIND_SAVED};
        switch_to(t, KIND_FRESH);
        after_switch(me);
        return;
    }
    Owner o = it->second;
    if (o.t == me) {
        if (o.kind == KIND_DELIVERY) resume_delivery(me);
        return;   // resuming our own OSSaveContext from inside SelectThread: just continue
    }
    switch_to(o.t, o.kind);
    after_switch(me);
}

// OSInitContext prepares a brand-new thread context: forget any fiber that
// previously owned this context (a recycled OSThread).
void HLE_OSInitContext(CPU* c) {
    u32 ctx = c->gpr[3];
    auto it = s_ctx.find(ctx);
    if (it != s_ctx.end()) {
        HostThread* old = it->second.t;
        if (old && old != c->thread && it->second.kind != KIND_FRESH) s_dead.push_back(old);
        s_ctx.erase(it);
    }
    orig_800E7AE0(c);
    s_ctx[ctx] = {nullptr, KIND_FRESH};
}

// ---------------------------------------------------------------- exception delivery
static void save_to_context(CPU* c, u32 ctx) {
    for (int i = 0; i < 32; i++) wr32(ctx + CTX_GPR + 4 * i, c->gpr[i]);
    wr32(ctx + CTX_CR, ppc_get_cr(c));
    wr32(ctx + CTX_LR, c->lr);
    wr32(ctx + CTX_CTR, c->ctr);
    wr32(ctx + CTX_XER, ppc_get_xer(c));
    wr32(ctx + CTX_SRR0, 0);
    wr32(ctx + CTX_SRR1, c->msr);
    for (int i = 0; i < 8; i++) wr32(ctx + CTX_GQR + 4 * i, c->gqr[i]);
    wr16(ctx + CTX_STATE, rd16(ctx + CTX_STATE) | OS_CONTEXT_STATE_EXC);
}

static int s_delivery_depth;

void os_deliver_exception(CPU* c, u32 exc) {
    HostThread* me = c->thread;
    u32 ctx = rd32(OS_CURRENT_CONTEXT);
    u32 handler = rd32(OS_EXCEPTION_TABLE + 4 * exc);
    if (!ctx || !handler) return;
    save_to_context(c, ctx);
    s_exc_count[exc & 15]++;
    Delivery* d = new Delivery;
    d->saved = *c;
    d->ctx = ctx;
    me->deliveries.push_back(d);
    s_ctx[ctx] = {me, KIND_DELIVERY};
    s_delivery_depth++;
    if (setjmp(d->jb) == 0) {
        c->msr = (c->msr & ~0x8000u) | 0x30;   // EE off, IR/DR on (as after the vector)
        c->gpr[3] = exc;
        c->gpr[4] = ctx;
        call_indirect(c, handler);
        // a handler that returns normally also resumes the interrupted code
    }
    s_delivery_depth--;
    Delivery* top = me->deliveries.back();
    me->deliveries.pop_back();
    s32 budget = c->budget;
    *c = top->saved;
    c->budget = budget;
    wr16(top->ctx + CTX_STATE, rd16(top->ctx + CTX_STATE) & ~OS_CONTEXT_STATE_EXC);
    delete top;
}

void os_check_interrupts(CPU* c) {
    for (int guard = 0; guard < 16; guard++) {
        if (!(c->msr & 0x8000)) return;
        if (dec_pending()) {
            dec_acknowledge();
            os_deliver_exception(c, 8);
            continue;
        }
        if (pi_irq_pending()) {
            os_deliver_exception(c, 4);
            continue;
        }
        return;
    }
}

// debugging: dump every guest thread (called from the watchdog thread; racy but informative)
void os_debug_dump() {
    LOG("dbg", "exceptions: ext=%u dec=%u  pi cause/mask pending=%d", s_exc_count[4], s_exc_count[8], (int)pi_irq_pending());
    for (HostThread* t : s_all) {
        u32 th = t->ctx;   // OSThread starts with its context
        if (th)
            LOG("dbg", "thread %08X%s pc=%08X lr=%08X state=%u prio=%d queue=%08X", th, t == s_cur ? "*" : " ",
                t->cpu.pc, t->cpu.lr, rd16(th + 0x2C8), (s32)rd32(th + 0x2D0), rd32(th + 0x2DC));
        else
            LOG("dbg", "boot thread%s pc=%08X lr=%08X", t == s_cur ? "*" : " ", t->cpu.pc, t->cpu.lr);
    }
}

// full report for the hang detector: every thread with its registers and guest call stack
// (pc is the entry of the innermost recompiled function; run tools/sym.py on the log for names)
static const char* thread_state_name(u16 s) {
    switch (s) { case 0: return "ready?"; case 1: return "ready"; case 2: return "running"; case 4: return "waiting"; case 8: return "moribund"; }
    return "?";
}

void os_debug_dump_full() {
    LOG("hang", "exceptions: ext=%u dec=%u, pi pending=%d, current thread=%08X", s_exc_count[4], s_exc_count[8],
        (int)pi_irq_pending(), rd32(OS_CURRENT_THREAD));
    for (HostThread* t : s_all) {
        CPU* c = &t->cpu;
        u32 th = t->ctx;
        if (th)
            LOG("hang", "thread %08X%s %s prio=%d queue=%08X  pc=%08X lr=%08X r1=%08X r3=%08X r4=%08X msr=%08X", th,
                t == s_cur ? "*" : " ", thread_state_name(rd16(th + 0x2C8)), (s32)rd32(th + 0x2D0), rd32(th + 0x2DC), c->pc,
                c->lr, c->gpr[1], c->gpr[3], c->gpr[4], c->msr);
        else
            LOG("hang", "boot thread%s  pc=%08X lr=%08X r1=%08X r3=%08X r4=%08X msr=%08X", t == s_cur ? "*" : " ", c->pc,
                c->lr, c->gpr[1], c->gpr[3], c->gpr[4], c->msr);
        std::string bt;
        char b[16];
        u32 sp = c->gpr[1];
        for (int d = 0; d < 20 && sp >= 0x80000000u && sp < 0x81800000u && !(sp & 3); d++) {
            u32 next = rd32(sp);
            if (next < 0x80000000u || next >= 0x81800000u) break;
            snprintf(b, sizeof b, " %08X", rd32(next + 4));
            bt += b;
            sp = next;
        }
        LOG("hang", "    callers:%s", bt.c_str());
    }
}

// ---------------------------------------------------------------- polling
static u64 s_last_pump;

void rt_poll(CPU* c) {
    c->budget = POLL_BUDGET;
    ev_run_due();
    u64 now = tb_now();
    if (now - s_last_pump > TB_HZ / 200) {
        s_last_pump = now;
        host_pump_messages();
        if (host_should_quit()) { host_shutdown(); ExitProcess(0); }
    }
    os_check_interrupts(c);
    // idle: no current thread means the scheduler is spinning in its idle loop
    if (rd32(OS_CURRENT_THREAD) == 0 && (c->msr & 0x8000)) {
        u64 next = ev_next_time();
        now = tb_now();
        if (next != ~0ull && next > now) {
            u64 us = (next - now) * 1000000 / TB_HZ;
            if (us > 1500) Sleep((DWORD)(us / 1000 - 1));
        }
    }
}

// ---------------------------------------------------------------- OSReport / OSPanic
struct VaReader {
    CPU* c;
    int gpr = 4, fpr = 1;
    u32 stack;
    explicit VaReader(CPU* cpu, int first_gpr) : c(cpu), gpr(first_gpr), stack(cpu->gpr[1] + 8) {}
    u32 next32() {
        if (gpr <= 10) return c->gpr[gpr++];
        u32 v = rd32(stack); stack += 4; return v;
    }
    u64 next64() {
        if (gpr & 1) {} else gpr++;   // pairs start at an odd register (r3:r4 ... r9:r10)
        if (gpr <= 9) { u64 v = ((u64)c->gpr[gpr] << 32) | c->gpr[gpr + 1]; gpr += 2; return v; }
        gpr = 11;
        stack = (stack + 7) & ~7u;
        u64 v = rd64(stack); stack += 8; return v;
    }
    double nextd() {
        if (fpr <= 8) return c->fpr[fpr++].d;
        stack = (stack + 7) & ~7u;
        u64 v = rd64(stack); stack += 8; double d; memcpy(&d, &v, 8); return d;
    }
};

static std::string guest_string(u32 a, size_t max = 4096) {
    std::string s;
    if (!a) return "(null)";
    for (size_t i = 0; i < max; i++) {
        char ch = (char)rd8(a + (u32)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

std::string guest_format(u32 fmt_addr, VaReader& va) {
    std::string fmt = guest_string(fmt_addr), out;
    for (size_t i = 0; i < fmt.size(); i++) {
        char ch = fmt[i];
        if (ch != '%') { out.push_back(ch); continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && strchr("-+ #0123456789.*", fmt[j])) {
            if (fmt[j] == '*') spec += std::to_string((s32)va.next32());
            else spec.push_back(fmt[j]);
            j++;
        }
        int lng = 0;
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) { if (fmt[j] == 'l' || fmt[j] == 'q' || fmt[j] == 'L') lng++; j++; }
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (lng >= 2) { spec += "ll"; spec.push_back(conv); snprintf(buf, sizeof buf, spec.c_str(), (long long)va.next64()); }
            else { spec.push_back(conv); snprintf(buf, sizeof buf, spec.c_str(), va.next32()); }
            out += buf; break;
        case 'p': snprintf(buf, sizeof buf, "%08X", va.next32()); out += buf; break;
        case 's': { spec.push_back('s'); std::string s = guest_string(va.next32()); snprintf(buf, sizeof buf, spec.c_str(), s.c_str()); out += buf; break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            spec.push_back(conv); snprintf(buf, sizeof buf, spec.c_str(), va.nextd()); out += buf; break;
        case '%': out.push_back('%'); break;
        default: out += spec; out.push_back(conv); break;
        }
        i = j;
    }
    return out;
}

void HLE_OSReport(CPU* c) {
    VaReader va(c, 4);
    std::string s = guest_format(c->gpr[3], va);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG("OSReport", "%s", s.c_str());
}

void HLE_OSPanic(CPU* c) {
    std::string file = guest_string(c->gpr[3]);
    VaReader va(c, 6);
    std::string msg = guest_format(c->gpr[5], va);
    rt_panic("OSPanic %s:%u: %s", file.c_str(), c->gpr[4], msg.c_str());
}
