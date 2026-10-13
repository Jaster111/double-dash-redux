// HLE of the Nintendo EAD "Zelda" family DSP microcode (hash 2FCDF1EC is the
// variant used by Mario Kart: Double Dash!!, Four Swords Adventures and Pikmin 2).
//
// Mail protocol, as observed from JAudio (osdsp_task.c / dsptask.c) and in line
// with Dolphin's ZeldaUCode::HandleMailDefault:
//   DSP -> CPU  0xDCD10000 + irq   task started            (task init callback)
//               0xF3551111         handshake, read synchronously by DspHandShake
//               0xDCD10004 + irq,  0xF355xxxx    command acknowledged (sync value xxxx)
//               0xDCD10005 + irq   all requested audio frames rendered
//   CPU -> DSP  0x0000NNNN, w0..   command packet: word count N (non-zero), then the words
//               0x00000000, 0x00GGFFFF   voice sync: voices of group GG (16 each) are ready,
//                                  FFFF = per-voice flags (bit 15 = first voice of the group)
//               0xCDD10001         replace ucode (next mails: a new boot task)
//               0xCDD10002         reboot to ROM
//               0xCDD10003         continue after frame end
// Command header w0: bit 31 = acknowledge when done (ack value = w0 >> 16),
// bits 24-30 = command.  Command 0x01 (setup) gives the voice count and the
// VPB/table addresses.  Command 0x02 renders (w0 >> 16 & 0xFF) frames of 0x50
// samples into the buffers w1 (left) / w2 (right) at volume w0 & 0xFFFF; the DSP
// renders as far as the voice syncs allow, acknowledges every finished frame with
// 0xFF00 | n and ends with 0xDCD10005.  Further commands stall until the CPU
// answers with 0xCDD10003.  The mixing itself is in zelda_renderer.cpp.
#include "ucode.h"
#include <vector>
#include <string>
#include <algorithm>

// zelda_renderer.cpp
void zelda_renderer_reset();
void zelda_setup(u32 vpbs, u32 tables, u32 afc, u32 reverb);
void zelda_render_start(u16 volume, u32 lbuf, u32 rbuf);
void zelda_prepare_frame();
void zelda_add_voice(u16 id);
void zelda_finalize_frame();

namespace {

struct ZeldaUcode;
ZeldaUcode* s_inst;   // for the hang report

struct ZeldaUcode : UcodeHle {
    u32 voices_per_frame = 64;   // set by command 01

    enum class State { Waiting, Sync, Command };
    State state = State::Waiting;
    u32 remaining = 0;
    std::vector<u32> cmd;

    bool can_execute = true;
    std::vector<std::vector<u32>> pending;

    // rendering progress of the current 0x02 command
    u32 frames_requested = 0, cur_frame = 0, cur_voice = 0, sync_max_voice = 0;
    u16 voice_flags[16] = {};   // per sync group, bit 15 = first voice: render it

    u32 n_mails = 0, n_cmds = 0, n_render = 0, n_frames = 0, last_mail = 0;
    std::vector<u32> last_cmd;

    ~ZeldaUcode() { if (s_inst == this) s_inst = nullptr; }
    ZeldaUcode() {
        s_inst = this;
        zelda_renderer_reset();
        dsp_send_mail_to_cpu(0xDCD10000, true);
        dsp_send_mail_to_cpu(0xF3551111, false);
    }

    static void ack(u16 value) {
        dsp_send_mail_to_cpu(0xDCD10004, true);
        dsp_send_mail_to_cpu(0xF3550000u | value, false);
    }

    bool rendering() const { return cur_frame < frames_requested; }

    // render as many voices/frames as the syncs received so far allow
    void render() {
        if (!rendering()) return;
        while (cur_frame < frames_requested) {
            if (cur_voice == 0) zelda_prepare_frame();
            while (cur_voice < voices_per_frame) {
                if (cur_voice >= sync_max_voice) return;   // wait for the next voice sync
                if (voice_flags[cur_voice >> 4] & (0x8000 >> (cur_voice & 15))) zelda_add_voice((u16)cur_voice);
                cur_voice++;
            }
            ack((u16)(0xFF00 | (cur_frame & 0xFF)));
            zelda_finalize_frame();
            n_frames++;
            cur_voice = 0;
            sync_max_voice = 0;
            cur_frame++;
        }
        dsp_send_mail_to_cpu(0xDCD10005, true);
    }

