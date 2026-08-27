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
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scripted.h"

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
// Same seam for the drive mode. It cannot be a bare store from the overlay:
// leaving mode 2 has to call bones::release(), which writes the skeleton, and
// the render thread must never touch engine state directly.
std::atomic<int> g_pendingMode{-1};
// s67: does the grip offset ride a basis that includes ROLL? See the long note
// at the use site - BRVR found that rotating the offset by a roll the mesh
// never rendered with "swung the hand through an arc that grew with the twist".
// True = today's behaviour.
std::atomic<bool> g_offsetRoll{true};

// ---- s67: PLACEMENT, SEPARATED FROM THE PIVOT ------------------------------
//
// The grip offset cannot do both jobs, and trying to make it costs a headset
// run every time. The algebra is short enough to keep in view:
//
//     gun renders at   gp.loc + R(theta) * (G - gripOffset)
//
// so gripOffset decides WHICH mesh point is stationary - it IS the pivot. Move
// it to correct the gun's height and you displace the pivot by the same amount,
// which turns into an orbit of that size the moment the wrist rotates. Measured
// s67: BRVR's values gave a correct pivot ("rotating in place correctly") with
// the gun 6 in low; shifting them 15 cm to fix the height bought the height and
// produced an 8 in orbit. Both observations are the same equation.
//
// THIS offset is applied in the VIEW frame instead, after the model transform,
// so it does not rotate with the wrist and cannot create a lever. It translates
// the whole rig rigidly: height, reach and lateral placement, with the pivot
// left exactly where the grip offset put it.
//
// Two knobs, two jobs: gripOffset = where it pivots, viewOffset = where it sits.
//
// s68: PER HAND, and that was a bug fix, not a tidy-up. This was one global
// triple applied to BOTH hands while apply_weapon_key() rewrote it from the
// RIGHT hand's weapon profile on every weapon change. Plasmids are the left
// hand and have no weapon profile, so nothing ever put it back: switching to a
// gun and back left both plasmids sitting at the gun's placement. The tester's
// report names the shape exactly - "correct as I switch back and forth between
// the plasmids, but then they BOTH get locked to another position when I switch
// to a weapon and switch back". Both, because one global served both hands.
std::atomic<float> g_viewFwdCm[2]{0.0f, 0.0f}, g_viewRightCm[2]{0.0f, 0.0f},
    g_viewUpCm[2]{0.0f, 0.0f};

// ---- s67 LATE ROTATION WRITE (BRVR S60, ported) --------------------------
// BRVR's S59 readback measured the game tick running AFTER its CalcView write
// and resetting the hands rotator: pitch and yaw held to a fraction of a
// degree, but ROLL was erased by 5 to 102 degrees, scaling with wrist twist.
// Its S60 fix is to write the rotator AGAIN later in the frame, past the tick.
//
// Confirmed on THIS machine, 2026-08-25: with BRVR installed for the A/B, its
// own readback logged `r=0.0 deg since our write` on every line for the whole
// run. Roll survives there, and the viewmodel is flawless. This tree had no
// equivalent, so its roll is the channel being eaten.
//
// BRVR calls this from Present (render thread) and pays for it with a cached
// pointer and a re-validation. We do NOT have to: scenedraw's build detour is
// the GAME thread, fires after CalcView, and runs before the frame is built -
// the same position in the frame without the cross-thread hazard. That is also
// where camera.cpp already does its stale-FOV restore, for the same reason.
void* g_lwObj = nullptr;
int32_t g_lwRot[3] = {0, 0, 0};
std::atomic<bool> g_lwValid{false};
std::atomic<bool> g_lateWrite{true}; // the fix; toggleable so it stays bisectable
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
// The RIGHT hand defaults are ZERO on purpose. The ideal pivot correction (pull
// the mesh's gun to the controller, ~-100 cm forward) is CULLED: the engine drops
// the whole rig the moment the actor origin goes behind the camera (live-proven -
// the rig vanished with the origin 32 UU back). Forward pull is therefore limited
// to roughly the controller's own distance from the face, and where that line
// sits is the user's in-headset call, not a default. The right hand is also the
// one the per-weapon profiles drive, so a default here would only ever be the
// value held for the half-second before the first weapon resolves.
//
// The LEFT hand is different on both counts, so s68 ships the tester's calibrated
// PLASMID pose as the default: nothing per-weapon drives the left hand (there are
// no per-plasmid profiles), so without a default it sits at zero until someone
// tunes it by hand. These are the values from the tester's own hands.ini, taken
// 2026-08-27 after the s68 headset run.
std::atomic<float> g_posFwdCm[2]{45.50f, 0.0f}, g_posRightCm[2]{-14.90f, 0.0f},
    g_posUpCm[2]{-12.30f, 0.0f};
