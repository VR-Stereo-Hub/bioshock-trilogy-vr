#include "game/bioshock1r/input_drive.h"

#include "core/hooks/d3d11_hook.h"
#include "core/hooks/pattern_scan.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>

namespace bvr::b1r::input_drive {
namespace {

// Dummy-EDX __fastcall stands in for thiscall (house pattern, camera.cpp).
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
// video re-init. Vtable identity (rebased RVA) is the safety check.
bool resolve(Objects& out) {
    out.base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!out.base) return false;

    auto** enginePtr = reinterpret_cast<void**>(out.base + patterns::kGameEnginePtrRva);
    if (!valid_ptr(enginePtr) || !valid_ptr(*enginePtr)) return false;
    auto* engine = static_cast<uint8_t*>(*enginePtr);

    auto** clientSlot = reinterpret_cast<void**>(engine + patterns::kEngineClientOffset);
    if (!valid_ptr(clientSlot) || !valid_ptr(*clientSlot)) return false;
    auto* client = static_cast<uint8_t*>(*clientSlot);
    if (*reinterpret_cast<void**>(client) != out.base + patterns::kClientVtableRva)
        return false;

    auto** vpData = reinterpret_cast<void**>(client + patterns::kClientViewportsDataOffset);
    auto* vpCount = reinterpret_cast<int32_t*>(client + patterns::kClientViewportsCountOffset);
    if (!valid_ptr(vpData) || !valid_ptr(*vpData) || *vpCount < 1) return false;
    auto** vpArray = static_cast<void**>(*vpData);
    if (!valid_ptr(vpArray[0])) return false;
    auto* viewport = static_cast<uint8_t*>(vpArray[0]);
    if (*reinterpret_cast<void**>(viewport) != out.base + patterns::kViewportVtableRva)
        return false;

    out.client = client;
    out.viewport = viewport;
    return true;
}

// SEH-isolated virtual calls (no C++ objects with destructors in this frame).
int seh_set_use_controller(void* client, int enable) {
    __try {
        auto fn = *reinterpret_cast<SetUseControllerFn*>(
            *reinterpret_cast<uint8_t**>(client) + patterns::kVtblSlot70Offset);
        fn(client, nullptr, enable);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int seh_update_input(void* viewport, float dt) {
    __try {
        auto fn = *reinterpret_cast<UpdateInputFn*>(
            *reinterpret_cast<uint8_t**>(viewport) + patterns::kVtblSlot70Offset);
        fn(viewport, nullptr, 0, dt);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

void poison(const char* what) {
    g_poisoned.store(true, std::memory_order_relaxed);
    BVR_LOG("[b1r] input drive: FAULT in %s - drive poisoned (vrinput off/on "
            "after a relaunch to retry)", what);
}

} // namespace

void on_frame(uint64_t nowMs) {
    bool wantOn = bvr::input::enabled();
    if (g_poisoned.load(std::memory_order_relaxed)) return;

    if (!wantOn) {
        if (g_armed) {
            Objects obj;
            if (resolve(obj)) {
                if (seh_set_use_controller(obj.client, FALSE))
                    poison("SetUseController(FALSE)");
                else
                    BVR_LOG("[b1r] input drive: disarmed (UseController off)");
            }
            g_armed = false;
        }
        return;
    }

    Objects obj;
    if (!resolve(obj)) {
        if (!g_loggedResolveFail) {
            BVR_LOG("[b1r] input drive: engine objects not resolvable yet - "
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
        BVR_LOG("[b1r] input drive: armed - UseController on, driving "
                "UpdateInput per present (client=%p viewport=%p)",
                obj.client, obj.viewport);
    }

    // Once per present: CalcView can fire several times per frame (menu
    // scenes reach several calls per present) but the stock engine ticks
    // input once per frame.
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
        BVR_LOG("[b1r] input drive: first UpdateInput call OK (dt %.3f)", dt);

    if (nowMs - g_rateWindowMs >= 1000) {
        uint32_t rate = 0;
        if (g_rateWindowMs)
            rate = static_cast<uint32_t>((count - g_rateWindowBase) * 1000ull /
                                         (nowMs - g_rateWindowMs));
        g_driveRate.store(rate, std::memory_order_relaxed);
        if (g_rateWindowMs)
            BVR_LOG("[b1r] input drive: %u/s (total %u)", rate, count);
        g_rateWindowMs = nowMs;
        g_rateWindowBase = count;
    }
}

void draw_debug_ui() {
    ImGui::Text("pad drive: %s | drives %u (%u/s)",
                g_poisoned.load(std::memory_order_relaxed) ? "POISONED"
                : bvr::input::enabled()                    ? "on"
                                                           : "off",
                g_driveCount.load(std::memory_order_relaxed),
                g_driveRate.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::input_drive
