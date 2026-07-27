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

// Model offsets. Position is in CENTIMETRES in the model's final (trimmed)
// frame; the rotation trim is degrees, applied in the CONTROLLER'S LOCAL frame
// as a quaternion compose - euler adds after conversion only behave at one
// controller orientation (the first headset test's "pivot" bug).
// Defaults are ZERO on purpose. The ideal pivot correction (pull the mesh's gun
// to the controller, ~-100 cm forward) is CULLED: the engine drops the whole
// rig the moment the actor origin goes behind the camera (live-proven - the
// rig vanished with the origin 32 UU back). Forward pull is therefore limited
// to roughly the controller's own distance from the face, and where that line
// sits is the user's in-headset call, not a default.
std::atomic<float> g_posFwdCm{0.0f}, g_posRightCm{0.0f}, g_posUpCm{0.0f};
std::atomic<float> g_rotPitchDeg{0.0f}, g_rotYawDeg{0.0f}, g_rotRollDeg{0.0f};

std::atomic<bool> g_writeRot{true}; // rotation write can be disabled on its own
int32_t g_probeLeft = 0;

// Cached actors, revalidated by vtable on every use.
void* g_handsActor = nullptr;
void* g_weaponActor = nullptr;
uint64_t g_lastHandsScanMs = 0;
uint64_t g_lastWeaponScanMs = 0;
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
    bool logEvery;  // probe: describe every instance
    bool chooseAny; // probe: choose nothing, so the whole list gets logged
};

// Both accept callbacks run inside the scan's SEH guard (patterns.cpp).
// A UClass default object carries the same vtable as a live actor but sits at
// the origin with zeroed fields; proximity to the camera separates the live
// viewmodel from it (and from `0xCCCCCCCC` stack debris).
bool accept_hands(void* obj, void* user) {
    ScanCtx* c = static_cast<ScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    float loc[3];
    int32_t rot[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);
    memcpy(rot, p + patterns::kActorViewDirOffset, sizeof rot);

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (c->logEvery)
        BVR_LOG("[hands] AHands match @ %p loc=(%.1f %.1f %.1f) rot=(%d %d %d) "
                "distToCam=%.1f UU",
                obj, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2], dist);
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
    void* best;
    float bestDist;
    bool logEvery;
};