std::atomic<float> g_rotPitchDeg[2]{-111.00f, 0.0f}, g_rotYawDeg[2]{-16.00f, 0.0f},
    g_rotRollDeg[2]{22.00f, 0.0f};

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
        // NO PAWN, NO RIG - so do not go looking for one. The gameplayView
        // that gets us here has a `viewActor == pc` escape hatch, and the main
        // menu is exactly that shape, so the drive arms on the menu and sweeps
        // the heap for an actor that cannot exist. Measured 2026-08-22: a
        // sweep started on the menu spent 5237 ms across 1245 slices and did
        // not finish until 30 s after the level had loaded; with the menu up
        // long enough it starves the frame (presents 3/s, input drive 15/s)
        // and reads as a dead controller. body::is_gameplay_view is the STRICT
        // predicate that already gates the head drive and the FOV write, and
        // is documented to read false on the menu attract scene.
        //
        // Only the START of a sweep is gated. A sweep already in flight keeps
        // its slices - the existing rule below, for the same reason: slices
        // spread minutes apart never finish.
        if (!g_handsScan.sweeping && !body::is_gameplay_view(ctx.viewActor)) return nullptr;
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
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : mode == 3 ? "BRVR" : "BONES",
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
        // View-frame placement: moves the rig without moving the pivot (see the
        // banner). Per hand since s68 - a suffix-less key from an older file
        // still loads into BOTH, which is the right migration: it is exactly
        // what the shared global used to mean.
        fprintf(f, "viewFwdCm%s=%.2f\n", s, g_viewFwdCm[h].load(std::memory_order_relaxed));
        fprintf(f, "viewRightCm%s=%.2f\n", s,
                g_viewRightCm[h].load(std::memory_order_relaxed));
        fprintf(f, "viewUpCm%s=%.2f\n", s, g_viewUpCm[h].load(std::memory_order_relaxed));
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
            g_mode.store(m < 0 ? 0 : m > 3 ? 3 : m, std::memory_order_relaxed);
        }
        else if (strcmp(key, "aimPose") == 0) g_useAimPose.store(v != 0.0f, std::memory_order_relaxed);
        else if (store_hand_key(key, "viewFwdCm", g_viewFwdCm, v)) {}
        else if (store_hand_key(key, "viewRightCm", g_viewRightCm, v)) {}
        else if (store_hand_key(key, "viewUpCm", g_viewUpCm, v)) {}
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


// Is ANYTHING in your hands - a weapon or a plasmid? Ported from the BRVR mod
// (Hands/HandsProbe.cpp `g_handsArmed`, read by Render/XRSession.cpp's crosshair
// gate), including both of its non-obvious rules.
//
// OR, NOT AND. Mid-equip one pointer is briefly null while the other has already
// been written, so requiring both would blink "unarmed" on every weapon switch.
// BRVR's ability-MODE test is the strict one - `ability && !holdable` - and that
// is a different question from this one. A plasmid counts as armed: you aim
// those.
//
// FAILS TOWARDS ARMED, deliberately, and this is the rule worth keeping. Every
// failure path returns true: an unknown rig, an unreadable pointer, a build
// whose offsets did not resolve. The only consumer is a cosmetic suppression,
// so a probe that never locks must leave the crosshair exactly as it has always
// been rather than strand it off with no way to notice why. BRVR: "A cosmetic
// gate should fail towards the old behaviour, never towards a permanently
// missing reticle."
bool armed() {
    if (!has_vtable(g_handsActor, patterns::kHandsVtableRva)) return true;
    const uint8_t* h = static_cast<const uint8_t*>(g_handsActor);
    void* hold = nullptr;
    void* abil = nullptr;
    const bool okH = read_ptr(h + patterns::kHandsCurrentHoldableOffset, &hold);
    const bool okA = read_ptr(h + patterns::kHandsCurrentAbilityOffset, &abil);
    if (!okH && !okA) return true; // neither slot readable - say nothing
    return (okH && hold != nullptr) || (okA && abil != nullptr);
}

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
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : mode == 3 ? "BRVR" : "BONES", patterns::kHandsVtableRva,
            patterns::kPlayerWeaponVtableRva);
}

// s64: the rig's geometry is collapsed by bone during an arm-hide re-check, so
// the engine keeps animating an actor the player cannot see. Tracked here
// because the collapse is write-only and needs an explicit edge on the way out.
bool g_rigWasCollapsed = false;


// ---- NUMPAD TUNER (s65, ported from the BRVR mod) --------------------------
//
// BRVR's HandsProbe.cpp PollGripKeys, cut down to the two modes William asked
// for. Its reason for existing transfers exactly: the grip offset and the model
// trim are "three numbers whose only test is 'does the gun pivot about the grip
// when I twist my wrist' - a visual judgement that cannot be made from a log and
// takes one rebuild per guess". The F10 sliders can do it, but not while both
// hands are on the controllers and the gun is held at the angle being judged.
//
//   Numpad 8 / 2   forward / back   (POSITION)   pitch   (ROTATION)
//   Numpad 6 / 4   right / left                  yaw
//   Numpad 0 / 5   up / down                     roll
//   Numpad 7       cycle step   0.5 -> 2 -> 5
//   Numpad 9       cycle mode   POSITION <-> ROTATION
//
// EDITS THE RIGHT (WEAPON) HAND, matching the F10 tuning-hand default.
//
// PER WEAPON. Every edit is stashed into the ACTIVE weapon's profile and the log
// line names it, so a pistol session cannot silently land on the wrench - which
// is the failure the per-weapon split exists to prevent. Persisting on every
// change is BRVR's behaviour too, and it is what makes a tuning session survive
// a crash; these numbers cost headset time and nothing else re-derives them.
//
// FOCUS-GATED. Without this the keys fire while alt-tabbed, which turns a stray
// numpad press in another window into a silent retune.
// s67: THREE modes, and the order matters - the safe one is first because it
// is the one that gets nudged most.
//   kTuneModePos  PLACEMENT  where the gun SITS. View frame, cannot orbit.
//   kTuneModeRot  ROTATION   the model trim.
//   kTuneModeCur  CROSSHAIR  the AIM ray - laser, dot and bullet together.
//
// PIVOT (the grip offset) is deliberately NOT on the numpad. It sits at BRVR's
// values, it is the one knob that can put the orbit back, and every session
// that nudged it in the headset lost time to exactly that. It stays reachable
// from the F10 sliders and `vrhands pos` for anyone re-deriving it.
constexpr int kTuneModePos = 0;
constexpr int kTuneModeRot = 1;
constexpr int kTuneModeCur = 2;
int g_tuneMode = kTuneModePos;
float g_tuneStep = 2.0f;

