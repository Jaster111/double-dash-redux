// Mario Kart: Double Dash!! — native Windows runtime for the recompiled DOL.
//
// Boot sequence (replacing IPL + apploader):
//   1. reserve guest memory, load main.dol sections, clear .bss
//   2. fill the low-memory boot info and place the FST at the top of MEM1
//   3. initialise devices, then run __start on the boot fiber
#include "common.h"
#include "hw/hw.h"
#include <windows.h>
#include <vector>
#include <string>

u8* g_mem;
extern const char* g_iso_path;
bool disc_read(u64 off, void* dst, u32 len);
void cpu_init();
void host_init();

static const u32 MEM1_SIZE = 0x01800000;
static const u32 LOCKED_CACHE = 0x20000000;   // physical alias of 0xE0000000

static void alloc_memory() {
    g_mem = (u8*)VirtualAlloc(nullptr, 0x40000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_mem) rt_panic("cannot reserve guest address space");
    if (!VirtualAlloc(g_mem, MEM1_SIZE, MEM_COMMIT, PAGE_READWRITE)) rt_panic("cannot commit MEM1");
    if (!VirtualAlloc(g_mem + LOCKED_CACHE, 0x4000, MEM_COMMIT, PAGE_READWRITE)) rt_panic("cannot commit locked cache");
}

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// main.dol straight from the disc image (boot.bin+0x420 holds its offset; game_disc.cpp has checked it)
static u32 load_dol() {
    u8 hdr[0x440], dh[0x100];
    if (!disc_read(0, hdr, sizeof hdr)) rt_panic("cannot read disc header");
    u32 dol_off = be32(hdr + 0x420), dol_size = 0;
    disc_read(dol_off, dh, sizeof dh);
    for (int i = 0; i < 18; i++) {
        u32 off = be32(&dh[4 * i]), size = be32(&dh[0x90 + 4 * i]);
        if (size && off + size > dol_size) dol_size = off + size;
    }
    std::vector<u8> dol(dol_size);
    if (!dol_size || !disc_read(dol_off, dol.data(), dol_size)) rt_panic("cannot read main.dol from the disc image");
    for (int i = 0; i < 18; i++) {
        u32 off = be32(&dol[0x00 + 4 * i]), addr = be32(&dol[0x48 + 4 * i]), size = be32(&dol[0x90 + 4 * i]);
        if (!size) continue;
        memcpy(host_ptr(addr), &dol[off], size);
    }
    u32 bss = be32(&dol[0xD8]), bss_size = be32(&dol[0xDC]);
    // .bss may contain .sdata/.sdata2 which are loaded data sections: only clear the gaps
    std::vector<std::pair<u32, u32>> data;
    for (int i = 7; i < 18; i++) {
        u32 addr = be32(&dol[0x48 + 4 * i]), size = be32(&dol[0x90 + 4 * i]);
        if (size) data.push_back({addr, addr + size});
    }
    for (u32 a = bss; a < bss + bss_size; a += 4) {
        bool loaded = false;
        for (auto& d : data) if (a >= d.first && a < d.second) { loaded = true; break; }
        if (!loaded) wr32(a, 0);
    }
    return be32(&dol[0xE0]);
}