bool accept_weapon(void* obj, void* user) {
    WeaponScanCtx* c = static_cast<WeaponScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);

    void* owner = *reinterpret_cast<void* const*>(p + patterns::kWeaponOwnerOffset);
    if (!owner) return false;
    void* ownerVtbl = *reinterpret_cast<void* const*>(owner);
    uint32_t ownerRva = static_cast<uint32_t>(static_cast<const uint8_t*>(ownerVtbl) -
                                              c->imageBase);
    if (ownerRva != patterns::kShockPlayerVtableRva) return false;

    float loc[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);
    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (c->logEvery)
        BVR_LOG("[hands] player weapon match @ %p loc=(%.1f %.1f %.1f) distToGunSpot=%.1f UU",
                obj, loc[0], loc[1], loc[2], dist);
    if (dist < 120.0f && dist < c->bestDist) {
        c->best = obj;
        c->bestDist = dist;
    }
    return false; // never "accept" - the nearest wins after the walk
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
        if (now - g_lastHandsScanMs < 2000) return nullptr;
        g_lastHandsScanMs = now;
    }
    ScanCtx sc{ctx.camX, ctx.camY, ctx.camZ, probeOnly, !probeOnly};
    int matches = 0;
    void* found = patterns::scan_for_vtable_object(
        patterns::kHandsVtableRva, patterns::kActorViewDirOffset + 12, &accept_hands, &sc,
        "AHands", &matches);
    g_lastMatches.store(matches, std::memory_order_relaxed);
    if (probeOnly) return nullptr;
    g_handsActor = found;
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
        if (now - g_lastWeaponScanMs < 2000) return nullptr;
        g_lastWeaponScanMs = now;
    }
    // Anchor at the expected gun spot: 50 UU along the current view.
    float dir[3];
    FRotator viewRot{ctx.camPitch, ctx.camYaw, 0};
    ue_rot_to_dir(viewRot, dir);
    WeaponScanCtx wc{ctx.camX + dir[0] * 50.0f, ctx.camY + dir[1] * 50.0f,
                     ctx.camZ + dir[2] * 50.0f, g_imageBase, nullptr, 1e9f, probeOnly};
    int matches = 0;
    patterns::scan_for_vtable_object(patterns::kPlayerWeaponVtableRva,
                                     patterns::kWeaponOwnerOffset + sizeof(void*),
                                     &accept_weapon, &wc, "APlayerWeapon", &matches);
    g_lastMatches.store(matches, std::memory_order_relaxed);
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
            g_handMode.load(std::memory_order_relaxed) == 0   ? "LEFT"
            : g_handMode.load(std::memory_order_relaxed) == 1 ? "RIGHT"
                                                              : "auto",
            g_writes.load(std::memory_order_relaxed));
    BVR_LOG("[hands]   weapon actor=%p (learned %p) | hands actor=%p (matches %d)",
            g_weaponActor, bvr::b1r::aim::learned_weapon_object(), g_handsActor,
            g_lastMatches.load(std::memory_order_relaxed));
    BVR_LOG("[hands]   offset pos fwd%+.1f right%+.1f up%+.1f cm | trim pitch%+.1f yaw%+.1f "
            "roll%+.1f deg | writeRot=%d",
            g_posFwdCm.load(std::memory_order_relaxed),
            g_posRightCm.load(std::memory_order_relaxed),
            g_posUpCm.load(std::memory_order_relaxed),
            g_rotPitchDeg.load(std::memory_order_relaxed),
            g_rotYawDeg.load(std::memory_order_relaxed),
            g_rotRollDeg.load(std::memory_order_relaxed),
            g_writeRot.load(std::memory_order_relaxed) ? 1 : 0);
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
    fprintf(f, "mode=%d\n", g_mode.load(std::memory_order_relaxed));
    fprintf(f, "aimPose=%d\n", g_useAimPose.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "posFwdCm=%.2f\n", g_posFwdCm.load(std::memory_order_relaxed));
    fprintf(f, "posRightCm=%.2f\n", g_posRightCm.load(std::memory_order_relaxed));
    fprintf(f, "posUpCm=%.2f\n", g_posUpCm.load(std::memory_order_relaxed));
    fprintf(f, "rotPitchDeg=%.2f\n", g_rotPitchDeg.load(std::memory_order_relaxed));
    fprintf(f, "rotYawDeg=%.2f\n", g_rotYawDeg.load(std::memory_order_relaxed));
    fprintf(f, "rotRollDeg=%.2f\n", g_rotRollDeg.load(std::memory_order_relaxed));
    fclose(f);
    BVR_LOG("[hands] offsets saved to hands.ini");
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
        else if (strcmp(key, "posFwdCm") == 0) g_posFwdCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "posRightCm") == 0) g_posRightCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "posUpCm") == 0) g_posUpCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotPitchDeg") == 0) g_rotPitchDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotYawDeg") == 0) g_rotYawDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotRollDeg") == 0) g_rotRollDeg.store(v, std::memory_order_relaxed);
        else --n;
    }
    fclose(f);
    if (n) BVR_LOG("[hands] loaded %d value(s) from hands.ini", n);
}

} // namespace

