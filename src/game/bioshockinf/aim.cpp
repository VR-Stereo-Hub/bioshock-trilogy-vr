#include "game/bioshockinf/aim.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::bsi::aim {
namespace {

// The seam's signature. ONE stack arg (the hidden return buffer for the
// FRotator), which is `ret 4` in the body - and the arg count MUST equal
// ret imm / 4 or the result is a Run-Time Check Failure #0 dialog that writes
// no crash dump. The install below refuses unless it can SEE that `C2 04 00`.
using GetBaseAimRotationFn = FRotator*(__fastcall*)(void* self, void* edx, FRotator* out);

GetBaseAimRotationFn g_original = nullptr;
void* g_target = nullptr;

// Log addresses as RVAs, never as VAs: an RVA is what ENGINE_NOTES records and
// what the offline tools take.
uint32_t to_rva(const void* p) {
    const uint8_t* base = patterns::image_base();
    return (base && p) ? static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base) : 0;
}

std::atomic<bool> g_probe{false};      // install + observe
std::atomic<bool> g_substitute{false}; // actually write the out-param
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_subs{0};
std::atomic<int> g_dumpLeft{0};

// Telemetry, published for the heartbeat / F10 / the flat acceptance. All are
// written on the game thread and read anywhere, so plain atomics.
std::atomic<float> g_lastEngineYawDeg{0.0f};
std::atomic<float> g_lastEnginePitchDeg{0.0f};
std::atomic<float> g_lastRayYawDeg{0.0f};
std::atomic<float> g_lastRayPitchDeg{0.0f};
// THE NUMBER THIS LANE EXISTS TO MOVE: how far the engine's own aim is from
// where the controller points. Read it BEFORE arming the write - a large value
// with the write off is the defect, and it collapsing to ~0 with the write on
// is the proof.
std::atomic<float> g_lastDivergenceDeg{0.0f};

uint64_t g_lastLogMs = 0;

// The controller ray, in GAME rotation units, built on exactly the basis the
// view drive uses: yaw is the game's own yaw plus the residual measured off the
// recenter, pitch and roll absolute from the controller. Using the view's basis
// is the point - it is what makes "where I look" and "where I shoot" the same
// coordinate system rather than two parallel derivations that drift.
bool controller_ray(FRotator* out) {
    bvr::vr::HeadPose hp{};
    if (!bvr::vr::get_hand_pose(1, /*aimPose=*/true, hp)) return false;
    int32_t gameYawUnits = 0, recenterYawUnits = 0;
    if (!camera::aim_basis(&gameYawUnits, &recenterYawUnits)) return false;

    const UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
    const int32_t handYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
    const int32_t residual = wrap_rot(handYawUnits - recenterYawUnits);
    out->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
    out->yaw = gameYawUnits + residual;
    // Roll is deliberately left at zero: an aim ROTATOR's roll does not steer a
    // trace, and a rolled controller must not tilt anything downstream that
    // reads this rotation for a basis.
    out->roll = 0;
    return true;
}

