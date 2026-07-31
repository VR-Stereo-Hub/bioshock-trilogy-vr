#include "game/bioshockinf/camera.h"

#include "core/framework/command.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::bsi::camera {
namespace {

// UE3 PODs. Deliberately declared HERE and not taken from game/shared/ue_math.h:
// that header states it is the Vengeance/UE2.5 family's convention, and this
// game's patterns.h states that on UE3 even shapes are suspect. Same layout by
// coincidence is not the same layout by contract.
struct FVector {
    float x, y, z;
};
// FRotator is 3x int32 in UE rotator units - 65536 units to a full turn, NOT
// degrees and NOT floats. Reinterpreting one as a float gives a denormal, which
// prints as 0.000 and cost BioShock 1 a long detour. Always %d, plus an
// explicit conversion when a human needs to read it.
struct FRotator {
    int32_t pitch, yaw, roll;
};

constexpr double kRotToDeg = 360.0 / 65536.0;

// APlayerController::GetPlayerViewPoint is __thiscall. __fastcall with a dummy
// EDX slot is register-, stack- and cleanup-identical, and works as a plain
// free function.
//
// EXACTLY TWO STACK ARGS. The target is `ret 8`, and `ret imm / 4 == 2` is a
// hard requirement: a mismatch returns on a misaligned stack and pops a
// "Run-Time Check Failure #0 - ESP was not properly saved" dialog which writes
// NO crash dump (RTC is a Debug compiler check, not an SEH fault). ONE typedef
// serves both the trampoline pointer and the detour so the two cannot disagree.
using GetViewPointFn = void(__fastcall*)(void* self, void* edx, FVector* loc, FRotator* rot);

GetViewPointFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_fired{false};
std::atomic<bool> g_loggedFirstFire{false};
std::atomic<uint32_t> g_callCount{0};
std::atomic<uint64_t> g_lastCallMs{0};

// Heartbeat state. Game thread only - it is only ever touched inside the
// throttled block, which the tid latch confines to one thread.
std::atomic<bool> g_heartbeat{true};
int g_beatsLeft = 10; // self-expiring burst; `bsicam heartbeat on` re-arms it
uint64_t g_lastBeatMs = 0;
uint32_t g_beatBaseCount = 0;

// Thread identity. The camera hook installs at ~T+0.4 s and the first Present
// lands at ~T+8.5 s, so the FIRST detour fire can precede any Present and
// d3d11_hook::last_present_tid() may still be 0. The comparison therefore lives
// on the throttled path, not in the first-fire line.
std::atomic<uint32_t> g_cameraTid{0};
std::atomic<uint32_t> g_foreignTidCalls{0};
std::atomic<bool> g_loggedThreadSplit{false};

// Path census. GetPlayerViewPoint has four internal paths and only the first is
// a cheap cached read; which one runs tells us whether +0x248 and +0x240 mean
// what session 34 inferred from shape.
std::atomic<uint32_t> g_pathCached{0};   // [this+0x248] bit 0 set
std::atomic<uint32_t> g_pathCamera{0};   // clear, and [this+0x240] non-null
std::atomic<uint32_t> g_pathTarget{0};   // clear, and [this+0x240] null
std::atomic<uint32_t> g_pathUnknown{0};  // `this` unreadable
std::atomic<bool> g_loggedMatrix{false};
std::atomic<void*> g_lastSelf{nullptr};

// Silence detection. The game-thread pump rides on this hook, so a camera that
// stops is a command surface that stops - and that must be a timestamped fact
// in the log, not a mystery. Detected on RESUME by comparing against the
// previous call's timestamp, which needs no extra state and no second thread:
// the only observer that can be sure a gap ended is the call that ends it.
constexpr uint64_t kSilenceReportMs = 2000;

struct Snapshot {
    FVector loc{};
    FRotator rot{};
    FVector cachedLoc{};  // [this+0x24C], the pre-transform cached POV
    bool cachedRead = false;
    bool valid = false;
};
Snapshot g_last;

// ---------------------------------------------------------------------------
// The 1 Hz probe. `this` is an engine pointer we did not create, so every read
// is SEH-guarded and the guarded body is POD-only: no logging and no allocation
// inside the guard, because MSVC under /EHsc does not run destructors during
// SEH unwinding and a fault taken while the log mutex was held would leave it
// held for the life of the process (BioShock 1 learned that expensively).
//
// This runs at 1 Hz and never per call: is_memory_valid is a VirtualQuery, and
// this detour can run 4000+ times a second.
// ---------------------------------------------------------------------------
struct Probe {
    bool ok = false;
    bool cachedFlag = false;
    bool cameraNonNull = false;
    FVector cachedLoc{};
    float matrix[16] = {};
    bool matrixOk = false;
};

Probe probe_self(void* self) {
    Probe p{};
    if (!self) return p;
    __try {
        const uint8_t* base = static_cast<const uint8_t*>(self);
        p.cachedFlag = (*reinterpret_cast<const uint8_t*>(base + 0x248) & 1u) != 0;
        p.cameraNonNull = *reinterpret_cast<void* const*>(base + 0x240) != nullptr;
        memcpy(&p.cachedLoc, base + 0x24C, sizeof p.cachedLoc);
        memcpy(p.matrix, base + 0x430, sizeof p.matrix);
        p.matrixOk = true;
        p.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.ok = false;
    }
    return p;
}

void log_matrix_once(const Probe& p) {
    if (!p.matrixOk) return;
    if (g_loggedMatrix.exchange(true)) return;
    // All four internal paths converge on a 4x4 SSE transform fed from
    // [this+0x430]. Whether the returned view is TRANSFORMED or merely copied
    // decides where an I4 HMD pose has to be injected, so it is measured here
    // rather than assumed.
    char line[320];
    int n = 0;
    line[0] = '\0';
    for (int i = 0; i < 16 && n >= 0 && static_cast<size_t>(n) + 12 < sizeof line; ++i)
        n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "%.4f%s", p.matrix[i],
                         (i % 4 == 3) ? " | " : " ");
    BVR_LOG("[bsi] camera: [this+0x430] 4x4 = %s", line);
}

