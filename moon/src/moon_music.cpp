// Lossless background music from the user's CD rips.
//
// The game's arranged music set (ADX_BGM.AFS entries 2048.., cues 0x1800..0x1822) is the same performance as the
// audio CD of the 1997 PC version. A tool (scratch/music/build_cd_music.py for now) cuts each cue out of the
// user's CD rips, sample-accurate, with its loop measured in the CD audio, and stores it as <cue>.flac (44.1 kHz
// stereo, lossless) in moon/music/, with cues.csv (loop points in CD samples, gain to the ADX level).
//
// Playback: the game keeps playing the ADX cue as always (CRI ADXT on the EE, mixed on the IOP), only muted, so
// its timing, loops, fades and end-of-track signal stay exactly the game's; the FLAC is mixed into the SPU2 output
// at the host (ps2SetAudioMixHook), started when the ADX handle reports "playing", at the volume the game asks
// for (ADXT_SetOutVol, 0.1 dB units), looping at the CD loop points.
//   ADXT_StartAfs(adxt, partition, file id)  0x25C6B0 -> 0x25C700 (same arguments, under the ADX lock; hooked: the game
//                                             reaches 0x25C6B0 by a tail jump, which the recompiler calls directly)
//   ADXT_SetOutVol(adxt, vol)                0x25A8C0  (-960 = silent .. 0)
//   ADXT_Stop(adxt)                          0x259AB8
//   BGM handle: *(u32 *)(gp - 0x4610) = 0x566FE0; status: signed byte at handle + 1 (3 = playing).
// Switches: PS2X_MOON_MUSIC=0 off; PS2X_MOON_MUSIC_DIR=<folder>; PS2X_MOON_MUSIC_GAIN=cd (CD master level instead
// of the ADX level); PS2X_MOON_MUSIC_TRACE=1 logs; PS2X_MOON_BGM_SET=0/1 forces the game's music set (test aid).

#include "moon_music.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO_DEFAULT_ALLOCATOR_OVERRIDE
#include "external/dr_flac.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Ps2AudioMixHook = void (*)(int16_t *frames, size_t frameCount);
void ps2SetAudioMixHook(Ps2AudioMixHook hook); // ps2_audio.cpp

namespace
{
    constexpr uint32_t kStartAfs = 0x0025C700u, kSetOutVol = 0x0025A8C0u, kStop = 0x00259AB8u;
    constexpr uint32_t kBgmHandle = 0x00566FE0u; // gp - 0x4610
    constexpr uint32_t kBgmRequest = 0x001F5770u; // BGM request router (cue -> set table -> file id)
    constexpr uint32_t kBgmSetFlags = 0x0056490Cu; // bits 7-8: music set (0: AFS 0.., 1: AFS 2048..)
    constexpr int32_t kSilent = -960;

    uint8_t *g_rdram = nullptr;
    PS2Runtime::RecompiledFunction g_startOrig = nullptr, g_volOrig = nullptr, g_stopOrig = nullptr, g_requestOrig = nullptr;
    std::string g_dir;
    bool g_trace = false, g_cdGain = false;

    struct CueInfo
    {
        uint32_t loopStart = 0, loopEnd = 0; // CD samples; loopEnd 0 = plays once
        float gain = 1.0f;                   // to the ADX level
    };
    std::map<uint32_t, CueInfo> g_cues; // cue id (0x18xx) -> info, from cues.csv

    struct Track
    {
        uint32_t cue = 0;
        std::vector<int16_t> pcm; // interleaved stereo 44.1 kHz
        uint64_t frames = 0;
        CueInfo info;
        std::atomic<bool> ready{false};
    };

    // Shared between the EE thread (hooks) and the IOP thread (mixer).
    std::mutex g_mutex;
    std::shared_ptr<Track> g_track; // armed cue, or null
    int32_t g_gameVol = 0;          // last ADXT_SetOutVol the game asked for (0.1 dB)
    bool g_started = false;
    bool g_muted = false;           // the ADX BGM handle is at -960 because a CD track stands in (or did)
    bool g_inStart = false;         // inside the start hook (the ADX start stops the handle first)
    double g_pos = 0.0;            // position in CD samples
    float g_curGain = 0.0f;

