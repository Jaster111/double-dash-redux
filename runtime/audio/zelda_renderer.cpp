// Zelda ucode audio renderer: voice decoding (PCM8/PCM16/AFC from ARAM, PCM16
// from MRAM, synthetic waves), resampling, filtering, mixing into the DSP's
// 0x50-sample buffers, reverb, and output of each frame to the CPU-provided
// left/right buffers.
//
// Behaviour follows Dolphin's ZeldaAudioRenderer (Source/Core/Core/HW/DSPHLE/
// UCodes/Zelda.cpp, GPL-2.0-or-later), specialised for ucode 2FCDF1EC: full size
// (0xC0 word) VPBs, 6 mixing destinations, target-volume ramps and doubled
// Dolby positional volumes.
#include "ucode.h"
#include <algorithm>
#include <array>
#include <stddef.h>

namespace {

typedef std::array<s16, 0x50> MixBuf;

s16 clamp16(s64 v) { return (s16)std::clamp<s64>(v, -0x8000, 0x7FFF); }

u16 rd16be(u32 a) { return BSWAP16(*(u16*)host_ptr(a)); }
void wr16be(u32 a, u16 v) { *(u16*)host_ptr(a) = BSWAP16(v); }
void copy_from_mram(s16* dst, u32 a, u32 count) { for (u32 i = 0; i < count; i++) dst[i] = (s16)rd16be(a + i * 2); }
void copy_to_mram(u32 a, const s16* src, u32 count) { for (u32 i = 0; i < count; i++) wr16be(a + i * 2, (u16)src[i]); }
u8 aram8(u32 a) { return *aram_ptr(a); }

#define ACCESSOR32(field, name) \
    u32 get_##name() const { return ((u32)field##_h << 16) | field##_l; } \
    void set_##name(u32 v) { field##_h = (u16)(v >> 16); field##_l = (u16)v; }

// Voice parameter block, 0xC0 words in MRAM.  The DSP writes back the first
// 0x80 words; the rest is read-only.
#pragma pack(push, 1)
struct VPB {
    u16 enabled;              // 00: zero = skip the voice
    u16 done;                 // 01: non-zero = skip the voice (set when it ends)
    u16 resampling_ratio;     // 02: 4.12, raw samples consumed per output sample
    u16 unk_03;
    u16 reset_vpb;            // 04: (re)start the voice: reload positions, clear filter state
    u16 end_reached;          // 05
    u16 use_constant_sample;  // 06: output constant_sample (used to fade a voice to silence)
    u16 samples_to_keep_count;
    struct Channel { u16 id; s16 target_volume, current_volume; u16 unk; };
    Channel channels[6];      // 08: non-Dolby mixing: destination buffer id + volume ramp
    u16 unk_20[8];
    u16 dolby_voice_position; // 28: 0XXXXXXX0YYYYYYY, 0x7F = left / front
    s16 dolby_reverb_factor;
    s16 dolby_volume_current;
    s16 dolby_volume_target;
    u16 use_dolby_volume;     // 2C
    u16 unk_2d[3];
    u16 current_pos_frac;     // 30: 0.12 fractional resampling position
    u16 unk_31;
    u16 afc_remaining_decoded_samples;
    s16 constant_sample;
    u16 current_position_h, current_position_l;   // 34
    u16 samples_before_loop;
    u16 unk_37;
    u16 current_aram_addr_h, current_aram_addr_l; // 38
    u16 remaining_length_h, remaining_length_l;   // 3A
    s16 resample_buffer[4];   // 3C: last 4 raw samples, history for the interpolator
    s16 variable_fir_history[20];
    s16 biquad_xn1, biquad_xn2, biquad_yn1, biquad_yn2;   // 54
    s16 afc_remaining_samples[0x10];                      // 58: last decoded AFC block
    s16 low_pass_yn1, low_pass_xn1;                       // 68
    u16 unk_6a[0x80 - 0x6A];
    // ---- read-only part
    u16 samples_source_type;  // 80
    u16 is_looping;
    s16 loop_yn1, loop_yn2;   // AFC decoder history at the loop point
    u16 filter_flags;         // 84: bits 0-4 variable FIR size, bit 5 biquad enable
    u16 end_requested;        // 85: fade out quickly, then set done
    u16 unk_86, unk_87;
    u16 loop_address_h, loop_address_l;                   // 88
    u16 loop_start_position_h, loop_start_position_l;     // 8A (really the end/loop length)
    u16 base_address_h, base_address_l;                   // 8C
    u16 unk_8e, unk_8f;
    u16 variable_fir_coeffs[20];
    s16 biquad_bn1, biquad_bn2, biquad_an1, biquad_an2;   // A4
    u16 low_pass_coeff;                                   // A8
    u16 padding[0xC0 - 0xA9];

