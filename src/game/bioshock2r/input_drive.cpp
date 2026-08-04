#include "game/bioshock2r/input_drive.h"

#include "core/gfx/hud_capture.h" // session 42: menukey gate (screen_only)
#include "core/hooks/d3d11_hook.h"
#include "core/hooks/pattern_scan.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock2r/camera.h" // session 42: menukey gate (view signals)
#include "game/bioshock2r/patterns.h"

#include <windows.h>
#include <xinput.h>

#include <imgui.h>

#include <atomic>
#include <cstring>

namespace bvr::b2r::input_drive {
namespace {

// Dummy-EDX __fastcall stands in for thiscall (house pattern, camera.cpp).
// Arg counts verified against the bodies' retn immediates offline (retn 4 /
// retn 8 - ENGINE_NOTES "Session 40"); a mismatch here is the ESP RTC trap.
using SetUseControllerFn = void(__fastcall*)(void* self, void* edx, int enable);
using UpdateInputFn = void(__fastcall*)(void* self, void* edx, int reset, float dt);

// Game-thread-only state (the overlay reads the atomics only).
bool g_armed = false;               // SetUseController(TRUE) has been applied
uint64_t g_lastDriveMs = 0;
uint32_t g_lastPresent = 0;
std::atomic<bool> g_poisoned{false};    // SEH fault latch - stops all driving
std::atomic<uint32_t> g_driveCount{0};
std::atomic<uint32_t> g_driveRate{0};   // drives/s, 1 Hz sample
uint64_t g_rateWindowMs = 0;
uint32_t g_rateWindowBase = 0;
bool g_loggedResolveFail = false;

struct Objects {
    uint8_t* base = nullptr;
    void* client = nullptr;
    void* viewport = nullptr;
};

bool valid_ptr(const void* p) {
    return p && bvr::pattern_scan::is_memory_valid(p, sizeof(void*));
}

// Re-resolved every frame: the client/viewport are heap objects that die on
// video re-init. Vtable identity (rebased RVA) is the safety check - here on
// all THREE hops (BS1 checked client/viewport only; the engine check is free
// because the vtable RVA is already banked for the Draw hook).
bool resolve(Objects& out) {
    out.base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!out.base) return false;

    auto** enginePtr = reinterpret_cast<void**>(out.base + patterns::kGameEnginePtrRva);
    if (!valid_ptr(enginePtr) || !valid_ptr(*enginePtr)) return false;
    auto* engine = static_cast<uint8_t*>(*enginePtr);
    if (*reinterpret_cast<void**>(engine) != out.base + patterns::kGameEngineVtableRva)
        return false;

    auto** clientSlot = reinterpret_cast<void**>(engine + patterns::kEngineClientOffset);
    if (!valid_ptr(clientSlot) || !valid_ptr(*clientSlot)) return false;
    auto* client = static_cast<uint8_t*>(*clientSlot);
    if (*reinterpret_cast<void**>(client) != out.base + patterns::kWindowsClientVtableRva)
        return false;

    auto** vpData = reinterpret_cast<void**>(client + patterns::kClientViewportsDataOffset);
    auto* vpCount = reinterpret_cast<int32_t*>(client + patterns::kClientViewportsCountOffset);
    if (!valid_ptr(vpData) || !valid_ptr(*vpData) || *vpCount < 1) return false;
    auto** vpArray = static_cast<void**>(*vpData);
    if (!valid_ptr(vpArray[0])) return false;
    auto* viewport = static_cast<uint8_t*>(vpArray[0]);
    if (*reinterpret_cast<void**>(viewport) !=
        out.base + patterns::kWindowsViewportVtableRva)
        return false;

