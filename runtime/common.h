// Shared runtime declarations (not included by generated code).
#pragma once
#include "ppc_rt.h"
#include <stdio.h>
#include <stdarg.h>

void rt_log(const char* cat, const char* fmt, ...);
[[noreturn]] void rt_panic(const char* fmt, ...);

#define LOG(cat, ...) rt_log(cat, __VA_ARGS__)

// ---------------------------------------------------------------- timing (timing.cpp)
// Emulated time is the Gekko timebase: bus clock / 4 = 40.5 MHz.
constexpr u64 TB_HZ = 40500000;
u64 tb_now();                       // current timebase
void tb_init();
typedef void (*EventFn)(u64 userdata);
void ev_schedule(u64 when_tb, EventFn fn, u64 userdata);   // fire at/after when_tb
void ev_cancel(EventFn fn);
u64 ev_next_time();                 // earliest pending event (or ~0)
void ev_run_due();                  // fire all events whose time has come

// ---------------------------------------------------------------- interrupts (hw/pi.cpp)
enum PiIrq : u32 {
    PI_IRQ_PI = 0x1, PI_IRQ_RSW = 0x2, PI_IRQ_DI = 0x4, PI_IRQ_SI = 0x8, PI_IRQ_EXI = 0x10,
    PI_IRQ_AI = 0x20, PI_IRQ_DSP = 0x40, PI_IRQ_MEM = 0x80, PI_IRQ_VI = 0x100,
    PI_IRQ_PE_TOKEN = 0x200, PI_IRQ_PE_FINISH = 0x400, PI_IRQ_CP = 0x800, PI_IRQ_DEBUG = 0x1000,
    PI_IRQ_HSP = 0x2000,
};
void pi_set_irq(u32 bit, bool on);
bool pi_irq_pending();              // cause & mask

// ---------------------------------------------------------------- cpu exceptions (cpu.cpp / os_hle.cpp)
bool dec_pending();
void dec_acknowledge();
void os_deliver_exception(CPU* c, u32 exc);
void os_check_interrupts(CPU* c);   // deliver pending interrupts if MSR[EE]
CPU* os_create_boot_thread();
CPU* os_current_cpu();

// ---------------------------------------------------------------- devices
void hw_init();
u32 guest_func_addr(const char* name);   // matched-name lookup (debug helpers)

// host
void host_pump_messages();
bool host_should_quit();