    ACCESSOR32(current_position, current_position)
    ACCESSOR32(current_aram_addr, current_aram_addr)
    ACCESSOR32(remaining_length, remaining_length)
    ACCESSOR32(loop_address, loop_address)
    ACCESSOR32(loop_start_position, loop_start_position)
    ACCESSOR32(base_address, base_address)
    s16& afc_yn2() { return afc_remaining_samples[0xE]; }
    s16& afc_yn1() { return afc_remaining_samples[0xF]; }
    u8 dolby_x() const { return (dolby_voice_position >> 8) & 0x7F; }
    u8 dolby_y() const { return dolby_voice_position & 0x7F; }
};
static_assert(sizeof(VPB) == 0x180, "VPB size");
static_assert(offsetof(VPB, dolby_voice_position) == 0x28 * 2, "VPB layout");
static_assert(offsetof(VPB, current_pos_frac) == 0x30 * 2, "VPB layout");
static_assert(offsetof(VPB, afc_remaining_samples) == 0x58 * 2, "VPB layout");
static_assert(offsetof(VPB, samples_source_type) == 0x80 * 2, "VPB layout");
static_assert(offsetof(VPB, low_pass_coeff) == 0xA8 * 2, "VPB layout");

// Reverb parameter block (4 of them, contiguous).
struct ReverbPB {
    u16 enabled;               // bit 0: filter before mixing, bit 1: filter after
    u16 circular_buffer_size;  // in 0x50-sample blocks
    u16 circular_buffer_base_h, circular_buffer_base_l;
    struct Dest { u16 buffer_id, volume; } dest[2];   // volume 1.15
    s16 filter_coeffs[8];
    ACCESSOR32(circular_buffer_base, circular_buffer_base)
};
#pragma pack(pop)
static_assert(sizeof(ReverbPB) == 0x20, "ReverbPB size");

enum SourceType : u16 {
    SRC_SQUARE_WAVE = 0, SRC_SAW_WAVE = 1, SRC_SQUARE_WAVE_25PCT = 3, SRC_CONST_PATTERN_1 = 4,
    SRC_AFC_LQ_FROM_ARAM = 5, SRC_CONST_PATTERN_0 = 7, SRC_PCM8_FROM_ARAM = 8, SRC_AFC_HQ_FROM_ARAM = 9,
    SRC_CONST_PATTERN_0_VARIABLE_STEP = 10, SRC_CONST_PATTERN_2 = 11, SRC_CONST_PATTERN_3 = 12,
    SRC_PCM16_FROM_ARAM = 16, SRC_PCM16_FROM_MRAM = 33,
};

struct Renderer {
    // set up by command 01
    u32 vpb_base = 0, reverb_pb_base = 0;
    std::array<s16, 0x100> resampling_coeffs{};
    std::array<s16, 0x100> const_patterns{};   // 4 patterns of 0x40 samples
    std::array<s16, 0x80> sine_table{};
    std::array<s16, 0x20> afc_coeffs{};

    // set up by command 02
    u32 out_l = 0, out_r = 0;
    u16 out_volume = 0;   // 4.12

    bool prepared = false;
    // The DSP's mixing buffers; the game names them by their DSP DRAM address.
    MixBuf front_l{}, front_r{}, back_l{}, back_r{};
    MixBuf front_l_reverb{}, front_r_reverb{}, back_l_reverb{}, back_r_reverb{};
    MixBuf unk0_reverb{}, unk1_reverb{}, unk0{}, unk1{}, unk2{};

    std::array<u16, 4> reverb_frame_idx{};
    std::array<s16, 8> reverb_last8[4] = {};

    u32 voices_mixed = 0;

    MixBuf* buffer_for_id(u16 id) {
        switch (id) {
        case 0x0D00: return &front_l;
        case 0x0D60: return &front_r;
        case 0x0F40: return &back_l;
        case 0x0CA0: return &back_r;
        case 0x0E80: return &front_l_reverb;
        case 0x0EE0: return &front_r_reverb;
        case 0x0C00: return &back_l_reverb;
        case 0x0C50: return &back_r_reverb;
        case 0x0DC0: return &unk0_reverb;
        case 0x0E20: return &unk1_reverb;
        case 0x09A0: return &unk0;
        case 0x0FA0: return &unk1;
        case 0x0B00: return &unk2;
        }
        static int warned;
        if (warned++ < 10) LOG("zelda", "mix to unknown buffer %04X", id);
        return nullptr;
    }

