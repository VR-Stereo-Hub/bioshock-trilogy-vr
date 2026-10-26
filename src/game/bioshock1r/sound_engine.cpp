#include "game/bioshock1r/sound_engine.h"

#include "core/util/log.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::sound_engine {
namespace {

// AActor::TriggerEffectEvent, thiscall: the FName by value (index, number), Other, Material, FVector*, FRotator*,
// two bools, Observer, the ReferenceTag FName by value, EffectEventInfo - 12 dwords (patterns.h).
using TriggerFn = int(__thiscall*)(void* self, int32_t n0, int32_t n1, void* other, void* mat, void* loc, void* rot,
                                   int32_t b0, int32_t b1, void* obs, int32_t r0, int32_t r1, void* info);
TriggerFn g_orig = nullptr;
void* g_target = nullptr;
bool g_tried = false;

// ---- FMOD Ex (the game's fmodex.dll, its C++ API: __stdcall, `this` first) ----
// 2026-10-21 (one cock sound played both strokes at once; each stroke should have its own half): an action sound is one file for a whole action, so a part of it is played - the channel FMOD starts for
// the event is moved to the part's start and stopped at its end. System::playSound is hooked to catch that channel
// (and to log which sound each of the gun's events plays: `[sound] <event> plays <sound>`).
using PlaySoundFn = int(__stdcall*)(void* system, int channelIndex, void* sound, int paused, void** channel);
using ChSetPosFn = int(__stdcall*)(void* ch, unsigned pos, unsigned unit);
using ChGetPosFn = int(__stdcall*)(void* ch, unsigned* pos, unsigned unit);
using ChStopFn = int(__stdcall*)(void* ch);
using SndNameFn = int(__stdcall*)(void* snd, char* name, int len);
using SndLengthFn = int(__stdcall*)(void* snd, unsigned* len, unsigned unit);
constexpr unsigned kTimeMs = 1; // FMOD_TIMEUNIT_MS
PlaySoundFn g_playOrig = nullptr;
ChSetPosFn g_chSetPos = nullptr;
ChGetPosFn g_chGetPos = nullptr;
ChStopFn g_chStop = nullptr;
SndNameFn g_sndName = nullptr;
SndLengthFn g_sndLength = nullptr;

struct Want {
    const char* sound = nullptr; // the part's sound (its FMOD name)
    unsigned fromMs = 0, toMs = 0;
    unsigned lengthMs = 0;       // the take the part was cut from (FMOD's length); 0: any
    bool first = true;           // the action's first part: another take plays whole here; later parts stop it
};
struct Live {
    void* ch;
    unsigned toMs;
};
std::mutex g_mx;
std::vector<Live> g_live;           // under g_mx: the parts playing, each stopped at its end
thread_local const Want* t_want = nullptr;
thread_local int t_caught = 0;      // this play_part's channels: 1 the part's sound, -1 only others
thread_local const wchar_t* t_event = nullptr; // the gun's effect event being fired on this thread

// The sound's FMOD name and length. The game makes its sounds from memory, and FMOD names those "(null)" (the 20:18
// run: every one) - so a part is matched by name only when the sound has one, and its length is logged to say which
// it was (reloadEndwav ~840 ms, pump_01 ~400, tommy reload_01 ~2350).
bool name_is(void* sound, const char* want, char* nm, int n, unsigned* lenMs = nullptr) {
    nm[0] = 0;
    if (lenMs) *lenMs = 0;
    if (!g_sndName || !sound) return false;
    __try {
        g_sndName(sound, nm, n);
        if (lenMs && g_sndLength) g_sndLength(sound, lenMs, kTimeMs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nm[0] = 0;
    }
    nm[n - 1] = 0;
    const bool unnamed = !nm[0] || !_stricmp(nm, "(null)");
    if (unnamed) strcpy_s(nm, n, "an unnamed sound");
    return want && (unnamed || !_stricmp(nm, want));
}

// FMOD calls on a channel that may have ended: SEH-guarded, in functions of their own (no unwinding objects).
int ch_position(void* ch, unsigned* pos) {
    __try {
        return g_chGetPos(ch, pos, kTimeMs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void ch_stop(void* ch) {
    __try {
        g_chStop(ch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void cut(void* ch, const Want& w, const char* nm) {
    const int r = g_chSetPos(ch, w.fromMs, kTimeMs);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_live.push_back({ch, w.toMs});
    }
    BVR_LOG("[sound] part of %s: %u-%u ms%s", nm, w.fromMs, w.toMs, r ? " - setPosition FAILED" : "");
}

// 2026-10-22: the sound the mod's event answers with is not always the take the parts are cut from. The shotgun's
// electric buck answers ShotgunReloadEnd with TWO responses (the map package: HandsShotgunReloadEnd_ammoElectric and
// _ammoElectricDefault): an electric layer - one of six takes, 757-1175 ms - starts inside the call, and the base
// take (3552 ms) a moment later, outside it, where no cut reached it: it played whole, both strokes, on each stroke
// (the 09:31 and 10:14 runs, "both directions and doubled"). So a part that met another sound inside the call waits
// this long for a sound of its take's length and cuts that one.
struct Pending {
    Want want;
    uint64_t untilMs = 0;
};
Pending g_pending; // under g_mx
constexpr uint64_t kLateMs = 150;

bool other_take(unsigned want, unsigned len) {
    const unsigned d = len > want ? len - want : want - len;
    return want && len && d * 50 > want;
}

int __stdcall play_detour(void* system, int channelIndex, void* sound, int paused, void** channel) {
    const int r = g_playOrig(system, channelIndex, sound, paused, channel);
    if (r || !channel || !*channel) return r;
    char nm[128];
    if (t_want) { // inside play_part's event: the part's sound is cut (the first; an event plays one sound)
        unsigned len = 0;
        if (t_caught <= 0 && name_is(sound, t_want->sound, nm, sizeof nm, &len)) {
            if (other_take(t_want->lengthMs, len)) {
                // The variant's own layer: whole once, with the action's first part; a later part stops it.
                if (t_want->first) {
                    BVR_LOG("[sound] %s's variant layer: %u ms long (the parts are cut from the %u ms take) - played whole",
                            t_want->sound, len, t_want->lengthMs);
                } else {
                    ch_stop(*channel);
                    BVR_LOG("[sound] %s's variant layer: %u ms long - stopped (it played whole with the first part)",
                            t_want->sound, len);
                }
                std::lock_guard<std::mutex> lk(g_mx);
                g_pending.want = *t_want;
                g_pending.untilMs = GetTickCount64() + kLateMs;
                t_caught = 1;
                return r;
            }
            BVR_LOG("[sound] the part's sound: %s, %u ms long (wanted %s)", nm, len, t_want->sound);
            cut(*channel, *t_want, nm);
            t_caught = 1;
        } else {
            if (!t_caught) t_caught = -1;
            BVR_LOG("[sound] part of %s wanted, the event plays %s", t_want->sound, nm[0] ? nm : "?");
        }
        return r;
    }
    if (t_event) {
        unsigned len = 0;
        name_is(sound, nullptr, nm, sizeof nm, &len);
        BVR_LOG("[sound] %ls plays %s (%u ms)", t_event, nm, len);
    }
    // The base take starting late, after a part met the variant's layer: cut to that part.
    {
        Want late;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_pending.untilMs && GetTickCount64() <= g_pending.untilMs) {
                late = g_pending.want;
                pending = true;
            } else {
                g_pending.untilMs = 0;
            }
        }
        if (pending) {
            unsigned len = 0;
            if (name_is(sound, late.sound, nm, sizeof nm, &len) && !other_take(late.lengthMs, len)) {
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_pending.untilMs = 0;
                }
                BVR_LOG("[sound] the take started late: %s, %u ms long (wanted %s)", nm, len, late.sound);
                cut(*channel, late, nm);
            }
        }
    }
    return r;
}

void install_fmod() {
    static bool s_tried = false;
    if (s_tried) return;
    s_tried = true;
    HMODULE m = GetModuleHandleW(L"fmodex.dll");
    if (!m) {
        BVR_LOG("[sound] fmodex.dll is not loaded: no parts of sounds");
        return;
    }
    void* play = reinterpret_cast<void*>(GetProcAddress(
        m, "?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z"));
    g_chSetPos = reinterpret_cast<ChSetPosFn>(GetProcAddress(m, "?setPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@II@Z"));
    g_chGetPos = reinterpret_cast<ChGetPosFn>(GetProcAddress(m, "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z"));
    g_chStop = reinterpret_cast<ChStopFn>(GetProcAddress(m, "?stop@Channel@FMOD@@QAG?AW4FMOD_RESULT@@XZ"));
    g_sndName = reinterpret_cast<SndNameFn>(GetProcAddress(m, "?getName@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z"));
    g_sndLength = reinterpret_cast<SndLengthFn>(GetProcAddress(m, "?getLength@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z"));
    if (!play || !g_chSetPos || !g_chGetPos || !g_chStop || !g_sndName) {
        BVR_LOG("[sound] fmodex.dll lacks an export (playSound %p setPosition %p getPosition %p stop %p getName %p): no "
                "parts of sounds", play, reinterpret_cast<void*>(g_chSetPos), reinterpret_cast<void*>(g_chGetPos),
                reinterpret_cast<void*>(g_chStop), reinterpret_cast<void*>(g_sndName));
        return;
    }
    MH_STATUS st = MH_CreateHook(play, reinterpret_cast<void*>(&play_detour), reinterpret_cast<void**>(&g_playOrig));
    if (st == MH_OK) st = MH_EnableHook(play);
    if (st != MH_OK) {
        BVR_LOG("[sound] hooking FMOD System::playSound failed: %s - no parts of sounds", MH_StatusToString(st));
        g_playOrig = nullptr;
        return;
    }
    BVR_LOG("[sound] FMOD System::playSound hooked: parts of sounds are on");
}

std::atomic<void*> g_hands{nullptr};
std::atomic<void*> g_weapon{nullptr};
std::atomic<bool> g_muteOn{false};
// The action events the gun's manual reload fires itself - muted when its clips fire them. The list is the gun's
// suppression record's (gun_suppress.h), handed over with the context (2026-10-21; it was a table here).
std::atomic<const wchar_t* const*> g_muted{nullptr};
thread_local bool t_playing = false; // play() is firing its own: let it through

bool muted(const wchar_t* event) {
    const wchar_t* const* list = g_muted.load(std::memory_order_relaxed);
    for (const wchar_t* const* e = list; e && *e; ++e)
        if (!_wcsicmp(*e, event)) return true;
    return false;
}

int __fastcall detour(void* self, void* /*edx*/, int32_t n0, int32_t n1, void* other, void* mat, void* loc, void* rot,
                      int32_t b0, int32_t b1, void* obs, int32_t r0, int32_t r1, void* info) {
    void* hands = g_hands.load(std::memory_order_relaxed);
    void* weapon = g_weapon.load(std::memory_order_relaxed);
    if (self && (self == hands || self == weapon)) {
        const wchar_t* name = patterns::fname_text(n0);
        const bool mute = !t_playing && name && g_muteOn.load(std::memory_order_relaxed) && muted(name);
        // Each event as it fires (the discovery log every gun's table is read from), but never the same one twice
        // within 150 ms.
        static wchar_t s_last[64] = {};
        static uint64_t s_lastMs = 0;
        const uint64_t now = GetTickCount64();
        if (name && (wcscmp(name, s_last) || now - s_lastMs > 150)) {
            wcsncpy_s(s_last, name, _TRUNCATE);
            s_lastMs = now;
            BVR_LOG("[sound] event %ls on the %s%s", name, self == hands ? "hands" : "weapon",
                    mute ? " - MUTED (the gun's manual reload fires it)" : t_playing ? " - played by the sound engine" : "");
        }
        if (mute) return 0;
        const wchar_t* was = t_event;
        t_event = name;
        const int r = g_orig(self, n0, n1, other, mat, loc, rot, b0, b1, obs, r0, r1, info);
        t_event = was;
        bones::on_effect_event(self); // 2026-10-25: a bolt it attached to the head socket goes onto the hand at once
        return r;
    }
    const int r = g_orig(self, n0, n1, other, mat, loc, rot, b0, b1, obs, r0, r1, info);
    bones::on_effect_event(self); // the player's and the second hands' events (bones.h)
    return r;
}

} // namespace

bool install(void* anyActor) {
    if (g_tried || !anyActor) return g_orig != nullptr;
    g_tried = true;
    void** vtbl = nullptr;
    __try {
        vtbl = *static_cast<void***>(anyActor);
        g_target = vtbl[patterns::kActorTriggerEffectEventVtbl / 4];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BVR_LOG("[sound] REFUSING to hook: the actor's vtable did not read");
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto t = reinterpret_cast<uintptr_t>(g_target);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (t < base || t >= base + nt->OptionalHeader.SizeOfImage) {
        BVR_LOG("[sound] REFUSING to hook: vtable +0x%X is %p, outside the game's image", patterns::kActorTriggerEffectEventVtbl,
                g_target);
        return false;
    }
    MH_STATUS st = MH_CreateHook(g_target, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_orig));
    if (st != MH_OK) {
        BVR_LOG("[sound] MH_CreateHook(TriggerEffectEvent) failed: %s", MH_StatusToString(st));
        g_orig = nullptr;
        return false;
    }
    st = MH_EnableHook(g_target);
    if (st != MH_OK) {
        BVR_LOG("[sound] MH_EnableHook(TriggerEffectEvent) failed: %s", MH_StatusToString(st));
        MH_RemoveHook(g_target);
        g_orig = nullptr;
        return false;
    }
    install_fmod();
    const uint8_t* b = static_cast<const uint8_t*>(g_target);
    BVR_LOG("[sound] the gun sound engine is on: AActor::TriggerEffectEvent at RVA 0x%X (head %02X %02X %02X %02X %02X %02X)",
            static_cast<uint32_t>(t - base), b[0], b[1], b[2], b[3], b[4], b[5]);
    return true;
}

void set_context(void* hands, void* weapon, const wchar_t* const* mutedEvents, bool manualReloadOn) {
    g_hands.store(hands, std::memory_order_relaxed);
    g_weapon.store(weapon, std::memory_order_relaxed);
    g_muted.store(mutedEvents, std::memory_order_relaxed);
    g_muteOn.store(manualReloadOn, std::memory_order_relaxed);
}

bool play(void* actor, const char* event, const char* referenceTag) {
    if (!g_orig || !actor || !event) return false;
    const int32_t idx = script::name_index(event);
    if (idx < 0) {
        BVR_LOG("[sound] play %s: no such name", event);
        return false;
    }
    const int32_t ref = referenceTag ? script::name_index(referenceTag) : 0;
    float zero[3] = {0, 0, 0};
    int32_t zrot[3] = {0, 0, 0};
    t_playing = true;
    int r = 0;
    __try {
        r = g_orig(actor, idx, 0, nullptr, nullptr, zero, zrot, 0, 0, nullptr, ref > 0 ? ref : 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    t_playing = false;
    BVR_LOG("[sound] played %s on the %s (%s)", event, actor == g_hands.load(std::memory_order_relaxed) ? "hands" : "weapon",
            r ? "a response ran" : "no response");
    return r != 0;
}

bool play_part(void* actor, const char* event, const char* sound, unsigned fromMs, unsigned toMs, unsigned lengthMs,
               bool first) {
    if (!g_orig || !g_playOrig || !actor || !event || !sound || toMs <= fromMs) return false;
    Want w;
    w.sound = sound;
    w.fromMs = fromMs;
    w.toMs = toMs;
    w.lengthMs = lengthMs;
    w.first = first;
    t_want = &w;
    t_caught = 0;
    const bool r = play(actor, event);
    t_want = nullptr;
    if (t_caught < 0) {
        // The event played something, none of it this sound: the table is wrong for this event - say so; the caller
        // falls back (what it did play goes on whole).
        BVR_LOG("[sound] part of %s: %s does not play it", sound, event);
        return false;
    }
    // Nothing started inside the event (the 20:18 run: it always starts inside): nothing to cut. With every sound
    // unnamed, one started later on another thread could be anyone's - it is left alone.
    if (t_caught == 0) BVR_LOG("[sound] part of %s: %s started no sound inside the call", sound, event);
    return t_caught > 0;
}

void tick() {
    if (!g_chGetPos) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (size_t i = 0; i < g_live.size();) {
        unsigned pos = 0;
        const int r = ch_position(g_live[i].ch, &pos);
        if (r == 0 && pos < g_live[i].toMs) {
            ++i;
            continue;
        }
        // At its end - or ended already (FMOD refuses a stale handle).
        if (r == 0) ch_stop(g_live[i].ch);
        g_live[i] = g_live.back();
        g_live.pop_back();
    }
}

bool ammo_tag(void* weapon, char* out, int n) {
    if (!weapon || !out || n < 2 || !script::properties_ready(weapon)) return false;
    const int32_t off = script::property_offset(weapon, L"CurrentAmmoSelection");
    void* ammo = nullptr;
    int32_t name = -1;
    __try {
        if (off < 0 || !(ammo = *reinterpret_cast<void**>(static_cast<uint8_t*>(weapon) + off))) return false;
        name = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(ammo) + patterns::kUObjectNameIndexOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const wchar_t* w = patterns::fname_text(name);
    if (!w) return false;
    int i = 0;
    for (; w[i] && i < n - 1; ++i) out[i] = static_cast<char>(w[i]);
    out[i] = 0;
    return true;
}

} // namespace bvr::b1r::sound_engine