void throttled(void* self, uint64_t now) {
    const uint32_t count = g_callCount.load(std::memory_order_relaxed);

    const Probe p = probe_self(self);
    if (!p.ok) {
        g_pathUnknown.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cachedFlag) {
        g_pathCached.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cameraNonNull) {
        g_pathCamera.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Paths 3 and 4 are ONE honest bucket. Separating them means calling
        // vtable slot +0x2C0 (GetViewTarget), and a virtual call out of a
        // detour can lazily create engine objects - the guard would be worse
        // than the thing it measures.
        g_pathTarget.fetch_add(1, std::memory_order_relaxed);
    }
    if (p.ok) {
        g_last.cachedLoc = p.cachedLoc;
        g_last.cachedRead = true;
        log_matrix_once(p);
    }

    // Thread split, once. Feeds DR-I5: if UE3's game thread and render thread
    // are the same here, the whole substrate question changes shape.
    const uint32_t presentTid = bvr::d3d11_hook::last_present_tid();
    if (presentTid != 0 && !g_loggedThreadSplit.exchange(true)) {
        const uint32_t camTid = g_cameraTid.load(std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: thread split - camera tid %u, present tid %u -> %s", camTid,
                presentTid,
                camTid == presentTid ? "SAME THREAD (game and render are one)"
                                     : "separate game and render threads");
    }

    if (!g_heartbeat.load(std::memory_order_relaxed) || g_beatsLeft <= 0) {
        g_lastBeatMs = 0;
        return;
    }
    if (g_lastBeatMs == 0) {
        g_lastBeatMs = now;
        g_beatBaseCount = count;
        return;
    }
    if (now - g_lastBeatMs < 1000) return;

    const uint32_t perSec =
        static_cast<uint32_t>((count - g_beatBaseCount) * 1000ull / (now - g_lastBeatMs));
    // Rotator components as %d ALWAYS, with degrees alongside. Never %f.
    BVR_LOG("[bsi] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) = (%.1f %.1f %.1f)deg "
            "(%u calls/s, %u total)",
            g_last.loc.x, g_last.loc.y, g_last.loc.z, g_last.rot.pitch, g_last.rot.yaw,
            g_last.rot.roll, g_last.rot.pitch * kRotToDeg, g_last.rot.yaw * kRotToDeg,
            g_last.rot.roll * kRotToDeg, perSec, count);
    if (g_last.cachedRead) {
        // The delta between the cached POV at +0x24C and the value actually
        // handed back. Non-zero means the documented 4x4 transform really is
        // applied on the way out - measured, not assumed.
        BVR_LOG("[bsi] camera: returned-minus-cached d=(%.3f %.3f %.3f)%s",
                g_last.loc.x - g_last.cachedLoc.x, g_last.loc.y - g_last.cachedLoc.y,
                g_last.loc.z - g_last.cachedLoc.z,
                (g_last.loc.x == g_last.cachedLoc.x && g_last.loc.y == g_last.cachedLoc.y &&
                 g_last.loc.z == g_last.cachedLoc.z)
                    ? "  <- identical, this path is a raw copy"
                    : "  <- TRANSFORMED");
    }
    g_lastBeatMs = now;
    g_beatBaseCount = count;
    --g_beatsLeft;
}