// The seam. Original FIRST, always - this hook observes and adjusts, it never
// replaces. Everything here is POD and branch-only: no allocation, no locking,
// and logging is rate-limited and outside any guarded region.
FRotator* __fastcall AimDetour(void* self, void* edx, FRotator* outBuf) {
    FRotator* r = g_original(self, edx, outBuf);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!r) return r;

    // Snapshot what the ENGINE thinks before touching anything, so the probe
    // keeps measuring the original even while the write is armed.
    const FRotator engine = *r;
    FRotator ray{};
    const bool haveRay = controller_ray(&ray);

    if (haveRay) {
        g_lastEngineYawDeg.store(static_cast<float>(engine.yaw) / kRotUnitsPerDegree,
                                 std::memory_order_relaxed);
        g_lastEnginePitchDeg.store(static_cast<float>(engine.pitch) / kRotUnitsPerDegree,
                                   std::memory_order_relaxed);
        g_lastRayYawDeg.store(static_cast<float>(ray.yaw) / kRotUnitsPerDegree,
                              std::memory_order_relaxed);
        g_lastRayPitchDeg.store(static_cast<float>(ray.pitch) / kRotUnitsPerDegree,
                                std::memory_order_relaxed);
        const float dYaw =
            static_cast<float>(wrap_rot(ray.yaw - engine.yaw)) / kRotUnitsPerDegree;
        const float dPitch =
            static_cast<float>(wrap_rot(ray.pitch - engine.pitch)) / kRotUnitsPerDegree;
        g_lastDivergenceDeg.store(sqrtf(dYaw * dYaw + dPitch * dPitch),
                                  std::memory_order_relaxed);

        if (g_substitute.load(std::memory_order_relaxed)) {
            *r = ray;
            g_subs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int dump = g_dumpLeft.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    if (dump > 0 || now - g_lastLogMs >= 1000) {
        if (dump > 0) g_dumpLeft.fetch_sub(1, std::memory_order_relaxed);
        else g_lastLogMs = now;
        // Rotator components as %d ALWAYS, with degrees alongside: an FRotator's
        // int32s reinterpret as denormal floats and print as 0.000, which cost
        // BioShock 1 a long detour.
        BVR_LOG("[bsi] aim: engine=(%d %d %d)=(%.1f %.1f)deg ray=(%d %d %d)=(%.1f %.1f)deg "
                "divergence=%.1f deg | ray %s, write %s (%u calls, %u substituted)",
                engine.pitch, engine.yaw, engine.roll,
                static_cast<float>(engine.pitch) / kRotUnitsPerDegree,
                static_cast<float>(engine.yaw) / kRotUnitsPerDegree, ray.pitch, ray.yaw,
                ray.roll, static_cast<float>(ray.pitch) / kRotUnitsPerDegree,
                static_cast<float>(ray.yaw) / kRotUnitsPerDegree,
                g_lastDivergenceDeg.load(std::memory_order_relaxed),
                haveRay ? "live" : "UNAVAILABLE (drive off, no recenter, or no hand pose)",
                g_substitute.load(std::memory_order_relaxed) ? "ARMED" : "off",
                g_calls.load(std::memory_order_relaxed),
                g_subs.load(std::memory_order_relaxed));
    }
    return r;
}

} // namespace

bool wants_install() {
    return (g_probe.load(std::memory_order_relaxed) ||
            g_substitute.load(std::memory_order_relaxed)) &&
           !g_installed.load(std::memory_order_relaxed);
}

