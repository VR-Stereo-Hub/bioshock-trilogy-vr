#include "swap_hold.h"
#include "patterns.h"
#include "wheel_state.h"
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace bvr::b1r::swap_hold {
namespace {

using FsCallbackFn = void(__cdecl*)(void* movie, const char* command, const char* args, void* user);

std::atomic<bool> g_enabled{true};
// The game thread only from here down: gameswf runs ActionScript, and so its fscommands,
// inside the call that drives the movie, and every such call is on the game thread.
int g_state = 0;            // 0 not checked, 1 verified, -1 refused (logged once)
FsCallbackFn* g_slot = nullptr;
FsCallbackFn g_original = nullptr;
bool g_replaying = false;

struct Held {
    bool valid = false;
    void* movie = nullptr;
    void* user = nullptr;
    char args[160] = {};
};
Held g_swap, g_select;      // SwitchHandModes; the last SelectPlasmid
unsigned g_heldSelects = 0; // SelectPlasmid commands held this opening (the log's count)

void hold(Held& h, void* movie, const char* args, void* user) {
    h.valid = true;
    h.movie = movie;
    h.user = user;
    strncpy_s(h.args, args ? args : "", _TRUNCATE);
}

void __cdecl hook(void* movie, const char* command, const char* args, void* user) {
    if (g_enabled.load(std::memory_order_relaxed) && !g_replaying && command && wheel_state::open_kind() >= 0) {
        if (!strcmp(command, "RunConsoleCommand") && args && strstr(args, "SwitchHandModes")) {
            hold(g_swap, movie, args, user);
            BVR_LOG("[swaphold] the wheel's hand swap is held until it closes (%s)", args);
            return;
        }
        if (!strcmp(command, "SelectPlasmid")) {
            hold(g_select, movie, args, user);
            ++g_heldSelects;
            return;
        }
    }
    g_original(movie, command, args, user);
}

uint32_t imm_at(const uint8_t* image, uint32_t rva) {
    uint32_t v = 0;
    memcpy(&v, image + rva + 2, 4);
    return v;
}

bool verify(const uint8_t* image) {
    const uint8_t* t = image + patterns::kFsCallbackTestRva;
    const uint8_t* u = image + patterns::kFsCallbackUserRva;
    const uint8_t* c = image + patterns::kFsCallbackCallRva;
    if (!bvr::pattern_scan::is_memory_valid(t, 6) || !bvr::pattern_scan::is_memory_valid(u, 6) ||
        !bvr::pattern_scan::is_memory_valid(c, 6) || t[0] != 0x39 || t[1] != 0x05 || u[0] != 0xFF ||
        u[1] != 0x35 || c[0] != 0xFF || c[1] != 0x15)
        return false;
    const uint32_t slot = imm_at(image, patterns::kFsCallbackTestRva);
    if (imm_at(image, patterns::kFsCallbackCallRva) != slot || imm_at(image, patterns::kFsCallbackUserRva) != slot - 4)
        return false;
    if (slot - reinterpret_cast<uintptr_t>(image) != patterns::kFsCallbackSlotRva) return false;
    if (!bvr::pattern_scan::is_memory_valid(reinterpret_cast<const void*>(static_cast<uintptr_t>(slot)), 4))
        return false;
    g_slot = reinterpret_cast<FsCallbackFn*>(static_cast<uintptr_t>(slot));
    return true;
}

} // namespace

void poll(const uint8_t* image) {
    if (g_state < 0 || !image) return;
    if (g_state == 0) {
        if (!verify(image)) {
            g_state = -1;
            BVR_LOG("[swaphold] gameswf's fscommand callback is not where this build keeps it (RVA 0x%X) - the "
                    "hand swap is never held", patterns::kFsCallbackSlotRva);
            return;
        }
        g_state = 1;
    }
    FsCallbackFn cur = *g_slot;
    if (!cur || cur == &hook) return; // not registered yet, or ours
    g_original = cur;
    *g_slot = &hook;
    BVR_LOG("[swaphold] fscommands pass through the mod (engine callback %p; F10 hold %s)",
            reinterpret_cast<void*>(cur), g_enabled.load() ? "on" : "off");
}

void on_release() {
    if (!g_swap.valid && !g_select.valid) return;
    if (!g_original) { clear(); return; }
    g_replaying = true;
    // The game's own order: the swap the opening asked for, then the last plasmid picked.
    if (g_swap.valid) g_original(g_swap.movie, "RunConsoleCommand", g_swap.args, g_swap.user);
    if (g_select.valid) g_original(g_select.movie, "SelectPlasmid", g_select.args, g_select.user);
    g_replaying = false;
    BVR_LOG("[swaphold] the wheel closed: sent %s%s%s (%u plasmid pick(s) held, the last one sent)",
            g_swap.valid ? "the hand swap" : "", g_swap.valid && g_select.valid ? " then " : "",
            g_select.valid ? g_select.args : "", g_heldSelects);
    clear();
}

void clear() {
    g_swap = Held{};
    g_select = Held{};
    g_heldSelects = 0;
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool on) {
    if (g_enabled.exchange(on, std::memory_order_relaxed) != on)
        BVR_LOG("[swaphold] hold the hand swap until the wheel closes: %s", on ? "ON" : "off");
}

} // namespace bvr::b1r::swap_hold