// ---------------------------------------------------------------------------
// The detour. READ-ONLY BY CONSTRUCTION: the out-params are copied into const
// locals and never written. There is no assignment through loc or rot anywhere
// in this translation unit.
// ---------------------------------------------------------------------------
void __fastcall GetViewPointDetour(void* self, void* edx, FVector* loc, FRotator* rot) {
    // Original FIRST. It writes both out-params on all four internal paths, so
    // after this call their writability is proven rather than assumed.
    g_original(self, edx, loc, rot);

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    const uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t now = GetTickCount64();
    const uint64_t prevCallMs = g_lastCallMs.exchange(now, std::memory_order_relaxed);
    g_fired.store(true, std::memory_order_relaxed);

    const uint32_t tid = GetCurrentThreadId();
    uint32_t expectedTid = 0;
    if (!g_cameraTid.compare_exchange_strong(expectedTid, tid, std::memory_order_relaxed) &&
        expectedTid != tid) {
        // A second thread dispatching this is DR-I5 evidence, not a bug - but
        // the throttled block is not thread-safe, so foreign threads are
        // counted and then leave.
        g_foreignTidCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Snapshot for the heartbeat. const locals: nothing here can write back.
    const FVector outLoc = loc ? *loc : FVector{};
    const FRotator outRot = rot ? *rot : FRotator{};
    g_last.loc = outLoc;
    g_last.rot = outRot;
    g_last.valid = true;
    g_lastSelf.store(self, std::memory_order_relaxed);

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[bsi] camera: FIRST FIRE - GetPlayerViewPoint detour live. this=%p tid=%u "
                "loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                self, tid, outLoc.x, outLoc.y, outLoc.z, outRot.pitch, outRot.yaw, outRot.roll);
    }

    // THE HANDOVER. This is the whole reason the command seam was built to be
    // pump-agnostic: from here commands run on the GAME thread, where anything
    // that touches the engine belongs. Safe to call every frame - it is 1 Hz
    // internally - and it latches on the first call, so the takeover happens
    // the moment the hook is proven live rather than on a timer.
    bvr::command::poll_from_game_thread(now);

    // A gap that just ended. During it the game-thread command pump was silent
    // too, so this line is what turns "the mod stopped responding" into a
    // timestamped fact. Level loads and cinematics are the expected causes.
    if (prevCallMs != 0 && now - prevCallMs >= kSilenceReportMs) {
        BVR_LOG("[bsi] camera: RESUMED after %llu ms silent - the game-thread command pump was "
                "silent with it for that whole window",
                static_cast<unsigned long long>(now - prevCallMs));
        g_lastBeatMs = 0; // reseed the base rather than report a fake calls/s spike
    }

    static uint64_t s_lastThrottle = 0;
    if (now - s_lastThrottle >= 1000) {
        s_lastThrottle = now;
        throttled(self, now);
    }
}