    out.client = client;
    out.viewport = viewport;
    return true;
}

// SEH-isolated virtual calls (no C++ objects with destructors in this frame).
int seh_set_use_controller(void* client, int enable) {
    __try {
        auto fn = *reinterpret_cast<SetUseControllerFn*>(
            *reinterpret_cast<uint8_t**>(client) +
            patterns::kClientSetUseControllerVtblOffset);
        fn(client, nullptr, enable);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int seh_update_input(void* viewport, float dt) {
    __try {
        auto fn = *reinterpret_cast<UpdateInputFn*>(
            *reinterpret_cast<uint8_t**>(viewport) +
            patterns::kViewportUpdateInputVtblOffset);
        fn(viewport, nullptr, 0, dt);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

void poison(const char* what) {
    g_poisoned.store(true, std::memory_order_relaxed);
    BVR_LOG("[b2r] input drive: FAULT in %s - drive poisoned (vrinput off/on "
            "after a relaunch to retry)", what);
}

// ---- Session 42: menukey (pad A -> keyboard Enter in menu contexts) ---------
// BS2's gameswf front-end activates on KEYBOARD input only: the composed A bit
// provably reaches the game (the same word's dpad navigates the menu, the ini
// pad map binds A=Use), but no menu item activates on it - while a SCANCODE
// Enter does (harness-proven; VK-only injection is ignored, tools/game-key.ps1
// header). So on the A rising edge, while a menu context holds, press Enter
// via keybd_event and mirror the release on the A falling edge (gameswf polls
// per frame; the press needs real duration). keybd_event over SendInput for
// the same x86 INPUT-layout reason as game-key.ps1. keybd_event is GLOBAL, so
// the gate also requires the game window foreground.
std::atomic<bool> g_menuKey{true};
std::atomic<bool> g_menuKeyForce{false};
std::atomic<uint32_t> g_menuKeyInjects{0};
bool g_prevA = false; // game thread only, like the drive state above
bool g_enterDown = false;
uint64_t g_enterDownMs = 0;

// Whole-token match (the vrhands offset/off lesson - never prefix-match verbs).
bool is_verb(const char* args, const char* verb) {
    size_t n = strlen(verb);
    if (strncmp(args, verb, n) != 0) return false;
    char t = args[n];
    return t == '\0' || t == ' ' || t == '\n' || t == '\r' || t == '\t';
}

bool game_foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void enter_key(bool down) {
    keybd_event(0, 0x1C /* Enter, scancode set 1 */,
                KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP), 0);
}

void enter_release(const char* why) {
    if (!g_enterDown) return;
    enter_key(false);
    g_enterDown = false;
    BVR_LOG("[b2r] menukey: Enter up (%s)", why);
}

void menu_key_tick(uint64_t nowMs) {
    uint16_t btns = 0;
    bvr::input::last_composed_buttons(&btns);
    bool a = (btns & XINPUT_GAMEPAD_A) != 0;
    bool rising = a && !g_prevA;
    bool falling = !a && g_prevA;
    g_prevA = a; // edges tracked even while off, so re-enabling can't replay one
    if (!g_menuKey.load(std::memory_order_relaxed)) {
        enter_release("menukey off");
        return;
    }
    if (g_enterDown) {
        // Mirror the pad release; the 1 s cap means a held A (gameplay charge,
        // a dropped falling edge) can never leave Enter stuck down.
        if (falling)
            enter_release("A released");
        else if (nowMs - g_enterDownMs > 1000)
            enter_release("held > 1s");
        return;
    }
    if (!rising) return;
    bool force = g_menuKeyForce.load(std::memory_order_relaxed);
    bool silent = camera::calcview_silent(400);
    bool strict = camera::last_strict_gameplay();
    bool screen = bvr::hud::screen_only();
    // Menu context = any leg. In gameplay all three read "gameplay" and the A
    // press stays what it is (use/loot) - no injection.
    if (!force && !silent && strict && !screen) return;
    if (!game_foreground()) return;
    enter_key(true);
    g_enterDown = true;
    g_enterDownMs = nowMs;
    g_menuKeyInjects.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b2r] menukey: A->Enter down (silent=%d strict=%d screen=%d force=%d)",
            silent ? 1 : 0, strict ? 1 : 0, screen ? 1 : 0, force ? 1 : 0);
}

} // namespace

void on_frame(uint64_t nowMs) {
    bool wantOn = bvr::input::enabled();
    if (g_poisoned.load(std::memory_order_relaxed)) {
        enter_release("drive poisoned");
        return;
    }

    if (!wantOn) {
        enter_release("drive off"); // menukey is inert without the pump
        if (g_armed) {
            Objects obj;
            if (resolve(obj)) {
                if (seh_set_use_controller(obj.client, FALSE))
                    poison("SetUseController(FALSE)");
                else
                    BVR_LOG("[b2r] input drive: disarmed (UseController off)");
            }
            g_armed = false;
        }
        return;
    }

    Objects obj;
    if (!resolve(obj)) {
        if (!g_loggedResolveFail) {
            BVR_LOG("[b2r] input drive: engine objects not resolvable yet - "
                    "will keep trying quietly");
            g_loggedResolveFail = true;
        }
        return;
    }
    g_loggedResolveFail = false;

    if (!g_armed) {
        // Last-hop injection: the Steam overlay eats calls routed through the
        // proxy export, so composed state must enter via the game's own IAT.
        bvr::input::hijack_import_slot(reinterpret_cast<void**>(
            obj.base + patterns::kXInputGetStateIatRva));
        if (seh_set_use_controller(obj.client, TRUE)) {
            poison("SetUseController(TRUE)");
            return;
        }
        g_armed = true;
        BVR_LOG("[b2r] input drive: armed - UseController on, driving "
                "UpdateInput per present (client=%p viewport=%p)",
                obj.client, obj.viewport);
    }

    // Once per present: the ProcessEvent lane fires thousands of times per
    // frame, but the stock engine ticks input once per frame. The stamp
    // lands BEFORE the call so UpdateInput's own event dispatches (which
    // re-enter the ProcessEvent detour) can never recurse the pump.
    uint32_t present = bvr::d3d11_hook::present_count();
    if (present == g_lastPresent) return;
    g_lastPresent = present;

    float dt = 0.016f;
    if (g_lastDriveMs && nowMs > g_lastDriveMs)
        dt = static_cast<float>(nowMs - g_lastDriveMs) / 1000.0f;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.2f) dt = 0.2f;
    g_lastDriveMs = nowMs;

