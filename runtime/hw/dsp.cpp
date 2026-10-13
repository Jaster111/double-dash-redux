// DSP interface block: mailboxes, control/status, ARAM DMA and audio DMA.
// The DSP core itself is high-level emulated in audio/ (ucode HLE).
#include "hw.h"
#include <vector>
#include <deque>
#include <algorithm>

enum : u32 {
    MBOX_IN_H = 0x00, MBOX_IN_L = 0x02, MBOX_OUT_H = 0x04, MBOX_OUT_L = 0x06, DSPCR = 0x0A,
    AR_SIZE = 0x12, AR_MODE = 0x16, AR_REFRESH = 0x1A, AR_MM_H = 0x20, AR_MM_L = 0x22, AR_AR_H = 0x24,
    AR_AR_L = 0x26, AR_CNT_H = 0x28, AR_CNT_L = 0x2A, AID_START_H = 0x30, AID_START_L = 0x32,
    AID_CTRL = 0x36, AID_LEFT = 0x3A,
};
enum : u16 {
    CR_RES = 1 << 0, CR_PIINT = 1 << 1, CR_HALT = 1 << 2, CR_AIDINT = 1 << 3, CR_AIDINTMSK = 1 << 4,
    CR_ARINT = 1 << 5, CR_ARINTMSK = 1 << 6, CR_DSPINT = 1 << 7, CR_DSPINTMSK = 1 << 8, CR_DSPDMA = 1 << 9,
    CR_DSPINIT = 1 << 11,
};

static const u32 ARAM_SIZE = 16 * 1024 * 1024;
static std::vector<u8> s_aram(ARAM_SIZE);
static u16 s_regs[0x100];
static u16 s_cr;
static u32 s_mbox_in, s_mbox_out;
static bool s_mbox_in_valid, s_mbox_out_valid;

// audio DMA state
static u32 s_aid_start, s_aid_len;      // programmed
static u32 s_aid_cur, s_aid_left;       // in flight (bytes)
static bool s_aid_on;

u8* aram_ptr(u32 a) { return &s_aram[a & (ARAM_SIZE - 1)]; }

void dsp_hle_mail_from_cpu(u32 mail);   // audio/dsp_hle.cpp
void dsp_hle_reset();
void dsp_hle_unhalt();
void audio_push_dma(const u8* be_samples, u32 bytes);   // host/audio.cpp

static void update_irq() {
    bool on = ((s_cr & CR_AIDINT) && (s_cr & CR_AIDINTMSK)) || ((s_cr & CR_ARINT) && (s_cr & CR_ARINTMSK)) ||
              ((s_cr & CR_DSPINT) && (s_cr & CR_DSPINTMSK));
    pi_set_irq(PI_IRQ_DSP, on);
}

// DSP -> CPU mail queue: a mail stays in the mailbox until the CPU reads its
// low half; mails flagged `irq` raise the DSP interrupt when they reach the front.
struct Mail { u32 v; bool irq; };
static std::deque<Mail> s_out_queue;

static void next_out_mail() {
    if (s_mbox_out_valid || s_out_queue.empty()) return;
    Mail m = s_out_queue.front();
    s_out_queue.pop_front();
    s_mbox_out = m.v;
    s_mbox_out_valid = true;
    if (m.irq) { s_cr |= CR_DSPINT; update_irq(); }
}

// DD_DBG_DSP=1: trace every mail in both directions
static const bool s_trace = getenv("DD_DBG_DSP") != nullptr;
u32 vi_field_count();

// the most recent mails in both directions, printed by the hang report
struct MailLog { u32 field, mail; u8 dir; };   // dir: 0 = CPU->DSP, 1 = DSP->CPU, 2 = DSP->CPU +irq
static MailLog s_mail_log[256];
static u32 s_mail_log_n;
static void log_mail(u32 mail, u8 dir) { s_mail_log[s_mail_log_n++ & 255] = {vi_field_count(), mail, dir}; }

// called by the DSP HLE
void dsp_send_mail_to_cpu(u32 mail, bool irq) {
    log_mail(mail, irq ? 2 : 1);
    if (s_trace) LOG("dspmail", "f%u  DSP->CPU %08X%s", vi_field_count(), mail, irq ? " +irq" : "");
    s_out_queue.push_back({mail, irq});
    next_out_mail();
}
void dsp_send_mail_to_cpu(u32 mail) { dsp_send_mail_to_cpu(mail, false); }
void dsp_clear_mail_queue() { s_out_queue.clear(); s_mbox_out_valid = false; }
void dsp_raise_interrupt() {
    s_cr |= CR_DSPINT;
    update_irq();
}
bool dsp_cpu_mail_pending() { return s_mbox_in_valid; }