    // ------------------------------------------------------------ mixing helpers
    // vol in 1.15, applied with saturation
    static void add_with_volume(s16* dst, const s16* src, size_t n, u16 vol) {
        for (size_t i = 0; i < n; i++) dst[i] += clamp16(((s32)src[i] * (s32)vol) >> 15);
    }
    // vol/step in 1.31 (16.16 ramp of a 1.15 volume); returns the final volume
    static s32 add_with_ramp(MixBuf& dst, const MixBuf& src, s32 vol, s32 step) {
        if (!vol && !step) return vol;
        for (size_t i = 0; i < dst.size(); i++) {
            dst[i] += (s16)(((vol >> 16) * src[i]) >> 16);
            vol += step;
        }
        return vol;
    }
    // shift = 16 - integer bits of the volume format (15 for 1.15, 12 for 4.12)
    static void apply_volume(MixBuf& buf, u16 vol, int shift) {
        for (s16& s : buf) s = clamp16((s32)((u32)s * (u32)vol) >> shift);
    }

    // ------------------------------------------------------------ frame
    void prepare_frame() {
        if (prepared) return;
        front_l.fill(0);
        front_r.fill(0);
        apply_volume(back_l, 0x6784, 15);
        apply_volume(back_r, 0x6784, 15);

        // reverb from earlier frames, then the Dolby surround reverb downmix
        apply_reverb(false);
        add_with_volume(front_l_reverb.data(), back_l_reverb.data(), 0x50, 0x7FFF);
        add_with_volume(front_r_reverb.data(), back_l_reverb.data(), 0x50, 0xB820);
        add_with_volume(front_l_reverb.data(), back_r_reverb.data() + 0x28, 0x28, 0xB820);
        add_with_volume(front_r_reverb.data(), back_r_reverb.data() + 0x28, 0x28, 0x7FFF);
        back_l_reverb.fill(0);
        back_r_reverb.fill(0);

        // patterns 2 and 3 evolve every frame (noise-like sources)
        s16* p2 = const_patterns.data() + 2 * 0x40;
        s32 yn2 = p2[0x3E], yn1 = p2[0x3F], v;
        for (int i = 0; i < 0x40; i += 2) {
            v = yn2 * yn1 - (p2[i] << 16);
            yn2 = yn1;
            yn1 = p2[i];
            p2[i] = (s16)(v >> 16);
            v = 2 * (yn2 * yn1 + (p2[i + 1] << 16));
            yn2 = yn1;
            yn1 = p2[i + 1];
            p2[i + 1] = (s16)(v >> 16);
        }
        s16* p3 = const_patterns.data() + 3 * 0x40;
        yn2 = p3[0x3E];
        yn1 = p3[0x3F];
        s16 acc = (s16)yn1;
        s16 step = (s16)(p3[0] + ((yn1 * yn2 + ((yn2 << 16) + yn1)) >> 16));
        step = (s16)((step & 0x1FF) | 0x2000);
        for (s32 i = 0; i < 0x40; i++) p3[i] = (s16)(acc + (i + 1) * step);

        prepared = true;
    }

    void finalize_frame() {
        apply_volume(front_l, out_volume, 12);
        apply_volume(front_r, out_volume, 12);
        copy_to_mram(out_l, front_l.data(), 0x50);
        copy_to_mram(out_r, front_r.data(), 0x50);
        out_l += 0x50 * 2;
        out_r += 0x50 * 2;
        apply_reverb(true);
        prepared = false;
    }