// NUMLOCK IS THE TRAP, and it is why the first cut did nothing. GetAsyncKeyState
// only reports VK_NUMPAD0..9 while NumLock is ON; with it OFF the very same keys
// send VK_UP/DOWN/LEFT/RIGHT, VK_INSERT, VK_CLEAR, VK_HOME and VK_PRIOR instead,
// so every bind silently reads "not pressed". BRVR carried VK_PRIOR as a second
// binding for the mode key, which is the same wound treated in one place.
//
// Both spellings are accepted. The alternates cost nothing when NumLock is on
// (the numpad sends the NUMPAD codes then, and the arrows are a different
// physical key), and they are what make the feature work at all when it is off.
bool key_down(int vkPrimary, int vkAlt) {
    return ((GetAsyncKeyState(vkPrimary) & 0x8000) != 0) ||
           (vkAlt && (GetAsyncKeyState(vkAlt) & 0x8000) != 0);
}

bool game_has_focus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void tuner_log(const char* what) {
    char wk[48] = "-";
    aim::weapon_key_name(wk, sizeof wk);
    if (g_tuneMode == kTuneModePos)
        BVR_LOG("[hands] numpad: %s %s PLACEMENT fwd %+.1f right %+.1f up %+.1f cm "
                "(step %.1f) - where the gun SITS; per weapon; cannot cause an orbit",
                wk, what, g_viewFwdCm[1].load(std::memory_order_relaxed),
                g_viewRightCm[1].load(std::memory_order_relaxed),
                g_viewUpCm[1].load(std::memory_order_relaxed), g_tuneStep);
    else if (g_tuneMode == kTuneModeCur) {
        float cp = 0.0f, cy = 0.0f;
        aim::aim_trim_deg(1, &cp, &cy);
        BVR_LOG("[hands] numpad: %s CROSSHAIR pitch %+.1f yaw %+.1f deg (step %.1f) - "
                "the AIM ray: laser, dot and bullet move together. PER WEAPON. %s",
                what, cp, cy, g_tuneStep, wk);
    }
    else
        BVR_LOG("[hands] numpad: %s %s ROTATION pitch %+.1f yaw %+.1f roll %+.1f deg "
                "(step %.1f)",
                wk, what, g_rotPitchDeg[1].load(std::memory_order_relaxed),
                g_rotYawDeg[1].load(std::memory_order_relaxed),
                g_rotRollDeg[1].load(std::memory_order_relaxed), g_tuneStep);
}