static void setup_boot_info() {
    u8 header[0x440];
    if (!disc_read(0, header, sizeof header)) rt_panic("cannot read disc header");
    memcpy(host_ptr(0x80000000), header, 0x20);       // game ID, maker, disc, version, streaming
    wr32(0x8000001C, 0xC2339F3D);                       // DVD magic
    wr32(0x80000020, 0x0D15EA5E);                       // boot kernel magic: normal boot
    wr32(0x80000024, 1);                                // version
    wr32(0x80000028, MEM1_SIZE);                        // physical memory size
    wr32(0x8000002C, 0x00000003);                       // console type: retail HW2
    wr32(0x80000030, 0);                                // arena lo: use linker value
    // FST at the top of MEM1
    u32 fst_off = be32(header + 0x424), fst_size = be32(header + 0x428), fst_max = be32(header + 0x42C);
    u32 fst_addr = (0x80000000u + MEM1_SIZE - ((fst_max + 31) & ~31u)) & ~31u;
    std::vector<u8> fst(fst_size);
    disc_read(fst_off, fst.data(), fst_size);
    memcpy(host_ptr(fst_addr), fst.data(), fst_size);
    wr32(0x80000034, fst_addr);                         // arena hi = below the FST
    wr32(0x80000038, fst_addr);
    wr32(0x8000003C, fst_max);
    wr32(0x800000CC, 1);                                // TV mode: PAL
    wr32(0x800000F0, MEM1_SIZE);                        // simulated memory size
    wr32(0x800000F8, 162000000);                        // bus clock
    wr32(0x800000FC, 486000000);                        // CPU clock
    LOG("boot", "game %.6s, FST %u bytes at %08X", (const char*)header, fst_size, fst_addr);
}

// debugging aid: periodically report where the guest is executing
static DWORD WINAPI watchdog(void*) {
    u32 last = 0;
    for (;;) {
        Sleep(3000);
        CPU* c = os_current_cpu();
        if (!c) continue;
        LOG("watchdog", "pc=%08X lr=%08X r1=%08X r3=%08X thread=%08X", c->pc, c->lr, c->gpr[1], c->gpr[3], rd32(0x800000E4));
        void os_debug_dump();
        os_debug_dump();
        last = c->pc;
    }
}

// The data directory (settings.ini, saves\, cache\, DoubleDashRedux.log) is the executable's directory, or in the
// development tree the project root (the directory holding orig\game.iso), so the exe can be started from
// anywhere (double-click, shortcut, out\Release\...).
static void find_data_dir() {
    if (GetFileAttributesA("orig/game.iso") != INVALID_FILE_ATTRIBUTES) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    std::string exe_dir = path;
    for (int up = 0; up < 6; up++) {
        char probe[MAX_PATH];
        snprintf(probe, sizeof probe, "%s\\orig\\game.iso", path);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) { SetCurrentDirectoryA(path); return; }
        char* s = strrchr(path, '\\');
        if (!s) break;
        *s = 0;
    }
    SetCurrentDirectoryA(exe_dir.c_str());
}

int main(int argc, char** argv) {
    SetProcessDPIAware();
    find_data_dir();
    cfg_load();
    // a windows-subsystem exe: the log goes to DoubleDashRedux.log, and to stderr when that is redirected (tools\run.ps1)
    // or when a console is asked for
    if (cfg_bool("debug.console", false, "DD_CONSOLE") && !GetStdHandle(STD_ERROR_HANDLE) && AllocConsole())
        freopen("CONOUT$", "w", stderr);
    bool launcher_wanted(int argc, char** argv);   // host/launcher.cpp
    bool launcher_run(int argc, char** argv);
    if (launcher_wanted(argc, argv) && !launcher_run(argc, argv)) return 0;
    void disc_select(int argc, char** argv);   // host/game_disc.cpp
    disc_select(argc, argv);
    tb_init();
    alloc_memory();
    cpu_init();
    host_init();
    void patches_init();   // host/patches.cpp (before hw_init: the renderer sizes the EFB for widescreen)
    patches_init();
    hw_init();
    u32 entry = load_dol();
    setup_boot_info();
    void dbg_init();
    dbg_init();
    void prof_init();
    prof_init();
    CPU* c = os_create_boot_thread();
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    c->msr = 0x00003032;    // FP | ME | IR | DR | RI, interrupts off
    LOG("boot", "starting at %08X", entry);
    call_indirect(c, entry);
    LOG("boot", "__start returned");
    return 0;
}

void hw_init() {
    vi_init();
    di_init();
    si_init();
    exi_init();
    dsp_init();
    ai_init();
    gx_init();
}

u32 guest_func_addr(const char*) { return 0; }