    // Called before rendering (feed last frames' reverb data back into the mix)
    // and after (store this frame's reverb sends in the MRAM delay lines).
    void apply_reverb(bool post) {
        if (!reverb_pb_base) return;
        MixBuf* bufs[4] = {&unk0_reverb, &unk1_reverb, &front_l_reverb, &front_r_reverb};
        for (int r = 0; r < 4; r++) {
            ReverbPB rpb;
            copy_from_mram((s16*)&rpb, reverb_pb_base + r * sizeof(ReverbPB), sizeof(ReverbPB) / 2);
            if (!rpb.enabled) continue;
            u32 mram = rpb.get_circular_buffer_base() + reverb_frame_idx[r] * 0x50 * 2;
            if (post) {
                copy_to_mram(mram, bufs[r]->data(), 0x50);
                reverb_frame_idx[r] = rpb.circular_buffer_size ? (reverb_frame_idx[r] + 1) % rpb.circular_buffer_size : 0;
                continue;
            }
            // 8 extra leading samples of history for the 8-tap filter
            std::array<s16, 0x58> buf;
            std::copy(reverb_last8[r].begin(), reverb_last8[r].end(), buf.begin());
            copy_from_mram(&buf[8], mram, 0x50);
            std::copy(buf.begin() + 0x50, buf.end(), reverb_last8[r].begin());
            auto filter = [&] {
                for (int i = 0; i < 0x50; i++) {
                    s32 s = 0;
                    for (int j = 0; j < 8; j++) s += (s32)buf[i + j] * rpb.filter_coeffs[j];
                    buf[i] = clamp16(s >> 15);
                }
            };
            if (rpb.enabled & 1) filter();
            for (auto& d : rpb.dest) {
                if (!d.buffer_id) continue;
                if (MixBuf* dst = buffer_for_id(d.buffer_id)) add_with_volume(dst->data(), buf.data(), 0x50, d.volume);
            }
            if (rpb.enabled & 2) filter();
            std::copy(buf.begin(), buf.begin() + 0x50, bufs[r]->begin());
        }
    }

    // ------------------------------------------------------------ voices
    void fetch_vpb(u16 id, VPB& v) { copy_from_mram((s16*)&v, vpb_base + id * sizeof(VPB), sizeof(VPB) / 2); }
    void store_vpb(u16 id, const VPB& v) { copy_to_mram(vpb_base + id * sizeof(VPB), (const s16*)&v, 0x80); }

    void add_voice(u16 id) {
        VPB vpb;
        fetch_vpb(id, vpb);
        if (!vpb.enabled || vpb.done) return;
        voices_mixed++;

        MixBuf in;
        load_input_samples(in, vpb);

        if (vpb.low_pass_coeff) low_pass(in, vpb);
        if ((vpb.filter_flags & 0x20) &&
            (vpb.biquad_an2 || vpb.biquad_an1 || vpb.biquad_bn2 || vpb.biquad_bn1 != 0x7FFF))
            biquad(in, vpb);
        // (the variable FIR filter, filter_flags bits 0-4, is not implemented)

        if (vpb.use_dolby_volume) {
            if (vpb.end_requested) {
                vpb.dolby_volume_target = vpb.dolby_volume_current / 2;
                if (vpb.dolby_volume_target == 0) vpb.done = true;
            }
            // positional volumes, 1.15; this ucode doubles them (shift 15, not 16)
            const int sh = 15;
            s16 right = sine_table[vpb.dolby_x()], back = sine_table[vpb.dolby_y()];
            s16 left = sine_table[vpb.dolby_x() ^ 0x7F], front = sine_table[vpb.dolby_y() ^ 0x7F];
            s16 quad[4] = {(s16)((left * front) >> sh), (s16)((left * back) >> sh), (s16)((right * front) >> sh),
                           (s16)((right * back) >> sh)};
            s16 delta = (s16)(vpb.dolby_volume_target - vpb.dolby_volume_current);
            s16 quad_delta[4], rev[4], rev_delta[4];
            for (int i = 0; i < 4; i++) quad_delta[i] = (s16)(((u16)quad[i] * delta) >> sh);
            for (int i = 0; i < 4; i++) quad[i] = (s16)((quad[i] * vpb.dolby_volume_current) >> sh);
            for (int i = 0; i < 4; i++) {
                rev[i] = (s16)((quad[i] * vpb.dolby_reverb_factor) >> sh);
                rev_delta[i] = (s16)((quad_delta[i] * vpb.dolby_reverb_factor) >> sh);
            }
            struct { MixBuf* buf; s16 vol, delta; } dst[8] = {
                {&front_l, quad[0], quad_delta[0]},        {&back_l, quad[1], quad_delta[1]},
                {&front_r, quad[2], quad_delta[2]},        {&back_r, quad[3], quad_delta[3]},
                {&front_l_reverb, rev[0], rev_delta[0]},   {&back_l_reverb, rev[1], rev_delta[1]},
                {&front_r_reverb, rev[2], rev_delta[2]},   {&back_r_reverb, rev[3], rev_delta[3]},
            };
            for (auto& d : dst) add_with_ramp(*d.buf, in, d.vol << 16, (d.delta << 16) / 0x50);
            vpb.dolby_volume_current = vpb.dolby_volume_target;
        } else {
            if (vpb.end_requested) {
                bool all_mute = true;
                for (auto& ch : vpb.channels) {
                    ch.target_volume = ch.current_volume / 2;
                    all_mute &= ch.target_volume == 0;
                }
                if (all_mute) vpb.done = true;
            }
            for (auto& ch : vpb.channels) {
                if (!ch.id) continue;
                s16 delta = (s16)(ch.target_volume - ch.current_volume);
                s32 step = (delta << 16) / 0x50;
                if (!ch.current_volume && !step) continue;
                MixBuf* dst = buffer_for_id(ch.id);
                if (!dst) continue;
                ch.current_volume = (s16)(add_with_ramp(*dst, in, ch.current_volume << 16, step) >> 16);
            }
        }

        if (!vpb.use_constant_sample) vpb.reset_vpb = 0;
        store_vpb(id, vpb);
    }

