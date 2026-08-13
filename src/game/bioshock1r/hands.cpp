// M7 visible hands + weapons. See hands.h for the design; ENGINE_NOTES
// "Viewmodel / AHands" for the derivations.
//
// Two drivable targets, learned from the first in-headset test (2026-07-25):
//
//   HANDS mode (default): drive the AHands actor - the one whose transform the
//   renderer actually honors. Its origin is the EYE anchor (Hands.UpdateLocation
//   places it at PawnOwner.Location + EyeHeight + PlayerViewOffset, rotated by
//   the view rotation - decompile, summarized in ENGINE_NOTES), so the mesh's
//   gun hangs ~50 UU in front of the pivot and rotations swing it on that
//   lever. The full pivot correction (pull the origin ~-100 cm so the gun sits
//   at the controller) is CULLED - the engine drops the rig once the origin
//   goes behind the camera - so how much correction is affordable is a
//   headset-side tuning question, bounded by how far out the hand is held.
//
//   GUN mode (experimental, inert): drive the WEAPON actor, whose origin sits
//   AT the visible gun. Would be the ideal pivot - but live-proven ineffective:
//   the renderer draws an ATTACHED weapon from its attachment matrix and
//   ignores the actor fields (full-rate writes to the live pistol moved
//   nothing). Kept only as the anchor for a future detach experiment - the
//   weapon's Base pointer (its attach parent, = the AHands actor) sits at
//   +0x450, adjacent to Owner at +0x454, the classic UE2 pair.

#include "game/bioshock1r/hands.h"