    uint32_t rd32(uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, g_rdram + (a & 0x01FFFFFFu), 4);
        return v;
    }

    void callOrig(PS2Runtime::RecompiledFunction fn, R5900Context *ctx, PS2Runtime *runtime, uint32_t a0, uint32_t a1)
    {
        const R5900Context saved = *ctx;
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 5, a1);
        fn(g_rdram, ctx, runtime);
        *ctx = saved;
    }

    void loadCues()
    {
        std::ifstream f(g_dir + "/cues.csv");
        std::string line;
        std::getline(f, line); // header: cue,afs_index,cd_track,cd_start_sample,length,loop_start,loop_end,gain_to_adx,...
        while (std::getline(f, line))
        {
            std::vector<std::string> col;
            std::string cell;
            std::istringstream ss(line);
            while (col.size() < 8 && std::getline(ss, cell, ','))
                col.push_back(cell);
            if (col.size() < 8 || col[4].empty())
                continue;
            CueInfo c;
            c.loopStart = col[5].empty() ? 0u : static_cast<uint32_t>(std::stoul(col[5]));
            c.loopEnd = col[6].empty() ? 0u : static_cast<uint32_t>(std::stoul(col[6]));
            if (c.loopEnd <= c.loopStart)
                c.loopEnd = 0u;
            c.gain = col[7].empty() ? 1.0f : std::stof(col[7]);
            g_cues[static_cast<uint32_t>(std::stoul(col[0], nullptr, 16))] = c;
        }
    }

    std::shared_ptr<Track> openCue(uint32_t cue)
    {
        auto it = g_cues.find(cue);
        if (it == g_cues.end())
            return nullptr;
        char name[16];
        std::snprintf(name, sizeof(name), "/%04x.flac", cue);
        const std::string path = g_dir + name;
        if (FILE *f = std::fopen(path.c_str(), "rb"))
            std::fclose(f);
        else
            return nullptr;
        auto t = std::make_shared<Track>();
        t->cue = cue;
        t->info = it->second;
        if (g_cdGain)
            t->info.gain = 1.0f;
        // Decoded off the EE thread (~0.1 s for a 2-minute cue); the mixer waits for it.
        std::thread([t, path]() {
            unsigned channels = 0, rate = 0;
            drflac_uint64 frames = 0;
            drflac_int16 *pcm = drflac_open_file_and_read_pcm_frames_s16(path.c_str(), &channels, &rate, &frames, nullptr);
            if (pcm && channels == 2 && rate == 44100)
            {
                t->pcm.assign(pcm, pcm + frames * 2u);
                t->frames = frames;
                t->ready = true;
            }
            else
                std::fprintf(stderr, "[moon-music] %s: not 44.1 kHz stereo FLAC\n", path.c_str());
            drflac_free(pcm, nullptr);
        }).detach();
        return t;
    }

    // ADXT_StartAfs(adxt, partition, fid)
    void startAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t adxt = GPR_U32(ctx, 4), part = GPR_U32(ctx, 5), fid = GPR_U32(ctx, 6);
        const bool bgm = adxt != 0u && adxt == rd32(kBgmHandle) && part == 0u;
        if (g_trace)
            std::fprintf(stderr, "[moon-music] ADXT_StartAfs(adxt %08x, part %u, fid %u) bgm handle %08x\n", adxt, part, fid,
                         rd32(kBgmHandle));
        std::shared_ptr<Track> next = (bgm && fid >= 2048u) ? openCue(0x1800u + (fid - 2048u)) : nullptr;
        bool restore = false;
        int32_t vol;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (bgm)
            {
                restore = g_muted && !next;
                g_track = next;
                g_started = false;
                g_pos = 0.0;
                g_muted = next != nullptr;
                g_inStart = true;
            }
            vol = g_gameVol;
        }
        if (g_trace && bgm)
            std::fprintf(stderr, "[moon-music] start fid %u%s\n", fid, next ? " (CD)" : "");
        g_startOrig(rdram, ctx, runtime);
        g_inStart = false;
        // The ADX plays muted while the CD track stands in; back to the game's volume when no CD track does.
        if (next)
            callOrig(g_volOrig, ctx, runtime, adxt, static_cast<uint32_t>(kSilent));
        else if (restore)
            callOrig(g_volOrig, ctx, runtime, adxt, static_cast<uint32_t>(vol));
    }

    // ADXT_SetOutVol(adxt, vol)
    void setOutVol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t adxt = GPR_U32(ctx, 4);
        if (adxt != 0u && adxt == rd32(kBgmHandle))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // The ADX library re-applies the handle's stored volume (our -960 mute) when playback starts; only
            // calls from the game itself are the game's volume.
            const int32_t v = static_cast<int32_t>(GPR_U32(ctx, 5));
            if (!(g_muted && v == kSilent && GPR_U32(ctx, 31) >= 0x00250000u))
                g_gameVol = v;
            if (g_trace)
                std::fprintf(stderr, "[moon-music] SetOutVol %d%s ra %08x\n", v, (g_track && g_muted) ? " (ADX muted)" : "", GPR_U32(ctx, 31));
            if (g_track && g_muted)
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(kSilent));
        }
        g_volOrig(rdram, ctx, runtime);
    }

    // ADXT_Stop(adxt)
    void stop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t adxt = GPR_U32(ctx, 4);
        if (adxt != 0u && adxt == rd32(kBgmHandle) && !g_inStart)
        {
            bool had;
            int32_t vol;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                had = g_muted;
                g_track.reset();
                g_muted = false;
                vol = g_gameVol;
            }
            g_stopOrig(rdram, ctx, runtime);
            if (had)
                callOrig(g_volOrig, ctx, runtime, adxt, static_cast<uint32_t>(vol)); // the next ADX cue at the game's volume
            return;
        }
        g_stopOrig(rdram, ctx, runtime);
    }

    // Test aid: PS2X_MOON_BGM_SET=0/1 forces the music set before the game maps a cue.
    void bgmRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_set = []() { const char *v = std::getenv("PS2X_MOON_BGM_SET"); return v ? std::atoi(v) : -1; }();
        if (s_set >= 0)
        {
            uint32_t v = rd32(kBgmSetFlags);
            v = (v & ~0x180u) | ((static_cast<uint32_t>(s_set) & 3u) << 7);
            std::memcpy(rdram + kBgmSetFlags, &v, 4);
        }
        g_requestOrig(rdram, ctx, runtime);
    }

    // IOP thread, every SPU2 output block (48 kHz stereo).
    void mix(int16_t *frames, size_t count)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_track || !g_track->ready)
            return;
        const uint32_t handle = rd32(kBgmHandle);
        const int8_t status = handle ? static_cast<int8_t>(g_rdram[(handle + 1u) & 0x01FFFFFFu]) : 0;
        if (g_trace)
        {
            static uint64_t s_blocks = 0;
            static int8_t s_last = -100;
            if (status != s_last || (++s_blocks % 400u) == 0u)
                std::fprintf(stderr, "[moon-music] mix cue %04x status %d started %d pos %.0f vol %d\n", g_track->cue, status,
                             g_started ? 1 : 0, g_pos, g_gameVol);
            s_last = status;
        }
        if (!g_started)
        {
            if (status != 3)
                return; // the ADX starts playing after its stream is buffered: start with it
            g_started = true;
            g_pos = 0.0;
            g_curGain = 0.0f;
        }
        else if (status != 3)
        {
            // Stopped or ended, however the game did it (some stops are tail jumps the hooks do not see).
            g_track.reset();
            return;
        }
        const Track &t = *g_track;
        const float target = t.info.gain * std::pow(10.0f, static_cast<float>(std::max(g_gameVol, kSilent)) / 200.0f);
        const float gain0 = g_curGain;
        const double step = 44100.0 / 48000.0;
        for (size_t i = 0; i < count; ++i)
        {
            if (g_pos >= static_cast<double>(t.frames))
                break;
            const uint64_t i0 = static_cast<uint64_t>(g_pos);
            const float f = static_cast<float>(g_pos - static_cast<double>(i0));
            uint64_t i1 = i0 + 1u;
            if (t.info.loopEnd && i1 >= t.info.loopEnd)
                i1 = t.info.loopStart;
            if (i1 >= t.frames)
                i1 = i0;
            const float g = gain0 + (target - gain0) * static_cast<float>(i + 1u) / static_cast<float>(count); // ramp per block
            for (int c = 0; c < 2; ++c)
            {
                const float s = static_cast<float>(t.pcm[i0 * 2u + c]) * (1.0f - f) + static_cast<float>(t.pcm[i1 * 2u + c]) * f;
                const float out = static_cast<float>(frames[i * 2u + c]) + s * g;
                frames[i * 2u + c] = static_cast<int16_t>(std::clamp(out, -32768.0f, 32767.0f));
            }
            g_pos += step;
            if (t.info.loopEnd && g_pos >= static_cast<double>(t.info.loopEnd))
                g_pos -= static_cast<double>(t.info.loopEnd - t.info.loopStart);
        }
        g_curGain = target;
    }
}

