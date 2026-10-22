// fsb_probe - read the game's own sound banks through the game's own fmodex.dll (2026-10-21).
//
//   fsb_probe <fmodex.dll> list <bank.fsb> [filter]        every subsound: index, name, length ms
//   fsb_probe <fmodex.dll> env  <bank.fsb> <name> [ms]     one subsound's loudness every <ms> (default 10)
//   fsb_probe <fmodex.dll> env  <sound.mp3> - [ms]         a single sound's (a BS1 Sound object's data is an MP3:
//                                                          model-export.ps1 / UModel -sounds writes it as .unk)
//
// Why: a gun's action sound is ONE file for a whole action (the shotgun's pump = back AND forward, the Tommy Gun's
// reload = drum out, drum in, bolt), and the manual reload plays it in parts as the player does each part. Where each
// part starts is read from the sound itself: its loudness envelope, decoded by FMOD from the bank on this machine.
// Nothing decoded is written to disk - only names, lengths and loudness numbers to stdout. Built and run by
// tools\fsb-probe.ps1; 32-bit (fmodex.dll is).
#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Result = int;
using CreateFn = Result(__stdcall*)(void** system);
using SetOutputFn = Result(__stdcall*)(void* system, int output);
using InitFn = Result(__stdcall*)(void* system, int maxChannels, unsigned flags, void* extra);
using CreateSoundFn = Result(__stdcall*)(void* system, const char* name, unsigned mode, void* exinfo, void** sound);
using NumSubFn = Result(__stdcall*)(void* sound, int* n);
using SubFn = Result(__stdcall*)(void* sound, int index, void** sub);
using NameFn = Result(__stdcall*)(void* sound, char* name, int len);
using LengthFn = Result(__stdcall*)(void* sound, unsigned* len, unsigned unit);
using FormatFn = Result(__stdcall*)(void* sound, int* type, int* format, int* channels, int* bits);
using DefaultsFn = Result(__stdcall*)(void* sound, float* freq, float* vol, float* pan, int* prio);
using ReadFn = Result(__stdcall*)(void* sound, void* buf, unsigned len, unsigned* read);
using ReleaseFn = Result(__stdcall*)(void* sound);

constexpr unsigned kModeDefault = 0x0, k2D = 0x8, kCreateStream = 0x80, kSoftware = 0x40, kOpenOnly = 0x100;
constexpr unsigned kMs = 1, kPcm = 2;
constexpr int kOutputNoSound = 2; // FMOD Ex: AUTODETECT 0, UNKNOWN 1, NOSOUND 2

bool contains(const char* hay, const char* needle) {
    for (; *hay; ++hay) {
        size_t k = 0;
        while (needle[k] && hay[k] && std::tolower((unsigned char)hay[k]) == std::tolower((unsigned char)needle[k])) ++k;
        if (!needle[k]) return true;
    }
    return !*needle;
}

