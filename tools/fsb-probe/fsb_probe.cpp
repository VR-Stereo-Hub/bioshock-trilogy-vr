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
        const unsigned frames = static_cast<unsigned>(freq * stepMs / 1000.0f);
        std::vector<short> buf(frames * ch);
        double peak = 0;
        std::vector<double> rms;
        for (;;) {
            unsigned got = 0;
            const Result rr = readData(s, buf.data(), static_cast<unsigned>(buf.size() * sizeof(short)), &got);
            const unsigned f = got / (sizeof(short) * ch);
            if (!f) break;
            double acc = 0;
            for (unsigned k = 0; k < f * ch; ++k) acc += double(buf[k]) * buf[k];
            const double v = std::sqrt(acc / (f * ch)) / 32768.0;
            rms.push_back(v);
            if (v > peak) peak = v;
            if (rr) break;
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