const char* path_summary(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "cached=%u camera=%u viewtarget-or-self=%u unreadable=%u",
                g_pathCached.load(), g_pathCamera.load(), g_pathTarget.load(),
                g_pathUnknown.load());
    return buf;
}

void log_status() {
    char paths[160];
    BVR_LOG("[bsi] camera: hook=%s enabled=%s fired=%s calls=%u silent=%llu ms tid=%u "
            "foreign-tid-calls=%u",
            g_hookLive.load() ? "installed" : "NOT installed",
            g_enabled.load() ? "yes" : "no", g_fired.load() ? "YES" : "no",
            g_callCount.load(), static_cast<unsigned long long>(silent_ms()),
            g_cameraTid.load(), g_foreignTidCalls.load());
    BVR_LOG("[bsi] camera: paths %s", path_summary(paths, sizeof paths));
}

} // namespace

bool install(const bvr::pattern_scan::ProcessImage& image) {
    (void)image;
    if (g_hookLive.load()) return true;

    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] camera: hook NOT installed - build gate closed. The game runs flat.");
        return false;
    }

    const uint8_t* target =
        patterns::rva_to_address(patterns::kGetPlayerViewPointRva, 64);
    if (!target) {
        BVR_LOG("[bsi] camera: hook NOT installed - RVA 0x%X is not readable",
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    // Prologue gate. A hardcoded RVA on the wrong build does not fail to work,
    // it detours whatever happens to live there - which is how a mod corrupts a
    // game rather than merely not helping. REFUSE on any mismatch.
    if (memcmp(target, patterns::kGetPlayerViewPointPrologue,
               sizeof patterns::kGetPlayerViewPointPrologue) != 0) {
        BVR_LOG("[bsi] camera: prologue MISMATCH at RVA 0x%X (got %02X %02X %02X %02X ...) - "
                "build changed? REFUSING hook, the game runs flat",
                patterns::kGetPlayerViewPointRva, target[0], target[1], target[2], target[3]);
        return false;
    }

    // Independently confirm the argument count the typedef declares. `ret 8`
    // is C2 08 00; if it is not in the body, the 2-stack-arg assumption is
    // wrong and hooking would pop the RTC dialog that writes no crash dump.
    constexpr size_t kRetScanBytes = 0x400;
    const uint8_t* body = patterns::rva_to_address(patterns::kGetPlayerViewPointRva,
                                                   kRetScanBytes);
    bool retFound = false;
    if (body) {
        for (size_t i = 0; i + 3 <= kRetScanBytes; ++i) {
            if (body[i] == 0xC2 && body[i + 1] == patterns::kGetPlayerViewPointRetImm &&
                body[i + 2] == 0x00) {
                retFound = true;
                break;
            }
        }
    }
    if (!retFound) {
        BVR_LOG("[bsi] camera: no `ret %u` (C2 %02X 00) found in the first 0x400 bytes of "
                "RVA 0x%X - the 2-stack-arg assumption is UNCONFIRMED. REFUSING hook rather "
                "than risking a misaligned return (the RTC dialog writes no crash dump).",
                patterns::kGetPlayerViewPointRetImm, patterns::kGetPlayerViewPointRetImm,
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    void* addr = const_cast<uint8_t*>(target);
    MH_STATUS status = MH_CreateHook(addr, reinterpret_cast<void*>(&GetViewPointDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_CreateHook failed: %s", MH_StatusToString(status));
        return false;
    }
    // Self-enabling so this hook's activation never rides on another module's
    // MH_EnableHook(MH_ALL_HOOKS).
    status = MH_EnableHook(addr);
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_EnableHook failed: %s", MH_StatusToString(status));
        MH_RemoveHook(addr);
        g_original = nullptr;
        return false;
    }

    g_target = addr;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] camera: READ-ONLY hook installed on GetPlayerViewPoint (target %p, "
            "RVA 0x%X, prologue and `ret %u` both verified). It observes only - nothing in "
            "this build writes the camera.",
            addr, patterns::kGetPlayerViewPointRva, patterns::kGetPlayerViewPointRetImm);
    return true;
}

bool has_fired() {
    return g_fired.load(std::memory_order_relaxed);
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void* last_player_controller() {
    return g_lastSelf.load(std::memory_order_relaxed);
}

uint64_t silent_ms() {
    const uint64_t last = g_lastCallMs.load(std::memory_order_relaxed);
    if (last == 0) return 0;
    const uint64_t now = GetTickCount64();
    return now > last ? now - last : 0;
}

bool handle_command(const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        log_status();
        return true;
    }
    if (strncmp(args, "paths", 5) == 0) {
        char buf[160];
        BVR_LOG("[bsi] camera: path census %s", path_summary(buf, sizeof buf));
        BVR_LOG("[bsi] camera: 'cached' = [this+0x248] bit 0 set (fast path, +0x24C/+0x258); "
                "'camera' = clear with [this+0x240] non-null; the third bucket is the view "
                "target and the controller's own fields, deliberately NOT separated because "
                "that needs a virtual call out of a detour.");
        return true;
    }
    if (strncmp(args, "tid", 3) == 0) {
        BVR_LOG("[bsi] camera: camera tid=%u present tid=%u foreign-tid calls=%u",
                g_cameraTid.load(), bvr::d3d11_hook::last_present_tid(),
                g_foreignTidCalls.load());
        return true;
    }
    if (strncmp(args, "matrix", 6) == 0) {
        g_loggedMatrix.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: re-arming the one-shot [this+0x430] dump for the next beat");
        return true;
    }
    if (strncmp(args, "heartbeat", 9) == 0) {
        const char* v = args + 9;
        while (*v == ' ') ++v;
        const bool on = strncmp(v, "off", 3) != 0;
        g_heartbeat.store(on, std::memory_order_relaxed);
        if (on) g_beatsLeft = 10;
        BVR_LOG("[bsi] camera: heartbeat %s%s", on ? "ON" : "off",
                on ? " (10-beat burst)" : "");
        return true;
    }
    if (strncmp(args, "off", 3) == 0) {
        // Deliberately does NOT uninstall. This stops the detour's body, which
        // stops the game-thread command pump, which is exactly the positive
        // control for the pump lease in core/framework/command: four seconds
        // later `vrcmd` must report the Present pump has resumed degraded.
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: observation DISABLED. The game-thread command pump goes silent "
                "with it - the Present pump should take back over within the lease window, "
                "degraded. `bsicam on` restores.");
        return true;
    }
    if (strncmp(args, "on", 2) == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_lastBeatMs = 0;
        BVR_LOG("[bsi] camera: observation enabled");
        return true;
    }
    return false;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Camera seam (DR-I2, read-only)")) return;
    ImGui::Text("hook: %s   fired: %s", hook_live() ? "installed" : "not installed",
                has_fired() ? "YES" : "no");
    ImGui::Text("calls: %u   silent: %llu ms", g_callCount.load(),
                static_cast<unsigned long long>(silent_ms()));
    ImGui::Text("tid: camera %u / present %u", g_cameraTid.load(),
                bvr::d3d11_hook::last_present_tid());
    ImGui::Text("paths: cached %u | camera %u | target %u | unreadable %u", g_pathCached.load(),
                g_pathCamera.load(), g_pathTarget.load(), g_pathUnknown.load());
    if (g_last.valid) {
        ImGui::Text("loc (%.1f %.1f %.1f)", g_last.loc.x, g_last.loc.y, g_last.loc.z);
        ImGui::Text("rot (%d %d %d) = (%.1f %.1f %.1f) deg", g_last.rot.pitch, g_last.rot.yaw,
                    g_last.rot.roll, g_last.rot.pitch * kRotToDeg, g_last.rot.yaw * kRotToDeg,
                    g_last.rot.roll * kRotToDeg);
    }
    ImGui::TextDisabled("read-only: nothing in this build writes the camera");
}

} // namespace bvr::bsi::camera