template <typename T> T get(HMODULE m, const char* n) {
    T f = reinterpret_cast<T>(GetProcAddress(m, n));
    if (!f) {
        std::fprintf(stderr, "fsb_probe: %s missing from fmodex.dll\n", n);
        std::exit(2);
    }
    return f;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: fsb_probe <fmodex.dll> list <bank> [filter] | env <bank> <name> [ms]\n");
        return 2;
    }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) {
        std::fprintf(stderr, "fsb_probe: cannot load %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    auto create = get<CreateFn>(m, "FMOD_System_Create");
    auto setOutput = get<SetOutputFn>(m, "FMOD_System_SetOutput");
    auto init = get<InitFn>(m, "FMOD_System_Init");
    auto createSound = get<CreateSoundFn>(m, "FMOD_System_CreateSound");
    auto numSub = get<NumSubFn>(m, "FMOD_Sound_GetNumSubSounds");
    auto sub = get<SubFn>(m, "FMOD_Sound_GetSubSound");
    auto name = get<NameFn>(m, "FMOD_Sound_GetName");
    auto length = get<LengthFn>(m, "FMOD_Sound_GetLength");
    auto format = get<FormatFn>(m, "FMOD_Sound_GetFormat");
    auto defaults = get<DefaultsFn>(m, "FMOD_Sound_GetDefaults");
    auto readData = get<ReadFn>(m, "FMOD_Sound_ReadData");
    auto release = get<ReleaseFn>(m, "FMOD_Sound_Release");

    void* sys = nullptr;
    Result r = create(&sys);
    if (!r) r = setOutput(sys, kOutputNoSound);
    if (!r) r = init(sys, 4, 0, nullptr);
    if (r) {
        std::fprintf(stderr, "fsb_probe: FMOD system: error %d\n", r);
        return 1;
    }
    // `seek <sound> <ms>`: how a CHANNEL's millisecond position maps onto the audio, for each way a sound can be
    // made - a channel seeked to <ms> reports its position in ms and in samples, against the decoded audio's own
    // samples (ms * 44.1). An MP3 without an exact length is seeked by an estimate; the mod's cut points are in
    // decoded time, so this says what to ask for.
    if (!std::strcmp(argv[2], "seek") && argc > 4) {
        using PlayFn = Result(__stdcall*)(void* sys, int idx, void* snd, int paused, void** ch);
        using SetPosFn = Result(__stdcall*)(void* ch, unsigned pos, unsigned unit);
        using GetPosFn = Result(__stdcall*)(void* ch, unsigned* pos, unsigned unit);
        auto play = get<PlayFn>(m, "FMOD_System_PlaySound");
        auto setPos = get<SetPosFn>(m, "FMOD_Channel_SetPosition");
        auto getPos = get<GetPosFn>(m, "FMOD_Channel_GetPosition");
        const unsigned want = static_cast<unsigned>(std::atoi(argv[4]));
        constexpr unsigned kCompressed = 0x200, kAccurate = 0x4000;
        const struct { const char* name; unsigned mode; } modes[] = {
            {"sample (decompressed)", k2D | kSoftware},
            {"compressed sample", k2D | kSoftware | kCompressed},
            {"stream", k2D | kSoftware | kCreateStream},
            {"sample, ACCURATETIME", k2D | kSoftware | kAccurate},
            {"compressed, ACCURATETIME", k2D | kSoftware | kCompressed | kAccurate},
        };
        for (const auto& md : modes) {
            void* snd = nullptr;
            if (createSound(sys, argv[3], md.mode, nullptr, &snd)) {
                std::printf("%-26s cannot be made\n", md.name);
                continue;
            }
            unsigned lenMs = 0, lenPcm = 0;
            length(snd, &lenMs, kMs);
            length(snd, &lenPcm, kPcm);
            void* ch = nullptr;
            unsigned posMs = 0, posPcm = 0;
            const Result pr = play(sys, -1, snd, 1, &ch); // FMOD_CHANNEL_FREE, paused
            const Result sr = pr ? pr : setPos(ch, want, kMs);
            if (!pr) {
                getPos(ch, &posMs, kMs);
                getPos(ch, &posPcm, kPcm);
            }
            std::printf("%-26s length %u ms = %u samples (%.0f ms at 44.1k) | seek %u ms -> reads %u ms, sample %u (%.1f ms "
                        "of decoded audio)%s\n",
                        md.name, lenMs, lenPcm, lenPcm / 44.1, want, posMs, posPcm, posPcm / 44.1, sr ? " - FAILED" : "");
            release(snd);
        }
        return 0;
    }
    // `pcm <sound> [ms]`: a single sound's loudness from its WHOLE decoded audio - made a decompressed sample with
    // FMOD's exact length (ACCURATETIME) and read with Sound::lock. 2026-10-21: the `env` mode's readData on an
    // open-only stream came back ~392 ms short on both MP3s measured (3109 of 3500 ms, 2403 of 2795) and every part
    // placed from it was off by that much; this is the timeline a channel's position uses.
    if (!std::strcmp(argv[2], "pcm")) {
        using LockFn = Result(__stdcall*)(void* snd, unsigned off, unsigned len, void** p1, void** p2, unsigned* l1,
                                          unsigned* l2);
        using UnlockFn = Result(__stdcall*)(void* snd, void* p1, void* p2, unsigned l1, unsigned l2);
        auto lock = get<LockFn>(m, "FMOD_Sound_Lock");
        auto unlock = get<UnlockFn>(m, "FMOD_Sound_Unlock");
        constexpr unsigned kAccurate = 0x4000, kPcmBytes = 4;
        void* snd = nullptr;
        if (createSound(sys, argv[3], k2D | kSoftware | kAccurate, nullptr, &snd)) {
            std::fprintf(stderr, "fsb_probe: %s cannot be made a sample\n", argv[3]);
            return 1;
        }
        int type = 0, fmt = 0, ch = 0, bits = 0;
        float freq = 0, vol = 0, pan = 0;
        int prio = 0;
        format(snd, &type, &fmt, &ch, &bits);
        defaults(snd, &freq, &vol, &pan, &prio);
        unsigned bytes = 0, ms = 0;
        length(snd, &bytes, kPcmBytes);
        length(snd, &ms, kMs);
        void *p1 = nullptr, *p2 = nullptr;
        unsigned l1 = 0, l2 = 0;
        if (bits != 16 || ch < 1 || freq <= 0 || lock(snd, 0, bytes, &p1, &p2, &l1, &l2) || !p1) {
            std::fprintf(stderr, "fsb_probe: cannot lock %s (%d bits, %d ch)\n", argv[3], bits, ch);
            return 1;
        }
        const short* pcm = static_cast<const short*>(p1);
        const unsigned total = l1 / (sizeof(short) * ch);
        const int stepMs = argc > 4 ? std::atoi(argv[4]) : 10;
        const unsigned frames = static_cast<unsigned>(freq * stepMs / 1000.0f);
        std::printf("# %s: %u ms, %u samples locked = %.0f ms, %d ch, %.0f Hz\n", argv[3], ms, total, total * 1000.0 / freq,
                    ch, freq);
        std::vector<double> rms;
        double peak = 0;
        for (unsigned at = 0; frames && at + frames <= total; at += frames) {
            double acc = 0;
            for (unsigned k = at * ch; k < (at + frames) * ch; ++k) acc += double(pcm[k]) * pcm[k];
            const double v = std::sqrt(acc / (frames * ch)) / 32768.0;
            rms.push_back(v);
            if (v > peak) peak = v;
        }
        for (size_t k = 0; k < rms.size(); ++k) {
            const double db = rms[k] > 1e-6 ? 20.0 * std::log10(rms[k]) : -120.0;
            const int bar = peak > 0 ? static_cast<int>(50.0 * rms[k] / peak + 0.5) : 0;
            std::printf("%6u  %6.1f  %.*s\n", static_cast<unsigned>(k * stepMs), db, bar,
                        "##################################################");
        }
        unlock(snd, p1, p2, l1, l2);
        release(snd);
        return 0;
    }
    void* bank = nullptr;
    r = createSound(sys, argv[3], kModeDefault | k2D | kSoftware | kCreateStream | kOpenOnly, nullptr, &bank);
    if (r) {
        std::fprintf(stderr, "fsb_probe: open %s: error %d\n", argv[3], r);
        return 1;
    }
    int n = 0;
    numSub(bank, &n);
    const bool env = !std::strcmp(argv[2], "env");
    const char* want = argc > 4 ? argv[4] : nullptr;
    const int stepMs = env && argc > 5 ? std::atoi(argv[5]) : 10;
    int found = 0;
    // A single sound (a Sound object's own data, as UModel exports it: an MP3) has no subsounds - it is the one.
    const bool single = n == 0;
    for (int i = 0; i < (single ? 1 : n); ++i) {
        void* s = nullptr;
        if (single)
            s = bank;
        else if (sub(bank, i, &s) || !s)
            continue;
        char nm[256] = {};
        name(s, nm, sizeof nm);
        if (want && !single && (env ? _stricmp(nm, want) != 0 : !contains(nm, want))) continue;
        ++found;
        unsigned ms = 0;
        length(s, &ms, kMs);
        if (!env) {
            std::printf("%5d  %7u ms  %s\n", i, ms, nm);
            continue;
        }
        int type = 0, fmt = 0, ch = 0, bits = 0;
        float freq = 0, vol = 0, pan = 0;
        int prio = 0;
        format(s, &type, &fmt, &ch, &bits);
        defaults(s, &freq, &vol, &pan, &prio);
        std::printf("# %s: %u ms, %d ch, %d bits, %.0f Hz (format %d)\n", nm, ms, ch, bits, freq, fmt);
        if (bits != 16 || ch < 1 || freq <= 0) {
            std::printf("# not 16-bit PCM once decoded - no envelope\n");
            continue;
        }
        // Timed by the samples decoded, never by the reads: a read may return fewer than asked (an MP3 frame at a
        // time), and counting each read as a step stretched the timeline (2026-10-21: parts placed 25% late).
        const unsigned frames = static_cast<unsigned>(freq * stepMs / 1000.0f);
        std::vector<short> all;
        std::vector<short> buf(4096 * ch);
        unsigned decoded = 0;
        for (;;) {
            unsigned got = 0;
            const Result rr = readData(s, buf.data(), static_cast<unsigned>(buf.size() * sizeof(short)), &got);
            const unsigned f = got / (sizeof(short) * ch);
            all.insert(all.end(), buf.begin(), buf.begin() + f * ch);
            decoded += f;
            if (!f || rr) break;
        }
        std::printf("# decoded %u samples = %.0f ms\n", decoded, decoded * 1000.0 / freq);
        double peak = 0;
        std::vector<double> rms;
        for (size_t at = 0; at + frames <= decoded; at += frames) {
            double acc = 0;
            for (size_t k = at * ch; k < (at + frames) * ch; ++k) acc += double(all[k]) * all[k];
            const double v = std::sqrt(acc / (frames * ch)) / 32768.0;
            rms.push_back(v);
            if (v > peak) peak = v;
        }
        // ms, dBFS, a bar against the loudest step
        for (size_t k = 0; k < rms.size(); ++k) {
            const double db = rms[k] > 1e-6 ? 20.0 * std::log10(rms[k]) : -120.0;
            const int bar = peak > 0 ? static_cast<int>(50.0 * rms[k] / peak + 0.5) : 0;
            std::printf("%6u  %6.1f  %.*s\n", static_cast<unsigned>(k * stepMs), db, bar,
                        "##################################################");
        }
    }
    if (!found) std::printf("# nothing in %s matches %s (%d subsounds)\n", argv[3], want ? want : "", n);
    release(bank);
    return 0;
}