#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::hands {
namespace {

const uint8_t* g_imageBase = nullptr;

std::atomic<bool> g_enabled{false};
std::atomic<int> g_pendingEnable{-1}; // overlay -> game thread (see aim.cpp)
std::atomic<int> g_mode{2};           // 0 = gun (inert), 1 = hands (actor pin,
                                      // retired), 2 = bones (M7-v2, default)
std::atomic<bool> g_useAimPose{true}; // aim pose = the ray the laser/bullet use
std::atomic<int> g_handMode{2};       // 0 left, 1 right, 2 auto
std::atomic<int> g_autoHand{1};       // the latched auto choice
// Model offsets, PER HAND (0 left / 1 right, same convention as aim.cpp): the
// pistol and the plasmid hand sit differently in the mesh, so one shared set
// meant tuning the weapon also moved the plasmid hand. Position is in
// CENTIMETRES in the model's final (trimmed) frame; the rotation trim is
// degrees, applied in the CONTROLLER'S LOCAL frame as a quaternion compose -
// euler adds after conversion only behave at one controller orientation (the
// first headset test's "pivot" bug).
// Defaults are ZERO on purpose. The ideal pivot correction (pull the mesh's gun
// to the controller, ~-100 cm forward) is CULLED: the engine drops the whole
// rig the moment the actor origin goes behind the camera (live-proven - the
// rig vanished with the origin 32 UU back). Forward pull is therefore limited
// to roughly the controller's own distance from the face, and where that line
// sits is the user's in-headset call, not a default.
std::atomic<float> g_posFwdCm[2]{0.0f, 0.0f}, g_posRightCm[2]{0.0f, 0.0f},
    g_posUpCm[2]{0.0f, 0.0f};
std::atomic<float> g_rotPitchDeg[2]{0.0f, 0.0f}, g_rotYawDeg[2]{0.0f, 0.0f},
    g_rotRollDeg[2]{0.0f, 0.0f};

std::atomic<bool> g_writeRot{true}; // rotation write can be disabled on its own
int32_t g_probeLeft = 0;

// Cached actors, revalidated by vtable on every use.
void* g_handsActor = nullptr;
void* g_weaponActor = nullptr;
uint64_t g_lastHandsScanMs = 0;
uint64_t g_lastWeaponScanMs = 0;
// Search state at file scope, not function-local, because the retry gates below
// have to be able to ASK whether a sliced sweep is mid-flight. Getting that
// wrong starves the sweep: the first smoke test of the sliced scanner had the
// rate limit refusing to continue an in-progress sweep for up to 32 s, while
// the caller counted every "still working" return as a miss and latched the
// scanner dormant before it had ever completed a single pass (session 27).
patterns::ObjectScan g_handsScan;
patterns::ObjectScan g_weaponScan;
uint32_t g_handsScanFails = 0; // consecutive empty scans -> backoff
void* g_lastPc = nullptr;
std::atomic<uint32_t> g_writes{0};
std::atomic<int32_t> g_lastMatches{0};

// Self-expiring synthetic lanes (the command file polls at 1 Hz, so holds
// outlive their command inside the DLL):
//   test    - camera-relative placement; proves the WRITE lands, no pose math.
//   simpose - a synthetic XR controller pose fed through the REAL mapping path
//             (trim quat, xr_pose_to_game, offsets), so the transform chain is
//             testable with no headset.
struct TestOffset {
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    float distUu = 60.0f;
    uint64_t deadline = 0;
};
TestOffset g_test;

struct SimPose {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    uint64_t deadline = 0;
};
SimPose g_sim;

// Last values written, for the overlay + the flat-test assertions.
std::atomic<float> g_lastX{0.0f}, g_lastY{0.0f}, g_lastZ{0.0f};
std::atomic<int32_t> g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read12(const void* src, void* out) {
    __try {
        memcpy(out, src, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write12(void* dst, const void* in) {
    __try {
        memcpy(dst, in, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool has_vtable(void* obj, uint32_t wantRva) {
    if (!obj) return false;
    void* vtbl = nullptr;
    if (!read_ptr(obj, &vtbl)) return false;
    return to_rva(vtbl) == wantRva;
}

// ---- finding the actors ------------------------------------------------------

struct ScanCtx {
    float camX, camY, camZ;
    bool chooseAny; // probe: choose nothing, so the whole list gets logged
};

// Both accept callbacks run inside the scan's SEH guard (heap_scan.cpp), so
// neither may LOG or ALLOCATE: MSVC does not run C++ destructors during SEH
// unwinding, and a fault taken while the log mutex is held would wedge logging
// for the life of the process (session 27). Diagnostics go out through `probe`
// and are formatted by the caller afterwards.
//
// A UClass default object carries the same vtable as a live actor but sits at
// the origin with zeroed fields; proximity to the camera separates the live
// viewmodel from it (and from `0xCCCCCCCC` stack debris).
//
// Probe slots: [0..2] location truncated to UU, [3] distance to the camera.
bool accept_hands(void* obj, void* user, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    ScanCtx* c = static_cast<ScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    float loc[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    probe[0] = static_cast<int32_t>(loc[0]);
    probe[1] = static_cast<int32_t>(loc[1]);
    probe[2] = static_cast<int32_t>(loc[2]);
    probe[3] = static_cast<int32_t>(dist);
    if (!c->chooseAny) return false;
    return dist < 2000.0f && (loc[0] != 0.0f || loc[1] != 0.0f || loc[2] != 0.0f);
}

// The player's weapon: an APlayerWeapon whose owning pawn ([w+0x454]) is the
// player. Both carried weapons pass that (BioShock keeps the stowed one as a
// live actor too, parked at the pawn - live: nearest-to-CAMERA picked the
// stowed wrench, 28 UU below the eye, over the pistol 60 UU ahead of it). So
// the anchor is the EXPECTED GUN SPOT, ~50 UU along the view, which only the
// equipped weapon hovers near. The aim map's learned object, which is the
// weapon that actually FIRES, takes priority over this scan entirely.
struct WeaponScanCtx {
    float camX, camY, camZ;
    const uint8_t* imageBase;
    void* handsActor; // structural accept: Base(+0x450) == the AHands rig
    void* best;
    float bestDist;
};

// Probe slots: [0..2] location truncated to UU, [3] distance to the expected
// gun spot, or -1 when the structural (attachment) accept fired instead.
bool accept_weapon(void* obj, void* user, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    WeaponScanCtx* c = static_cast<WeaponScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);

    void* owner = *reinterpret_cast<void* const*>(p + patterns::kWeaponOwnerOffset);
    if (!owner) return false;
    void* ownerVtbl = *reinterpret_cast<void* const*>(owner);
    uint32_t ownerRva = static_cast<uint32_t>(static_cast<const uint8_t*>(ownerVtbl) -
                                              c->imageBase);
    if (ownerRva != patterns::kShockPlayerVtableRva) return false;

    // Structural accept first (session 21): the EQUIPPED weapon is attached
    // to the AHands rig - its Base (+0x450) is the hands actor. Distance to
    // the expected gun spot depends on pose/state and misses in some boot
    // states (live: 2 owner-matched candidates, both >120 UU); attachment
    // does not.
    void* base = *reinterpret_cast<void* const*>(p + patterns::kActorBaseOffset);

    float loc[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);
    probe[0] = static_cast<int32_t>(loc[0]);
    probe[1] = static_cast<int32_t>(loc[1]);
    probe[2] = static_cast<int32_t>(loc[2]);

    if (c->handsActor && base == c->handsActor) {
        c->best = obj;
        c->bestDist = 0.0f;
        probe[3] = -1; // structural accept: attached to the rig
        return false;
    }

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    probe[3] = static_cast<int32_t>(dist);
    if (dist < 120.0f && dist < c->bestDist) {
        c->best = obj;
        c->bestDist = dist;
    }
    return false; // never "accept" - attachment/nearest wins after the walk
}

bool weapon_valid(void* w) {
    if (!has_vtable(w, patterns::kPlayerWeaponVtableRva)) return false;
    void* owner = nullptr;
    if (!read_ptr(static_cast<const uint8_t*>(w) + patterns::kWeaponOwnerOffset, &owner))
        return false;
    return has_vtable(owner, patterns::kShockPlayerVtableRva);
}

void* find_hands_actor(const FrameContext& ctx, bool probeOnly) {
    if (!probeOnly) {
        if (has_vtable(g_handsActor, patterns::kHandsVtableRva)) return g_handsActor;
        g_handsActor = nullptr;
        uint64_t now = GetTickCount64();
        // Exponential backoff on consecutive empty scans (2s -> 32s cap).
        // The rig legitimately does not exist for long stretches (the NG+
        // intro runs minutes with no hands), and the full heap scan can take
        // SECONDS on a grown late-game heap - without backoff the 2 s
        // cooldown dragged the whole game to ~1 fps for the entire intro
        // (session 18 part 3, live). Reset on success, world change, and
        // re-enable, so pickup stays prompt when the rig actually appears.
        uint32_t shift = g_handsScanFails > 4 ? 4 : g_handsScanFails;
        // The backoff must never gate a sweep that has already started, or the
        // slices are spread minutes apart and the pass never finishes.
        if (!g_handsScan.sweeping && now - g_lastHandsScanMs < (2000ull << shift)) return nullptr;
        g_lastHandsScanMs = now;
    }
    ScanCtx sc{ctx.camX, ctx.camY, ctx.camZ, !probeOnly};
    patterns::ScanResult r{};
    if (!patterns::run_object_scan(g_handsScan, patterns::kHandsVtableRva,
                                   patterns::kActorViewDirOffset + 12, &accept_hands, &sc, r))
        return nullptr; // sliced sweep still running - no stall, retry next frame
    patterns::log_scan_result("AHands", g_handsScan, r, probeOnly || g_handsScanFails == 0);
    g_lastMatches.store(r.matches, std::memory_order_relaxed);
    if (probeOnly) return nullptr;
    g_handsActor = r.object;
    if (r.object) {
        g_handsScanFails = 0;
    } else {
        ++g_handsScanFails;
    }
    return g_handsActor;
}

void* find_weapon_actor(const FrameContext& ctx, bool probeOnly) {
    // The object the aim seam learned from the trigger IS the equipped gun.
    void* learned = bvr::b1r::aim::learned_weapon_object();
    if (!probeOnly && weapon_valid(learned)) {
        g_weaponActor = learned;
        return learned;
    }
    if (!probeOnly) {
        if (weapon_valid(g_weaponActor)) return g_weaponActor;
        g_weaponActor = nullptr;
        uint64_t now = GetTickCount64();
        // As above: never gate an in-flight sweep.
        if (!g_weaponScan.sweeping && now - g_lastWeaponScanMs < patterns::kScanRetryMs)
            return nullptr;
        g_lastWeaponScanMs = now;
    }
    // Anchor at the expected gun spot: 50 UU along the current view.
    float dir[3];
    FRotator viewRot{ctx.camPitch, ctx.camYaw, 0};
    ue_rot_to_dir(viewRot, dir);
    WeaponScanCtx wc{ctx.camX + dir[0] * 50.0f,
                     ctx.camY + dir[1] * 50.0f,
                     ctx.camZ + dir[2] * 50.0f,
                     g_imageBase,
                     has_vtable(g_handsActor, patterns::kHandsVtableRva) ? g_handsActor
                                                                         : nullptr,
                     nullptr,
                     1e9f};
    patterns::ScanResult r{};
    if (!patterns::run_object_scan(g_weaponScan, patterns::kPlayerWeaponVtableRva,
                                   patterns::kWeaponOwnerOffset + sizeof(void*), &accept_weapon,
                                   &wc, r))
        return nullptr; // sliced sweep still running - no stall, retry next frame

    // This resolver picks through WeaponScanCtx::best rather than by accepting a
    // candidate, so ScanResult::object is ALWAYS null here. Reporting that as
    // "chosen=00000000" is what made the shipped log unreadable: an external
    // crash log could not tell whether the resolver had succeeded or failed
    // (session 27). Log the real outcome.
    patterns::log_scan_result("APlayerWeapon", g_weaponScan, r, probeOnly);
    g_lastMatches.store(r.matches, std::memory_order_relaxed);
    BVR_LOG("[hands] player weapon resolver: %d match(es) -> %s @ %p%s", r.matches,
            wc.best ? (wc.bestDist == 0.0f ? "ATTACHED to the rig" : "nearest to the gun spot")
                    : "NONE",
            wc.best, wc.best && wc.bestDist > 0.0f ? " (distance pick)" : "");
    if (probeOnly) return nullptr;
    g_weaponActor = wc.best;
    return g_weaponActor;
}

void load_config();
void save_config();

void log_status() {
    int mode = g_mode.load(std::memory_order_relaxed);
    BVR_LOG("[hands] status: %s | mode=%s pose=%s | hand=%s | writes=%u",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : "BONES",
            g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
            g_handMode.load(std::memory_order_relaxed) == 0 ? "LEFT"
            : g_handMode.load(std::memory_order_relaxed) == 1
                ? "RIGHT"
                : (g_autoHand.load(std::memory_order_relaxed) == 0 ? "auto(L)" : "auto(R)"),
            g_writes.load(std::memory_order_relaxed));
    BVR_LOG("[hands]   weapon actor=%p (learned %p) | hands actor=%p (matches %d)",
            g_weaponActor, bvr::b1r::aim::learned_weapon_object(), g_handsActor,
            g_lastMatches.load(std::memory_order_relaxed));
    for (int h = 0; h < 2; ++h) {
        BVR_LOG("[hands]   offset %s pos fwd%+.1f right%+.1f up%+.1f cm | trim pitch%+.1f "
                "yaw%+.1f roll%+.1f deg%s",
                h == 0 ? "L" : "R",
                g_posFwdCm[h].load(std::memory_order_relaxed),
                g_posRightCm[h].load(std::memory_order_relaxed),
                g_posUpCm[h].load(std::memory_order_relaxed),
                g_rotPitchDeg[h].load(std::memory_order_relaxed),
                g_rotYawDeg[h].load(std::memory_order_relaxed),
                g_rotRollDeg[h].load(std::memory_order_relaxed),
                h == 1 ? (g_writeRot.load(std::memory_order_relaxed) ? " | writeRot=1"
                                                                     : " | writeRot=0")
                       : "");
    }
    uint64_t now = GetTickCount64();
    BVR_LOG("[hands]   last write loc=(%.1f %.1f %.1f) rot=(%d %d %d) testHold=%dms "
            "simHold=%dms",
            g_lastX.load(std::memory_order_relaxed), g_lastY.load(std::memory_order_relaxed),
            g_lastZ.load(std::memory_order_relaxed),
            g_lastPitch.load(std::memory_order_relaxed),
            g_lastYaw.load(std::memory_order_relaxed),
            g_lastRoll.load(std::memory_order_relaxed),
            g_test.deadline > now ? static_cast<int>(g_test.deadline - now) : 0,
            g_sim.deadline > now ? static_cast<int>(g_sim.deadline - now) : 0);
}

// ---- persistence -----------------------------------------------------------
// Every weapon model sits differently in the hand, so these numbers are found
// by eye in the headset and must survive the session that found them. Plain
// key=value text in the mod's own data dir - no new dependency, and the user
// can read it.

void config_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\hands.ini", bvr::log::data_dir());
}

void save_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[hands] could not write hands.ini");
        return;
    }
    fprintf(f, "# BioShock VR - M7 viewmodel offsets (cm / degrees, model-local frame)\n");
    fprintf(f, "# Per-hand keys: ...L = left (plasmid), ...R = right (weapon). A legacy\n");
    fprintf(f, "# suffix-less key (posFwdCm=...) still loads and applies to BOTH hands.\n");
    fprintf(f, "mode=%d\n", g_mode.load(std::memory_order_relaxed));
    fprintf(f, "aimPose=%d\n", g_useAimPose.load(std::memory_order_relaxed) ? 1 : 0);
    for (int h = 0; h < 2; ++h) {
        const char* s = h == 0 ? "L" : "R";
        fprintf(f, "posFwdCm%s=%.2f\n", s, g_posFwdCm[h].load(std::memory_order_relaxed));
        fprintf(f, "posRightCm%s=%.2f\n", s, g_posRightCm[h].load(std::memory_order_relaxed));
        fprintf(f, "posUpCm%s=%.2f\n", s, g_posUpCm[h].load(std::memory_order_relaxed));
        fprintf(f, "rotPitchDeg%s=%.2f\n", s, g_rotPitchDeg[h].load(std::memory_order_relaxed));
        fprintf(f, "rotYawDeg%s=%.2f\n", s, g_rotYawDeg[h].load(std::memory_order_relaxed));
        fprintf(f, "rotRollDeg%s=%.2f\n", s, g_rotRollDeg[h].load(std::memory_order_relaxed));
    }
    fclose(f);
    BVR_LOG("[hands] offsets saved to hands.ini");
}

// "posFwdCmL"/"posFwdCmR" store one hand; the legacy suffix-less "posFwdCm"
// (pre-per-hand ini files) stores BOTH, so an old hands.ini keeps working.
bool store_hand_key(const char* key, const char* base, std::atomic<float> (&dst)[2], float v) {
    size_t n = strlen(base);
    if (strncmp(key, base, n) != 0) return false;
    if (key[n] == '\0') {
        dst[0].store(v, std::memory_order_relaxed);
        dst[1].store(v, std::memory_order_relaxed);
        return true;
    }
    if ((key[n] == 'L' || key[n] == 'R') && key[n + 1] == '\0') {
        dst[key[n] == 'R' ? 1 : 0].store(v, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void load_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file yet is normal
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[64] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
            continue;
        ++n;
        if (strcmp(key, "mode") == 0) {
            int m = static_cast<int>(v);
            g_mode.store(m < 0 ? 0 : m > 2 ? 2 : m, std::memory_order_relaxed);
        }
        else if (strcmp(key, "aimPose") == 0) g_useAimPose.store(v != 0.0f, std::memory_order_relaxed);
        else if (store_hand_key(key, "posFwdCm", g_posFwdCm, v)) {}
        else if (store_hand_key(key, "posRightCm", g_posRightCm, v)) {}
        else if (store_hand_key(key, "posUpCm", g_posUpCm, v)) {}
        else if (store_hand_key(key, "rotPitchDeg", g_rotPitchDeg, v)) {}
        else if (store_hand_key(key, "rotYawDeg", g_rotYawDeg, v)) {}
        else if (store_hand_key(key, "rotRollDeg", g_rotRollDeg, v)) {}
        else --n;
    }
    fclose(f);
    if (n) BVR_LOG("[hands] loaded %d value(s) from hands.ini", n);
}

} // namespace

// BioShock holds ONE thing at a time, so the viewmodel belongs to whichever
// hand the player last engaged. Two ways to engage, both latched here from
// the state the bridge itself composes (same source as aim.cpp's object map):
//   - the TRIGGERS (fire = switch-and-fire, XENON_RT/LT), and
//   - the BUMPERS (the grips compose to LB/RB, and a bumper press switches
//     the raised hand with NO trigger event - the M8 grip-switch bug was the
//     latch only learning from triggers, so a grip switch left the model on
//     the stale controller until the next trigger pull).
// Bumpers are checked first so a same-frame trigger wins (firing is the
// stronger evidence of which hand the player means). Shared with the aim
// laser so the beam leaves the hand that is actually holding the weapon.
int active_hand() {
    int mode = g_handMode.load(std::memory_order_relaxed);
    if (mode == 0 || mode == 1) return mode;

    bool lb = false, rb = false;
    bvr::input::last_composed_bumpers(&lb, &rb);
    if (rb && !lb) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lb && !rb) g_autoHand.store(0, std::memory_order_relaxed);

    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    if (rt >= 64 && lt < 64) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lt >= 64 && rt < 64) g_autoHand.store(0, std::memory_order_relaxed);
    return g_autoHand.load(std::memory_order_relaxed);
}

void* hands_actor() {
    return g_handsActor;
}

void* weapon_actor() {
    // Primary (session 21 part 2): read Hands.CurrentHoldable straight off
    // the rig - THE equipped weapon by definition, updated by the engine at
    // equip time. The old learned/cache preference pinned the resolver to
    // the previously FIRED weapon across wheel switches (an unequipped
    // weapon keeps its vtable and owner), which broke per-weapon profile
    // swapping in the first headset run.
    if (has_vtable(g_handsActor, patterns::kHandsVtableRva)) {
        void* hold = nullptr;
        if (read_ptr(static_cast<const uint8_t*>(g_handsActor) +
                         patterns::kHandsCurrentHoldableOffset,
                     &hold) &&
            weapon_valid(hold)) {
            g_weaponActor = hold;
            return hold;
        }
    }
    void* w = weapon_valid(g_weaponActor) ? g_weaponActor
                                          : bvr::b1r::aim::learned_weapon_object();
    return weapon_valid(w) ? w : nullptr;
}

void* resolve_weapon_actor(const FrameContext& ctx) {
    return find_weapon_actor(ctx, false);
}

bool weapon_scan_in_progress() { return g_weaponScan.sweeping; }

bool current_holdable(void** out) {
    // Raw rig read, CLASS-AGNOSTIC: the MachineGun and GrenadeLauncher carry
    // a different native vtable than kPlayerWeaponVtableRva, so the
    // vtable-gated weapon_actor() path rejected them and fell back to the
    // stale cached weapon - the session-21 part-3 defect (their profile key
    // never changed and edits landed in the previous weapon's profile). The
    // profile layer keys on the CLASS NAME, which validates through the
    // UClass vtable instead - any holdable class resolves. Returns false
    // when the rig itself is unknown/unreadable (callers then fall back to
    // the legacy paths); *out may be null (nothing equipped).
    if (!has_vtable(g_handsActor, patterns::kHandsVtableRva)) return false;
    void* hold = nullptr;
    if (!read_ptr(static_cast<const uint8_t*>(g_handsActor) +
                      patterns::kHandsCurrentHoldableOffset,
                  &hold))
        return false;
    *out = hold;
    return true;
}

// Live mesh-alignment trim, read by `vraim synccheck` so its model chain runs
// on the REAL tuned values (session 20).
float model_trim_pitch_deg(int hand) {
    return g_rotPitchDeg[hand & 1].load(std::memory_order_relaxed);
}
float model_trim_yaw_deg(int hand) {
    return g_rotYawDeg[hand & 1].load(std::memory_order_relaxed);
}
float model_trim_roll_deg(int hand) {
    return g_rotRollDeg[hand & 1].load(std::memory_order_relaxed);
}

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    load_config();
    int mode = g_mode.load(std::memory_order_relaxed);
    BVR_LOG("[hands] init: mode=%s (AHands vtable 0x%X, APlayerWeapon vtable 0x%X)",
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : "BONES", patterns::kHandsVtableRva,
            patterns::kPlayerWeaponVtableRva);
}

void on_calcview(const FrameContext& ctx) {
    // Overlay request, applied from THIS thread (same rule as aim.cpp: the
    // render thread must never touch engine state directly).
    int pending = g_pendingEnable.exchange(-1, std::memory_order_relaxed);
    if (pending == 1) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_handsScanFails = 0; // re-enable: scan promptly
        BVR_LOG("[hands] ON (overlay) - viewmodel follows the controller");
    } else if (pending == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF (overlay) - engine placement restored");
    }