// BioShock holds ONE thing at a time (the trigger that fires also switches
// hands - XENON_RT/LT), so the viewmodel belongs to whichever hand last fired.
// Seeded from the triggers the bridge itself composes, exactly like aim.cpp's
// object map. Shared with the aim laser so the beam leaves the hand that is
// actually holding the weapon.
int active_hand() {
    int mode = g_handMode.load(std::memory_order_relaxed);
    if (mode == 0 || mode == 1) return mode;

    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    if (rt >= 64 && lt < 64) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lt >= 64 && rt < 64) g_autoHand.store(0, std::memory_order_relaxed);
    return g_autoHand.load(std::memory_order_relaxed);
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

    // Where the model goes.
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
            // hand would occupy, oriented by the sim angles, recenter identity.
            pos[0] = 0.15f;  // meters right of the recenter origin
            pos[1] = -0.20f; // below it
            pos[2] = -0.35f; // in front (XR forward is -Z)
            xr_local_trim_quat(g_sim.pitchDeg / kRadToDeg, g_sim.yawDeg / kRadToDeg,
                               g_sim.rollDeg / kRadToDeg, quat);
            mapCtx.recenterYawRad = 0.0f;
            mapCtx.recenterPx = mapCtx.recenterPy = mapCtx.recenterPz = 0.0f;
        } else {
            bvr::vr::HeadPose hp{};
            bool aimPose = g_useAimPose.load(std::memory_order_relaxed);
            if (!ctx.vrDriving || !bvr::vr::get_hand_pose(active_hand(), aimPose, hp)) return;
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
                            active_hand(), hp.px, hp.py, hp.pz, hp.qx, hp.qy, hp.qz, hp.qw,
                            aimPose ? "aim" : "grip");
                }
            }
        }

        // Mesh-alignment trim, composed in the controller's local frame so it
        // holds at EVERY controller orientation.
        float trim[4], q2[4];
        xr_local_trim_quat(g_rotPitchDeg.load(std::memory_order_relaxed) / kRadToDeg,
                           g_rotYawDeg.load(std::memory_order_relaxed) / kRadToDeg,
                           g_rotRollDeg.load(std::memory_order_relaxed) / kRadToDeg, trim);
        quat_mul(quat, trim, q2);

        gp = xr_pose_to_game(mapCtx, pos, q2);

        // The aim calibration trim, applied exactly the way the fire ray and
        // the laser apply it, so the barrel and the bullet stay one ray.
        gp.rot.pitch += static_cast<int32_t>(bvr::b1r::aim::trim_pitch_deg() *
                                             kRotUnitsPerDegree);
        gp.rot.yaw += static_cast<int32_t>(bvr::b1r::aim::trim_yaw_deg() *
                                           kRotUnitsPerDegree);
    }

    // Position offset rides the final (trimmed) frame: "2 cm forward" means
    // along the barrel as finally oriented.
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    float uuPerCm = ctx.worldScale / 100.0f;
    float of = g_posFwdCm.load(std::memory_order_relaxed) * uuPerCm;
    float orr = g_posRightCm.load(std::memory_order_relaxed) * uuPerCm;
    float ou = g_posUpCm.load(std::memory_order_relaxed) * uuPerCm;
    float loc[3] = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
                    gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
                    gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};

    if (g_mode.load(std::memory_order_relaxed) == 2) {
        // BONES (M7-v2): the actor stays engine-placed (eye anchor, correct
        // culling, correct engine-side FX anchoring) and the hand CLUSTER
        // moves to the controller instead.
        gp.loc = {loc[0], loc[1], loc[2]};
        if (!bones::drive(ctx, target, gp, active_hand())) return;
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
        BVR_LOG("[hands] scale has no working lever yet - three flat-proven dead ends "
                "2026-07-27 (ENGINE_NOTES session 16 part 2): cluster bone .s blows the "
                "attached weapon up (the attach path inverts chain scale), and the rig "
                "actor's DrawScale is geometry-inert on the fg path. Needs the "
                "attach/fg render-path disasm or the vm_draw lane - next session's task");
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
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &f, &r, &u) == 3) {
            g_posFwdCm.store(f, std::memory_order_relaxed);
            g_posRightCm.store(r, std::memory_order_relaxed);
            g_posUpCm.store(u, std::memory_order_relaxed);
            BVR_LOG("[hands] pos offset fwd%+.1f right%+.1f up%+.1f cm", f, r, u);
        } else {
            BVR_LOG("[hands] usage: vrhands pos <fwdCm> <rightCm> <upCm>");
        }
    } else if (strcmp(verb, "rot") == 0) {
        float p = 0.0f, y = 0.0f, r = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &p, &y, &r) == 3) {
            g_rotPitchDeg.store(p, std::memory_order_relaxed);
            g_rotYawDeg.store(y, std::memory_order_relaxed);
            g_rotRollDeg.store(r, std::memory_order_relaxed);
            BVR_LOG("[hands] rot trim pitch%+.1f yaw%+.1f roll%+.1f deg", p, y, r);
        } else {
            BVR_LOG("[hands] usage: vrhands rot <pitchDeg> <yawDeg> <rollDeg>");
        }
    } else if (strcmp(verb, "writerot") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_writeRot.store(on, std::memory_order_relaxed);
        BVR_LOG("[hands] rotation write %s", on ? "ON" : "off (position only)");
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
                "probe|hand|pos|rot|writerot|save|reload|test|simpose|testclear|status)",
                verb);
    }
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) &&
           (g_weaponActor != nullptr || g_handsActor != nullptr);
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

    float f = g_posFwdCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset forward (cm)", &f, -120.0f, 120.0f))
        g_posFwdCm.store(f, std::memory_order_relaxed);
    float r = g_posRightCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset right (cm)", &r, -120.0f, 120.0f))
        g_posRightCm.store(r, std::memory_order_relaxed);
    float u = g_posUpCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset up (cm)", &u, -120.0f, 120.0f))
        g_posUpCm.store(u, std::memory_order_relaxed);

    float rp = g_rotPitchDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim pitch (deg)", &rp, -90.0f, 90.0f))
        g_rotPitchDeg.store(rp, std::memory_order_relaxed);
    float ry = g_rotYawDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim yaw (deg)", &ry, -90.0f, 90.0f))
        g_rotYawDeg.store(ry, std::memory_order_relaxed);
    float rr = g_rotRollDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim roll (deg)", &rr, -180.0f, 180.0f))
        g_rotRollDeg.store(rr, std::memory_order_relaxed);

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
