#include "game/bioshockinf/fire.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/aim.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/frame_context.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::bsi::fire {
namespace {

// The seam's signature (patterns.h derivation): `__thiscall` with TWO stack
// args - the hidden FVector return buffer and the optional Weapon* - hence
// `ret 8`, and 8/4 == 2 is the arg count this detour MUST declare (the RTC
// no-dump rule). Returns the retbuf pointer in eax.
using FireStartFn = FVector*(__fastcall*)(void* self, void* edx, FVector* out, void* weapon);

FireStartFn g_original = nullptr;
void* g_target = nullptr;

uint32_t to_rva(const void* p) {
    const uint8_t* base = patterns::image_base();
    return (base && p) ? static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base) : 0;
}

// SHIPS ARMED (s46): the flat acceptance was unambiguous - the probe measured
// the engine origin at the camera (77.4 UU / 51.6 cm from the hand, the exact
// hole-vs-dot parallax the headset reported), and the substitution moved the
// trace start to the correct per-hand origin on both hands with zero faults.
// The headset judges hole-on-dot; `bsifire off` is the bisect lever.
std::atomic<bool> g_probe{true};       // install + observe (read-only)
std::atomic<bool> g_substitute{true};  // rewrite the returned origin
std::atomic<bool> g_installed{false};

// Counters. The native dispatches on ANY AXPawn - NPCs ask it too - so the
// player gate is self == the latched PC's pawn, and the split is telemetry.
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_playerCalls{0};
std::atomic<uint32_t> g_subs{0};
std::atomic<uint32_t> g_noCtx{0};       // player call, but no valid FrameContext
std::atomic<uint32_t> g_noPose{0};      // player call, but the hand pose is gone
std::atomic<uint32_t> g_clampRefusals{0};
std::atomic<int> g_dumpLeft{0};

// Last player-call telemetry (game thread writes, anywhere reads).
std::atomic<float> g_lastEngine[3] = {};
std::atomic<float> g_lastOurs[3] = {};
std::atomic<float> g_lastDispUu{0.0f};
std::atomic<int> g_lastHand{1};
std::atomic<uint32_t> g_lastWeaponRva{0}; // weapon OBJECT ptr is transient; keep it raw

uint64_t g_lastLogMs = 0;

// Refuse a substitution that would move the origin absurdly far - a melee /
// Sky-Hook trace is SHORT, and an origin teleported past its target is how
// BS1's wrench broke. 200 UU is ~1.3 m at the calibrated 150 UU/m: camera-to
// -hand is 60-90 UU, so anything past 200 means a broken basis, not a long
// arm. (BS2 precedent, its own number re-derived from this game's scale.)
constexpr float kMaxDisplacementUu = 200.0f;