    // World change: the old actors died with the old world, and recycled heap
    // addresses must never be written to. The scale bookkeeping is dropped, not
    // restored - restore would write into a stranger.
    if (ctx.pc != g_lastPc) {
        if (g_lastPc && (g_handsActor || g_weaponActor))
            BVR_LOG("[hands] world changed - actor caches cleared");
        g_lastPc = ctx.pc;
        g_handsActor = nullptr;
        g_weaponActor = nullptr;
        g_lastHandsScanMs = 0;
        g_lastWeaponScanMs = 0;
        g_handsScanFails = 0; // new world: scan promptly again
        bones::on_world_change();
    }

    // One-shot probe: describe every instance of both classes, choose none.
    if (g_probeLeft > 0) {
        --g_probeLeft;
        find_hands_actor(ctx, true);
        find_weapon_actor(ctx, true);
    }

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Cutscene guard, same predicate the aim ray uses: during normal play the
    // view actor is the player's own pawn.
    bool gameplayView = false;
    if (ctx.viewActor) {
        void* vtbl = nullptr;
        if (read_ptr(ctx.viewActor, &vtbl))
            gameplayView = (to_rva(vtbl) == patterns::kShockPlayerVtableRva);
        if (!gameplayView && ctx.viewActor == ctx.pc) gameplayView = true;
    }
    // Session 29: the cinematic gate, made EXPLICIT. Until now the hands
    // stopped during a cutscene only as a side effect of ctx.vrDriving going
    // false with the head drive - an accident, not a contract, and one that
    // authored+look breaks by design (it drives the head again, which would
    // hand the controllable rig straight back over the authored animation).
    if (gameplayView && bvr::hud::cinematic_hold() &&
        bvr::vr::cine_drive() != bvr::vr::CineDrive::Off) {
        gameplayView = false;
        // Release HERE, not only on the cinematic entry edge. Measured in
        // headset (session 29): switching drive mode to `off` mid-cutscene
        // resumes the drive, which collapses the inactive hand - and switching
        // back gates the only code that can restore it, because
        // restore_hidden() lives inside drive(). The hand then stays collapsed
        // for the rest of the scene (log: hiddenHand=0 cacheAge=32578ms at the
        // exit edge, 32.5 s being exactly the moment the gate re-closed).
        // Releasing where the suppression happens closes that by construction,
        // and release() is idempotent - it self-limits to one real pass.
        bones::release("hands gated for cinematic");
    }

