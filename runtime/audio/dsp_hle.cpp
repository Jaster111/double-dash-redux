// DSP high-level emulation.
//
// Stage 1: the boot ROM task-loading handshake (as used by __DSP_boot_task):
//   DSP -> CPU  0x8071FEED                (ROM ready)
//   CPU -> DSP  0x80F3A001, iram_mmem_addr
//               0x80F3C002, iram_addr
//               0x80F3A002, iram_length
//               0x80F3B002, dram_length
//               0x80F3D001, start_vector
// After that the uploaded ucode runs; it is identified by a hash of its IRAM
// image and handed to the matching ucode HLE.
#include "../hw/hw.h"
#include <vector>

void dsp_send_mail_to_cpu(u32 mail);
void dsp_send_mail_to_cpu(u32 mail, bool irq);
void dsp_clear_mail_queue();
void dsp_raise_interrupt();

enum class DspState { Reset, BootRom, Ucode };
static DspState s_state = DspState::Reset;
static std::vector<u32> s_boot_mails;
static u32 s_ucode_hash;

#include "ucode.h"
static UcodeHle* s_ucode;
UcodeHle* make_zelda_ucode(u32 hash);   // audio/zelda_ucode.cpp (nullptr if unknown)

static u32 hash_iram(u32 addr, u32 len) {
    // same as Dolphin's UCode CRC (HashEctor) so ucode IDs can be compared
    u32 crc = 0;
    const u8* p = host_ptr(addr);
    for (u32 i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

static bool s_rom_greeted;

void dsp_hle_reset() {
    s_state = DspState::BootRom;
    s_boot_mails.clear();
    delete s_ucode;
    s_ucode = nullptr;
    s_rom_greeted = false;
    dsp_clear_mail_queue();
}

// the boot ROM announces itself once the DSP leaves halt after a reset
void dsp_hle_unhalt() {
    if (s_state == DspState::BootRom && !s_rom_greeted) {
        s_rom_greeted = true;
        dsp_send_mail_to_cpu(0x8071FEED);
    }
}

void dsp_hle_mail_from_cpu(u32 m) {
    switch (s_state) {
    case DspState::Reset:
        LOG("dsp", "mail %08X before reset", m);
        return;
    case DspState::BootRom:
        s_boot_mails.push_back(m);
        if (s_boot_mails.size() == 10) {
            // bit 31 of every CPU->DSP mail is the mailbox-full flag, not data
            u32 mmem = s_boot_mails[1] & 0x7FFFFFFF, iaddr = s_boot_mails[3] & 0xFFFF, ilen = s_boot_mails[5] & 0xFFFF;
            u32 dlen = s_boot_mails[7] & 0xFFFF, start = s_boot_mails[9] & 0xFFFF;
            s_ucode_hash = hash_iram(mmem, ilen);
            LOG("dsp", "ucode task: mmem=%08X iram=%04X len=%X dram=%X start=%04X hash=%08X", mmem, iaddr, ilen,
                dlen, start, s_ucode_hash);
            s_state = DspState::Ucode;
            s_ucode = make_zelda_ucode(s_ucode_hash);
            if (!s_ucode) LOG("dsp", "no HLE for ucode %08X: audio disabled", s_ucode_hash);
        }
        return;
    case DspState::Ucode:
        if (s_ucode) s_ucode->mail(m);
        else LOG("dsp", "mail to unknown ucode: %08X", m);
        return;
    }
}