    static void low_pass(MixBuf& buf, VPB& vpb) {
        s32 yn1 = vpb.reset_vpb ? 0 : vpb.low_pass_yn1, xn1 = vpb.reset_vpb ? 0 : vpb.low_pass_xn1;
        s32 coeff = vpb.low_pass_coeff;   // 9.7
        for (s16& s : buf) {
            s32 xn0 = s;
            s16 yn0 = clamp16((((s64)(xn0 - xn1)) * coeff >> 7) + yn1);
            s = yn0;
            yn1 = yn0;
            xn1 = xn0;
        }
        vpb.low_pass_yn1 = (s16)yn1;
        vpb.low_pass_xn1 = (s16)xn1;
    }

    static void biquad(MixBuf& buf, VPB& vpb) {
        s32 xn1 = vpb.biquad_xn1, xn2 = vpb.biquad_xn2, yn1 = vpb.biquad_yn1, yn2 = vpb.biquad_yn2;
        for (s16& s : buf) {
            s32 xn0 = s;
            s64 t = (s64)vpb.biquad_bn1 * xn1 + (s64)vpb.biquad_bn2 * xn2 + (s64)vpb.biquad_an1 * yn1 +
                    (s64)vpb.biquad_an2 * yn2;
            s16 yn0 = clamp16(t >> 15);
            s = yn0;
            xn2 = xn1; xn1 = xn0;
            yn2 = yn1; yn1 = yn0;
        }
        vpb.biquad_xn1 = (s16)xn1; vpb.biquad_xn2 = (s16)xn2;
        vpb.biquad_yn1 = (s16)yn1; vpb.biquad_yn2 = (s16)yn2;
    }

    // raw (pre-resampling) samples needed for 0x50 output samples
    static u16 needed_raw_samples(const VPB& v) { return (u16)((v.current_pos_frac + 0x50 * (u32)v.resampling_ratio) >> 12); }