static void aram_dma() {
    u32 mm = (((u32)s_regs[AR_MM_H / 2] << 16) | s_regs[AR_MM_L / 2]) & 0x03FFFFE0u;
    u32 ar = (((u32)s_regs[AR_AR_H / 2] << 16) | s_regs[AR_AR_L / 2]) & 0x03FFFFE0u;
    u32 cnt = ((u32)s_regs[AR_CNT_H / 2] << 16) | s_regs[AR_CNT_L / 2];
    bool to_mram = cnt & 0x80000000u;
    u32 n = cnt & 0x7FFFFFE0u;
    static int logged;
    if (logged < 0) { logged++; LOG("aram", "DMA %s mm=%08X ar=%08X len=%X", to_mram ? "ARAM->MRAM" : "MRAM->ARAM", mm, ar, n); }
    if (mm + n > 0x01800000u) { LOG("aram", "DMA beyond MEM1 ignored"); n = mm < 0x01800000u ? 0x01800000u - mm : 0; }
    for (u32 i = 0; i < n; i += 32) {
        if (to_mram) memcpy(host_ptr(mm + i), aram_ptr(ar + i), 32);
        else memcpy(aram_ptr(ar + i), host_ptr(mm + i), 32);
    }
    s_regs[AR_CNT_H / 2] &= 0x8000;
    s_regs[AR_CNT_L / 2] = 0;
    s_cr |= CR_ARINT;
    update_irq();
}

// ---------------------------------------------------------------- audio DMA (32 kHz stereo s16)
static u64 aid_block_ticks(u32 bytes) { return (u64)bytes / 4 * TB_HZ / 32000; }

static void aid_event(u64) {
    if (!s_aid_on) return;
    // a block has been consumed: hand it to the host and restart from the programmed address
    audio_push_dma(host_ptr(s_aid_cur), s_aid_left);
    s_aid_cur = s_aid_start;
    s_aid_left = s_aid_len;
    s_cr |= CR_AIDINT;
    update_irq();
    ev_schedule(tb_now() + aid_block_ticks(s_aid_left ? s_aid_left : 32), aid_event, 0);
}

static void aid_control(u16 v) {
    s_aid_len = (u32)(v & 0x7FFF) * 32;
    bool on = v & 0x8000;
    if (on && !s_aid_on) {
        s_aid_on = true;
        s_aid_cur = s_aid_start;
        s_aid_left = s_aid_len;
        // the AI latches the block immediately and signals that the next may be queued
        s_cr |= CR_AIDINT;
        update_irq();
        ev_cancel(aid_event);
        ev_schedule(tb_now() + aid_block_ticks(s_aid_len ? s_aid_len : 32), aid_event, 0);
    } else if (!on) {
        s_aid_on = false;
        ev_cancel(aid_event);
    }
}

void dsp_init() { s_cr = 0; }

void zelda_debug_dump();   // audio/zelda_ucode.cpp
void dsp_debug_dump() {
    LOG("hang", "dsp: cr=%04X mbox_in=%08X%s mbox_out=%08X%s out_queue=%zu  aid: on=%d start=%08X len=%X left=%X",
        s_cr, s_mbox_in, s_mbox_in_valid ? "(full)" : "", s_mbox_out, s_mbox_out_valid ? "(full)" : "",
        s_out_queue.size(), (int)s_aid_on, s_aid_start, s_aid_len, s_aid_left);
    zelda_debug_dump();
    u32 n = std::min<u32>(s_mail_log_n, 256);
    LOG("hang", "last %u DSP mails (of %u):", n, s_mail_log_n);
    for (u32 i = s_mail_log_n - n; i != s_mail_log_n; i++) {
        const MailLog& m = s_mail_log[i & 255];
        static const char* dirs[] = {"CPU->DSP", "DSP->CPU", "DSP->CPU +irq"};
        LOG("hang", "  f%u %s %08X", m.field, dirs[m.dir], m.mail);
    }
}

static u32 dsp_read16(u32 off) {
    if (off >= 0x200) return 0;
    switch (off) {
    case MBOX_IN_H: return (u16)((s_mbox_in >> 16) & 0x7FFF) | (s_mbox_in_valid ? 0x8000 : 0);
    case MBOX_IN_L: return (u16)s_mbox_in;
    case MBOX_OUT_H: return (u16)((s_mbox_out >> 16) & 0x7FFF) | (s_mbox_out_valid ? 0x8000 : 0);
    case MBOX_OUT_L: {
        u16 v = (u16)s_mbox_out;
        if (s_mbox_out_valid) { s_mbox_out_valid = false; next_out_mail(); }
        return v;
    }
    case DSPCR: return s_cr & ~(CR_DSPDMA | CR_DSPINIT);
    case AID_LEFT: return (u16)(s_aid_on ? s_aid_left / 32 : 0);
    case AR_MODE: return s_regs[off / 2] | 1;   // ARAM controller ready
    }
    return s_regs[off / 2];
}

