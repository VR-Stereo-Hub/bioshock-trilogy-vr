#include "menu_blur.h"
#include "patterns.h"
#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace bvr::b1r::menu_blur {
namespace {

// void __thiscall Run(this): a __fastcall detour takes `this` in ecx and ignores edx.
using RunFn = void(__fastcall*)(void* self, void* edx);
RunFn g_original = nullptr;
std::atomic<bool> g_installed{false}, g_suppressed{false};
std::atomic<unsigned> g_skipped{0}; // runs skipped since the gate opened (render thread)

void __fastcall detour(void* self, void* edx) {
    // One check per run: the runner is called once per view, not per draw.
    if (g_suppressed.load(std::memory_order_relaxed)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_original(self, edx);
}

} // namespace

bool install(const bvr::pattern_scan::ProcessImage& image) {
    uint8_t* target = const_cast<uint8_t*>(image.base) + patterns::kPostEffectRunRva;
    const uint8_t* tail = target + patterns::kPostEffectRunTailAt;
    if (!bvr::pattern_scan::is_memory_valid(target, patterns::kPostEffectRunTailAt + sizeof patterns::kPostEffectRunTail) ||
        memcmp(target, patterns::kPostEffectRunHead, sizeof patterns::kPostEffectRunHead) != 0 ||
        memcmp(tail, patterns::kPostEffectRunTail, sizeof patterns::kPostEffectRunTail) != 0) {
        BVR_LOG("[menublur] the post-effect runner's prologue does not match at RVA 0x%X - the menu blur stays",
                patterns::kPostEffectRunRva);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_original));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        BVR_LOG("[menublur] hooking the post-effect runner failed: %s - the menu blur stays", MH_StatusToString(st));
        return false;
    }
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[menublur] post-effect runner hooked at %p (RVA 0x%X; skipped only while a menu or wheel is up)",
            target, patterns::kPostEffectRunRva);
    return true;
}

void set_suppressed(bool on) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    if (g_suppressed.exchange(on, std::memory_order_relaxed) == on) return;
    if (on) {
        g_skipped.store(0, std::memory_order_relaxed);
        return;
    }
    BVR_LOG("[menublur] menu closed: the blur pass was skipped %u time(s) while it was up",
            g_skipped.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::menu_blur
