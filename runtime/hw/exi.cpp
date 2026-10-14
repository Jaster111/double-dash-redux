// EXI: external interface.  Channel 0 device 1 is the IPL/RTC/SRAM chip.
// Memory card slots are provided by memcard.cpp (when a card is inserted).
// SRAM layout: OSSram (0x00-0x13) then OSSramEx (0x14-0x3F).
#include "hw.h"
#include "exi.h"
#include <time.h>
#include <windows.h>
#include <string>

// ---------------------------------------------------------------- IPL / RTC / SRAM
struct IplDevice : ExiDevice {
    u32 addr = 0, pos = 0, rw = 0;
    u8 sram[64]{};
    u8 rtc[4]{};

    IplDevice() {
        // defaults matching a freshly-initialised console (English, stereo)
        sram[0x12] = 0;          // language: English
        sram[0x13] = 0x2C;       // flags: stereo | bit3/5 set like a configured console
        // SRAM-ex: memory card flash IDs (zero) and their checksums (~sum), as left by the IPL
        // after unlocking a card; memcard.cpp replaces them with the ID a loaded card was formatted with
        sram[0x3A] = sram[0x3B] = 0xFF;
        fix_checksum();
    }
    void fix_checksum() {
        u16 sum = 0, inv = 0;
        for (int i = 0x0C; i < 0x14; i += 2) {
            u16 w = (u16)((sram[i] << 8) | sram[i + 1]);
            sum += w;
            inv += (u16)~w;
        }
        sram[0] = (u8)(sum >> 8); sram[1] = (u8)sum;
        sram[2] = (u8)(inv >> 8); sram[3] = (u8)inv;
    }
    void select() override { addr = 0; pos = 0; rw = 0; }
    void deselect() override { if (addr & 0x80000000u) fix_checksum_if_sram(); }
    void fix_checksum_if_sram() {}
    void update_rtc() {
        // seconds since 2000-01-01 (the OS adds the SRAM counter bias)
        time_t now = time(nullptr);
        struct tm t2000 = {};
        t2000.tm_year = 100; t2000.tm_mday = 1;
        time_t base = _mkgmtime(&t2000);
        u32 secs = (u32)(now - base);
        // local time, like a console set to the user's clock
        TIME_ZONE_INFORMATION tz;
        if (GetTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID) secs -= tz.Bias * 60;
        rtc[0] = (u8)(secs >> 24); rtc[1] = (u8)(secs >> 16); rtc[2] = (u8)(secs >> 8); rtc[3] = (u8)secs;
    }
    u8 transfer(u8 in, bool) override {
        if (pos < 4) {
            addr = (addr << 8) | in;
            pos++;
            if (pos == 4) { rw = 0; update_rtc(); }
            return 0;
        }
        bool write = addr & 0x80000000u;
        u32 a = addr & 0x7FFFFFFFu;
        u8 out = 0;
        if (a < 0x20000000u) {
            out = 0;   // IPL ROM (fonts) not available
        } else if (a < 0x20000100u) {
            u32 o = ((a - 0x20000000u) >> 6) + rw;
            if (o < 4) { if (write) rtc[o] = in; else out = rtc[o]; }
        } else if (a < 0x20001100u) {
            u32 o = ((a - 0x20000100u) >> 6) + rw;
            if (o < 64) { if (write) sram[o] = in; else out = sram[o]; }
        } else if (a >= 0x20010000u && a < 0x20010100u) {
            if (write && in) { static std::string line; if (in == '\n' || in == '\r') { if (!line.empty()) LOG("uart", "%s", line.c_str()); line.clear(); } else line.push_back((char)in); }
        }
        rw++;
        return out;
    }
};

// ---------------------------------------------------------------- channels
struct ExiChannel {
    u32 csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    ExiDevice* dev[3] = {};
    int selected = -1;
};
static ExiChannel s_ch[3];
static IplDevice s_ipl;

void sram_set_flash_id(int slot, const u8 id[12]) {
    u8 sum = 0;
    for (int i = 0; i < 12; i++) sum += s_ipl.sram[0x14 + 12 * slot + i] = id[i];
    s_ipl.sram[0x3A + slot] = (u8)~sum;
}

enum : u32 {
    CSR_EXIINTMASK = 1 << 0, CSR_EXIINT = 1 << 1, CSR_TCINTMASK = 1 << 2, CSR_TCINT = 1 << 3,
    CSR_EXTINTMASK = 1 << 10, CSR_EXTINT = 1 << 11, CSR_EXT = 1 << 12, CSR_ROMDIS = 1 << 13,
};