    if (!gameplayView) return;

    bool gunMode = g_mode.load(std::memory_order_relaxed) == 0;
    if (gunMode) {
        // Live-proven dead end kept only as a future detach experiment: the
        // renderer draws an ATTACHED weapon from its attachment matrix, so
        // writing the weapon actor's own transform changes nothing (and the
        // 2026-07-25 evening session suggests it can desync the attach state).
        // Refuse rather than write.
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[hands] gun mode is inert on this engine (attached weapons render "
                    "from the attach matrix) - use mode hands");
        }
        return;
    }
    void* target = find_hands_actor(ctx, false);
    if (!target) return;

    // Where the model goes. The hand is resolved once and used for the pose,
    // the aim trim, and both per-hand model offsets below.
    const int hand = active_hand();
    GamePose gp{};
    uint64_t now = GetTickCount64();
    if (now < g_test.deadline) {
        // Camera-relative lane: proves the write lands, no pose math involved.
        gp.rot.yaw = ctx.camYaw + static_cast<int32_t>(g_test.yawDeg * kRotUnitsPerDegree);
        gp.rot.pitch = ctx.camPitch + static_cast<int32_t>(g_test.pitchDeg * kRotUnitsPerDegree);
        gp.rot.roll = 0;
        float dir[3];
        ue_rot_to_dir(gp.rot, dir);
        gp.loc = {ctx.camX + dir[0] * g_test.distUu, ctx.camY + dir[1] * g_test.distUu,
                  ctx.camZ + dir[2] * g_test.distUu};
    } else {
        float pos[3], quat[4];
        FrameContext mapCtx = ctx;
        if (now < g_sim.deadline) {
            // Synthetic XR pose through the REAL mapping path: a fixed spot a
            // hand would occupy, oriented by the sim angles.
            pos[0] = 0.15f;  // meters right of the recenter origin
            pos[1] = -0.20f; // below it
            pos[2] = -0.35f; // in front (XR forward is -Z)
            xr_local_trim_quat(g_sim.pitchDeg / kRadToDeg, g_sim.yawDeg / kRadToDeg,
                               g_sim.rollDeg / kRadToDeg, quat);
            // POSITION zero only. The recenter YAW is deliberately left alone
            // (session 17): forcing it to 0 here made the parked synthetic hand
            // BODY-locked while a real controller is RECENTER-locked, so with
            // the M7.5 yaw transfer armed the parked gun swung by the full head
            // angle - indistinguishable by eye from the sessions-12-16
            // head-coupling defect, but a pure artifact of this line. It was a
            // no-op before the transfer existed: every flat baseline arms
            // `simhead 0 0 0` first, which sets the recenter yaw to exactly 0.
            mapCtx.recenterPx = mapCtx.recenterPy = mapCtx.recenterPz = 0.0f;
        } else {
            bvr::vr::HeadPose hp{};
            bool aimPose = g_useAimPose.load(std::memory_order_relaxed);
            if (!ctx.vrDriving || !bvr::vr::get_hand_pose(hand, aimPose, hp)) return;
            pos[0] = hp.px;
            pos[1] = hp.py;
            pos[2] = hp.pz;
            quat[0] = hp.qx;
            quat[1] = hp.qy;
            quat[2] = hp.qz;
            quat[3] = hp.qw;
            if (bvr::b1r::bones::telemetry_on()) {
                static uint64_t lastTlm = 0;
                if (now - lastTlm >= 200) {
                    lastTlm = now;
                    BVR_LOG("[tlm] ctrl%d xr p=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f) "
                            "pose=%s",
                            hand, hp.px, hp.py, hp.pz, hp.qx, hp.qy, hp.qz, hp.qw,
                            aimPose ? "aim" : "grip");
                }
            }
        }

        // Mesh-alignment trim (per hand), composed in the controller's local
        // frame so it holds at EVERY controller orientation. The chain is a
        // pure function in frame_context.h, shared with `vraim synccheck`.
        gp = model_pose_from_xr(mapCtx, pos, quat,
                                g_rotPitchDeg[hand].load(std::memory_order_relaxed),
                                g_rotYawDeg[hand].load(std::memory_order_relaxed),
                                g_rotRollDeg[hand].load(std::memory_order_relaxed));
        // The aim calibration trim is deliberately NOT applied to the model
        // (session 18 part 3): re-trimming the ray must not move the tuned
        // model. The legacy `aligntrim` euler coupling was DELETED in session
        // 20 - euler adds after conversion were the wrong algebra everywhere
        // but the tuning pose, and the unification left nothing for it to do.
    }

    // Position offset rides the final (trimmed) frame: "2 cm forward" means
    // along the barrel as finally oriented.
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    float uuPerCm = ctx.worldScale / 100.0f;
    float of = g_posFwdCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float orr = g_posRightCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float ou = g_posUpCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float loc[3] = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
                    gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
                    gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};

    if (g_mode.load(std::memory_order_relaxed) == 2) {
        // BONES (M7-v2): the actor stays engine-placed (eye anchor, correct
        // culling, correct engine-side FX anchoring) and the hand CLUSTER
        // moves to the controller instead.
        gp.loc = {loc[0], loc[1], loc[2]};
        // The weapon-scale lane rides the same per-frame slot (session 61);
        // it no-ops at wscale 1.0 and drops itself on weapon switches.
        bones::wskel_drive();
        if (!bones::drive(ctx, target, gp, hand)) return;
    } else {
        uint8_t* p = static_cast<uint8_t*>(target);
        bool wrote = write12(p + patterns::kActorLocOffset, loc);
        if (g_writeRot.load(std::memory_order_relaxed)) {
            int32_t rot[3] = {gp.rot.pitch, gp.rot.yaw, gp.rot.roll};
            wrote = write12(p + patterns::kActorViewDirOffset, rot) || wrote;
        }
        if (!wrote) {
            g_handsActor = nullptr; // the write faulted - stop trusting this pointer
            return;
        }
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastX.store(loc[0], std::memory_order_relaxed);
    g_lastY.store(loc[1], std::memory_order_relaxed);
    g_lastZ.store(loc[2], std::memory_order_relaxed);
    g_lastPitch.store(gp.rot.pitch, std::memory_order_relaxed);
    g_lastYaw.store(gp.rot.yaw, std::memory_order_relaxed);
    g_lastRoll.store(gp.rot.roll, std::memory_order_relaxed);
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        log_status();
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_handsScanFails = 0; // re-enable: scan promptly
        BVR_LOG("[hands] ON - viewmodel follows the controller");
        log_status();
    } else if (strcmp(verb, "off") == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF - engine placement restored");
    } else if (strcmp(verb, "mode") == 0) {
        int mode = strncmp(rest, "gun", 3) == 0     ? 0
                   : strncmp(rest, "hands", 5) == 0 ? 1
                                                    : 2;
        g_mode.store(mode, std::memory_order_relaxed);
        BVR_LOG("[hands] mode = %s",
                mode == 0   ? "GUN (inert - renderer ignores an attached weapon's actor fields)"
                : mode == 1 ? "HANDS (actor pinning - retired, kept for A/B)"
                            : "BONES (M7-v2: hand cluster follows the controller)");
    } else if (strcmp(verb, "pose") == 0) {
        bool aim = strncmp(rest, "aim", 3) == 0;
        g_useAimPose.store(aim, std::memory_order_relaxed);
        BVR_LOG("[hands] pose source = %s", aim ? "AIM (matches laser + bullets)"
                                                : "GRIP (physical hand axis)");
    } else if (strcmp(verb, "scale") == 0) {
        // "scale [l|r|both] <f>" - no side = both hands. Session 61: the
        // lever the s16 dead ends never tested (bones.h set_scale).
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 2;
        } else if (strncmp(rest, "both", 4) == 0) {
            nums = rest + 4;
        }
        float f = 0.0f;
        if (sscanf_s(nums, "%f", &f) == 1 && f > 0.0f) {
            bones::set_scale(side, f);
            BVR_LOG("[hands] scale %s = %.3f (L=%.3f R=%.3f; 1.0 = authored)",
                    side < 0 ? "both" : side == 1 ? "right" : "left", f, bones::scale(0),
                    bones::scale(1));
        } else {
            BVR_LOG("[hands] usage: vrhands scale [l|r|both] <f> (current L=%.3f R=%.3f; "
                    "probe mode via vrbones scalemode)",
                    bones::scale(0), bones::scale(1));
        }
    } else if (strcmp(verb, "wscale") == 0) {
        float f = 0.0f;
        if (sscanf_s(rest, "%f", &f) == 1 && f > 0.0f) {
            bones::set_weapon_scale(f);
            BVR_LOG("[hands] weapon scale = %.3f (1.0 = authored, lane drops itself)",
                    bones::weapon_scale());
        } else {
            BVR_LOG("[hands] usage: vrhands wscale <f> (current %.3f; uniform about the "
                    "grip, per-frame drive of the holdable's own skeleton)",
                    bones::weapon_scale());
        }
    } else if (strcmp(verb, "probe") == 0) {
        int n = 1;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 1;
        if (n > 30) n = 30;
        g_probeLeft = n;
        BVR_LOG("[hands] probe armed for %d frame(s) - listing AHands + player weapons", n);
    } else if (strcmp(verb, "hand") == 0) {
        int mode = rest[0] == 'l' ? 0 : rest[0] == 'r' ? 1 : 2;
        g_handMode.store(mode, std::memory_order_relaxed);
        BVR_LOG("[hands] hand = %s", mode == 0 ? "LEFT" : mode == 1 ? "RIGHT" : "auto");
    } else if (strcmp(verb, "pos") == 0) {
        // "pos [l|r] <fwd> <right> <up>" - no side = both hands (the legacy
        // form, kept so the acceptance harness and old scripts still work).
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(nums, "%f %f %f", &f, &r, &u) == 3) {
            for (int h = 0; h < 2; ++h) {
                if (side >= 0 && h != side) continue;
                g_posFwdCm[h].store(f, std::memory_order_relaxed);
                g_posRightCm[h].store(r, std::memory_order_relaxed);
                g_posUpCm[h].store(u, std::memory_order_relaxed);
            }
            BVR_LOG("[hands] pos offset (%s) fwd%+.1f right%+.1f up%+.1f cm",
                    side < 0 ? "both" : side == 1 ? "right" : "left", f, r, u);
        } else {
            BVR_LOG("[hands] usage: vrhands pos [l|r] <fwdCm> <rightCm> <upCm>");
        }
    } else if (strcmp(verb, "rot") == 0) {
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float p = 0.0f, y = 0.0f, r = 0.0f;
        if (sscanf_s(nums, "%f %f %f", &p, &y, &r) == 3) {
            for (int h = 0; h < 2; ++h) {
                if (side >= 0 && h != side) continue;
                g_rotPitchDeg[h].store(p, std::memory_order_relaxed);
                g_rotYawDeg[h].store(y, std::memory_order_relaxed);
                g_rotRollDeg[h].store(r, std::memory_order_relaxed);
            }
            BVR_LOG("[hands] rot trim (%s) pitch%+.1f yaw%+.1f roll%+.1f deg",
                    side < 0 ? "both" : side == 1 ? "right" : "left", p, y, r);
        } else {
            BVR_LOG("[hands] usage: vrhands rot [l|r] <pitchDeg> <yawDeg> <rollDeg>");
        }
    } else if (strcmp(verb, "writerot") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_writeRot.store(on, std::memory_order_relaxed);
        BVR_LOG("[hands] rotation write %s", on ? "ON" : "off (position only)");
    } else if (strcmp(verb, "fname") == 0) {
        // Session 20: the name-system gate. `fname <idx>` resolves any name
        // index; `fname weapon` reads the cached weapon actor's attach-bone
        // FName (+0xF0 {index, number}) - the stage-4 acceptance.
        if (strncmp(rest, "weapon", 6) == 0) {
            void* w = weapon_valid(g_weaponActor) ? g_weaponActor
                                                  : bvr::b1r::aim::learned_weapon_object();
            if (!weapon_valid(w)) {
                BVR_LOG("[hands] fname: no live weapon actor (fire once so the aim seam "
                        "learns it)");
                return;
            }
            const int32_t* nm = reinterpret_cast<const int32_t*>(
                static_cast<uint8_t*>(w) + patterns::kActorAttachBoneNameOffset);
            const wchar_t* t = patterns::fname_text(nm[0]);
            BVR_LOG("[hands] weapon attach-bone FName idx=%d num=%d -> '%S' "
                    "(GNames count %d)",
                    nm[0], nm[1], t ? t : L"<unresolved>", patterns::fname_count());
        } else {
            int idx = 0;
            if (sscanf_s(rest, "%d", &idx) == 1) {
                const wchar_t* t = patterns::fname_text(idx);
                BVR_LOG("[hands] fname %d -> '%S' (GNames count %d)", idx,
                        t ? t : L"<unresolved>", patterns::fname_count());
            } else {
                BVR_LOG("[hands] usage: vrhands fname <index>|weapon");
            }
        }
    } else if (strcmp(verb, "swaykill") == 0) {
        if (strncmp(rest, "status", 6) == 0)
            BVR_LOG("[hands] swaykill %s", bones::sway_kill() ? "ON" : "off");
        else
            bones::set_sway_kill(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "hideinactive") == 0) {
        bones::set_hide_inactive(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "save") == 0) {
        save_config();
    } else if (strcmp(verb, "reload") == 0) {
        load_config();
        log_status();
    } else if (strcmp(verb, "test") == 0) {
        float yaw = 0.0f, pitch = 0.0f, dist = 60.0f;
        int hold = 0;
        int n = sscanf_s(rest, "%f %f %f %d", &yaw, &pitch, &dist, &hold);
        if (n < 2) {
            BVR_LOG("[hands] usage: vrhands test <yawDeg> <pitchDeg> [distUU] [holdMs]");
            return;
        }
        if (n < 3 || dist <= 0.0f) dist = 60.0f;
        if (hold <= 0) hold = 30000;
        if (hold > 120000) hold = 120000;
        g_test.yawDeg = yaw;
        g_test.pitchDeg = pitch;
        g_test.distUu = dist;
        g_test.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[hands] test placement: yaw %+.1f pitch %+.1f dist %.0f UU for %d ms", yaw,
                pitch, dist, hold);
    } else if (strcmp(verb, "simpose") == 0) {
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        int hold = 0;
        int n = sscanf_s(rest, "%f %f %f %d", &yaw, &pitch, &roll, &hold);
        if (n < 3) {
            BVR_LOG("[hands] usage: vrhands simpose <yawDeg> <pitchDeg> <rollDeg> [holdMs]");
            return;
        }
        if (hold <= 0) hold = 30000;
        if (hold > 120000) hold = 120000;
        g_sim.yawDeg = yaw;
        g_sim.pitchDeg = pitch;
        g_sim.rollDeg = roll;
        g_sim.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[hands] sim pose: yaw %+.1f pitch %+.1f roll %+.1f for %d ms (real mapping "
                "path, synthetic controller)",
                yaw, pitch, roll, hold);
    } else if (strcmp(verb, "testclear") == 0) {
        g_test.deadline = 0;
        g_sim.deadline = 0;
        BVR_LOG("[hands] test + sim placements cleared");
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else {
        BVR_LOG("[hands] unknown command '%s' (on|off|mode gun|hands|pose aim|grip|scale|"
                "probe|hand|pos|rot|writerot|hideinactive|save|reload|test|simpose|"
                "testclear|status)",
                verb);
    }
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) &&
           (g_weaponActor != nullptr || g_handsActor != nullptr);
}

