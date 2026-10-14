// Host audio output: the AI DMA blocks (32 kHz stereo s16 big endian, each
// frame stored as R then L) go to an XAudio2 source voice.
//
// Emulated time follows the wall clock, so blocks arrive at the device rate
// give or take scheduling jitter.  The playback rate is nudged (at most
// +-0.5%, inaudible) to keep the queue near TARGET_CHUNKS, which absorbs the
// drift between the host clock and the sound card's.  The queue is bounded:
// blocks that would push it past MAX_QUEUED_MS are dropped, and after an
// underrun output restarts only once PRIME_CHUNKS are queued again so a stall
// gives one gap, not a crackle.
//
// DD_WAV=file  also writes everything that is output to a .wav file
// DD_MUTE=1    no device output (the .wav dump still works)
// DD_VOLUME=v  output volume 0..1 (default 1; 0 keeps the device running silently)
#include "../common.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <math.h>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace {

const u32 RATE = 32000;
const u32 CHUNK_FRAMES = RATE / 100;   // 10 ms per submitted buffer
const u32 NUM_SLOTS = 32;
const u32 MAX_QUEUED_MS = 150;
const u32 PRIME_CHUNKS = 6;
const float TARGET_CHUNKS = 6.0f;

IXAudio2* s_xa;
IXAudio2MasteringVoice* s_master;
IXAudio2SourceVoice* s_voice;
bool s_init_done, s_priming = true;
std::vector<s16> s_slots[NUM_SLOTS];
u32 s_next_slot, s_primed;
std::vector<s16> s_pending;   // interleaved L/R, host order, not yet submitted
u64 s_dropped_frames;
u32 s_underruns, s_stat_n;
u64 s_stat_queued;   // sum of queued buffers at each submit (average queue depth)
u32 s_stat_min = ~0u, s_stat_max;
float s_fill = TARGET_CHUNKS, s_ratio = 1.0f;   // smoothed queue depth, current frequency ratio

FILE* s_wav;
u32 s_wav_bytes;

void wav_write_header() {
    u32 riff = 36 + s_wav_bytes, fmt_size = 16, rate = RATE, byte_rate = RATE * 4, data = s_wav_bytes;
    u16 pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(s_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, s_wav); fwrite(&riff, 4, 1, s_wav); fwrite("WAVEfmt ", 1, 8, s_wav);
    fwrite(&fmt_size, 4, 1, s_wav); fwrite(&pcm, 2, 1, s_wav); fwrite(&ch, 2, 1, s_wav);
    fwrite(&rate, 4, 1, s_wav); fwrite(&byte_rate, 4, 1, s_wav); fwrite(&align, 2, 1, s_wav);
    fwrite(&bits, 2, 1, s_wav); fwrite("data", 1, 4, s_wav); fwrite(&data, 4, 1, s_wav);
    fseek(s_wav, 0, SEEK_END);
}

void init() {
    s_init_done = true;
    if (const char* f = getenv("DD_WAV")) {
        s_wav = fopen(f, "wb");
        if (s_wav) wav_write_header();
        else LOG("audio", "cannot create %s", f);
    }
    if (cfg_bool("audio.mute", false, "DD_MUTE")) { LOG("audio", "muted (audio.mute / DD_MUTE)"); return; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // may already be initialised; either way is fine
    HRESULT hr = XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) hr = s_xa->CreateMasteringVoice(&s_master);
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    if (SUCCEEDED(hr)) hr = s_xa->CreateSourceVoice(&s_voice, &wf);
    if (SUCCEEDED(hr)) hr = s_voice->Start();
    if (SUCCEEDED(hr)) {   // DD_VOLUME: 0..1 factor; audio.volume: percent
        float vol = getenv("DD_VOLUME") ? (float)atof(getenv("DD_VOLUME")) : cfg_int("audio.volume", 100) / 100.0f;
        if (vol != 1.0f) s_master->SetVolume(std::clamp(vol, 0.0f, 1.0f));
    }
    if (FAILED(hr)) {
        LOG("audio", "XAudio2 init failed (%08X): no sound output", (u32)hr);
        s_voice = nullptr;
        return;
    }
    for (auto& s : s_slots) s.resize(CHUNK_FRAMES * 2);
    LOG("audio", "XAudio2 output: %u Hz stereo, %u ms max latency", RATE, MAX_QUEUED_MS);
}