    void load_input_samples(MixBuf& out, VPB& vpb) {
        // 4 samples of history + up to 0x500 raw samples (ratio 0xFFFF) + AFC block rounding
        std::array<s16, 4 + 0x500 + 0x10> raw;
        std::copy(vpb.resample_buffer, vpb.resample_buffer + 4, raw.begin());

        if (vpb.use_constant_sample) { out.fill(vpb.constant_sample); return; }

        switch (vpb.samples_source_type) {
        case SRC_SQUARE_WAVE:
        case SRC_SQUARE_WAVE_25PCT: {
            u32 shift = vpb.samples_source_type == SRC_SQUARE_WAVE ? 1 : 2;
            u32 mask = (1u << shift) - 1, ratio = (u32)vpb.resampling_ratio << (shift - 1);
            u32 pos = (u32)vpb.current_pos_frac << shift;
            for (s16& s : out) {
                s = ((pos >> 16) & mask) ? (s16)0xC000 : 0x4000;
                pos += ratio;
            }
            vpb.current_pos_frac = (u16)(pos >> shift);
            break;
        }
        case SRC_SAW_WAVE: {
            u32 pos = vpb.current_pos_frac;
            for (s16& s : out) {
                s = (s16)pos;
                pos += vpb.resampling_ratio >> 1;
            }
            vpb.current_pos_frac = (u16)pos;
            break;
        }
        case SRC_CONST_PATTERN_0:
        case SRC_CONST_PATTERN_0_VARIABLE_STEP:
        case SRC_CONST_PATTERN_1:
        case SRC_CONST_PATTERN_2:
        case SRC_CONST_PATTERN_3: {
            u16 t = vpb.samples_source_type;
            int idx = t == SRC_CONST_PATTERN_1 ? 1 : t == SRC_CONST_PATTERN_2 ? 2 : t == SRC_CONST_PATTERN_3 ? 3 : 0;
            bool variable_step = t == SRC_CONST_PATTERN_0_VARIABLE_STEP;
            const s16* pattern = const_patterns.data() + idx * 0x40;
            u32 pos = (u32)vpb.current_pos_frac << 6, step = (u32)vpb.resampling_ratio << 5;
            for (size_t i = 0; i < out.size(); i++) {
                out[i] = pattern[pos >> 16];
                pos = (pos + step) % (0x40u << 16);
                if (variable_step) pos = ((pos << 10) + back_r[i] * vpb.resampling_ratio) >> 10;
            }
            vpb.current_pos_frac = (u16)(pos >> 6);
            break;
        }
        case SRC_PCM8_FROM_ARAM:
            download_pcm_from_aram(raw.data() + 4, vpb, needed_raw_samples(vpb), 1);
            resample(vpb, raw.data(), out);
            break;
        case SRC_AFC_HQ_FROM_ARAM:
        case SRC_AFC_LQ_FROM_ARAM:
            download_afc_from_aram(raw.data() + 4, vpb, needed_raw_samples(vpb));
            resample(vpb, raw.data(), out);
            break;
        case SRC_PCM16_FROM_ARAM:
            download_pcm_from_aram(raw.data() + 4, vpb, needed_raw_samples(vpb), 2);
            resample(vpb, raw.data(), out);
            break;
        case SRC_PCM16_FROM_MRAM:
            download_raw_from_mram(raw.data() + 4, vpb, needed_raw_samples(vpb));
            resample(vpb, raw.data(), out);
            break;
        default: {
            static u64 warned;   // one bit per type below 64
            if (vpb.samples_source_type >= 64 || !(warned & (1ull << vpb.samples_source_type))) {
                if (vpb.samples_source_type < 64) warned |= 1ull << vpb.samples_source_type;
                LOG("zelda", "unimplemented sample source %u", vpb.samples_source_type);
            }
            out.fill(0);
            break;
        }
        }
    }

    // src[0..3] is the history from the previous frame
    void resample(VPB& vpb, const s16* src, MixBuf& out) {
        u32 ratio = vpb.resampling_ratio, pos = vpb.current_pos_frac;   // 20.12
        if ((ratio >> 12) >= 4) {   // heavy decimation: no interpolation
            for (s16& s : out) {
                pos += ratio;
                s = src[pos >> 12];
            }
        } else {
            for (s16& s : out) {
                // 4-tap polyphase filter, 0x40 phases from the top bits of the fraction
                const s16* c = &resampling_coeffs[((pos & 0xFFF) >> 6) * 4];
                const s16* in = &src[pos >> 12];
                s64 acc = 0;
                for (int i = 0; i < 4; i++) acc += (s64)2 * c[i] * in[i];
                s = clamp16(acc >> 16);
                pos += ratio;
            }
        }
        for (int i = 0; i < 4; i++) vpb.resample_buffer[i] = src[(pos >> 12) + i];
        vpb.constant_sample = out[out.size() - 1];
        vpb.current_pos_frac = (u16)(pos & 0xFFF);
    }

