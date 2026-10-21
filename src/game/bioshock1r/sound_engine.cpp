#include "game/bioshock1r/sound_engine.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
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

std::atomic<void*> g_hands{nullptr};
std::atomic<void*> g_weapon{nullptr};
std::atomic<bool> g_muteOn{false};
wchar_t g_cls[64] = {};
thread_local bool t_playing = false; // play() is firing its own: let it through

// The action events each gun's manual reload fires itself (sound_engine::play) - muted when its clips fire them.
struct Muted {
    const wchar_t* cls;
    const wchar_t* event;
};
// Shotgun: its pump (fired by every shot's Fire clip as it plays past its pump cue, before the player pumps) and its
// reload's end (the end clip's pump). The engine fires PumpShotgun as the player begins each pump.
const Muted kMuted[] = {
    {L"Shotgun", L"PumpShotgun"},
    {L"Shotgun", L"ShotgunReloadEnd"},
};

bool muted(const wchar_t* event) {
    for (const Muted& m : kMuted)
        if (!_wcsicmp(m.cls, g_cls) && !_wcsicmp(m.event, event)) return true;
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
    }
    return g_orig(self, n0, n1, other, mat, loc, rot, b0, b1, obs, r0, r1, info);
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
    const uint8_t* b = static_cast<const uint8_t*>(g_target);
    BVR_LOG("[sound] the gun sound engine is on: AActor::TriggerEffectEvent at RVA 0x%X (head %02X %02X %02X %02X %02X %02X)",
            static_cast<uint32_t>(t - base), b[0], b[1], b[2], b[3], b[4], b[5]);
    return true;
}

void set_context(void* hands, void* weapon, const wchar_t* weaponClass, bool manualReloadOn) {
    g_hands.store(hands, std::memory_order_relaxed);
    g_weapon.store(weapon, std::memory_order_relaxed);
    wcsncpy_s(g_cls, weaponClass ? weaponClass : L"", _TRUNCATE);
    g_muteOn.store(manualReloadOn, std::memory_order_relaxed);
}

bool play(void* actor, const char* event) {
    if (!g_orig || !actor || !event) return false;
    const int32_t idx = script::name_index(event);
    if (idx < 0) {
        BVR_LOG("[sound] play %s: no such name", event);
        return false;
    }
    float zero[3] = {0, 0, 0};
    int32_t zrot[3] = {0, 0, 0};
    t_playing = true;
    int r = 0;
    __try {
        r = g_orig(actor, idx, 0, nullptr, nullptr, zero, zrot, 0, 0, nullptr, 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    t_playing = false;
    BVR_LOG("[sound] played %s on the %s (%s)", event, actor == g_hands.load(std::memory_order_relaxed) ? "hands" : "weapon",
            r ? "a response ran" : "no response");
    return r != 0;
}

} // namespace bvr::b1r::sound_engine