    void execute(const std::vector<u32>& c) {
        u32 w0 = c.empty() ? 0 : c[0];
        u32 op = (w0 >> 24) & 0x7F;
        n_cmds++;
        last_cmd = c;
        if (op == 0x02) {
            n_render++;
            can_execute = false;   // until the CPU sends 0xCDD10003
            zelda_render_start((u16)w0, c.size() > 1 ? c[1] : 0, c.size() > 2 ? c[2] : 0);
            frames_requested = (w0 >> 16) & 0xFF;
            cur_frame = cur_voice = sync_max_voice = 0;
            if (!frames_requested) dsp_send_mail_to_cpu(0xDCD10005, true);
            render();
            return;
        }
        if (op == 0x01 && c.size() >= 5) {
            voices_per_frame = std::min<u32>(w0 & 0xFFFF, 256);
            zelda_setup(c[1], c[2], c[3], c[4]);
        } else {
            static int logged;
            if (logged < 30) {
                logged++;
                std::string s;
                char b[16];
                for (u32 w : c) { snprintf(b, sizeof b, " %08X", w); s += b; }
                LOG("zelda", "unhandled command%s", s.c_str());
            }
        }
        if (w0 & 0x80000000u) ack((u16)(w0 >> 16));
    }

    void run_pending() {
        while (can_execute && !pending.empty()) {
            std::vector<u32> c = pending.front();
            pending.erase(pending.begin());
            execute(c);
        }
    }

    void mail(u32 m) override {
        n_mails++;
        last_mail = m;
        switch (state) {
        case State::Waiting:
            if (m & 0x80000000u) {   // control mail
                if ((m >> 16) != 0xCDD1) { LOG("zelda", "unexpected control mail %08X", m); return; }
                switch (m & 0xFFFF) {
                case 1: LOG("zelda", "ucode replace requested"); break;
                case 2: LOG("zelda", "reboot to ROM requested"); break;
                case 3: can_execute = true; run_pending(); break;   // continue
                default: LOG("zelda", "unknown control mail %08X", m);
                }
                return;
            }
            if (!(m & 0xFFFF)) {     // voice sync: the flags follow in the next mail
                static int warned;
                if (!rendering() && warned++ < 10) LOG("zelda", "voice sync %08X while not rendering", m);
                state = State::Sync;
                return;
            }
            remaining = m & 0xFFFF;
            cmd.clear();
            state = State::Command;
            return;
        case State::Sync: {
            u32 group = (m >> 16) & 0xF;
            voice_flags[group] = (u16)m;
            sync_max_voice = (group + 1) * 16;
            state = State::Waiting;
            render();
            return;
        }
        case State::Command:
            cmd.push_back(m);
            if (--remaining == 0) {
                state = State::Waiting;
                pending.push_back(cmd);
                run_pending();
            }
            return;
        }
    }
};

}  // namespace

void zelda_debug_dump() {
    ZeldaUcode* z = s_inst;
    if (!z) { LOG("hang", "zelda: no ucode instance"); return; }
    std::string s;
    char b[16];
    for (size_t i = 0; i < z->last_cmd.size() && i < 8; i++) { snprintf(b, sizeof b, " %08X", z->last_cmd[i]); s += b; }
    static const char* states[] = {"waiting", "sync", "command"};
    LOG("hang", "zelda: state=%s remaining=%u can_execute=%d pending=%zu mails=%u (last %08X) cmds=%u renders=%u "
        "frames=%u (render %u/%u, voice %u/%u) last cmd:%s",
        states[(int)z->state], z->remaining, (int)z->can_execute, z->pending.size(), z->n_mails, z->last_mail, z->n_cmds,
        z->n_render, z->n_frames, z->cur_frame, z->frames_requested, z->cur_voice, z->sync_max_voice, s.c_str());
}

UcodeHle* make_zelda_ucode(u32 hash) {
    if (hash == 0x2FCDF1EC) return new ZeldaUcode;
    return nullptr;
}