namespace moon_music
{
void install(PS2Runtime &runtime)
{
    if (const char *v = std::getenv("PS2X_MOON_MUSIC"); v && std::strcmp(v, "0") == 0)
        return;
    const char *dir = std::getenv("PS2X_MOON_MUSIC_DIR");
    g_dir = dir ? dir : PS2Runtime::getIoPaths().elfDirectory.string() + "/../music";
    loadCues();
    if (g_cues.empty())
        return; // no CD music: the game's own ADX music
    g_trace = [] { const char *v = std::getenv("PS2X_MOON_MUSIC_TRACE"); return v && std::strcmp(v, "0") != 0; }();
    g_cdGain = [] { const char *v = std::getenv("PS2X_MOON_MUSIC_GAIN"); return v && std::strcmp(v, "cd") == 0; }();
    g_rdram = runtime.memory().getRDRAM();
    g_startOrig = runtime.lookupFunction(kStartAfs);
    g_volOrig = runtime.lookupFunction(kSetOutVol);
    g_stopOrig = runtime.lookupFunction(kStop);
    g_requestOrig = runtime.lookupFunction(kBgmRequest);
    if (!g_startOrig || !g_volOrig || !g_stopOrig || !g_requestOrig || !g_rdram)
    {
        std::fprintf(stderr, "[moon-music] ADX functions not found; CD music off\n");
        return;
    }
    runtime.replaceFunction(kStartAfs, &startAfs);
    runtime.replaceFunction(kSetOutVol, &setOutVol);
    runtime.replaceFunction(kStop, &stop);
    runtime.replaceFunction(kBgmRequest, &bgmRequest);
    ps2SetAudioMixHook(&mix);
    std::fprintf(stderr, "[moon-music] CD music: %zu cues from %s\n", g_cues.size(), g_dir.c_str());
}
}