bool try_install() {
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] aim: REFUSED - build gate closed");
        return false;
    }
    // The implementation is a VIRTUAL, so it is read off the live pawn rather
    // than from a static RVA. The pawn comes from the latched PC's field the
    // grant lane already named, not from a scan.
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4)) {
        BVR_LOG("[bsi] aim: no readable player controller yet - the camera hook must fire "
                "first");
        return false;
    }
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn || !bvr::pattern_scan::is_memory_valid(pawn, 4)) {
        BVR_LOG("[bsi] aim: no pawn at PC+0x%X yet (still loading, or a pawnless state) - "
                "this is a wait, not a failure",
                patterns::kPcPawnOffset);
        return false;
    }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(pawn);
    const uint32_t slot = patterns::kPawnGetBaseAimRotationVtblOffset;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, slot + sizeof(void*))) {
        BVR_LOG("[bsi] aim: REFUSED - pawn vtable %p not readable through +0x%X", (void*)vt,
                slot);
        return false;
    }
    void* impl = const_cast<uint8_t*>(vt[slot / sizeof(void*)]);
    if (!impl || !bvr::pattern_scan::is_memory_valid(impl, 0x40)) {
        BVR_LOG("[bsi] aim: REFUSED - vtable slot +0x%X is not readable code", slot);
        return false;
    }

    // THE ARITY GATE, and it is load-bearing. `ret imm / 4` must equal the
    // detour's stack-arg count; a mismatch pops Run-Time Check Failure #0 and
    // writes NO crash dump. Refuse rather than risk it - the same check the
    // camera hook makes, which is the stricter of the two patterns in the tree.
    const uint8_t* body = static_cast<const uint8_t*>(impl);
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x200; ++i) {
        if (!bvr::pattern_scan::is_memory_valid(body + i, 3)) break;
        if (body[i] == 0xC2 && body[i + 1] == patterns::kPawnGetBaseAimRotationRetImm &&
            body[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] aim: REFUSED - no `ret %u` found in the first 0x200 bytes at rva 0x%X. "
                "The arg count must equal ret imm/4 or the RTC dialog (which writes no dump) "
                "is the result; the derivation is stale, do not hook it.",
                patterns::kPawnGetBaseAimRotationRetImm, to_rva(impl));
        return false;
    }

    if (MH_CreateHook(impl, reinterpret_cast<void*>(&AimDetour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        BVR_LOG("[bsi] aim: MH_CreateHook failed at rva 0x%X", to_rva(impl));
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] aim: MH_EnableHook failed at rva 0x%X", to_rva(impl));
        return false;
    }
    g_target = impl;
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] aim: seam hooked - APawn::GetBaseAimRotation impl rva 0x%X, read off the "
            "LIVE pawn %p vtable slot +0x%X (a virtual, so no static RVA is needed). Mode: "
            "%s. This is where the fire direction comes from: with a controller the stock "
            "body delegates to the CONTROLLER's rotation, which is why the shot follows the "
            "BODY and not the head today.",
            to_rva(impl), pawn, slot,
            g_substitute.load(std::memory_order_relaxed) ? "SUBSTITUTING" : "PROBE (read-only)");
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsiaim") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "probe on", 8) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: PROBE armed - the seam installs on the next camera tick and only "
                "OBSERVES. It refuses to substitute, so this diagnostic cannot change what "
                "it measures. Read `divergence` - that is the angle between the engine's own "
                "aim and where the controller points.");
    } else if (strncmp(args, "probe off", 9) == 0) {
        g_probe.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: probe off (an installed hook stays installed and passes through - "
                "a thread may still be returning through the trampoline)");
    } else if (strncmp(args, "dump", 4) == 0) {
        int n = 8;
        sscanf_s(args + 4, "%d", &n);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        g_dumpLeft.store(n, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: dumping the next %d calls in full", n);
    } else if (strncmp(args, "on", 2) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: SUBSTITUTION ARMED - the pawn's aim rotation now comes from the "
                "right controller. The engine's own rotation is never written; only this "
                "function's out-param is, so drive-off is a byte-identical passthrough.");
    } else if (strncmp(args, "off", 3) == 0) {
        g_substitute.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: substitution off - the engine's own aim rotation stands");
    } else {
        BVR_LOG("[bsi] aim: installed=%d probe=%d write=%d | calls=%u substituted=%u | "
                "engine=(%.1f yaw %.1f pitch) ray=(%.1f yaw %.1f pitch) divergence=%.1f deg "
                "| bsiaim probe on|off | on|off | dump <n> | status",
                g_installed.load(std::memory_order_relaxed) ? 1 : 0,
                g_probe.load(std::memory_order_relaxed) ? 1 : 0,
                g_substitute.load(std::memory_order_relaxed) ? 1 : 0,
                g_calls.load(std::memory_order_relaxed), g_subs.load(std::memory_order_relaxed),
                g_lastEngineYawDeg.load(std::memory_order_relaxed),
                g_lastEnginePitchDeg.load(std::memory_order_relaxed),
                g_lastRayYawDeg.load(std::memory_order_relaxed),
                g_lastRayPitchDeg.load(std::memory_order_relaxed),
                g_lastDivergenceDeg.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("AIM (I7) - controller aims, head looks")) return;
    bool probe = g_probe.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Install the aim seam (probe, read-only)", &probe))
        g_probe.store(probe, std::memory_order_relaxed);
    bool sub = g_substitute.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Aim with the RIGHT CONTROLLER", &sub)) {
        if (sub) g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(sub, std::memory_order_relaxed);
    }
    ImGui::Text("hook %s   calls %u   substituted %u",
                g_installed.load(std::memory_order_relaxed) ? "LIVE" : "not installed",
                g_calls.load(std::memory_order_relaxed), g_subs.load(std::memory_order_relaxed));
    // The number to watch in the headset: with the write off it is how wrong
    // the shot is; with it on it should sit at ~0.
    ImGui::Text("divergence %.1f deg  (engine yaw %.1f / ray yaw %.1f)",
                g_lastDivergenceDeg.load(std::memory_order_relaxed),
                g_lastEngineYawDeg.load(std::memory_order_relaxed),
                g_lastRayYawDeg.load(std::memory_order_relaxed));
    ImGui::TextDisabled("bsiaim probe on|off | on|off | dump <n> | status");
}

} // namespace bvr::bsi::aim