static void update_irq() {
    bool on = false;
    for (auto& c : s_ch) {
        if ((c.csr & CSR_EXIINT) && (c.csr & CSR_EXIINTMASK)) on = true;
        if ((c.csr & CSR_TCINT) && (c.csr & CSR_TCINTMASK)) on = true;
        if ((c.csr & CSR_EXTINT) && (c.csr & CSR_EXTINTMASK)) on = true;
    }
    pi_set_irq(PI_IRQ_EXI, on);
}

void exi_init() {
    // ntd bit 6: EuRGB60 (PAL60) enabled in the IPL settings.  The game then offers 60 / 50 Hz at boot
    // (60 Hz preselected, chosen automatically when its countdown runs out).  50 Hz judders on 60 Hz and
    // faster monitors, so this is the default; video.pal60 = 0 / DD_60HZ=0 leaves it off (50 Hz, no prompt).
    if (cfg_bool("video.pal60", true, "DD_60HZ")) {
        s_ipl.sram[0x11] |= 0x40;
        s_ipl.fix_checksum();
    }
    s_ch[0].dev[0] = memcard_device(0);
    s_ch[0].dev[1] = &s_ipl;
    s_ch[1].dev[0] = memcard_device(1);
}

void exi_raise_exiint(int chn) {
    s_ch[chn].csr |= CSR_EXIINT;
    update_irq();
}

static void refresh_ext(int chn) {
    ExiChannel& c = s_ch[chn];
    if (chn < 2 && c.dev[0] && c.dev[0]->present()) c.csr |= CSR_EXT;
    else c.csr &= ~CSR_EXT;
}

static void do_transfer(int chn) {
    ExiChannel& c = s_ch[chn];
    ExiDevice* d = c.selected >= 0 ? c.dev[c.selected] : nullptr;
    u32 rw = (c.cr >> 2) & 3;
    if (c.cr & 2) {   // DMA
        u8* p = host_ptr(c.mar & 0x03FFFFE0u);
        u32 n = c.len;
        if (d) {
            if (rw == 0) d->dma_read(p, n);
            else d->dma_write(p, n);
        } else if (rw == 0) memset(p, 0xFF, n);
    } else {          // immediate, 1..4 bytes from the top of DATA
        int n = ((c.cr >> 4) & 3) + 1;
        u32 out = 0;
        for (int i = 0; i < n; i++) {
            u8 in = (u8)(c.data >> (24 - 8 * i));
            u8 r = d ? d->transfer(rw == 0 ? 0 : in, rw != 0) : 0xFF;
            out |= (u32)r << (24 - 8 * i);
        }
        if (rw != 1) c.data = out;
    }
    c.cr &= ~1u;
    c.csr |= CSR_TCINT;
    update_irq();
}

static u32 exi_read32(u32 off) {
    int chn = off / 0x14;
    if (chn > 2) return 0;
    ExiChannel& c = s_ch[chn];
    switch (off % 0x14) {
    case 0x00: refresh_ext(chn); return c.csr;
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    case 0x10: return c.data;
    }
    return 0;
}

static void exi_write32(u32 off, u32 v) {
    int chn = off / 0x14;
    if (chn > 2) return;
    ExiChannel& c = s_ch[chn];
    switch (off % 0x14) {
    case 0x00: {
        u32 ack = v & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT);
        u32 keep = c.csr & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT | CSR_EXT) & ~ack;
        int old_sel = c.selected;
        c.csr = keep | (v & ~(CSR_EXIINT | CSR_TCINT | CSR_EXTINT | CSR_EXT));
        u32 cs = (v >> 7) & 7;
        int sel = cs & 1 ? 0 : cs & 2 ? 1 : cs & 4 ? 2 : -1;
        if (sel != old_sel) {
            if (old_sel >= 0 && c.dev[old_sel]) c.dev[old_sel]->deselect();
            c.selected = sel;
            if (sel >= 0 && c.dev[sel]) c.dev[sel]->select();
        }
        update_irq();
        return;
    }
    case 0x04: c.mar = v; return;
    case 0x08: c.len = v; return;
    case 0x0C: c.cr = v; if (v & 1) do_transfer(chn); return;
    case 0x10: c.data = v; return;
    }
}

static u32 exi_read16(u32 off) { u32 v = exi_read32(off & ~3u); return (off & 2) ? lo16(v) : hi16(v); }
static void exi_write16(u32 off, u16 v) {
    u32 cur = exi_read32(off & ~3u);
    if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
    exi_write32(off & ~3u, cur);
}

const MmioBlock g_exi_block = {exi_read16, exi_write16, exi_read32, exi_write32};