void poll_numpad_tuner() {
    struct Bind { int vk; int alt; int axis; float sign; };
    // alt = what the same physical key sends with NumLock OFF.
    static const Bind kBinds[6] = {
        {VK_NUMPAD8, VK_UP, 0, +1.0f},    {VK_NUMPAD2, VK_DOWN, 0, -1.0f},
        {VK_NUMPAD6, VK_RIGHT, 1, +1.0f}, {VK_NUMPAD4, VK_LEFT, 1, -1.0f},
        {VK_NUMPAD0, VK_INSERT, 2, +1.0f}, {VK_NUMPAD5, VK_CLEAR, 2, -1.0f},
    };
    static bool prev[6] = {};
    static bool prevStep = false;
    static bool prevMode = false;

    // Say it once, with the NumLock state, so a dead tuner explains itself in
    // the log instead of looking like the feature was never built.
    static bool s_told = false;
    if (!s_told) {
        s_told = true;
        BVR_LOG("[hands] numpad tuner armed - NumLock is %s. 8/2 fwd, 6/4 right, 0/5 up; "
                "7 cycles step, 9 cycles PLACEMENT/ROTATION/CROSSHAIR. Both NumLock states work.",
                (GetKeyState(VK_NUMLOCK) & 1) ? "ON" : "OFF");
    }

    // Keys BEFORE focus, so "pressed but the window was not foreground" is a
    // thing the log can say rather than a silent nothing.
    bool anyDown = key_down(VK_NUMPAD7, VK_HOME) || key_down(VK_NUMPAD9, VK_PRIOR);
    for (int i = 0; i < 6 && !anyDown; ++i)
        anyDown = key_down(kBinds[i].vk, kBinds[i].alt);
    if (!game_has_focus()) {
        static uint64_t s_lastBlockedMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (anyDown && nowMs - s_lastBlockedMs >= 2000) {
            s_lastBlockedMs = nowMs;
            BVR_LOG("[hands] numpad: key held, but the game window is not foreground - "
                    "ignoring (click the game window first)");
        }
        return;
    }

    // Mode, then step - both edge-detected so a held key moves one notch.
    const bool modeDown = key_down(VK_NUMPAD9, VK_PRIOR);
    if (modeDown && !prevMode) {
        g_tuneMode = (g_tuneMode == kTuneModePos)   ? kTuneModeRot
                     : (g_tuneMode == kTuneModeRot) ? kTuneModeCur
                                                    : kTuneModePos;
        tuner_log("now editing");
    }
    prevMode = modeDown;

    const bool stepDown = key_down(VK_NUMPAD7, VK_HOME);
    if (stepDown && !prevStep) {
        g_tuneStep = (g_tuneStep < 1.0f) ? 2.0f : (g_tuneStep < 3.0f ? 5.0f : 0.5f);
        tuner_log("step now");
    }
    prevStep = stepDown;

    bool moved = false;
    for (int i = 0; i < 6; ++i) {
        const bool down = key_down(kBinds[i].vk, kBinds[i].alt);
        if (down && !prev[i]) {
            // s67 SIGN TRAP, and it cost a headset run. BRVR mode SUBTRACTS the
            // grip offset (actorLoc = target - R*offset), so a raw "+up" on the
            // stored value moves the gun DOWN on screen - and lengthens the
            // lever while it does, which reads as the desync suddenly exploding.
            // The keys are labelled for what the tester SEES, so invert the
            // delta in the mode where the value is negated at the use site.
            // No sign flip any more: the view placement is ADDED in every mode,
            // so up is up. (The old inversion existed because mode 3 subtracts
            // the grip offset, which these keys no longer touch.)
            // BRVR mode subtracts the GRIP offset (actorLoc = target - R*grip),
            // so raw "+up" there moves the gun down. Invert for that one case so
            // the keys always mean what the tester sees. Placement is added in
            // every mode and needs no flip.
            const float d = kBinds[i].sign * g_tuneStep;
            if (g_tuneMode == kTuneModeCur) {
                // CROSSHAIR: the aim ray's pitch/yaw. 8/2 pitch, 6/4 yaw. The
                // ray carries no roll in this tree (the camera owns roll), so
                // 0/5 has nothing to move - say so rather than silently ignore.
                if (kBinds[i].axis == 2) {
                    BVR_LOG("[hands] numpad: CROSSHAIR has no roll axis here - the aim ray "
                            "carries no roll (the camera owns it). Use 8/2 pitch, 6/4 yaw.");
                    prev[i] = down;
                    continue;
                }
                float cp = 0.0f, cy = 0.0f;
                aim::aim_trim_deg(1, &cp, &cy);
                if (kBinds[i].axis == 0) cp += d; else cy += d;
                aim::set_aim_trim_all(cp, cy);
                moved = true;
                prev[i] = down;
                continue;
            }
            // s67: POSITION MODE NOW EDITS THE PLACEMENT, NOT THE PIVOT.
            //
            // g_posFwdCm/RightCm/UpCm is the grip offset, and the grip offset IS
            // the pivot - gun renders at gp.loc + R*(G - gripOffset). Nudging it
            // in the headset to move the gun displaces the pivot by the same
            // amount and the gun starts orbiting, which is what "the offset
            // buttons seem to make it desync more" is. The view-frame placement
            // does the job the tester actually wants and cannot create a lever,
            // so that is what the keys drive.
            //
            // The pivot stays reachable from the F10 sliders and `vrhands pos`
            // for whoever genuinely needs to re-derive it. It should sit at the
            // BRVR values and be left alone.
            std::atomic<float>* dst =
                (g_tuneMode == kTuneModePos)
                    ? (kBinds[i].axis == 0 ? g_viewFwdCm : kBinds[i].axis == 1 ? g_viewRightCm
                                                                              : g_viewUpCm)
                    : (kBinds[i].axis == 0 ? g_rotPitchDeg : kBinds[i].axis == 1 ? g_rotYawDeg
                                                                                 : g_rotRollDeg);
            // Every one of these is now a per-hand array, index 1 = right. The
            // numpad tuner is the WEAPON hand's tuner by design (it is what the
            // hand on the controller is holding while tuning); the left hand's
            // equivalents are the F10 sliders, which follow the L/R radio.
            const int di = 1;
            dst[di].store(dst[di].load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
            moved = true;
        }
        prev[i] = down;
    }
    if (!moved) return;
    tuner_log("set");
    // Persist immediately, both files: save_weapon_profiles() stashes the live
    // values into the active profile first, so weapons.ini picks up the edit.
    save_offsets();
    aim::save_weapon_profiles();
}

void on_calcview(const FrameContext& ctx) {
    poll_numpad_tuner(); // in-headset grip/trim tuning - see its banner
    // Overlay request, applied from THIS thread (same rule as aim.cpp: the
    // render thread must never touch engine state directly).
    int pendMode = g_pendingMode.exchange(-1, std::memory_order_relaxed);
    if (pendMode >= 0) {
        g_mode.store(pendMode, std::memory_order_relaxed);
        if (pendMode != 2) bones::release("hands mode change (overlay)");
        BVR_LOG("[hands] mode = %s (overlay)",
                pendMode == 0   ? "GUN"
                : pendMode == 1 ? "HANDS"
                : pendMode == 3 ? "BRVR (grip pose for position, aim for rotation, "
                                  "offset SUBTRACTED - BRVR's own values)"
                                : "BONES");
    }
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
    // s64: a scripted scene joins the same gate. The engine is animating the
    // hands on purpose there, and leaving the controller in charge lets the
    // player drag the rig around mid-scene. Same release path, same reason.
    const bool scriptedScene =
        scripted::freeze_hands_in_scenes() && scripted::scripted_window();
    if (gameplayView && (scriptedScene || (bvr::hud::cinematic_hold() &&
                                           bvr::vr::cine_drive() !=
                                               bvr::vr::CineDrive::Off))) {
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

    // s64: the rig itself. Hidden while a scene runs with no animation playing
    // (the game is walking you into position), shown the instant one starts.
    // Applied ABOVE the early return, so the unhide still runs on the frame the
    // gate closes - session 29's collapsed-hand bug was exactly a restore living
    // inside code the gate had already skipped.
    {
        // The cached pointer, or resolve it here if it is empty. find_hands_actor
        // normally runs ~20 lines BELOW this - past the scripted gate's early
        // return - so during a scene an empty cache would never refill and the
        // hide would silently do nothing. The resolver has its own scan
        // cooldown, so asking is cheap when it cannot answer.
        void* rig = hands_actor();
        if (!rig) rig = find_hands_actor(ctx, false);

        // s64 round 8: sample first, then decide - only this site owns the rig
        // actor and runs every CalcView. Outside a window we report "cannot
        // answer", which resets the latch to VISIBLE so a window can never open
        // onto a stale "still". The bones history is deliberately not reset
        // across that gap, so the first in-window sample reads as a spike and
        // shows the arms for a frame - the safe direction, and self-correcting.
        // ---- READ, THEN HIDE. THE ACTOR IS NEVER SCALED DOWN ----------------
        //
        // The rig is hidden by collapsing its BONES, with the actor left at full
        // DrawScale3D. That one choice removes the whole problem the previous
        // three attempts were built around: an actor scaled to 0.0001 leaves
        // whatever the engine animates, so the bone array freezes (measured: 293
        // consecutive samples, 0 of 47 bones moving) and the motion gate can hide
        // the arms but never bring them back. At full scale the engine keeps
        // animating it every frame, so the reading is always honest and none of
        // the re-check machinery is needed.
        //
        // ORDER IS THE WHOLE THING. The sample is taken FIRST, off the pose the
        // engine wrote early this frame; the collapse is written after. So we
        // never measure our own write, which is the trap INVARIANTS.md warns
        // about - avoided by sequencing rather than by choosing a different bone.
        if (rig && scripted::scripted_window()) {
            bones::keep_evaluating(rig);

            float smoothed = 0.0f, raw = 0.0f, pos[3] = {};
            bool stale = false;
            const bool have = bones::hand_motion(rig, &smoothed, &raw, pos, &stale);
            // STALE IS NOT AN ANSWER, so it must not become one. The bone still
            // holds our own collapse, which means the engine has not refreshed it
            // since - feeding that in either direction is wrong: as motion it is
            // a 5000-unit spike that pins the arms up, as stillness it hides them
            // on our own write. Leave the verdict alone and wait for a real one.
            if (!stale)
                scripted::note_hand_motion(have, smoothed, raw, bones::motion_bone());
        } else {
            scripted::note_hand_motion(false, 0.0f, 0.0f, bones::motion_bone());
        }

        if (rig) {
            const bool hide = scripted::want_rig_hidden();
            if (hide) {
                bones::collapse_rig(rig);
            } else if (g_rigWasCollapsed) {
                // No restore write: the engine re-evaluates the whole array early
                // next frame, so simply stopping puts the authored pose back. All
                // this does is re-flag the array so the render pass rebuilds too.
                bones::end_collapse(rig);
            }
            g_rigWasCollapsed = hide;
        }
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

            // BRVR MODE SPLITS THE TWO POSES, and this is not a detail - it is
            // the shape of the thing being ported. BRVR's DriveHands takes the
            // ORIENTATION from the aim pose (so barrel, laser and bullet stay
            // one ray) and the POSITION from the grip pose (which is the
            // controller handle, i.e. where the hand physically is):
            //     QuatMul(hp.aimQuat, qOff, qFinal);                 // aim
            //     const float* P = hp.gripValid ? hp.gripPos : hp.aimPos;  // grip
            // This tree's own ENGINE_NOTES reached the same conclusion in M6
            // and the code never took it: "grip stays the right choice for
            // placing a hand/weapon MODEL (M7)".
            //
            // Falls back to whatever the single-pose read already gave us if
            // the grip pose is not being tracked, which is BRVR's own gripValid
            // ternary - never refuse to draw a hand over this.
            if (g_mode.load(std::memory_order_relaxed) == 3) {
                bvr::vr::HeadPose hAim{}, hGrip{};
                if (bvr::vr::get_hand_pose(hand, true, hAim)) {
                    quat[0] = hAim.qx;
                    quat[1] = hAim.qy;
                    quat[2] = hAim.qz;
                    quat[3] = hAim.qw;
                }
                if (bvr::vr::get_hand_pose(hand, false, hGrip)) {
                    pos[0] = hGrip.px;
                    pos[1] = hGrip.py;
                    pos[2] = hGrip.pz;
                }
            }
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
    //
    // SIGN IS MODE-DEPENDENT, and that is not a wart - it is the whole
    // difference between the two systems (s67):
    //
    //   BONES (mode 2): the anchor bone IS the hand, so the offset ADDS - it
    //     nudges the pinned bone away from the controller.
    //   BRVR  (mode 3): the actor origin sits at the EYE with the hand authored
    //     ~44 UU out in mesh space, so the offset SUBTRACTS - it pulls the
    //     actor BACK along its own axes until the mesh's hand lands on the
    //     controller. BRVR's CameraHook.cpp does exactly this:
    //         wx -= (Fx*gX + Rx*gY + Ux*gZ);
    //     and its tuned values are ~44-58 forward for that reason. They are the
    //     actor-origin-to-hand vector, NOT a small trim.
    //
    // The two conventions therefore need DIFFERENT NUMBERS in the same
    // weapons.ini fields. Loading BRVR's values in mode 2 (or the reverse) puts
    // the gun half a metre from where it belongs. See the mode command's log
    // line, which says which convention is live.
    const bool brvrMode = g_mode.load(std::memory_order_relaxed) == 3;
    float fwd[3], right[3], up[3];
    // ---- s67: THE BASIS THE OFFSET RIDES, AND WHETHER IT CARRIES ROLL -------
    // BRVR, CameraHook.cpp, after the S59 readback measured the game erasing
    // roll by 5-102 deg while pitch and yaw held:
    //
    //   "So roll was never landing. Writing it anyway was ACTIVELY HARMFUL: the
    //    grip correction rotated the offset by a roll the mesh never rendered
    //    with, which swung the hand through an arc that grew with the twist.
    //    That was the residual drift. We now write only what survives, and
    //    correct with the same values."
    //
    // "An arc that grew with the twist" is the s67 symptom verbatim: a pure
    // roll sweep traces an oval, zero at 0 deg, peak at 180, closed at 360.
    //
    // So the offset basis can be built WITHOUT roll. If the rendered mesh does
    // not carry our roll, an offset rotated by that roll swings on exactly such
    // an arc, and no value of the offset can cancel it - which is what two
    // tuning sessions found the hard way.
    //
    // Default ON = today's behaviour (roll included), so this changes nothing
    // until it is switched. `vrhands offsetroll off` / the F10 checkbox is the
    // A/B, and `[tlm] rollcheck` in bones.cpp is the measurement that says
    // which way it should go.
    if (g_offsetRoll.load(std::memory_order_relaxed)) {
        ue_rot_basis(gp.rot, fwd, right, up);
    } else {
        FRotator noRoll = gp.rot;
        noRoll.roll = 0;
        ue_rot_basis(noRoll, fwd, right, up);
    }
    float uuPerCm = ctx.worldScale / 100.0f;
    const float sgn = brvrMode ? -1.0f : 1.0f;
    float of = g_posFwdCm[hand].load(std::memory_order_relaxed) * uuPerCm * sgn;
    float orr = g_posRightCm[hand].load(std::memory_order_relaxed) * uuPerCm * sgn;
    float ou = g_posUpCm[hand].load(std::memory_order_relaxed) * uuPerCm * sgn;
    float loc[3] = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
                    gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
                    gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};

    // View-frame placement (see the banner): rigid translation of the whole rig
    // in the CAMERA's basis, so "up" means up in the headset and stays that way
    // however the wrist is turned. Roll is dropped - the camera's own roll must
    // not tip the viewmodel's placement.
    {
        const int vh = hand & 1; // s68: per hand - the plasmid sits differently
        const float vf = g_viewFwdCm[vh].load(std::memory_order_relaxed);
        const float vr = g_viewRightCm[vh].load(std::memory_order_relaxed);
        const float vu = g_viewUpCm[vh].load(std::memory_order_relaxed);
        if (vf != 0.0f || vr != 0.0f || vu != 0.0f) {
            FRotator camRot{ctx.camPitch, ctx.camYaw, 0};
            float cf[3], cr[3], cu[3];
            ue_rot_basis(camRot, cf, cr, cu);
            const float f2 = vf * uuPerCm, r2 = vr * uuPerCm, u2 = vu * uuPerCm;
            for (int i = 0; i < 3; ++i)
                loc[i] += cf[i] * f2 + cr[i] * r2 + cu[i] * u2;
        }
    }

    const int driveMode = g_mode.load(std::memory_order_relaxed);
    if (driveMode == 3) {
        // BRVR shape, both halves. The actor write below carries the rig to the
        // controller; this keeps the cluster RIGID at its authored pose so the
        // actor has something coherent to carry. Releasing it to the engine
        // instead - which is what the first port of this mode did - leaves the
        // rig animating underneath a moving actor, which is not BRVR and is not
        // a test of BRVR.
        bones::set_freeze_only(true);
        // The weapon's OWN skeleton is the only lever that sizes the gun - the
        // rig actor's DrawScale does not size geometry (bones.cpp, session 61).
        // Mode 2 has always called this; mode 3 did not, which is why the gun
        // came back full size when the freeze landed.
        bones::wskel_drive();
        bones::drive(ctx, target, gp, hand);
    } else if (bones::freeze_only()) {
        bones::set_freeze_only(false);
    }

    if (driveMode == 2) {
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
            // Publish for the late re-apply. The game tick is about to reset
            // this rotator; late_write() puts it back after that happens.
            g_lwObj = target;
            memcpy(g_lwRot, rot, sizeof rot);
            g_lwValid.store(true, std::memory_order_relaxed);
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
        int mode = strncmp(rest, "gun", 3) == 0      ? 0
                   : strncmp(rest, "brvr", 4) == 0   ? 3
                   : strncmp(rest, "hands", 5) == 0  ? 1
                                                     : 2;
        g_mode.store(mode, std::memory_order_relaxed);
        // Leaving the bone drive: hand the skeleton back EXPLICITLY. Simply not
        // calling drive() is not enough (see bones.h release()): reapply() keeps
        // repainting the cached pose for up to 100 ms AND keeps clearing the
        // dirty flag while it does, so the engine never takes the cluster back
        // and the actor would carry a rig frozen in our last pose.
        if (mode != 2) bones::release("hands mode change");
        BVR_LOG("[hands] mode = %s",
                mode == 0   ? "GUN (inert - renderer ignores an attached weapon's actor fields)"
                : mode == 1 ? "HANDS (actor pinning - retired, kept for A/B)"
                : mode == 3 ? "BRVR (actor pinning, BRVR's exact chain: GRIP pose for "
                              "position, AIM pose for rotation, grip offset SUBTRACTED along "
                              "the model axes. Offsets in this mode are BRVR's ~44-58 fwd, "
                              "NOT the small mode-2 numbers - see the sign note in on_calcview)"
                            : "BONES (M7-v2: hand cluster follows the controller)");
    } else if (strcmp(verb, "viewpos") == 0) {
        // s68: "viewpos [l|r] <fwd> <right> <up>". The hand is optional and
        // defaults to RIGHT, which is what the command has always meant - the
        // triple only became per-hand when the plasmids turned out to be sharing
        // the weapon's copy of it.
        const char* p2 = rest;
        int vh = 1;
        if (*p2 == 'l' || *p2 == 'L') { vh = 0; ++p2; }
        else if (*p2 == 'r' || *p2 == 'R') { vh = 1; ++p2; }
        while (*p2 == ' ' || *p2 == '	') ++p2;
        float vf = 0.0f, vr = 0.0f, vu = 0.0f;
        if (sscanf_s(p2, "%f %f %f", &vf, &vr, &vu) == 3) {
            g_viewFwdCm[vh].store(vf, std::memory_order_relaxed);
            g_viewRightCm[vh].store(vr, std::memory_order_relaxed);
            g_viewUpCm[vh].store(vu, std::memory_order_relaxed);
            save_offsets();
        }
        BVR_LOG("[hands] view placement %s fwd%+.1f right%+.1f up%+.1f cm - changes where "
                "the gun SITS without touching where it PIVOTS",
                vh == 0 ? "L (plasmid)" : "R (weapon)",
                g_viewFwdCm[vh].load(std::memory_order_relaxed),
                g_viewRightCm[vh].load(std::memory_order_relaxed),
                g_viewUpCm[vh].load(std::memory_order_relaxed));
    } else if (strcmp(verb, "offsetroll") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_offsetRoll.store(on, std::memory_order_relaxed);
        BVR_LOG("[hands] grip offset basis %s - BRVR builds it WITHOUT roll, because "
                "the game erases the roll we write and an offset rotated by a roll the "
                "mesh never rendered with 'swung the hand through an arc that grew with "
                "the twist'",
                on ? "INCLUDES roll (today)" : "drops roll (the BRVR shape)");
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

void late_write() {
    if (!g_lateWrite.load(std::memory_order_relaxed)) return;
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 2) {
        // Bone drive: the cached cluster write is the thing that has to survive
        // the tick, and reapply() already replays exactly it (with its own
        // 100 ms freshness guard, so a stale cache cannot keep painting).
        bones::reapply();
        return;
    }

    // Actor drive (modes 1 and 3) - the direct BRVR S60 port.
    if (!g_lwValid.load(std::memory_order_relaxed) || !g_lwObj) return;
    // The hands actor dies with the level. Re-validate against the probe's
    // current target and drop the cache the moment it moves, or a load writes
    // through freed memory. BRVR learned this one as a crash.
    if (g_lwObj != g_handsActor || !has_vtable(g_lwObj, patterns::kHandsVtableRva)) {
        g_lwValid.store(false, std::memory_order_relaxed);
        return;
    }
    if (!write12(static_cast<uint8_t*>(g_lwObj) + patterns::kActorViewDirOffset, g_lwRot))
        g_lwValid.store(false, std::memory_order_relaxed);
}

void set_model_trim_deg(int hand, float pitchDeg, float yawDeg, float rollDeg) {
    if (hand < 0 || hand > 1) return;
    g_rotPitchDeg[hand].store(pitchDeg, std::memory_order_relaxed);
    g_rotYawDeg[hand].store(yawDeg, std::memory_order_relaxed);
    g_rotRollDeg[hand].store(rollDeg, std::memory_order_relaxed);
}

void model_offset_cm(int hand, float* fwdCm, float* rightCm, float* upCm) {
    if (hand < 0 || hand > 1) return;
    if (fwdCm) *fwdCm = g_posFwdCm[hand].load(std::memory_order_relaxed);
    if (rightCm) *rightCm = g_posRightCm[hand].load(std::memory_order_relaxed);
    if (upCm) *upCm = g_posUpCm[hand].load(std::memory_order_relaxed);
}

void view_offset_cm(int hand, float* fwdCm, float* rightCm, float* upCm) {
    if (hand < 0 || hand > 1) return;
    if (fwdCm) *fwdCm = g_viewFwdCm[hand].load(std::memory_order_relaxed);
    if (rightCm) *rightCm = g_viewRightCm[hand].load(std::memory_order_relaxed);
    if (upCm) *upCm = g_viewUpCm[hand].load(std::memory_order_relaxed);
}

void set_view_offset_cm(int hand, float fwdCm, float rightCm, float upCm) {
    if (hand < 0 || hand > 1) return;
    g_viewFwdCm[hand].store(fwdCm, std::memory_order_relaxed);
    g_viewRightCm[hand].store(rightCm, std::memory_order_relaxed);
    g_viewUpCm[hand].store(upCm, std::memory_order_relaxed);
}

void set_model_offset_cm(int hand, float fwdCm, float rightCm, float upCm) {
    if (hand < 0 || hand > 1) return;
    g_posFwdCm[hand].store(fwdCm, std::memory_order_relaxed);
    g_posRightCm[hand].store(rightCm, std::memory_order_relaxed);
    g_posUpCm[hand].store(upCm, std::memory_order_relaxed);
}

void save_offsets() {
    save_config();
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Hands + weapon (M7)")) return;

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Viewmodel follows the controller", &on))
        g_pendingEnable.store(on ? 1 : 0, std::memory_order_relaxed);

    {
        // s67 drive-mode selector. It exists because the BRVR-vs-BONES question
        // can only be answered by eye, and the tester cannot type with both
        // hands on the controllers.
        int m = g_mode.load(std::memory_order_relaxed);
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (ImGui::RadioButton("BONES", &m, 2)) g_pendingMode.store(2, std::memory_order_relaxed);
        ImGui::SameLine();
        if (ImGui::RadioButton("BRVR", &m, 3)) g_pendingMode.store(3, std::memory_order_relaxed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "BONES  - this tree's own drive: the hand CLUSTER is retargeted\n"
                "         inside an actor the engine leaves at the eye.\n"
                "BRVR   - the other mod's drive, ported whole: the ACTOR carries\n"
                "         both position and rotation, position comes from the GRIP\n"
                "         pose, rotation from the AIM pose, and the grip offset is\n"
                "         SUBTRACTED along the model's own axes.\n\n"
                "THE TWO MODES NEED DIFFERENT NUMBERS IN THE SAME FIELDS. BRVR's\n"
                "offsets are the actor-origin-to-hand vector (~44-58 forward);\n"
                "mode 2's are a small trim. weapons.ini currently holds BRVR's\n"
                "values, so BONES will look wrong until they are put back\n"
                "(weapons.ini.bak-pre-brvr).");
    }

    {
        // s67. The A/B for "an arc that grew with the twist" - see the basis
        // note in on_calcview and BRVR's S59/S60 readback.
        bool orl = g_offsetRoll.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Grip offset follows wrist ROLL (untick = BRVR)", &orl))
            g_offsetRoll.store(orl, std::memory_order_relaxed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "BRVR measured the game ERASING the roll it wrote - 5 to 102 deg,\n"
                "scaling with wrist twist - while pitch and yaw held fine. Its\n"
                "conclusion: rotating the grip offset by a roll the mesh never\n"
                "rendered with 'swung the hand through an arc that grew with the\n"
                "twist'.\n\n"
                "That is this bug's exact shape: roll the controller like a pole\n"
                "and the gun traces an oval - nothing at 0 deg, worst at 180,\n"
                "closed again at 360.\n\n"
                "UNTICK to build the offset in a roll-free frame. Turn on\n"
                "'vrbones telemetry' first and read [tlm] rollcheck: if drift\n"
                "grows with target roll, the engine is eating our roll and this\n"
                "is the fix.");
    }

    bool aimPose = g_useAimPose.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Align to the AIM ray (matches laser; off = grip pose)", &aimPose))
        g_useAimPose.store(aimPose, std::memory_order_relaxed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ignored in BRVR mode, which always splits the two poses:\n"
                          "grip for position, aim for rotation.");

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

    ImGui::TextDisabled("PLACEMENT (view frame) - moves it WITHOUT moving the pivot");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "USE THESE for a gun that sits too low, too high or too far out. They"
            " translate the rig in your VIEW frame, after the model transform, so\n"
            "they do not rotate with your wrist and CANNOT create an orbit.\n\n"
            "The GRIP OFFSET below is a different job: it decides which point the\n"
            "gun PIVOTS about. Using it to fix height moves the pivot by the same\n"
            "amount - measured s67, a 15 cm height fix bought an 8 inch orbit.");
    {
        // s68: per hand, following the L/R radio above. These used to drive ONE
        // triple shared by both hands, which is how the plasmids ended up parked
        // at the weapon's placement - see the banner on g_viewFwdCm. The R values
        // belong to the equipped weapon's profile and are stashed back into it on
        // the next weapon change; the L values are the plasmids' own and persist
        // in hands.ini.
        float vf = g_viewFwdCm[tuneHand].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("place forward (cm)", &vf, -60.0f, 60.0f))
            g_viewFwdCm[tuneHand].store(vf, std::memory_order_relaxed);
        float vr = g_viewRightCm[tuneHand].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("place right (cm)", &vr, -60.0f, 60.0f))
            g_viewRightCm[tuneHand].store(vr, std::memory_order_relaxed);
        float vu = g_viewUpCm[tuneHand].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("place up (cm)", &vu, -60.0f, 60.0f))
            g_viewUpCm[tuneHand].store(vu, std::memory_order_relaxed);
    }
    ImGui::TextDisabled("GRIP OFFSET - where the model PIVOTS (not where it sits)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "These three ARE the fix for \"the weapon swings on a circle around the\n"
            "controller\". At 0/0/0 the model's own ORIGIN is pinned to your hand,\n"
            "so its grip - which is somewhere else in the model - orbits that point\n"
            "as you rotate. Dial them until the weapon pivots about the GRIP when\n"
            "you twist your wrist. That is a visual judgement and cannot be read\n"
            "off a log, which is why it is a slider and not a constant.\n\n"
            "SAVED PER WEAPON since s65: the number is the model's own\n"
            "origin-to-grip vector, so every weapon needs its own. Tune with that\n"
            "weapon in hand, then press Save preset values - it writes weapons.ini\n"
            "as well, and switching weapons restores each one's numbers.");
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

    // Session 61: hand + weapon scale (deliberately independent of world
    // scale - the rig can be the wrong size while the world is right).
    // The hand slider edits the tuning hand's cluster; the weapon slider is
    // uniform about the grip and only binds skeletal holdables (the wrench
    // is a rigid mesh and stays authored).
    float hs = bones::scale(tuneHand);
    if (ImGui::SliderFloat("model scale (x, independent of worldscale)", &hs, 0.2f, 4.0f))
        bones::set_scale(tuneHand, hs);
    if (ImGui::Button("scale both hands to this")) bones::set_scale(-1, hs);
    float ws = bones::weapon_scale();
    if (ImGui::SliderFloat("WEAPON scale (uniform, about the grip)", &ws, 0.3f, 2.5f))
        bones::set_weapon_scale(ws);

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