static void dsp_write16(u32 off, u16 v) {
    if (off >= 0x200) return;
    switch (off) {
    case MBOX_IN_H: s_mbox_in = (s_mbox_in & 0xFFFF) | ((u32)v << 16); return;
    case MBOX_IN_L:
        // the DSP HLE consumes mails immediately, so the mailbox never looks full
        s_mbox_in = (s_mbox_in & 0xFFFF0000u) | v;
        log_mail(s_mbox_in, 0);
        if (s_trace) LOG("dspmail", "f%u  CPU->DSP %08X", vi_field_count(), s_mbox_in);
        dsp_hle_mail_from_cpu(s_mbox_in);
        return;
    case DSPCR: {
        u16 ack = v & (CR_AIDINT | CR_ARINT | CR_DSPINT);
        bool was_halted = s_cr & CR_HALT;
        s_cr = (s_cr & (CR_AIDINT | CR_ARINT | CR_DSPINT) & ~ack) |
               (v & ~(CR_AIDINT | CR_ARINT | CR_DSPINT | CR_RES));
        if (v & CR_RES) dsp_hle_reset();
        if (was_halted && !(s_cr & CR_HALT)) dsp_hle_unhalt();
        update_irq();
        return;
    }
    case AR_CNT_L:
        s_regs[off / 2] = v;
        aram_dma();
        return;
    case AID_START_H: s_aid_start = (s_aid_start & 0xFFFF) | ((u32)(v & 0x03FF) << 16); s_regs[off / 2] = v; return;
    case AID_START_L: s_aid_start = (s_aid_start & 0xFFFF0000u) | (v & 0xFFE0); s_regs[off / 2] = v; return;
    case AID_CTRL: s_regs[off / 2] = v; aid_control(v); return;
    }
    s_regs[off / 2] = v;
}

const MmioBlock g_dsp_block = {dsp_read16, dsp_write16, nullptr, nullptr};

// ============================================================ AI (0x0C006C00)
// Streaming audio is unused by the game, but the SDK measures the sample-rate
// converter by watching AISCNT, so the counter must advance in real time while
// PSTAT is set.
static u32 s_aicr, s_aivr, s_aiscnt, s_aiit;
static u64 s_aiscnt_tb;        // timebase when the counter was last latched

static u32 ai_rate() { return (s_aicr & 2) ? 48000 : 32000; }   // AFR

static u32 ai_counter() {
    if (!(s_aicr & 1)) return s_aiscnt;
    u64 now = tb_now();
    u64 samples = (now - s_aiscnt_tb) * ai_rate() / TB_HZ;
    if (samples) {
        s_aiscnt += (u32)samples;
        s_aiscnt_tb += samples * TB_HZ / ai_rate();
    }
    return s_aiscnt;
}

static void ai_update_irq() { pi_set_irq(PI_IRQ_AI, (s_aicr & 0x8) && (s_aicr & 0x4)); }

static void ai_it_event(u64) {
    if (!(s_aicr & 1)) return;
    if (ai_counter() >= s_aiit) { s_aicr |= 0x8; ai_update_irq(); }
}

static void ai_schedule_it() {
    ev_cancel(ai_it_event);
    if (!(s_aicr & 1) || !(s_aicr & 4)) return;
    u32 cur = ai_counter();
    if (s_aiit <= cur) return;
    ev_schedule(s_aiscnt_tb + (u64)(s_aiit - cur) * TB_HZ / ai_rate() + 1, ai_it_event, 0);
}

void ai_init() {}

static u32 ai_read32(u32 off) {
    switch (off) {
    case 0x00: return s_aicr;
    case 0x04: return s_aivr;
    case 0x08: return ai_counter();
    case 0x0C: return s_aiit;
    }
    return 0;
}
static void ai_write32(u32 off, u32 v) {
    switch (off) {
    case 0x00: {
        ai_counter();                                   // latch before changing state
        bool was_playing = s_aicr & 1;
        if (v & 0x20) s_aiscnt = 0;                     // SCRESET
        s_aicr = (s_aicr & 0x8 & ~(v & 0x8)) | (v & ~0x28);   // AIINT write-1-to-clear
        if ((s_aicr & 1) && !was_playing) s_aiscnt_tb = tb_now();
        ai_update_irq();
        ai_schedule_it();
        return;
    }
    case 0x04: s_aivr = v; return;
    case 0x08: return;
    case 0x0C: s_aiit = v; ai_schedule_it(); return;
    }
}
static u32 ai_read16(u32 off) { u32 v = ai_read32(off & ~3u); return (off & 2) ? lo16(v) : hi16(v); }
static void ai_write16(u32 off, u16 v) {
    u32 cur = ai_read32(off & ~3u);
    if (off & 2) set_lo16(cur, v); else set_hi16(cur, v);
    ai_write32(off & ~3u, cur);
}
const MmioBlock g_ai_block = {ai_read16, ai_write16, ai_read32, ai_write32};
