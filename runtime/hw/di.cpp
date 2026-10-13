// DI: DVD interface, backed by an ISO image of the disc.
#include "hw.h"
#include <windows.h>

enum : u32 {
    DISR = 0x00, DICVR = 0x04, DICMDBUF0 = 0x08, DICMDBUF1 = 0x0C, DICMDBUF2 = 0x10, DIMAR = 0x14,
    DILENGTH = 0x18, DICR = 0x1C, DIIMMBUF = 0x20, DICFG = 0x24,
};
enum : u32 {
    DISR_DEINTMASK = 0x02, DISR_DEINT = 0x04, DISR_TCINTMASK = 0x08, DISR_TCINT = 0x10,
    DISR_BRKINTMASK = 0x20, DISR_BRKINT = 0x40,
    DICVR_CVRINTMASK = 0x02, DICVR_CVRINT = 0x04,
};

static u32 s_reg[0x10];
static HANDLE s_iso = INVALID_HANDLE_VALUE;
static u64 s_iso_size;
static u32 s_error;

const char* g_iso_path = "orig/game.iso";
void dbg_on_disc_read(u64 off, u32 mar, u32 n);   // host/debug.cpp

bool disc_read(u64 off, void* dst, u32 len) {
    if (s_iso == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(s_iso, dst, len, &got, &ov)) return false;
    if (got < len) memset((u8*)dst + got, 0, len - got);
    return true;
}

void di_init() {
    s_iso = CreateFileA(g_iso_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (s_iso == INVALID_HANDLE_VALUE) rt_panic("cannot open disc image %s", g_iso_path);
    LARGE_INTEGER sz;
    GetFileSizeEx(s_iso, &sz);
    s_iso_size = (u64)sz.QuadPart;
    s_reg[DICFG / 4] = 0;
}

static void update_irq() {
    u32 sr = s_reg[DISR / 4], cv = s_reg[DICVR / 4];
    bool on = ((sr & DISR_TCINT) && (sr & DISR_TCINTMASK)) || ((sr & DISR_DEINT) && (sr & DISR_DEINTMASK)) ||
              ((sr & DISR_BRKINT) && (sr & DISR_BRKINTMASK)) || ((cv & DICVR_CVRINT) && (cv & DICVR_CVRINTMASK));
    pi_set_irq(PI_IRQ_DI, on);
}

static void complete(u64 ok) {
    s_reg[DICR / 4] &= ~1u;
    if (ok) s_reg[DISR / 4] |= DISR_TCINT;
    else s_reg[DISR / 4] |= DISR_DEINT;
    update_irq();
}

static void execute() {
    u32 cmd = s_reg[DICMDBUF0 / 4];
    u32 mar = s_reg[DIMAR / 4] & 0x03FFFFE0u;
    u32 len = s_reg[DILENGTH / 4];
    bool ok = true;
    switch (cmd >> 24) {
    case 0xA8: {   // read (sub-command 0x00 data, 0x40 disc ID)
        u64 off = (u64)s_reg[DICMDBUF1 / 4] << 2;
        u32 n = s_reg[DICMDBUF2 / 4];
        if (n > len) n = len;
        if (off + n > s_iso_size) { ok = false; s_error = 0x052100; break; }   // LBA out of range
        disc_read(off, host_ptr(mar), n);
        dbg_on_disc_read(off, mar, n);
        s_reg[DIMAR / 4] += n;
        s_reg[DILENGTH / 4] = len - n;
        break;
    }
    case 0x12: {   // inquiry: drive revision info
        u8* p = host_ptr(mar);
        memset(p, 0, len < 0x20 ? len : 0x20);
        if (len >= 8) { p[2] = 0x00; p[3] = 0x02; p[4] = 0x20; p[5] = 0x02; p[6] = 0x04; p[7] = 0x02; }
        s_reg[DILENGTH / 4] = 0;
        break;
    }
    case 0xAB: break;                                    // seek
    case 0xE0: s_reg[DIIMMBUF / 4] = s_error; break;     // request error
    case 0xE1: case 0xE2: case 0xE4: s_reg[DIIMMBUF / 4] = 0; break;   // audio streaming
    case 0xE3: break;                                    // stop motor
    default:
        LOG("di", "unknown command %08X %08X %08X", cmd, s_reg[DICMDBUF1 / 4], s_reg[DICMDBUF2 / 4]);
        break;
    }
    // complete a little later, like the drive would
    ev_schedule(tb_now() + TB_HZ / 2000, complete, ok);
}

static u32 di_read32(u32 off) {
    if (off >= 0x40) return 0;
    return s_reg[off / 4];
}

static void di_write32(u32 off, u32 v) {
    if (off >= 0x40) return;
    switch (off) {
    case DISR: {
        u32& r = s_reg[DISR / 4];
        r = (r & (DISR_TCINT | DISR_DEINT | DISR_BRKINT)) & ~(v & (DISR_TCINT | DISR_DEINT | DISR_BRKINT));
        r |= v & (DISR_TCINTMASK | DISR_DEINTMASK | DISR_BRKINTMASK);
        update_irq();
        return;
    }
    case DICVR: {
        u32& r = s_reg[DICVR / 4];
        r = (r & DICVR_CVRINT & ~(v & DICVR_CVRINT)) | (v & DICVR_CVRINTMASK);   // cover closed (bit 0 = 0)
        update_irq();
        return;
    }
    case DICR:
        s_reg[DICR / 4] = v;
        if (v & 1) execute();
        return;
    }
    s_reg[off / 4] = v;
}

static u32 di_read16(u32 off) { u32 v = di_read32(off & ~3u); return (off & 2) ? lo16(v) : hi16(v); }
static void di_write16(u32 off, u16 v) {
    u32 cur = di_read32(off & ~3u);
    if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
    di_write32(off & ~3u, cur);
}

const MmioBlock g_di_block = {di_read16, di_write16, di_read32, di_write32};