    // bytes = 1 (PCM8) or 2 (PCM16), big endian in ARAM
    static void download_pcm_from_aram(s16* dst, VPB& vpb, u16 count, u32 bytes) {
        if (vpb.done) { std::fill(dst, dst + count, 0); return; }
        if (vpb.reset_vpb) {
            vpb.set_remaining_length(vpb.get_loop_start_position() - vpb.get_current_position());
            vpb.set_current_aram_addr(vpb.get_base_address() + vpb.get_current_position() * bytes);
        }
        vpb.end_reached = 0;
        while (count) {
            if (vpb.end_reached) {
                vpb.end_reached = 0;
                if (!vpb.is_looping) {
                    std::fill(dst, dst + count, 0);
                    vpb.done = 1;
                    break;
                }
                vpb.set_current_position(vpb.get_loop_address());
                vpb.set_remaining_length(vpb.get_loop_start_position() - vpb.get_current_position());
                vpb.set_current_aram_addr(vpb.get_base_address() + vpb.get_current_position() * bytes);
            }
            u16 n = (u16)std::min<u32>(vpb.get_remaining_length(), count);
            u32 a = vpb.get_current_aram_addr();
            for (u16 i = 0; i < n; i++, a += bytes)
                *dst++ = bytes == 1 ? (s16)((s8)aram8(a) << 8) : (s16)(aram8(a) << 8 | aram8(a + 1));
            vpb.set_remaining_length(vpb.get_remaining_length() - n);
            vpb.set_current_aram_addr(a);
            count -= n;
            if (!vpb.get_remaining_length()) vpb.end_reached = 1;
        }
    }

    void download_afc_from_aram(s16* dst, VPB& vpb, u16 count) {
        if (vpb.reset_vpb) {
            vpb.afc_yn1() = 0;
            vpb.afc_yn2() = 0;
            vpb.afc_remaining_decoded_samples = 0;
            vpb.set_remaining_length(vpb.get_loop_start_position());
            vpb.set_current_aram_addr(vpb.get_base_address());
        }
        if (vpb.done) { std::fill(dst, dst + count, 0); return; }
        if (vpb.afc_remaining_decoded_samples > 0x10) vpb.afc_remaining_decoded_samples = 0x10;

        for (;;) {
            // first the samples left over from the last decoded block
            u16 n = std::min(vpb.afc_remaining_decoded_samples, count);
            const s16* base = &vpb.afc_remaining_samples[0x10 - vpb.afc_remaining_decoded_samples];
            for (u16 i = 0; i < n; i++) *dst++ = base[i];
            vpb.afc_remaining_decoded_samples -= n;
            count -= n;
            if (count == 0) return;

            if (count <= vpb.get_remaining_length()) {
                u16 blocks = (count + 0xF) >> 4, decoded = blocks << 4;
                if (decoded < vpb.get_remaining_length()) {
                    vpb.afc_remaining_decoded_samples = decoded - count;
                    vpb.set_remaining_length(vpb.get_remaining_length() - decoded);
                } else {
                    vpb.afc_remaining_decoded_samples = (u16)(vpb.get_remaining_length() - count);
                    vpb.set_remaining_length(0);
                }
                decode_afc(vpb, dst, blocks);
                if (vpb.afc_remaining_decoded_samples) {
                    for (int i = 0; i < 0x10; i++) vpb.afc_remaining_samples[i] = dst[decoded - 0x10 + i];
                    if (!vpb.get_remaining_length() && vpb.get_loop_start_position()) {
                        // realign the leftovers for the next loop iteration
                        const s16* b = vpb.afc_remaining_samples + ((vpb.get_loop_start_position() + 0xF) & 0xF);
                        for (u16 i = 0; i < vpb.afc_remaining_decoded_samples; i++)
                            vpb.afc_remaining_samples[0x10 - i - 1] = *b--;
                    }
                }
                return;
            }

            // more requested than left: finish the sound, or loop
            if (vpb.get_remaining_length()) {
                count -= (u16)vpb.get_remaining_length();
                decode_afc(vpb, dst, (vpb.get_remaining_length() + 0xF) >> 4);
                dst += vpb.get_remaining_length();
            }
            if (!vpb.is_looping) {
                vpb.done = 1;
                std::fill(dst, dst + count, 0);
                return;
            }
            // the source type is also the block size in bytes (5 or 9)
            vpb.set_current_aram_addr(vpb.get_base_address() + (vpb.get_loop_address() >> 4) * vpb.samples_source_type);
            vpb.afc_yn2() = vpb.loop_yn2;
            vpb.afc_yn1() = vpb.loop_yn1;
            decode_afc(vpb, vpb.afc_remaining_samples, 1);
            vpb.afc_remaining_decoded_samples = 0x10 - (vpb.get_loop_address() & 0xF);
            vpb.set_remaining_length(vpb.get_loop_start_position() - vpb.afc_remaining_decoded_samples -
                                     vpb.get_loop_address());
        }
    }