FVector* __fastcall FireStartDetour(void* self, void* edx, FVector* out, void* weapon) {
    FVector* r = g_original(self, edx, out, weapon);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!r) return r;

    // Player gate: this native answers for every pawn on the map. Only the
    // latched PC's own pawn gets the substituted origin - the same ownership
    // rule BS1 used, this game's field (kPcPawnOffset, s42 derivation).
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4)) return r;
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn || self != pawn) return r;
    g_playerCalls.fetch_add(1, std::memory_order_relaxed);
    // s46 stance glue: a player shot resets the SubtleFidget stance, so it
    // opens the ready-pose capture window (bones.cpp).
    bones::note_player_fire();

    const FVector engine = *r;
    g_lastEngine[0].store(engine.x, std::memory_order_relaxed);
    g_lastEngine[1].store(engine.y, std::memory_order_relaxed);
    g_lastEngine[2].store(engine.z, std::memory_order_relaxed);
    g_lastWeaponRva.store(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon)),
                          std::memory_order_relaxed);

    // The hand origin, from the SAME chain the aim dot uses: same latched
    // hand, same trim, same frame basis - agreement by construction.
    const FrameContext& fc = camera::frame_context();
    if (!fc.valid) {
        g_noCtx.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    const int hand = aim::last_aiming_hand();
    bvr::vr::HeadPose hp{};
    if (!bvr::vr::get_hand_pose(hand, /*aimPose=*/true, hp)) {
        g_noPose.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    const float pos[3] = {hp.px, hp.py, hp.pz};
    const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
    const GamePose gp =
        ray_pose_from_xr(fc, pos, quat, aim::trim_get(hand, 0), aim::trim_get(hand, 1));

    // The ray-origin sliders, in the trimmed ray's own frame. They are stored
    // in CENTIMETERS and the dot applies them in XR meters (aim.cpp) - here
    // the target space is game UU, so the conversion is worldScale/100, the
    // hands.cpp offset pattern. One slider set moves dot + laser + fire origin
    // together.
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    const float uuPerCm = fc.worldScale * 0.01f;
    const float f = aim::origin_get(hand, 0) * uuPerCm;
    const float rr = aim::origin_get(hand, 1) * uuPerCm;
    const float u = aim::origin_get(hand, 2) * uuPerCm;
    FVector ours{};
    ours.x = gp.loc.x + fwd[0] * f + right[0] * rr + up[0] * u;
    ours.y = gp.loc.y + fwd[1] * f + right[1] * rr + up[1] * u;
    ours.z = gp.loc.z + fwd[2] * f + right[2] * rr + up[2] * u;
    g_lastOurs[0].store(ours.x, std::memory_order_relaxed);
    g_lastOurs[1].store(ours.y, std::memory_order_relaxed);
    g_lastOurs[2].store(ours.z, std::memory_order_relaxed);
    g_lastHand.store(hand, std::memory_order_relaxed);

    const float dx = ours.x - engine.x, dy = ours.y - engine.y, dz = ours.z - engine.z;
    const float disp = sqrtf(dx * dx + dy * dy + dz * dz);
    g_lastDispUu.store(disp, std::memory_order_relaxed);

    bool wrote = false;
    if (g_substitute.load(std::memory_order_relaxed)) {
        if (disp > kMaxDisplacementUu) {
            g_clampRefusals.fetch_add(1, std::memory_order_relaxed);
        } else {
            // xyz ONLY. The Floating wrapper appends its own 4th dword after
            // we return; the buffer here is a 12-byte FVector.
            r->x = ours.x;
            r->y = ours.y;
            r->z = ours.z;
            g_subs.fetch_add(1, std::memory_order_relaxed);
            wrote = true;
        }
    }

    int dump = g_dumpLeft.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    if (dump > 0 || now - g_lastLogMs >= 1000) {
        if (dump > 0) g_dumpLeft.fetch_sub(1, std::memory_order_relaxed);
        else g_lastLogMs = now;
        BVR_LOG("[bsi] fire: hand=%c weapon=%p engine=(%.1f %.1f %.1f) hand-origin=(%.1f "
                "%.1f %.1f) displacement=%.1f UU (%.1f cm) %s | calls=%u player=%u subs=%u "
                "noCtx=%u noPose=%u clampRefused=%u",
                hand ? 'R' : 'L', weapon, engine.x, engine.y, engine.z, ours.x, ours.y,
                ours.z, disp, disp / fc.worldScale * 100.0f,
                wrote ? "SUBSTITUTED" : (g_substitute.load(std::memory_order_relaxed)
                                             ? "refused"
                                             : "probe"),
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_subs.load(std::memory_order_relaxed),
                g_noCtx.load(std::memory_order_relaxed),
                g_noPose.load(std::memory_order_relaxed),
                g_clampRefusals.load(std::memory_order_relaxed));
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
        BVR_LOG("[bsi] fire: REFUSED - build gate closed");
        return false;
    }
    uint8_t* impl = const_cast<uint8_t*>(patterns::image_base()) +
                    patterns::kXGetWeaponStartTraceLocationImplRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x200)) {
        BVR_LOG("[bsi] fire: REFUSED - impl rva 0x%X not readable",
                patterns::kXGetWeaponStartTraceLocationImplRva);
        return false;
    }
    // Gate 1: the pinned prologue. If the running build does not have exactly
    // these bytes here, the RVA means something else - refuse, never detour
    // whatever is there (the kGetPlayerViewPointPrologue discipline).
    if (memcmp(impl, patterns::kXGetWeaponStartTraceLocationPrologue,
               sizeof patterns::kXGetWeaponStartTraceLocationPrologue) != 0) {
        BVR_LOG("[bsi] fire: REFUSED - prologue at rva 0x%X does not match the derivation "
                "(stale build?)",
                patterns::kXGetWeaponStartTraceLocationImplRva);
        return false;
    }
    // Gate 2: the arity. `ret imm/4` must equal the detour's stack-arg count
    // or the result is the RTC dialog that writes no crash dump.
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x200; ++i) {
        if (impl[i] == 0xC2 && impl[i + 1] == patterns::kXGetWeaponStartTraceLocationRetImm &&
            impl[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] fire: REFUSED - no `ret %u` in the first 0x200 bytes at rva 0x%X",
                patterns::kXGetWeaponStartTraceLocationRetImm,
                patterns::kXGetWeaponStartTraceLocationImplRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&FireStartDetour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        BVR_LOG("[bsi] fire: MH_CreateHook failed at rva 0x%X",
                patterns::kXGetWeaponStartTraceLocationImplRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fire: MH_EnableHook failed at rva 0x%X",
                patterns::kXGetWeaponStartTraceLocationImplRva);
        return false;
    }
    g_target = impl;
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fire: seam hooked - AXPawn::XGetWeaponStartTraceLocation impl rva 0x%X "
            "(the single choke point: its own thunk AND the Floating wrapper's 13 C++ call "
            "sites route through it). Mode: %s. The body reads the camera's "
            "GetPlayerViewPoint, which is WHY bullets leave the face today.",
            patterns::kXGetWeaponStartTraceLocationImplRva,
            g_substitute.load(std::memory_order_relaxed) ? "SUBSTITUTING" : "PROBE (read-only)");
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsifire") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "probe on", 8) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fire: PROBE armed - installs on the next camera tick, read-only. "
                "Fire a shot and read the engine-vs-hand origin line.");
    } else if (strncmp(args, "probe off", 9) == 0) {
        g_probe.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fire: probe off (an installed hook stays installed and passes "
                "through)");
    } else if (strncmp(args, "dump", 4) == 0) {
        int n = 8;
        sscanf_s(args + 4, "%d", &n);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        g_dumpLeft.store(n, std::memory_order_relaxed);
        BVR_LOG("[bsi] fire: dumping the next %d player calls in full", n);
    } else if (strncmp(args, "on", 2) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fire: ORIGIN SUBSTITUTION ARMED - the player's weapon trace now "
                "starts at the aiming hand (xyz only, %.0f UU displacement cap). Direction "
                "stays at the aim seam.",
                kMaxDisplacementUu);
    } else if (strncmp(args, "off", 3) == 0) {
        g_substitute.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fire: substitution off - the engine's own trace origin stands");
    } else {
        BVR_LOG("[bsi] fire: installed=%d probe=%d write=%d | calls=%u player=%u subs=%u "
                "noCtx=%u noPose=%u clampRefused=%u | last hand=%c engine=(%.1f %.1f %.1f) "
                "ours=(%.1f %.1f %.1f) disp=%.1f UU | bsifire probe on|off | on|off | "
                "dump <n> | status",
                g_installed.load(std::memory_order_relaxed) ? 1 : 0,
                g_probe.load(std::memory_order_relaxed) ? 1 : 0,
                g_substitute.load(std::memory_order_relaxed) ? 1 : 0,
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_subs.load(std::memory_order_relaxed),
                g_noCtx.load(std::memory_order_relaxed),
                g_noPose.load(std::memory_order_relaxed),
                g_clampRefusals.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed) ? 'R' : 'L',
                g_lastEngine[0].load(std::memory_order_relaxed),
                g_lastEngine[1].load(std::memory_order_relaxed),
                g_lastEngine[2].load(std::memory_order_relaxed),
                g_lastOurs[0].load(std::memory_order_relaxed),
                g_lastOurs[1].load(std::memory_order_relaxed),
                g_lastOurs[2].load(std::memory_order_relaxed),
                g_lastDispUu.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("FIRE ORIGIN (I8) - bullets from the gun")) return;
    bool probe = g_probe.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Install the fire-origin seam (probe, read-only)", &probe))
        g_probe.store(probe, std::memory_order_relaxed);
    bool sub = g_substitute.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Trace starts at the AIMING HAND", &sub)) {
        if (sub) g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(sub, std::memory_order_relaxed);
    }
    ImGui::Text("hook %s   calls %u   player %u   substituted %u",
                g_installed.load(std::memory_order_relaxed) ? "LIVE" : "not installed",
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_subs.load(std::memory_order_relaxed));
    ImGui::Text("engine-vs-hand displacement %.1f UU", g_lastDispUu.load(std::memory_order_relaxed));
    ImGui::TextDisabled("origin offsets ride the AIM block's ray-origin sliders");
    ImGui::TextDisabled("bsifire probe on|off | on|off | dump <n> | status");
}

} // namespace bvr::bsi::fire