void save_offsets() {
    save_config();
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Hands + weapon (M7)")) return;

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Viewmodel follows the controller", &on))
        g_pendingEnable.store(on ? 1 : 0, std::memory_order_relaxed);

    bool aimPose = g_useAimPose.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Align to the AIM ray (matches laser; off = grip pose)", &aimPose))
        g_useAimPose.store(aimPose, std::memory_order_relaxed);

    int hand = g_handMode.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("left", &hand, 0)) g_handMode.store(0, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("right", &hand, 1)) g_handMode.store(1, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("auto", &hand, 2)) g_handMode.store(2, std::memory_order_relaxed);

    // The six sliders below edit ONE hand's offsets - the selector picks
    // which (in-headset tuning wants one set of sliders, not twelve).
    // Separate from the drive-hand radio above: that picks which controller
    // OWNS the viewmodel, this picks which hand's numbers the sliders show.
    static int tuneHand = 1; // start on the weapon hand
    ImGui::Text("Tuning hand:");
    ImGui::SameLine();
    ImGui::RadioButton("L (plasmid)", &tuneHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("R (weapon)", &tuneHand, 1);

    float f = g_posFwdCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset forward (cm)", &f, -120.0f, 120.0f))
        g_posFwdCm[tuneHand].store(f, std::memory_order_relaxed);
    float r = g_posRightCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset right (cm)", &r, -120.0f, 120.0f))
        g_posRightCm[tuneHand].store(r, std::memory_order_relaxed);
    float u = g_posUpCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset up (cm)", &u, -120.0f, 120.0f))
        g_posUpCm[tuneHand].store(u, std::memory_order_relaxed);

    float rp = g_rotPitchDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim pitch (deg)", &rp, -90.0f, 90.0f))
        g_rotPitchDeg[tuneHand].store(rp, std::memory_order_relaxed);
    float ry = g_rotYawDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim yaw (deg)", &ry, -90.0f, 90.0f))
        g_rotYawDeg[tuneHand].store(ry, std::memory_order_relaxed);
    float rr = g_rotRollDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim roll (deg)", &rr, -180.0f, 180.0f))
        g_rotRollDeg[tuneHand].store(rr, std::memory_order_relaxed);

    if (ImGui::Button("Save offsets")) save_config();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) load_config();

    bones::draw_debug_ui();

    ImGui::Text("weapon %p | hands %p | writes %u", g_weaponActor, g_handsActor,
                g_writes.load(std::memory_order_relaxed));
    ImGui::Text("last loc (%.0f %.0f %.0f) rot (%d %d %d)",
                g_lastX.load(std::memory_order_relaxed),
                g_lastY.load(std::memory_order_relaxed),
                g_lastZ.load(std::memory_order_relaxed),
                g_lastPitch.load(std::memory_order_relaxed),
                g_lastYaw.load(std::memory_order_relaxed),
                g_lastRoll.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::hands
