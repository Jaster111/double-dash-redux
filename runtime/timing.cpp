// Emulated timebase and a small timed-event queue.
//
// The timebase follows host wall-clock time so that VI retraces, alarms and
// audio DMA happen at real-hardware rates regardless of how fast the
// recompiled code runs.
#include "common.h"
#include <windows.h>
#include <vector>
#include <algorithm>

static LARGE_INTEGER s_qpc0, s_freq;

void tb_init() {
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_qpc0);
}

u64 tb_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    u64 d = (u64)(q.QuadPart - s_qpc0.QuadPart);
    // split to avoid overflow: d * TB_HZ / freq
    u64 f = (u64)s_freq.QuadPart;
    return (d / f) * TB_HZ + (d % f) * TB_HZ / f;
}

struct Event { u64 when; EventFn fn; u64 data; };
static std::vector<Event> s_events;   // small; linear scans are fine

void ev_schedule(u64 when_tb, EventFn fn, u64 userdata) {
    s_events.push_back({when_tb, fn, userdata});
}

void ev_cancel(EventFn fn) {
    s_events.erase(std::remove_if(s_events.begin(), s_events.end(), [&](const Event& e) { return e.fn == fn; }),
                   s_events.end());
}

u64 ev_next_time() {
    u64 t = ~0ull;
    for (auto& e : s_events) t = std::min(t, e.when);
    return t;
}

void ev_debug_dump() {
    u64 now = tb_now();
    for (auto& e : s_events)
        LOG("hang", "event fn=%p in %lld us", (void*)e.fn, (long long)((s64)(e.when - now) * 1000000 / (s64)TB_HZ));
    void cpu_debug_dump();
    cpu_debug_dump();
}

void ev_run_due() {
    u64 now = tb_now();
    for (;;) {
        size_t best = (size_t)-1;
        for (size_t i = 0; i < s_events.size(); i++)
            if (s_events[i].when <= now && (best == (size_t)-1 || s_events[i].when < s_events[best].when)) best = i;
        if (best == (size_t)-1) return;
        Event e = s_events[best];
        s_events.erase(s_events.begin() + best);
        e.fn(e.data);
    }
}