    if (seh_update_input(obj.viewport, dt)) {
        poison("UpdateInput");
        return;
    }
    uint32_t count = g_driveCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1)
        BVR_LOG("[b2r] input drive: first UpdateInput call OK (dt %.3f)", dt);

    if (nowMs - g_rateWindowMs >= 1000) {
        uint32_t rate = 0;
        if (g_rateWindowMs)
            rate = static_cast<uint32_t>((count - g_rateWindowBase) * 1000ull /
                                         (nowMs - g_rateWindowMs));
        g_driveRate.store(rate, std::memory_order_relaxed);
        if (g_rateWindowMs)
            BVR_LOG("[b2r] input drive: %u/s (total %u)", rate, count);
        g_rateWindowMs = nowMs;
        g_rateWindowBase = count;
    }

    // Session 42: A->Enter translation, once per present, on the fresh pad
    // word the pump just produced.
    menu_key_tick(nowMs);
}

void handle_menukey_command(const char* args) {
    if (is_verb(args, "on")) {
        g_menuKey.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] menukey ON (pad A activates menu items via scancode Enter)");
    } else if (is_verb(args, "off")) {
        g_menuKey.store(false, std::memory_order_relaxed);
        BVR_LOG("[b2r] menukey off");
    } else if (strncmp(args, "force on", 8) == 0) {
        g_menuKeyForce.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] menukey FORCE (gate open everywhere - diagnostic only; "
                "gameplay A will also type Enter)");
    } else if (strncmp(args, "force off", 9) == 0) {
        g_menuKeyForce.store(false, std::memory_order_relaxed);
        BVR_LOG("[b2r] menukey force off");
    } else {
        BVR_LOG("[b2r] menukey status: %s force=%d injects=%u | gate now: "
                "silent=%d strict=%d screen=%d foreground=%d "
                "(menukey on|off|force on|force off|status)",
                g_menuKey.load(std::memory_order_relaxed) ? "ON" : "off",
                g_menuKeyForce.load(std::memory_order_relaxed) ? 1 : 0,
                g_menuKeyInjects.load(std::memory_order_relaxed),
                camera::calcview_silent(400) ? 1 : 0,
                camera::last_strict_gameplay() ? 1 : 0,
                bvr::hud::screen_only() ? 1 : 0, game_foreground() ? 1 : 0);
    }
}

bool resolve_engine_objects(void** client, void** viewport) {
    Objects obj;
    if (!resolve(obj)) return false;
    *client = obj.client;
    *viewport = obj.viewport;
    return true;
}

void draw_debug_ui() {
    ImGui::Text("pad drive: %s | drives %u (%u/s) | menu-A %s (%u)",
                g_poisoned.load(std::memory_order_relaxed) ? "POISONED"
                : bvr::input::enabled()                    ? "on"
                                                           : "off",
                g_driveCount.load(std::memory_order_relaxed),
                g_driveRate.load(std::memory_order_relaxed),
                g_menuKey.load(std::memory_order_relaxed) ? "on" : "off",
                g_menuKeyInjects.load(std::memory_order_relaxed));
}

} // namespace bvr::b2r::input_drive