void submit_chunk(const s16* frames) {
    XAUDIO2_VOICE_STATE st;
    s_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued == 0 && !s_priming) { s_priming = true; s_primed = 0; s_underruns++; }
    s_stat_n++; s_stat_queued += st.BuffersQueued;
    s_stat_min = std::min(s_stat_min, st.BuffersQueued); s_stat_max = std::max(s_stat_max, st.BuffersQueued);
    if (!s_priming) {   // rate control: ~1 s smoothing, 0.1% per buffer of error
        s_fill += (st.BuffersQueued - s_fill) * 0.01f;
        float r = std::clamp(1.0f + (s_fill - TARGET_CHUNKS) * 0.001f, 0.995f, 1.005f);
        if (fabsf(r - s_ratio) > 0.0001f) { s_ratio = r; s_voice->SetFrequencyRatio(r); }
    }
    if (st.BuffersQueued * 10 >= MAX_QUEUED_MS) { s_dropped_frames += CHUNK_FRAMES; return; }
    std::vector<s16>& slot = s_slots[s_next_slot++ % NUM_SLOTS];
    memcpy(slot.data(), frames, CHUNK_FRAMES * 4);
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = CHUNK_FRAMES * 4;
    b.pAudioData = (const BYTE*)slot.data();
    if (s_priming) {
        // hold playback until a few chunks are queued
        if (s_primed == 0) s_voice->Stop();
        s_voice->SubmitSourceBuffer(&b);
        if (++s_primed >= PRIME_CHUNKS) { s_priming = false; s_voice->Start(); }
        return;
    }
    s_voice->SubmitSourceBuffer(&b);
}

}  // namespace

// called by the AI DMA (hw/dsp.cpp) for every block it has played
void audio_push_dma(const u8* be_samples, u32 bytes) {
    if (!s_init_done) init();
    u32 frames = bytes / 4;
    if (!frames) return;
    size_t base = s_pending.size();
    s_pending.resize(base + frames * 2);
    const u16* src = (const u16*)be_samples;
    for (u32 i = 0; i < frames * 2; i += 2) {   // the AI plays R, L pairs
        s_pending[base + i] = (s16)BSWAP16(src[i + 1]);
        s_pending[base + i + 1] = (s16)BSWAP16(src[i]);
    }

    if (s_wav) {
        fwrite(&s_pending[base], 4, frames, s_wav);
        s_wav_bytes += frames * 4;
        wav_write_header();
    }

    size_t off = 0;
    if (s_voice)
        for (; s_pending.size() - off >= CHUNK_FRAMES * 2; off += CHUNK_FRAMES * 2) submit_chunk(&s_pending[off]);
    else
        off = s_pending.size();
    s_pending.erase(s_pending.begin(), s_pending.begin() + off);

    // every ~5 s of submitted audio: queue statistics (10 ms buffers)
    static u64 s_prod_frames, s_prod_t0 = GetTickCount64();
    s_prod_frames += frames;
    if (s_stat_n >= 500) {
        u64 now = GetTickCount64();
        LOG("audio", "queue %.1f buffers avg (min %u max %u), rate x%.4f, %u underruns, dropped %llu ms so far; produced %.1f frames/s",
            (double)s_stat_queued / s_stat_n, s_stat_min, s_stat_max, s_ratio, s_underruns, s_dropped_frames * 1000 / RATE,
            s_prod_frames * 1000.0 / (double)(now - s_prod_t0 + 1));
        s_prod_frames = 0; s_prod_t0 = now;
        s_stat_n = 0; s_stat_queued = 0; s_stat_min = ~0u; s_stat_max = 0;
    }
}
