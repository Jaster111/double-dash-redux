// Memory card (EXI channel 0/1, device 0), stored as a Dolphin-compatible raw image.
//
// Commands, as issued by the SDK CARD library (CARDBios.c):
//   00 00 | id id id id        EXIGetID: card size in Mbit (0x04 = 59 blocks)
//   83 00 | st                 read status
//   89                         clear status
//   81 xx                      interrupt enable (bit 0)
//   52 a1 a2 a3 a4 <latency> | DMA read      read array
//   F2 a1 a2 a3 a4 + DMA write (128 bytes)   page program, starts on deselect
//   F1 a1 a2                   sector erase (8 KiB), starts on deselect
//   F4 00 00                   chip erase
// Program/erase completion clears BUSY and raises the EXI interrupt if enabled.
//
// The card reports itself as already unlocked (status bit 0x40), like a card the
// IPL has unlocked at power-on; the SDK then skips __CARDUnlock and its DSP task
// and only checks the flash ID checksum in SRAM-ex, which sram_set_flash_id keeps
// consistent with the card's format serial.
#include "hw.h"
#include "exi.h"
#include <windows.h>
#include <time.h>
#include <vector>
#include <string>

namespace {

enum : u8 { ST_READY = 0x01, ST_PROGRAMERR = 0x08, ST_ERASEERR = 0x10, ST_SLEEP = 0x20, ST_UNLOCKED = 0x40, ST_BUSY = 0x80 };
const u32 BLOCK = 0x2000;          // sector / CARD block size
const u32 DEFAULT_SIZE = 0x80000;  // 4 Mbit: 59 user blocks
const bool s_trace = getenv("DD_DBG_EXI") != nullptr;

u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
void put16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
u64 be64(const u8* p) { u64 v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

// __CARDCheckSum: sum and inverted sum of big-endian u16 words
void card_checksum(const u8* p, u32 len, u8* out) {
    u16 sum = 0, inv = 0;
    for (u32 i = 0; i + 1 < len; i += 2) {
        u16 w = be16(p + i);
        sum += w;
        inv += (u16)~w;
    }
    if (sum == 0xFFFF) sum = 0;
    if (inv == 0xFFFF) inv = 0;
    put16(out, sum);
    put16(out + 2, inv);
}

// the SDK's serial scrambling: serial[i] = flashID[i] + rand_i (CARDFormat / VerifyID)
template <typename F> void serial_rand(u64 seed, F per_byte) {
    u64 r = seed;
    for (int i = 0; i < 12; i++) {
        r = (r * 1103515245ull + 12345) >> 16;
        per_byte(i, (u8)r);
        r = ((r * 1103515245ull + 12345) >> 16) & 0x7FFF;
    }
}

struct MemoryCard : ExiDevice {
    int slot;
    std::string path;
    std::vector<u8> data;
    FILE* file = nullptr;

    u8 status = ST_READY | ST_UNLOCKED;
    bool int_enabled = false;
    u32 pos = 0, addr = 0;
    u8 cmd = 0, args[4] = {};
    u32 program_len = 0;
    u8 program_buf[128];

    MemoryCard(int s, const std::string& p) : slot(s), path(p) {}

    u32 mbits() const { return (u32)(data.size() / 0x20000); }

    bool open() {
        if (FILE* f = fopen(path.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            u32 mb = (u32)(n / 0x20000);
            if (n % 0x20000 || !(mb == 4 || mb == 8 || mb == 16 || mb == 32 || mb == 64 || mb == 128)) {
                LOG("memcard", "%s: unexpected size %ld, not inserted", path.c_str(), n);
                fclose(f);
                return false;
            }
            data.resize(n);
            fread(data.data(), 1, n, f);
            fclose(f);
            LOG("memcard", "slot %c: %s (%u blocks)", 'A' + slot, path.c_str(), (unsigned)(data.size() / BLOCK - 5));
        } else {
            data.assign(DEFAULT_SIZE, 0xFF);
            format();
            LOG("memcard", "slot %c: created formatted card %s", 'A' + slot, path.c_str());
        }
        file = fopen(path.c_str(), "r+b");
        if (!file) file = fopen(path.c_str(), "w+b");
        if (!file) { LOG("memcard", "cannot write %s: saving disabled", path.c_str()); return false; }
        fwrite(data.data(), 1, data.size(), file);
        fflush(file);
        derive_flash_id();
        return true;
    }

    // same layout as __CARDFormatRegionAsync, with flash ID zero
    void format() {
        u8* id = &data[0];
        memset(id, 0xFF, 5 * BLOCK);
        time_t now = time(nullptr);
        struct tm t2000 = {};
        t2000.tm_year = 100; t2000.tm_mday = 1;
        u64 tb = (u64)(now - _mkgmtime(&t2000)) * TB_HZ;
        serial_rand(tb, [&](int i, u8 r) { id[i] = r; });
        for (int i = 0; i < 8; i++) id[12 + i] = (u8)(tb >> (56 - 8 * i));
        memset(id + 20, 0, 12);         // RTC bias, language, VI DTV status
        put16(id + 0x20, 0);            // device ID
        put16(id + 0x22, (u16)mbits());
        put16(id + 0x24, 0);            // font encoding: ANSI (PAL)
        card_checksum(id, 0x1FC, id + 0x1FC);
        for (int i = 0; i < 2; i++) {   // directories (all entries free)
            u8* dir = &data[(1 + i) * BLOCK];
            put16(dir + 0x1FFA, (u16)i);
            card_checksum(dir, BLOCK - 4, dir + 0x1FFC);
        }
        for (int i = 0; i < 2; i++) {   // block allocation tables
            u8* fat = &data[(3 + i) * BLOCK];
            memset(fat, 0, BLOCK);
            put16(fat + 4, (u16)i);                              // check code
            put16(fat + 6, (u16)(data.size() / BLOCK - 5));      // free blocks
            put16(fat + 8, 4);                                   // last allocated
            card_checksum(fat + 4, BLOCK - 4, fat);
        }
    }

    // recover the flash ID the card was formatted with (Dolphin: SetCardFlashID)
    void derive_flash_id() {
        u8 fid[12];
        const u8* serial = &data[0];
        serial_rand(be64(serial + 12), [&](int i, u8 r) { fid[i] = (u8)(serial[i] - r); });
        sram_set_flash_id(slot, fid);
    }

    void persist(u32 off, u32 len) {
        if (!file) return;
        fseek(file, (long)off, SEEK_SET);
        fwrite(&data[off], 1, len, file);
        fflush(file);
    }

    u32 mask() const { return (u32)data.size() - 1; }

    static void done_event(u64 slot);
    void finish_later(u64 us) {
        status = (status | ST_BUSY) & ~ST_READY;
        ev_schedule(tb_now() + us * TB_HZ / 1000000, done_event, (u64)slot);
    }
    void finish() {
        status = (status | ST_READY) & ~ST_BUSY;
        if (int_enabled) exi_raise_exiint(slot);
    }

    void select() override { pos = 0; program_len = 0; }

    void deselect() override {
        if (pos == 0) return;
        if (s_trace) LOG("memcard", "%c: cmd %02X addr %05X len %u", 'A' + slot, cmd, addr, pos);
        switch (cmd) {
        case 0xF1: {   // sector erase
            u32 a = (((u32)args[0] << 17) | ((u32)args[1] << 9)) & mask() & ~(BLOCK - 1);
            memset(&data[a], 0xFF, BLOCK);
            persist(a, BLOCK);
            finish_later(2000);
            break;
        }
        case 0xF2:     // page program
            if (program_len) {
                u32 a = addr & mask() & ~127u;
                memcpy(&data[a], program_buf, program_len);
                persist(a, program_len);
                finish_later(500);
            }
            break;
        case 0xF4:     // chip erase
            memset(data.data(), 0xFF, data.size());
            persist(0, (u32)data.size());
            finish_later(10000);
            break;
        }
        pos = 0;
    }

    u32 array_addr() const {
        return (((u32)args[0] & 0x7F) << 17) | ((u32)args[1] << 9) | (((u32)args[2] & 3) << 7) | (args[3] & 0x7F);
    }

    u8 transfer(u8 in, bool write) override {
        u32 p = pos++;
        if (p == 0) {
            cmd = in;
            switch (cmd) {
            case 0x89: status &= ~(ST_PROGRAMERR | ST_ERASEERR); break;   // clear status
            case 0x87: status &= ~ST_SLEEP; break;                        // wake up
            case 0x88: status |= ST_SLEEP; break;                         // sleep
            case 0x00: case 0x52: case 0x81: case 0x83: case 0xF1: case 0xF2: case 0xF4: break;
            default: LOG("memcard", "%c: unknown command %02X", 'A' + slot, cmd);
            }
            return 0xFF;
        }
        if (p <= 4) {
            args[p - 1] = in;
            if (cmd == 0x52 && p == 4) addr = array_addr();
            if (cmd == 0xF2 && p == 4) addr = array_addr();
        }
        switch (cmd) {
        case 0x00:     // device ID, big-endian, after the two command bytes
            return p >= 2 && p < 6 ? (u8)(mbits() >> (8 * (5 - p))) : 0;
        case 0x83:
            return status;
        case 0x81:
            if (p == 1) int_enabled = in & 1;
            return 0xFF;
        case 0x52:     // data follows the address and the latency bytes
            if (p >= 5 && !write) { u8 v = data[addr & mask()]; addr++; return v; }
            return 0xFF;
        case 0xF2:
            if (p >= 5 && write && program_len < 128) program_buf[program_len++] = in;
            return 0xFF;
        }
        return 0xFF;
    }

    void dma_read(u8* dst, u32 len) override {
        if (cmd != 0x52) { memset(dst, 0xFF, len); return; }
        for (u32 i = 0; i < len; i++) dst[i] = data[(addr + i) & mask()];
        addr += len;
        pos += len;
    }

    void dma_write(const u8* src, u32 len) override {
        if (cmd != 0xF2) return;
        u32 n = len < 128 - program_len ? len : 128 - program_len;
        memcpy(program_buf + program_len, src, n);
        program_len += n;
        pos += len;
    }
};

MemoryCard* s_cards[2];

void MemoryCard::done_event(u64 slot) {
    if (s_cards[slot]) s_cards[slot]->finish();
}

}  // namespace

// DD_MEMCARD_A / DD_MEMCARD_B: card image path, or "none" for an empty slot.
// Default: slot A = saves/MemoryCardA.raw (created and formatted on first run), slot B empty.
ExiDevice* memcard_device(int slot) {
    const char* env = getenv(slot == 0 ? "DD_MEMCARD_A" : "DD_MEMCARD_B");
    std::string path = env ? env : slot == 0 ? "saves/MemoryCardA.raw" : "none";
    if (path.empty() || path == "none") return nullptr;
    if (!env) CreateDirectoryA("saves", nullptr);
    MemoryCard* c = new MemoryCard(slot, path);
    if (!c->open()) { delete c; return nullptr; }
    return s_cards[slot] = c;
}
