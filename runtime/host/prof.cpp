// Sampling profiler (debugging aid).
//
// DD_PROF=1        sample the emulation thread's instruction pointer ~2000x per second and log
//                  the hottest functions every DD_PROF_SECS seconds (default 10) as [prof] lines.
//                  Names come from the exe's PDB (link with /DEBUG); guest functions show as
//                  fn_8xxxxxxx (run the log through tools/sym.py for their game names).
// DD_PROF_TOP=N    number of functions per report (default 25); the hottest ones also list their
//                  main callers (one unwind step from the sampled instruction).
// DD_PROF_LINES=F  also break the samples of the functions whose name contains F down by source line.
#include "../common.h"
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static HANDLE s_target;
static u32 s_secs, s_top;
static const char* s_lines_fn;

// return address of the function executing at ctx (one unwind step); 0 if it cannot be determined
static u64 caller_of(CONTEXT ctx) {
    __try {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fe) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher, nullptr);
            return ctx.Rip;
        }
        return *(const DWORD64*)ctx.Rsp;   // leaf function: the return address is on top of the stack
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static std::string symbol_name(HANDLE proc, u64 addr, u64* start) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, si)) { *start = addr - disp; return si->Name; }
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod);
    char path[MAX_PATH] = "?";
    if (mod) {
        GetModuleFileNameA(mod, path, MAX_PATH);
        *start = (u64)mod;
        const char* s = strrchr(path, '\\');
        return std::string(s ? s + 1 : path) + "!?";
    }
    *start = 0;
    return "?";
}

static DWORD WINAPI prof_thread(void*) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::unordered_map<u64, u32> hist;
    std::unordered_map<u64, std::unordered_map<u64, u32>> callers;   // rip -> caller address -> samples
    u32 total = 0;
    u64 t0 = GetTickCount64();
    for (;;) {
        Sleep(0);
        // ~2 kHz: busy-wait on QPC between samples (Sleep(1) would cap us at 1 kHz)
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        do { YieldProcessor(); QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 2000 < f.QuadPart);
        if (SuspendThread(s_target) == (DWORD)-1) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        bool ok = GetThreadContext(s_target, &ctx) != 0;
        u64 caller = ok ? caller_of(ctx) : 0;   // while the thread is still suspended
        ResumeThread(s_target);
        if (ok) { hist[ctx.Rip]++; callers[ctx.Rip][caller]++; total++; }
        if (GetTickCount64() - t0 < s_secs * 1000ull) continue;
        // aggregate by function (and by calling function)
        struct Fn { std::string name; u32 n = 0; std::unordered_map<u64, u32> callers; };
        std::unordered_map<u64, Fn> by_fn;
        std::unordered_map<u64, std::pair<u64, std::string>> sym;   // address -> (function start, name)
        auto lookup = [&](u64 a) -> std::pair<u64, std::string>& {
            auto it = sym.find(a);
            if (it != sym.end()) return it->second;
            u64 start;
            std::string name = symbol_name(proc, a, &start);
            return sym[a] = {start, name};
        };
        for (auto& [rip, n] : hist) {
            auto& f = lookup(rip);
            Fn& e = by_fn[f.first];
            if (e.name.empty()) e.name = f.second;
            e.n += n;
            for (auto& [ca, cn] : callers[rip]) e.callers[ca ? lookup(ca).first : 0] += cn;
        }
        std::vector<Fn*> v;
        for (auto& [k, e] : by_fn) v.push_back(&e);
        std::sort(v.begin(), v.end(), [](Fn* x, Fn* y) { return x->n > y->n; });
        LOG("prof", "---- %u samples in %.1f s", total, (GetTickCount64() - t0) / 1000.0);
        for (size_t i = 0; i < v.size() && i < s_top; i++) {
            LOG("prof", "%5.1f%%  %s", 100.0 * v[i]->n / total, v[i]->name.c_str());
            if (i >= 12) continue;
            std::vector<std::pair<u64, u32>> cs(v[i]->callers.begin(), v[i]->callers.end());
            std::sort(cs.begin(), cs.end(), [](auto& x, auto& y) { return x.second > y.second; });
            for (size_t j = 0; j < cs.size() && j < 3; j++) {
                if (cs[j].second * 10 < v[i]->n) break;   // only callers with >= 10% of the function's samples
                std::string cname = "?";
                for (auto& [a, sn] : sym) if (sn.first == cs[j].first) { cname = sn.second; break; }
                LOG("prof", "           <- %3.0f%% %s", 100.0 * cs[j].second / v[i]->n, cname.c_str());
            }
        }
        if (s_lines_fn) {   // per source line of the selected functions
            std::unordered_map<std::string, u32> lines;
            u32 fn_total = 0;
            for (auto& [rip, n] : hist) {
                if (lookup(rip).second.find(s_lines_fn) == std::string::npos) continue;
                IMAGEHLP_LINE64 li = {sizeof li};
                DWORD disp = 0;
                std::string key = "?";
                if (SymGetLineFromAddr64(proc, rip, &disp, &li)) {
                    const char* f = strrchr(li.FileName, '\\');
                    key = std::string(f ? f + 1 : li.FileName) + ":" + std::to_string(li.LineNumber);
                }
                lines[key] += n;
                fn_total += n;
            }
            std::vector<std::pair<std::string, u32>> lv(lines.begin(), lines.end());
            std::sort(lv.begin(), lv.end(), [](auto& x, auto& y) { return x.second > y.second; });
            LOG("prof", "lines of *%s* (%u samples):", s_lines_fn, fn_total);
            for (size_t i = 0; i < lv.size() && i < 30; i++)
                LOG("prof", "  %5.1f%%  %s", 100.0 * lv[i].second / (fn_total ? fn_total : 1), lv[i].first.c_str());
        }
        hist.clear();
        callers.clear();
        total = 0;
        t0 = GetTickCount64();
    }
}

void prof_init() {
    if (!getenv("DD_PROF")) return;
    s_secs = getenv("DD_PROF_SECS") ? (u32)atoi(getenv("DD_PROF_SECS")) : 10;
    s_top = getenv("DD_PROF_TOP") ? (u32)atoi(getenv("DD_PROF_TOP")) : 25;
    if (!s_secs) s_secs = 10;
    s_lines_fn = getenv("DD_PROF_LINES");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CreateThread(nullptr, 0, prof_thread, nullptr, 0, nullptr);
    LOG("prof", "sampling profiler on, report every %u s", s_secs);
}