    // AFC: 16 samples per block; 9-byte blocks of 4-bit nibbles (HQ) or 5-byte
    // blocks of 2-bit codes (LQ), header = scale exponent << 4 | coefficient pair
    void decode_afc(VPB& vpb, s16* dst, size_t blocks) {
        u32 a = vpb.get_current_aram_addr();
        bool hq = vpb.samples_source_type == SRC_AFC_HQ_FROM_ARAM;
        vpb.set_current_aram_addr(a + (u32)blocks * vpb.samples_source_type);
        for (size_t b = 0; b < blocks; b++) {
            u8 hdr = aram8(a++);
            s16 delta = (s16)(1 << ((hdr >> 4) & 0xF));
            int idx = hdr & 0xF;
            s16 nib[16];
            if (hq) {
                for (int i = 0; i < 16; i += 2) {
                    u8 v = aram8(a++);
                    nib[i] = (s16)((s16)((v >> 4) << 12) >> 1);
                    nib[i + 1] = (s16)((s16)((v & 0xF) << 12) >> 1);
                }
            } else {
                for (int i = 0; i < 16; i += 4) {
                    u8 v = aram8(a++);
                    for (int j = 0; j < 4; j++) nib[i + j] = (s16)((s16)(((v >> (6 - 2 * j)) & 3) << 14) >> 1);
                }
            }
            s32 yn1 = vpb.afc_yn1(), yn2 = vpb.afc_yn2();
            for (s16 n : nib) {
                s32 s = delta * n + yn1 * afc_coeffs[idx * 2] + yn2 * afc_coeffs[idx * 2 + 1];
                s = std::clamp(s >> 11, -0x8000, 0x7FFF);
                *dst++ = (s16)s;
                yn2 = yn1;
                yn1 = s;
            }
            vpb.afc_yn2() = (s16)yn2;
            vpb.afc_yn1() = (s16)yn1;
        }
    }

    static void download_raw_from_mram(s16* dst, VPB& vpb, u16 count) {
        u32 a = vpb.get_base_address() + vpb.current_position_h * 2;
        u32 remaining = vpb.get_remaining_length();
        if (count > remaining) {
            s16 last = 0;
            if (remaining) {
                copy_from_mram(dst, a, remaining);
                last = dst[remaining - 1];
            }
            for (u32 i = remaining; i < count; i++) dst[i] = last;
            vpb.current_position_h += (u16)remaining;
            vpb.set_remaining_length(0);
            vpb.done = 1;
        } else {
            vpb.set_remaining_length(remaining - count);
            vpb.samples_before_loop = vpb.loop_start_position_h - vpb.current_position_h;
            if (count <= vpb.samples_before_loop) {
                copy_from_mram(dst, a, count);
                vpb.current_position_h += count;
            } else {
                copy_from_mram(dst, a, vpb.samples_before_loop);
                vpb.set_base_address(vpb.get_loop_address());
                vpb.current_position_h = count - vpb.samples_before_loop;
                copy_from_mram(dst + vpb.samples_before_loop, vpb.get_loop_address(), vpb.current_position_h);
            }
        }
    }
};

Renderer* s_r = new Renderer;

}  // namespace

void zelda_renderer_reset() { *s_r = Renderer(); }

// command 01: vpbs = VPB array, tables = resampling coefficients (0x100), const
// patterns (0x100) and the sine table (0x80), afc = AFC coefficient pairs,
// reverb = the 4 reverb parameter blocks
void zelda_setup(u32 vpbs, u32 tables, u32 afc, u32 reverb) {
    s_r->vpb_base = vpbs;
    copy_from_mram(s_r->resampling_coeffs.data(), tables, 0x100);
    copy_from_mram(s_r->const_patterns.data(), tables + 0x200, 0x100);
    copy_from_mram(s_r->sine_table.data(), tables + 0x400, 0x80);
    copy_from_mram(s_r->afc_coeffs.data(), afc, 0x20);
    s_r->reverb_pb_base = reverb;
    LOG("zelda", "setup: vpbs=%08X tables=%08X afc=%08X reverb=%08X", vpbs, tables, afc, reverb);
}

// command 02: frames are written consecutively from lbuf/rbuf, volume is 4.12
void zelda_render_start(u16 volume, u32 lbuf, u32 rbuf) {
    s_r->out_volume = volume;
    s_r->out_l = lbuf;
    s_r->out_r = rbuf;
}

void zelda_prepare_frame() { s_r->prepare_frame(); }
void zelda_add_voice(u16 id) { s_r->add_voice(id); }
void zelda_finalize_frame() { s_r->finalize_frame(); }
u32 zelda_voices_mixed() { return s_r->voices_mixed; }
