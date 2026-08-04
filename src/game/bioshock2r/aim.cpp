#include "game/bioshock2r/aim.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/bones.h"
#include "game/bioshock2r/hands.h"

#include <windows.h>
#include <MinHook.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace bvr::b2r::aim {

std::atomic<bool> g_probeArmed{false};

namespace {

// ---- the seam hooks ---------------------------------------------------------
// Both impls are __thiscall; __fastcall detours with a dummy EDX slot are
// register/stack/cleanup-identical (the camera detours' proven pattern). Arg
// counts match ret imm/4 EXACTLY - the scanimpl lesson: a mismatch kills the
// game with an ESP RTC dialog that writes no crash dump.
// Weapon: (FVector* outLoc, FRotator* outRot, FVector* outEffectLoc), ret 0xC.
using WeaponGpfsFn = int(__fastcall*)(void* self, void* edx, FVector* outLoc,
                                      FRotator* outRot, FVector* outEffect);
// Ability: (ShockPlayer* tester, FVector* outLoc, FRotator* outRot,
//           FVector* outEffectLoc), ret 0x10.
using AbilityGpfsFn = int(__fastcall*)(void* self, void* edx, void* tester,
                                       FVector* outLoc, FRotator* outRot,
                                       FVector* outEffect);
WeaponGpfsFn g_origWeaponGpfs = nullptr;
AbilityGpfsFn g_origAbilityGpfs = nullptr;
std::atomic<bool> g_hookLive{false};

// Substitution controls (game thread applies; command seam writes).
// DEFAULT ON since the session-39 wrap (user request, first-look build): every
// gate below still requires strict gameplay + the HMD driving + a fresh
// tracked ray, so the defaults are inert flat and only bite in the headset.
std::atomic<bool> g_aimEnabled{true};    // master (`vraim on|off`)
std::atomic<bool> g_seamWeapon{true};    // per-seam gates (`vraim seam ...`)
std::atomic<bool> g_seamAbility{true};

// The synthetic test ray: yaw/pitch OFFSET in degrees from the live view
// rotation, self-expiring (BS1's decal-proof lane, fresh numbers).
std::atomic<float> g_testYawDeg{0.0f};
std::atomic<float> g_testPitchDeg{0.0f};
std::atomic<uint64_t> g_testDeadlineMs{0};

// Live view rotation from the CalcView tail (pass 1, post drive).
std::atomic<int32_t> g_viewPitch{0}, g_viewYaw{0}, g_viewRoll{0};
std::atomic<bool> g_viewGameplay{false};
std::atomic<uint64_t> g_viewStampMs{0};

// ---- the XR hand rays -------------------------------------------------------
// One per hand, rebuilt every pass-1 CalcView from the frame context. The fire
// seam reads these; the laser re-derives its own copy render-side from the
// same pose and the same trims (core's design note - shared algebra, not
// shared data), and the aim dot is published FROM this ray so "dot == shot" is
// true by construction rather than by argument.
struct HandRay {
    bool valid = false;
    FVector origin{};
    FRotator rot{};
    uint64_t stampMs = 0;
};
HandRay g_ray[2]; // game thread only; 0 = left (plasmid), 1 = right (weapon)

// DEFAULT ON since the session-39 wrap (user request): all inert without a
// live tracked controller in strict gameplay.
std::atomic<bool> g_handRayOn{true};    // `vraim handray` - XR poses drive the seam
std::atomic<bool> g_useAimPose{true};   // aim pose (runtime ray) vs grip pose
std::atomic<bool> g_laserOn{true};
std::atomic<bool> g_dotOn{true};
std::atomic<float> g_dotDistM{3.0f};
// Session 40 (user decision): BS2 is natively dual-wield, so BOTH hands get a
// laser and a dot at once - unlike BS1, where only one hand is ever active.
// Per-hand enables; core's second slot is a BS2-only addition that leaves the
// BS1 path byte-identical.
std::atomic<bool> g_laserHandOn[2] = {{true}, {true}};
std::atomic<bool> g_dotHandOn[2] = {{true}, {true}};
// Bullet-origin substitution (BS1's `vraim origin on` parity): write the fire
// start location from the hand ray so shots leave the HAND, not the head.
// Default ON, with a displacement refusal BS1 never had.
std::atomic<bool> g_handOrigin{true};
std::atomic<float> g_originMaxUu{200.0f};
std::atomic<uint32_t> g_originRefusals{0};
// Per-hand trims (degrees) and ray-origin offsets (cm), BS1's tuning surface.
// Defaults = the user's in-headset calibration, baked 2026-08-04 session 41
// round 2 (their explicit ask; BS1 session-21 precedent). Left = the plasmid
// hand's global tuning; right = the baseline the per-weapon profiles seed
// from. The preset still overrides key-by-key.
std::atomic<float> g_trimPitch[2] = {{1.19f}, {17.88f}};
std::atomic<float> g_trimYaw[2] = {{23.25f}, {-16.69f}};
std::atomic<float> g_posFwdCm[2] = {{0.0f}, {-3.58f}};
std::atomic<float> g_posRightCm[2] = {{3.18f}, {-1.19f}};
std::atomic<float> g_posUpCm[2] = {{0.0f}, {11.13f}};
// Telemetry for `vraim status` / the flat sweep.
std::atomic<float> g_lastRayYawDeg[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_lastRayPitchDeg[2] = {{0.0f}, {0.0f}};
std::atomic<bool> g_rayValid[2] = {{false}, {false}};
std::atomic<bool> g_loggedDotRoundTrip{false};

// The ray freshness gate, BS1's shape: enabled + gameplay + valid + young.
// Also the single place that decides a hand has a usable ray at all.
constexpr uint64_t kRayStaleMs = 250;

// Telemetry.
std::atomic<uint32_t> g_wepCalls{0}, g_abiCalls{0}, g_subs{0};
std::atomic<bool> g_loggedWepArgs{false}, g_loggedAbiArgs{false};

// The last weapon object seen at the fire seam (its detour `this`) - the
// GROUND TRUTH for the session-41 holdable-offset derivation: `vrbones
// holdscan find` scans the AHands actor for exactly this pointer. Read-only
// diagnostic; never dereferenced without fresh validation.
std::atomic<void*> g_lastWeaponThis{nullptr};

// Log the first N substitutions with BEFORE and AFTER rotators. This is the
// numeric half of the decoupled-aim proof: the engine's own value, the value
// we wrote, and the delta - which must equal the commanded offset in rotator
// units. Rotators print as INTs (the FRotator denormal trap).
std::atomic<uint32_t> g_subLogsLeft{6};

// Is this hand's XR ray usable right now? (BS1's ray_for gate.)
bool ray_for(int hand, FVector* origin, FRotator* rot) {
    if (hand < 0 || hand > 1) return false;
    if (!g_handRayOn.load(std::memory_order_relaxed)) return false;
    const HandRay& r = g_ray[hand];
    if (!r.valid) return false;
    if (GetTickCount64() - r.stampMs > kRayStaleMs) return false;
    if (origin) *origin = r.origin;
    if (rot) *rot = r.rot;
    return true;
}

// One substituted-rotator computation for both seams. Returns false when
// nothing should be substituted this call (the original's out-params stand).
// Sources in priority order: the live XR hand ray, then the synthetic test
// ray (the decal-proof lane, and the calibration lever when no headset is on).
bool substituted_rot(FRotator* rot, int hand) {
    if (!g_aimEnabled.load(std::memory_order_relaxed)) return false;
    uint64_t now = GetTickCount64();
    // View freshness: the rotation base must be this frame's - a stale base
    // aims at wherever the camera last was (matches the 250 ms ray gate BS1
    // ships; scripted scenes silence CalcView and disarm this).
    if (now - g_viewStampMs.load(std::memory_order_relaxed) > kRayStaleMs) return false;
    if (!g_viewGameplay.load(std::memory_order_relaxed)) return false;

    FRotator before = *rot;
    FRotator handRot{};
    if (ray_for(hand, nullptr, &handRot)) {
        rot->pitch = handRot.pitch;
        rot->yaw = handRot.yaw;
        // Roll deliberately preserved (BS1 shape): aim carries no roll.
        if (g_subLogsLeft.load(std::memory_order_relaxed) > 0) {
            g_subLogsLeft.fetch_sub(1, std::memory_order_relaxed);
            int32_t dYaw = wrap_rot(rot->yaw - before.yaw);
            int32_t dPitch = wrap_rot(rot->pitch - before.pitch);
            BVR_LOG("[b2r] aim substitute (hand %d XR ray): rot (%d %d %d) -> (%d %d %d), "
                    "delta yaw %.2f deg pitch %.2f deg",
                    hand, before.pitch, before.yaw, before.roll, rot->pitch, rot->yaw,
                    rot->roll, dYaw / kRotUnitsPerDegree, dPitch / kRotUnitsPerDegree);
        }
        return true;
    }
    if (now > g_testDeadlineMs.load(std::memory_order_relaxed)) return false;
    rot->pitch = wrap_rot(g_viewPitch.load(std::memory_order_relaxed) +
                          static_cast<int32_t>(g_testPitchDeg.load(std::memory_order_relaxed) *
                                               kRotUnitsPerDegree));
    rot->yaw = wrap_rot(g_viewYaw.load(std::memory_order_relaxed) +
                        static_cast<int32_t>(g_testYawDeg.load(std::memory_order_relaxed) *
                                             kRotUnitsPerDegree));
    // Roll deliberately preserved (BS1 shape).
    if (g_subLogsLeft.load(std::memory_order_relaxed) > 0) {
        g_subLogsLeft.fetch_sub(1, std::memory_order_relaxed);
        int32_t dYaw = wrap_rot(rot->yaw - before.yaw);
        int32_t dPitch = wrap_rot(rot->pitch - before.pitch);
        BVR_LOG("[b2r] aim substitute: rot (%d %d %d) -> (%d %d %d), delta yaw %d units "
                "(%.2f deg) pitch %d (%.2f deg)",
                before.pitch, before.yaw, before.roll, rot->pitch, rot->yaw, rot->roll,
                dYaw, dYaw / kRotUnitsPerDegree, dPitch, dPitch / kRotUnitsPerDegree);
    }
    return true;
}

// Bullet ORIGIN substitution (session 40): move the fire start to the hand
// ray's origin so the shot leaves the model, not the head. Runs only when the
// rotator substitution already succeeded (same ray, same gates), and refuses
// implausible displacements - BS1 shipped this with no clamp at all, and its
// measured real displacement was 40-47 UU at worldScale 100, so 200 UU is a
// wide-but-finite guard against a bad frame context writing a shot across the
// map. On refusal the engine's own location stands (the original ran first).
void substitute_origin(FVector* outLoc, FVector* outEffect, int hand) {
    if (!outLoc) return;
    if (!g_handOrigin.load(std::memory_order_relaxed)) return;
    FVector origin{};
    if (!ray_for(hand, &origin, nullptr)) return;
    float dx = origin.x - outLoc->x, dy = origin.y - outLoc->y, dz = origin.z - outLoc->z;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float maxUu = g_originMaxUu.load(std::memory_order_relaxed);
    if (d > maxUu) {
        uint32_t n = g_originRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3)
            BVR_LOG("[b2r] aim origin REFUSED (hand %d): engine (%.1f %.1f %.1f) -> hand "
                    "(%.1f %.1f %.1f) is %.1f UU > %.1f limit",
                    hand, outLoc->x, outLoc->y, outLoc->z, origin.x, origin.y, origin.z, d,
                    maxUu);
        return;
    }
    if (g_subLogsLeft.load(std::memory_order_relaxed) > 0)
        BVR_LOG("[b2r] aim origin (hand %d): loc (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), "
                "displacement %.1f UU",
                hand, outLoc->x, outLoc->y, outLoc->z, origin.x, origin.y, origin.z, d);
    *outLoc = origin;
    // The effect (muzzle flash / tracer start) rides the same point, or the
    // shot would visibly begin somewhere the bullet does not.
    if (outEffect) *outEffect = origin;
}

int __fastcall WeaponGpfsDetour(void* self, void* edx, FVector* outLoc, FRotator* outRot,
                                FVector* outEffect) {
    int r = g_origWeaponGpfs(self, edx, outLoc, outRot, outEffect);
    g_wepCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastWeaponThis.store(self, std::memory_order_relaxed);
    if (!g_loggedWepArgs.exchange(true, std::memory_order_relaxed) && outLoc && outRot)
        // Rotator prints as INTs - the FRotator denormal trap: as floats these
        // are denormals and print 0.000.
        BVR_LOG("[b2r] weapon GetPerfectFireStart #1: self=%p ret=%d loc=(%.1f %.1f %.1f) "
                "rot=(%d %d %d) effect=(%.1f %.1f %.1f)",
                self, r, outLoc->x, outLoc->y, outLoc->z, outRot->pitch, outRot->yaw,
                outRot->roll, outEffect ? outEffect->x : 0.0f,
                outEffect ? outEffect->y : 0.0f, outEffect ? outEffect->z : 0.0f);
    // Weapons are the RIGHT hand (BS1's fallback; BS2's native dual-wield puts
    // the weapon in the right hand and the plasmid in the left).
    if (outRot && g_seamWeapon.load(std::memory_order_relaxed) &&
        substituted_rot(outRot, 1)) {
        substitute_origin(outLoc, outEffect, 1);
        g_subs.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

int __fastcall AbilityGpfsDetour(void* self, void* edx, void* tester, FVector* outLoc,
                                 FRotator* outRot, FVector* outEffect) {
    int r = g_origAbilityGpfs(self, edx, tester, outLoc, outRot, outEffect);
    g_abiCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_loggedAbiArgs.exchange(true, std::memory_order_relaxed) && outLoc && outRot)
        BVR_LOG("[b2r] ability GetPerfectFireStart #1: self=%p tester=%p ret=%d "
                "loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                self, tester, r, outLoc->x, outLoc->y, outLoc->z, outRot->pitch,
                outRot->yaw, outRot->roll);
    // Abilities (plasmids) are the LEFT hand on BS2's native dual-wield.
    if (outRot && g_seamAbility.load(std::memory_order_relaxed) &&
        substituted_rot(outRot, 0)) {
        substitute_origin(outLoc, outEffect, 0);
        g_subs.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

void install_seam_hooks(const bvr::pattern_scan::ProcessImage& image) {
    patterns::GpfsImpls impls{};
    patterns::resolve_gpfs_impls(image, impls);
    bool any = false;
    if (impls.weapon) {
        MH_STATUS st = MH_CreateHook(impls.weapon,
                                     reinterpret_cast<void*>(&WeaponGpfsDetour),
                                     reinterpret_cast<void**>(&g_origWeaponGpfs));
        if (st == MH_OK) st = MH_EnableHook(impls.weapon);
        if (st != MH_OK)
            BVR_LOG("[b2r] weapon GetPerfectFireStart hook failed: %s",
                    MH_StatusToString(st));
        else
            any = true;
    }
    if (impls.ability) {
        MH_STATUS st = MH_CreateHook(impls.ability,
                                     reinterpret_cast<void*>(&AbilityGpfsDetour),
                                     reinterpret_cast<void**>(&g_origAbilityGpfs));
        if (st == MH_OK) st = MH_EnableHook(impls.ability);
        if (st != MH_OK)
            BVR_LOG("[b2r] ability GetPerfectFireStart hook failed: %s",
                    MH_StatusToString(st));
        else
            any = true;
    }
    g_hookLive.store(any, std::memory_order_relaxed);
    if (any)
        BVR_LOG("[b2r] aim seam hooks live (weapon=%d ability=%d) - telemetry mode, "
                "substitution armed by `vraim on` + a ray source",
                impls.weapon && g_origWeaponGpfs ? 1 : 0,
                impls.ability && g_origAbilityGpfs ? 1 : 0);
}

// ---- fire-watch -------------------------------------------------------------
// One row per fire-chain name: the Lane-A index global (null = none exists in
// this exe), the UFunction* learned when FindFunctionChecked resolves that
// index, and hit counters on both sides of the dispatch. Counters are plain
// uint32 - ProcessEvent is game-thread-only and so is every reader.
struct WatchRow {
    const char* name = nullptr;
    const uint8_t* indexGlobal = nullptr;
    void* fn = nullptr;    // learned UFunction*, null until seen
    uint32_t ffHits = 0;   // FindFunctionChecked resolutions of this index
    uint32_t peHits = 0;   // ProcessEvent dispatches of the learned pointer
};
WatchRow g_watch[patterns::FireNames::kMax];
int g_watchCount = 0;

// ---- census -----------------------------------------------------------------
// Dedup table of every dispatched function-name index while armed. Fixed and
// linear-probed; 0xFFFFFFFF marks an empty slot (index 0 = 'None' is a valid
// FName). Game thread only.
constexpr uint32_t kCensusSlots = 256;
struct CensusSlot {
    uint32_t index;
    uint32_t hits;
};
CensusSlot g_census[kCensusSlots];
uint32_t g_censusUsed = 0;
uint32_t g_censusOverflow = 0; // dispatches dropped because the table filled
uint32_t g_censusUnreadable = 0;

// UFunction name-field offset, self-derived (-1 = not yet). Derivation: scan
// the PlayerCalcView UFunction (known pointer, known index from
// *g_calcViewIndexGlobal) for {index, 0} - the 8-byte FName {index, number}.
int g_nameOffset = -1;
const uint8_t* g_calcViewIndexGlobal = nullptr; // patterns::Symbols.fnameIndexGlobal
uint32_t g_nameOffsetCandidates = 0;            // >1 = ambiguous, logged

uint64_t g_lastSummaryMs = 0;
uint32_t g_peSeen = 0; // total ProcessEvent dispatches while armed

void census_clear() {
    memset(g_census, 0xFF, sizeof(g_census));
    for (auto& s : g_census) s.hits = 0;
    g_censusUsed = 0;
    g_censusOverflow = 0;
    g_censusUnreadable = 0;
    g_peSeen = 0;
}

void census_add(uint32_t index) {
    uint32_t h = (index * 2654435761u) & (kCensusSlots - 1);
    for (uint32_t i = 0; i < kCensusSlots; ++i) {
        CensusSlot& s = g_census[(h + i) & (kCensusSlots - 1)];
        if (s.index == index) {
            ++s.hits;
            return;
        }
        if (s.index == 0xFFFFFFFF) {
            s.index = index;
            s.hits = 1;
            ++g_censusUsed;
            return;
        }
    }
    ++g_censusOverflow;
}

// Try to pin the UFunction name-field offset from a known (fn, index) pair.
// Requires the number half to be zero right behind the index - every
// fire-chain name is number-less. First match wins; every match is counted
// so an ambiguity names itself in the log (the census text output is the
// cross-check: a wrong offset prints garbage names).
void derive_name_offset(void* fn, uint32_t knownIndex) {
    if (!bvr::pattern_scan::is_memory_valid(fn, 0x100)) return;
    const uint8_t* p = static_cast<const uint8_t*>(fn);
    int first = -1;
    uint32_t matches = 0;
    for (int off = 0; off + 8 <= 0x100; off += 4) {
        uint32_t idx = 0, num = 0;
        memcpy(&idx, p + off, 4);
        memcpy(&num, p + off + 4, 4);
        if (idx == knownIndex && num == 0) {
            ++matches;
            if (first < 0) first = off;
        }
    }
    if (first >= 0) {
        g_nameOffset = first;
        g_nameOffsetCandidates = matches;
        BVR_LOG("[b2r] aim probe: UFunction name offset = +0x%X (%u candidate(s) in the "
                "first 0x100 bytes%s)",
                first, matches,
                matches > 1 ? " - AMBIGUOUS, census text is the cross-check" : "");
    }
}

bool token(const char* args, const char* word, const char** rest = nullptr) {
    size_t n = strlen(word);
    if (strncmp(args, word, n) != 0) return false;
    char t = args[n];
    if (t != '\0' && t != ' ' && t != '\t' && t != '\r' && t != '\n') return false;
    if (rest) {
        const char* r = args + n;
        while (*r == ' ' || *r == '\t') ++r;
        *rest = r;
    }
    return true;
}

void log_status() {
    BVR_LOG("[b2r] vraim status: probe %s, %d fire-name(s), name offset %s, census "
            "%u/%u used (overflow %u, unreadable %u), pe seen %u",
            g_probeArmed.load(std::memory_order_relaxed) ? "ARMED" : "off", g_watchCount,
            g_nameOffset >= 0 ? "derived" : "PENDING", g_censusUsed, kCensusSlots,
            g_censusOverflow, g_censusUnreadable, g_peSeen);
    for (int i = 0; i < g_watchCount; ++i) {
        const WatchRow& w = g_watch[i];
        BVR_LOG("[b2r]   %-20s global=%s fn=%p ff=%u pe=%u", w.name,
                w.indexGlobal ? "yes" : "no", w.fn, w.ffHits, w.peHits);
    }
    uint64_t now = GetTickCount64();
    uint64_t viewAge = now - g_viewStampMs.load(std::memory_order_relaxed);
    uint64_t testLeft = g_testDeadlineMs.load(std::memory_order_relaxed);
    BVR_LOG("[b2r]   seam: hooks %s, enable %s, weapon %s ability %s, calls wep=%u "
            "abi=%u subs=%u, view age %llu ms (gameplay=%d), test %s (yaw %.1f pitch "
            "%.1f, %lld ms left)",
            g_hookLive.load(std::memory_order_relaxed) ? "LIVE" : "off",
            g_aimEnabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_seamWeapon.load(std::memory_order_relaxed) ? "on" : "OFF",
            g_seamAbility.load(std::memory_order_relaxed) ? "on" : "OFF",
            g_wepCalls.load(std::memory_order_relaxed),
            g_abiCalls.load(std::memory_order_relaxed),
            g_subs.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(viewAge),
            g_viewGameplay.load(std::memory_order_relaxed) ? 1 : 0,
            now < testLeft ? "ACTIVE" : "off",
            g_testYawDeg.load(std::memory_order_relaxed),
            g_testPitchDeg.load(std::memory_order_relaxed),
            static_cast<long long>(testLeft - now));
    BVR_LOG("[b2r]   rays: handray %s pose %s, L %s yaw %.1f pitch %.1f, R %s yaw %.1f "
            "pitch %.1f; laser %s (L %s R %s), dot %s (L %s R %s) %.1f m",
            g_handRayOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
            g_rayValid[0].load(std::memory_order_relaxed) ? "valid" : "-",
            g_lastRayYawDeg[0].load(std::memory_order_relaxed),
            g_lastRayPitchDeg[0].load(std::memory_order_relaxed),
            g_rayValid[1].load(std::memory_order_relaxed) ? "valid" : "-",
            g_lastRayYawDeg[1].load(std::memory_order_relaxed),
            g_lastRayPitchDeg[1].load(std::memory_order_relaxed),
            g_laserOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_laserHandOn[0].load(std::memory_order_relaxed) ? "on" : "off",
            g_laserHandOn[1].load(std::memory_order_relaxed) ? "on" : "off",
            g_dotOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_dotHandOn[0].load(std::memory_order_relaxed) ? "on" : "off",
            g_dotHandOn[1].load(std::memory_order_relaxed) ? "on" : "off",
            g_dotDistM.load(std::memory_order_relaxed));
    for (int h = 0; h < 2; ++h)
        BVR_LOG("[b2r]   %s tuning: trim %.1f/%.1f, ray pos %.1f/%.1f/%.1f cm",
                h ? "R" : "L", g_trimPitch[h].load(std::memory_order_relaxed),
                g_trimYaw[h].load(std::memory_order_relaxed),
                g_posFwdCm[h].load(std::memory_order_relaxed),
                g_posRightCm[h].load(std::memory_order_relaxed),
                g_posUpCm[h].load(std::memory_order_relaxed));
    BVR_LOG("[b2r]   origin substitution: %s (max %.0f UU, %u refusals) - shots leave "
            "the %s",
            g_handOrigin.load(std::memory_order_relaxed) ? "ON" : "off",
            g_originMaxUu.load(std::memory_order_relaxed),
            g_originRefusals.load(std::memory_order_relaxed),
            g_handOrigin.load(std::memory_order_relaxed) ? "HAND" : "engine's own point");
}

void census_dump() {
    BVR_LOG("[b2r] aim probe census: %u distinct name(s), overflow %u, unreadable %u, "
            "pe seen %u, name offset %s",
            g_censusUsed, g_censusOverflow, g_censusUnreadable, g_peSeen,
            g_nameOffset >= 0 ? "derived" : "PENDING (dump is empty until the probe "
                                            "sees PlayerCalcView resolve once)");
    for (uint32_t i = 0; i < kCensusSlots; ++i) {
        const CensusSlot& s = g_census[i];
        if (s.index == 0xFFFFFFFF) continue;
        char text[48];
        if (!patterns::fname_text(s.index, text, sizeof(text)))
            snprintf(text, sizeof(text), "<unreadable>");
        BVR_LOG("[b2r]   census idx=%-6u hits=%-6u %s", s.index, s.hits, text);
    }
}

// Defined in the profile section below; init() runs first in the file.
void seed_default_profiles();
void load_weapon_profiles();

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols) {
    patterns::FireNames names{};
    patterns::resolve_fire_names(image, names);
    g_watchCount = names.count;
    for (int i = 0; i < names.count; ++i) {
        g_watch[i].name = names.name[i];
        g_watch[i].indexGlobal = names.indexGlobal[i];
    }
    g_calcViewIndexGlobal = symbols.fnameIndexGlobal;
    census_clear();
    install_seam_hooks(image);

    // GNames smoke test - index 0 is 'None' on every UE2 build; the
    // PlayerCalcView index (live in the cached global) must read back as
    // itself. Failure is loud and non-fatal: the census degrades to raw
    // indexes, the fire-watch is unaffected.
    char t0[48], t1[48];
    bool ok0 = patterns::fname_text(0, t0, sizeof(t0));
    uint32_t cvIdx = g_calcViewIndexGlobal
                         ? *reinterpret_cast<const uint32_t*>(g_calcViewIndexGlobal)
                         : 0;
    bool ok1 = cvIdx ? patterns::fname_text(cvIdx, t1, sizeof(t1)) : false;
    BVR_LOG("[b2r] aim probe init: %d fire-name(s); GNames[0]=%s GNames[%u]=%s",
            g_watchCount, ok0 ? t0 : "<FAIL>", cvIdx, ok1 ? t1 : "<FAIL>");
    if (ok0 && strcmp(t0, "None") != 0)
        BVR_LOG("[b2r] aim probe: GNames[0] is not 'None' - RVA suspect, census text "
                "untrusted");

    // Per-weapon profiles: the user's baked calibration seeds the defaults
    // (session 41 round 2 - it IS a value source now, rule (a) satisfied by
    // construction); weapons.ini then overrides key-by-key.
    seed_default_profiles();
    load_weapon_profiles();
}

// --- per-weapon profiles (session 41; BS1's session-21 shape, adapted) ------
// RIGHT hand + uniform weapon scale only, keyed by the holdable's class name
// (the user's session-41 decision: the left/plasmid hand does not change on
// weapon switches, so its tuning stays global in vrpreset until a per-plasmid
// key becomes derivable). The live atomics remain the single truth - sliders
// and commands keep writing them; this layer stashes them into the active
// profile on swap/save and restores on swap-back. Session-21 rules, all four:
// (a) the resolver IDLES until a value source exists (preset baseline
//     captured or weapons.ini loaded) - the seeding-race fix;
// (b) identity comes from the rig's live holdable only - no fire-learned
//     fallback lanes exist on BS2 at all;
// (c) the holdable is never vtable-gated - object_class_name validates the
//     CLASS;
// (d) an unresolvable class CLEARS the key (edits touch no profile, logged).
namespace {

struct WeaponProfile {
    float aimTrimPitch, aimTrimYaw, aimPosFwd, aimPosRight, aimPosUp;
    float modTrimPitch, modTrimYaw, modTrimRoll;
    float modOffFwd, modOffRight, modOffUp, modScale;
    float wScale;
    // Session 41 round 2: the weapon's own offset from the hand (attach-pivot
    // base), cm in the hand's trimmed basis.
    float wOffFwd, wOffRight, wOffUp;
};
std::map<std::string, WeaponProfile> g_weaponProfiles;
std::string g_weaponKey;              // "" = none
void* g_weaponKeyActor = nullptr;     // pointer the key was resolved from
bool g_weaponKeySim = false;          // `wkey sim` latch (flat testing)
uint32_t g_weaponSwaps = 0;
std::mutex g_weaponKeyUiMutex;        // overlay copy (render thread reads)
char g_weaponKeyUi[48] = "-";
WeaponProfile g_presetBaseline{};
bool g_presetBaselineValid = false;

WeaponProfile snapshot_live() {
    WeaponProfile p{};
    p.aimTrimPitch = g_trimPitch[1].load(std::memory_order_relaxed);
    p.aimTrimYaw = g_trimYaw[1].load(std::memory_order_relaxed);
    p.aimPosFwd = g_posFwdCm[1].load(std::memory_order_relaxed);
    p.aimPosRight = g_posRightCm[1].load(std::memory_order_relaxed);
    p.aimPosUp = g_posUpCm[1].load(std::memory_order_relaxed);
    p.modTrimPitch = hands::trim_pitch(1);
    p.modTrimYaw = hands::trim_yaw(1);
    p.modTrimRoll = hands::trim_roll(1);
    p.modOffFwd = hands::off_fwd_cm(1);
    p.modOffRight = hands::off_right_cm(1);
    p.modOffUp = hands::off_up_cm(1);
    p.modScale = bones::scale_of(1);
    p.wScale = bones::weapon_scale();
    p.wOffFwd = bones::weapon_off_fwd_cm();
    p.wOffRight = bones::weapon_off_right_cm();
    p.wOffUp = bones::weapon_off_up_cm();
    return p;
}

void apply_profile_values(const WeaponProfile& p) {
    g_trimPitch[1].store(p.aimTrimPitch, std::memory_order_relaxed);
    g_trimYaw[1].store(p.aimTrimYaw, std::memory_order_relaxed);
    g_posFwdCm[1].store(p.aimPosFwd, std::memory_order_relaxed);
    g_posRightCm[1].store(p.aimPosRight, std::memory_order_relaxed);
    g_posUpCm[1].store(p.aimPosUp, std::memory_order_relaxed);
    hands::set_trim(1, p.modTrimPitch, p.modTrimYaw, p.modTrimRoll);
    hands::set_offset(1, p.modOffFwd, p.modOffRight, p.modOffUp);
    bones::set_scale(1, p.modScale);
    bones::set_weapon_scale(p.wScale);
    bones::set_weapon_offset(p.wOffFwd, p.wOffRight, p.wOffUp);
}

// Class name -> ini-safe key: printable ASCII, '.'/'=' (the line format's own
// separators) mapped to '_', capped at 47.
void narrow_key(const char* name, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < cap && j < 47; ++i) {
        char c = name[i];
        if (c < 32 || c > 126) continue;
        if (c == '.' || c == '=') c = '_';
        out[j++] = c;
    }
    out[j] = '\0';
}

void stash_active_profile() {
    if (g_weaponKey.empty()) return;
    g_weaponProfiles[g_weaponKey] = snapshot_live();
}

void apply_weapon_key(const std::string& key, const char* why) {
    if (key == g_weaponKey) return;
    stash_active_profile();
    g_weaponKey = key;
    {
        std::lock_guard<std::mutex> lk(g_weaponKeyUiMutex);
        strncpy_s(g_weaponKeyUi, key.empty() ? "-" : key.c_str(), _TRUNCATE);
    }
    if (key.empty()) {
        BVR_LOG("[b2r] weapon profile: key cleared (%s)", why);
        return;
    }
    ++g_weaponSwaps;
    auto it = g_weaponProfiles.find(key);
    if (it == g_weaponProfiles.end()) {
        // First sight: seed from the CAPTURED preset baseline, never from the
        // outgoing weapon's live values (session-21's seeding rule).
        WeaponProfile p = g_presetBaselineValid ? g_presetBaseline : snapshot_live();
        g_weaponProfiles[key] = p;
        apply_profile_values(p);
        BVR_LOG("[b2r] weapon profile '%s' CREATED from %s (%s)", key.c_str(),
                g_presetBaselineValid ? "the preset baseline" : "current R values",
                why);
    } else {
        apply_profile_values(it->second);
        BVR_LOG("[b2r] weapon profile '%s' applied (%s)", key.c_str(), why);
    }
}

// Per-frame resolver (throttled). Keys the profile off the rig's LIVE
// holdable; a rig-unknown frame is NO SIGNAL (key kept), a resolvable change
// swaps, null holdable or unresolvable class clears.
void update_weapon_profile() {
    if (g_weaponKeySim) return;
    if (!g_presetBaselineValid && g_weaponProfiles.empty()) return; // rule (a)
    static uint32_t throttle = 0;
    if ((++throttle & 15) != 0) return;
    void* w = nullptr;
    if (!bones::current_holdable(&w)) return; // rig unknown - no signal
    if (w == g_weaponKeyActor) return;        // steady-state cost: one compare
    char name[48] = {};
    bool resolved = w && patterns::object_class_name(w, name, sizeof name);
    if (w && !resolved) {
        // rule (d): edits must not land in the previous weapon's profile.
        g_weaponKeyActor = w;
        BVR_LOG("[b2r] weapon profile: holdable %p has NO resolvable class name - "
                "key cleared, slider edits will touch no profile until it resolves",
                w);
        apply_weapon_key(std::string(), "unresolvable class");
        return;
    }
    g_weaponKeyActor = w;
    if (!w) {
        apply_weapon_key(std::string(), "nothing equipped");
        return;
    }
    char key[48];
    narrow_key(name, key, sizeof key);
    apply_weapon_key(std::string(key), "weapon change");
}

void weapons_ini_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\weapons.ini", bvr::log::data_dir());
}

// One field table drives load, save and the format doc: <Class>.<field>=<v>.
struct ProfileField {
    const char* name;
    float WeaponProfile::* member;
};
constexpr ProfileField kProfileFields[] = {
    {"aimTrimPitch", &WeaponProfile::aimTrimPitch},
    {"aimTrimYaw", &WeaponProfile::aimTrimYaw},
    {"aimPosFwd", &WeaponProfile::aimPosFwd},
    {"aimPosRight", &WeaponProfile::aimPosRight},
    {"aimPosUp", &WeaponProfile::aimPosUp},
    {"modTrimPitch", &WeaponProfile::modTrimPitch},
    {"modTrimYaw", &WeaponProfile::modTrimYaw},
    {"modTrimRoll", &WeaponProfile::modTrimRoll},
    {"modOffFwd", &WeaponProfile::modOffFwd},
    {"modOffRight", &WeaponProfile::modOffRight},
    {"modOffUp", &WeaponProfile::modOffUp},
    {"modScale", &WeaponProfile::modScale},
    {"wScale", &WeaponProfile::wScale},
    {"wOffFwd", &WeaponProfile::wOffFwd},
    {"wOffRight", &WeaponProfile::wOffRight},
    {"wOffUp", &WeaponProfile::wOffUp},
};

// The user's in-headset calibration, baked as DEFAULT profiles (2026-08-04
// session 41 round 2, their explicit ask; BS1's seed_default_profiles
// precedent). Seeded BEFORE weapons.ini loads, so a user file overrides
// key-by-key - and their existence is itself a value source, which keeps the
// resolver live even on a virgin install (rule (a) satisfied by
// construction). Shared model trims across all: trim 12.5/-7.5/3.25 scale
// 0.76-0.77; per-weapon aim trims/pos + wScale as tuned.
void seed_default_profiles() {
    struct Row2 {
        const char* key;
        float aTP, aTY, aPF, aPR, aPU, mScale, wScale, wOF, wOR, wOU;
    };
    // Round-3 bake (2026-08-04): the user's round-2 in-headset pass added the
    // per-weapon weapon offsets and a shotgun model-scale touch-up.
    static const Row2 kRows[] = {
        {"PlayerDistanceHackingTool", 11.72f, -8.94f, 0.00f, 0.00f, 11.13f, 0.76f,
         0.75f, 0.00f, 0.00f, 0.00f},
        {"PlayerDrill", 15.50f, -10.93f, -19.87f, 2.38f, 18.68f, 0.76f, 0.75f, 0.00f,
         0.00f, 0.00f},
        {"PlayerGrenadeLauncher", 10.33f, -12.32f, 0.00f, -4.37f, 26.62f, 0.76f,
         0.75f, 0.00f, 0.00f, 0.00f},
        {"PlayerMachineGun", 12.52f, -9.93f, 0.00f, -3.97f, 26.62f, 0.76f, 0.75f,
         -7.47f, 0.00f, 0.00f},
        {"PlayerResearchVideoCamera", 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 1.35f, 1.00f,
         0.00f, 0.00f, 0.00f},
        {"PlayerRivetGun", 17.88f, -16.69f, -3.58f, -1.19f, 11.13f, 0.76f, 0.77f,
         -6.30f, 0.00f, 0.00f},
        {"PlayerShotgun", 14.31f, -14.31f, 1.99f, -0.79f, 10.33f, 0.79f, 0.77f,
         -11.44f, 0.00f, 0.00f},
        {"PlayerSpeargun", 14.90f, -15.89f, 17.48f, -17.48f, 29.40f, 0.76f, 0.75f,
         -7.24f, 0.00f, -2.10f},
    };
    for (const Row2& r : kRows) {
        WeaponProfile p{};
        p.aimTrimPitch = r.aTP;
        p.aimTrimYaw = r.aTY;
        p.aimPosFwd = r.aPF;
        p.aimPosRight = r.aPR;
        p.aimPosUp = r.aPU;
        p.modTrimPitch = 12.50f;
        p.modTrimYaw = -7.50f;
        p.modTrimRoll = 3.25f;
        p.modOffFwd = 0.0f;
        p.modOffRight = 0.0f;
        p.modOffUp = 0.0f;
        p.modScale = r.mScale;
        p.wScale = r.wScale;
        p.wOffFwd = r.wOF;
        p.wOffRight = r.wOR;
        p.wOffUp = r.wOU;
        g_weaponProfiles[r.key] = p;
    }
    BVR_LOG("[b2r] weapon profiles: %zu defaults seeded (user calibration bake)",
            g_weaponProfiles.size());
}

void load_weapon_profiles() {
    wchar_t path[MAX_PATH];
    weapons_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file = nothing tuned yet
    char line[160];
    int values = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[48] = {}, field[32] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%47[^.].%31[^=]=%f", key, (unsigned)sizeof key, field,
                     (unsigned)sizeof field, &v) != 3)
            continue;
        // Scales guard the same band the setters do; a corrupt line must not
        // zero a weapon.
        WeaponProfile& p = g_weaponProfiles[key]; // default-constructed once
        bool known = false;
        for (const auto& pf : kProfileFields)
            if (strcmp(field, pf.name) == 0) {
                if ((pf.member == &WeaponProfile::modScale ||
                     pf.member == &WeaponProfile::wScale) &&
                    !(v > 0.05f && v < 20.0f))
                    break;
                p.*pf.member = v;
                known = true;
                break;
            }
        if (known) ++values;
    }
    fclose(f);
    if (!g_weaponProfiles.empty())
        BVR_LOG("[b2r] weapons.ini: %zu profile(s), %d value(s) loaded",
                g_weaponProfiles.size(), values);
}

} // namespace

void save_weapon_profiles() {
    stash_active_profile(); // the active weapon's live edits are part of it
    wchar_t path[MAX_PATH];
    weapons_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[b2r] could not write weapons.ini");
        return;
    }
    fprintf(f, "# BioShock 2 VR - per-weapon RIGHT-hand aim/model profiles\n");
    fprintf(f, "# <Class>.<field>=<value>; fields: aimTrim*/aimPos* (deg/cm), "
               "modTrim*/modOff* (deg/cm), modScale, wScale\n");
    for (const auto& kv : g_weaponProfiles)
        for (const auto& pf : kProfileFields)
            fprintf(f, "%s.%s=%.2f\n", kv.first.c_str(), pf.name,
                    kv.second.*pf.member);
    fclose(f);
    BVR_LOG("[b2r] weapons.ini: %zu profile(s) saved to %ls", g_weaponProfiles.size(),
            path);
}

void note_preset_baseline() {
    g_presetBaseline = snapshot_live();
    g_presetBaselineValid = true;
    BVR_LOG("[b2r] weapon profiles: preset baseline captured (new profiles seed "
            "from it)");
}

void reapply_weapon_profile() {
    // The preset values are a BASELINE, not an edit: the active profile beats
    // them, and nothing is stashed on the way (stashing here would overwrite
    // the profile with the just-loaded preset values).
    if (g_weaponKey.empty()) return;
    auto it = g_weaponProfiles.find(g_weaponKey);
    if (it == g_weaponProfiles.end()) return;
    apply_profile_values(it->second);
    BVR_LOG("[b2r] weapon profile '%s' re-applied over the preset",
            g_weaponKey.c_str());
}

void weapon_key_ui(char* out, size_t cap) {
    std::lock_guard<std::mutex> lk(g_weaponKeyUiMutex);
    strncpy_s(out, cap, g_weaponKeyUi, _TRUNCATE);
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void on_calcview(const FrameContext& ctx, bool strictGameplay) {
    uint64_t now = GetTickCount64();
    g_viewPitch.store(ctx.camPitch, std::memory_order_relaxed);
    g_viewYaw.store(ctx.camYaw, std::memory_order_relaxed);
    g_viewRoll.store(ctx.camRoll, std::memory_order_relaxed);
    g_viewGameplay.store(strictGameplay, std::memory_order_relaxed);
    g_viewStampMs.store(now, std::memory_order_relaxed);

    // Per-weapon profile resolver (session 41) - throttled, idles until a
    // value source exists, keys off the rig's live holdable.
    if (strictGameplay) update_weapon_profile();

    // Build both hand rays through the SAME transform the camera just used.
    bool useAim = g_useAimPose.load(std::memory_order_relaxed);
    bool gate = strictGameplay && ctx.vrDriving;
    for (int h = 0; h < 2; ++h) {
        bvr::vr::HeadPose hp{};
        if (!gate || !bvr::vr::get_hand_pose(h, useAim, hp)) {
            g_ray[h].valid = false;
            g_rayValid[h].store(false, std::memory_order_relaxed);
            continue;
        }
        float pos[3] = {hp.px, hp.py, hp.pz};
        float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        GamePose gp = ray_pose_from_xr(ctx, pos, quat,
                                       g_trimPitch[h].load(std::memory_order_relaxed),
                                       g_trimYaw[h].load(std::memory_order_relaxed));
        // Ray-origin offset, in the FINAL trimmed ray's basis: cm -> UU by
        // worldScale/100, so the game side and core's laser (which applies the
        // same cm offset in the same frame) agree by construction.
        float fwd[3], right[3], up[3];
        ue_rot_basis(gp.rot, fwd, right, up);
        float k = ctx.worldScale / 100.0f;
        float oFwd = g_posFwdCm[h].load(std::memory_order_relaxed) * k;
        float oRight = g_posRightCm[h].load(std::memory_order_relaxed) * k;
        float oUp = g_posUpCm[h].load(std::memory_order_relaxed) * k;
        gp.loc.x += fwd[0] * oFwd + right[0] * oRight + up[0] * oUp;
        gp.loc.y += fwd[1] * oFwd + right[1] * oRight + up[1] * oUp;
        gp.loc.z += fwd[2] * oFwd + right[2] * oRight + up[2] * oUp;

        g_ray[h].valid = true;
        g_ray[h].origin = gp.loc;
        g_ray[h].rot = gp.rot;
        g_ray[h].stampMs = now;
        g_rayValid[h].store(true, std::memory_order_relaxed);
        g_lastRayYawDeg[h].store(gp.rot.yaw / kRotUnitsPerDegree, std::memory_order_relaxed);
        g_lastRayPitchDeg[h].store(gp.rot.pitch / kRotUnitsPerDegree,
                                   std::memory_order_relaxed);
    }

    // Publish BOTH lasers and BOTH dots (session 40, the user's call): BS2 is
    // natively dual-wield, so the weapon hand and the plasmid hand each get
    // their own beam ending on their own dot. Core's slot 1 is an additive
    // BS2-only lane; the shared dot budget means the layer count cannot grow
    // past what one beam could already reach.
    //
    // Core re-derives each ray render-side from the same hand pose and the
    // same trims, so these fields MUST mirror the ray's.
    float dotDistM = g_dotDistM.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        int slot = (h == 1) ? 0 : 1; // right hand keeps slot 0 (BS1 parity)
        bvr::vr::LaserConfig lc{};
        lc.enabled = g_laserOn.load(std::memory_order_relaxed) &&
                     g_laserHandOn[h].load(std::memory_order_relaxed) && gate;
        lc.hand = h;
        lc.pitchTrimDeg = g_trimPitch[h].load(std::memory_order_relaxed);
        lc.yawTrimDeg = g_trimYaw[h].load(std::memory_order_relaxed);
        lc.posFwdCm = g_posFwdCm[h].load(std::memory_order_relaxed);
        lc.posRightCm = g_posRightCm[h].load(std::memory_order_relaxed);
        lc.posUpCm = g_posUpCm[h].load(std::memory_order_relaxed);
        // Four dots per hand: two beams share core's 8-quad budget.
        lc.dots = 4;
        // The beam ENDS on the aim dot instead of running past it. The first
        // look reported "two dots" - the beam's 6 m end dot and the fixed 3 m
        // aim dot sitting apart. One bright point per hand now.
        lc.farM = dotDistM;
        bvr::vr::set_laser_slot(slot, lc);

        // Publish the aim dot FROM the final game-space ray (core's design
        // note: the dot is the point the shot starts from, mapped back into
        // XR, so "dot == shot" is exact rather than congruent).
        bvr::vr::AimDotConfig dc{};
        dc.enabled = g_dotOn.load(std::memory_order_relaxed) &&
                     g_dotHandOn[h].load(std::memory_order_relaxed);
        FVector origin{};
        FRotator rrot{};
        if (dc.enabled && ray_for(h, &origin, &rrot)) {
            float dir[3];
            ue_rot_to_dir(rrot, dir);
            float distUu = dotDistM * ctx.worldScale;
            FVector pt{origin.x + dir[0] * distUu, origin.y + dir[1] * distUu,
                       origin.z + dir[2] * distUu};
            game_point_to_xr(ctx, pt, dc.posXr);
            dc.valid = true;
            // One-shot round-trip self-check: map the point back out and
            // compare. A mismatch means the forward/inverse pair disagree -
            // the one bug class that would make the dot lie about where the
            // shot goes.
            if (h == 1 && !g_loggedDotRoundTrip.exchange(true, std::memory_order_relaxed)) {
                const float identQ[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                GamePose back = xr_pose_to_game(ctx, dc.posXr, identQ);
                float dx = back.loc.x - pt.x, dy = back.loc.y - pt.y,
                      dz = back.loc.z - pt.z;
                BVR_LOG("[b2r] aim dot round-trip: game (%.1f %.1f %.1f) -> xr (%.3f "
                        "%.3f %.3f) -> game (%.1f %.1f %.1f), error %.4f UU",
                        pt.x, pt.y, pt.z, dc.posXr[0], dc.posXr[1], dc.posXr[2],
                        back.loc.x, back.loc.y, back.loc.z,
                        sqrtf(dx * dx + dy * dy + dz * dz));
            }
        }
        bvr::vr::set_aim_dot_slot(slot, dc);
    }
}

void probe_findfunc(uint32_t nameIndex, uint32_t nameNumber, void* fn) {
    if (!fn) return;
    for (int i = 0; i < g_watchCount; ++i) {
        WatchRow& w = g_watch[i];
        if (!w.indexGlobal) continue;
        if (nameIndex == *reinterpret_cast<const uint32_t*>(w.indexGlobal)) {
            ++w.ffHits;
            if (w.fn != fn) {
                w.fn = fn;
                BVR_LOG("[b2r] aim probe: %s UFunction learned: %p", w.name, fn);
            }
        }
    }
    // Self-derive the UFunction name offset off the one (fn, index) pair we
    // know for certain - PlayerCalcView resolves hundreds of times a second,
    // so this lands within a frame of arming.
    if (g_nameOffset < 0 && nameNumber == 0 && g_calcViewIndexGlobal &&
        nameIndex == *reinterpret_cast<const uint32_t*>(g_calcViewIndexGlobal))
        derive_name_offset(fn, nameIndex);
}

void probe_process_event(void* fn) {
    ++g_peSeen;
    if (!fn) return;
    for (int i = 0; i < g_watchCount; ++i) {
        WatchRow& w = g_watch[i];
        if (w.fn && w.fn == fn) ++w.peHits;
    }
    if (g_nameOffset >= 0) {
        const uint8_t* p = static_cast<const uint8_t*>(fn) + g_nameOffset;
        if (bvr::pattern_scan::is_memory_valid(p, 4)) {
            uint32_t idx = 0;
            memcpy(&idx, p, 4);
            census_add(idx);
        } else {
            ++g_censusUnreadable;
        }
    }
}

void poll_tick(uint64_t now) {
    if (!g_probeArmed.load(std::memory_order_relaxed)) return;
    if (now - g_lastSummaryMs < 1000) return;
    g_lastSummaryMs = now;

    static uint32_t s_lastPeSeen = 0;
    static uint32_t s_lastFf[patterns::FireNames::kMax] = {};
    static uint32_t s_lastPe[patterns::FireNames::kMax] = {};
    char line[256];
    int pos = 0;
    for (int i = 0; i < g_watchCount; ++i) {
        const WatchRow& w = g_watch[i];
        if (w.ffHits == s_lastFf[i] && w.peHits == s_lastPe[i]) continue;
        pos += snprintf(line + pos, sizeof(line) - pos, " %s ff+%u pe+%u", w.name,
                        w.ffHits - s_lastFf[i], w.peHits - s_lastPe[i]);
        s_lastFf[i] = w.ffHits;
        s_lastPe[i] = w.peHits;
        if (pos >= static_cast<int>(sizeof(line)) - 48) break;
    }
    if (pos > 0)
        BVR_LOG("[b2r] aim probe:%s (pe/s=%u census=%u)", line, g_peSeen - s_lastPeSeen,
                g_censusUsed);
    s_lastPeSeen = g_peSeen;
}

bool handle_command(const char* args) {
    const char* rest = nullptr;
    if (*args == '\0' || token(args, "status")) {
        log_status();
        return true;
    }
    if (token(args, "probe", &rest)) {
        if (token(rest, "on")) {
            census_clear();
            g_probeArmed.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim probe on (fire-watch + census armed)");
        } else if (token(rest, "off")) {
            g_probeArmed.store(false, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim probe off");
        } else if (token(rest, "clear")) {
            census_clear();
            for (int i = 0; i < g_watchCount; ++i) {
                g_watch[i].ffHits = 0;
                g_watch[i].peHits = 0;
            }
            BVR_LOG("[b2r] command: vraim probe clear");
        } else if (token(rest, "dump")) {
            log_status();
            census_dump();
        } else {
            BVR_LOG("[b2r] vraim probe on|off|clear|dump");
        }
        return true;
    }
    if (token(args, "handray", &rest)) {
        g_handRayOn.store(token(rest, "on"), std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim handray %s (XR hand poses %s the fire seam)",
                g_handRayOn.load(std::memory_order_relaxed) ? "on" : "off",
                g_handRayOn.load(std::memory_order_relaxed) ? "drive" : "released from");
        return true;
    }
    // laser on|off (master) | laser l|r on|off (per hand - BS2 shows both).
    if (token(args, "laser", &rest)) {
        char side[4] = {};
        char state[8] = {};
        if (sscanf_s(rest, "%3s %7s", side, static_cast<unsigned>(sizeof side), state,
                     static_cast<unsigned>(sizeof state)) == 2 &&
            (side[0] == 'l' || side[0] == 'r') && side[1] == '\0') {
            int h = (side[0] == 'l') ? 0 : 1;
            g_laserHandOn[h].store(strncmp(state, "off", 3) != 0, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim laser %c %s", side[0],
                    g_laserHandOn[h].load(std::memory_order_relaxed) ? "on" : "off");
        } else {
            g_laserOn.store(token(rest, "on"), std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim laser %s (L %s, R %s)",
                    g_laserOn.load(std::memory_order_relaxed) ? "on" : "off",
                    g_laserHandOn[0].load(std::memory_order_relaxed) ? "on" : "off",
                    g_laserHandOn[1].load(std::memory_order_relaxed) ? "on" : "off");
        }
        return true;
    }
    if (token(args, "dot", &rest)) {
        float d = 0.0f;
        char side[4] = {};
        char state[8] = {};
        if (sscanf_s(rest, "%3s %7s", side, static_cast<unsigned>(sizeof side), state,
                     static_cast<unsigned>(sizeof state)) == 2 &&
            (side[0] == 'l' || side[0] == 'r') && side[1] == '\0') {
            int h = (side[0] == 'l') ? 0 : 1;
            g_dotHandOn[h].store(strncmp(state, "off", 3) != 0, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim dot %c %s", side[0],
                    g_dotHandOn[h].load(std::memory_order_relaxed) ? "on" : "off");
            return true;
        }
        if (token(rest, "on")) {
            g_dotOn.store(true, std::memory_order_relaxed);
        } else if (token(rest, "off")) {
            g_dotOn.store(false, std::memory_order_relaxed);
        } else if (sscanf_s(rest, "%f", &d) == 1 && d > 0.0f) {
            g_dotDistM.store(d, std::memory_order_relaxed);
            g_dotOn.store(true, std::memory_order_relaxed);
        }
        BVR_LOG("[b2r] command: vraim dot %s (dist %.1f m)",
                g_dotOn.load(std::memory_order_relaxed) ? "on" : "off",
                g_dotDistM.load(std::memory_order_relaxed));
        return true;
    }
    if (token(args, "trim", &rest)) {
        char hand[4] = {};
        float p = 0.0f, y = 0.0f;
        if (sscanf_s(rest, "%3s %f %f", hand, static_cast<unsigned>(sizeof hand), &p,
                     &y) == 3) {
            int h = (hand[0] == 'l') ? 0 : 1;
            g_trimPitch[h].store(p, std::memory_order_relaxed);
            g_trimYaw[h].store(y, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim trim %c pitch %.1f yaw %.1f", hand[0], p, y);
        } else {
            BVR_LOG("[b2r] vraim trim l|r <pitchDeg> <yawDeg>");
        }
        return true;
    }
    // `pos` = the ray-ORIGIN OFFSET in cm (BS1 calls it `pos` too). Session 40
    // renamed it off `origin`, which now means BS1's fire-origin SUBSTITUTION
    // toggle - two different things wearing one name was a live foot-gun.
    if (token(args, "pos", &rest)) {
        char hand[4] = {};
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(rest, "%3s %f %f %f", hand, static_cast<unsigned>(sizeof hand), &f,
                     &r, &u) == 4) {
            int h = (hand[0] == 'l') ? 0 : 1;
            g_posFwdCm[h].store(f, std::memory_order_relaxed);
            g_posRightCm[h].store(r, std::memory_order_relaxed);
            g_posUpCm[h].store(u, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim pos %c fwd %.1f right %.1f up %.1f cm", hand[0],
                    f, r, u);
        } else {
            BVR_LOG("[b2r] vraim pos l|r <fwdCm> <rightCm> <upCm>");
        }
        return true;
    }
    // Bullet-origin substitution (BS1 parity): shots leave the HAND, not the
    // head. `origin max <uu>` tunes the displacement refusal.
    if (token(args, "origin", &rest)) {
        float m = 0.0f;
        if (sscanf_s(rest, "max %f", &m) == 1 && m > 0.0f) {
            g_originMaxUu.store(m, std::memory_order_relaxed);
        } else {
            g_handOrigin.store(!token(rest, "off"), std::memory_order_relaxed);
        }
        BVR_LOG("[b2r] command: vraim origin %s (max displacement %.0f UU, %u refusals) - "
                "shots leave the %s",
                g_handOrigin.load(std::memory_order_relaxed) ? "on" : "off",
                g_originMaxUu.load(std::memory_order_relaxed),
                g_originRefusals.load(std::memory_order_relaxed),
                g_handOrigin.load(std::memory_order_relaxed) ? "HAND" : "engine's own point");
        return true;
    }
    if (token(args, "pose", &rest)) {
        g_useAimPose.store(!token(rest, "grip"), std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim pose %s",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip");
        return true;
    }
    if (token(args, "sublog", &rest)) {
        unsigned n = 0;
        g_subLogsLeft.store(sscanf_s(rest, "%u", &n) == 1 ? n : 6,
                            std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim sublog %u (next N substitutions log before/after)",
                g_subLogsLeft.load(std::memory_order_relaxed));
        return true;
    }
    if (token(args, "on")) {
        g_aimEnabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim on (substitution armed; needs a live ray source)");
        return true;
    }
    if (token(args, "off")) {
        g_aimEnabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim off");
        return true;
    }
    if (token(args, "seam", &rest)) {
        const char* rest2 = nullptr;
        std::atomic<bool>* gate = nullptr;
        const char* which = nullptr;
        if (token(rest, "weapon", &rest2)) {
            gate = &g_seamWeapon;
            which = "weapon";
        } else if (token(rest, "ability", &rest2)) {
            gate = &g_seamAbility;
            which = "ability";
        }
        if (gate) {
            gate->store(token(rest2, "on"), std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim seam %s %s", which,
                    gate->load(std::memory_order_relaxed) ? "on" : "OFF");
        } else {
            BVR_LOG("[b2r] vraim seam weapon|ability on|off");
        }
        return true;
    }
    if (token(args, "test", &rest)) {
        if (token(rest, "off")) {
            g_testDeadlineMs.store(0, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim test off");
            return true;
        }
        // `vraim test r <yawDeg> <pitchDeg> [holdMs]` - the hand letter is
        // accepted for BS1 grammar parity; the test ray feeds both seams.
        char hand[4] = {};
        float yaw = 0.0f, pitch = 0.0f;
        unsigned hold = 0;
        int got = sscanf_s(rest, "%3s %f %f %u", hand,
                           static_cast<unsigned>(sizeof hand), &yaw, &pitch, &hold);
        if (got >= 3) {
            g_testYawDeg.store(yaw, std::memory_order_relaxed);
            g_testPitchDeg.store(pitch, std::memory_order_relaxed);
            uint64_t holdMs = hold ? hold : 15000;
            g_testDeadlineMs.store(GetTickCount64() + holdMs, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim test %s yaw %.1f pitch %.1f for %llu ms "
                    "(offset from the live view rot)",
                    hand, yaw, pitch, static_cast<unsigned long long>(holdMs));
        } else {
            BVR_LOG("[b2r] vraim test r <yawDeg> <pitchDeg> [holdMs] | off");
        }
        return true;
    }
    // Per-weapon profiles (session 41).
    if (token(args, "weapon", &rest)) {
        char ui[48];
        weapon_key_ui(ui, sizeof ui);
        void* hold = nullptr;
        bool rigKnown = bones::current_holdable(&hold);
        char cls[48] = {};
        bool clsOk = hold && patterns::object_class_name(hold, cls, sizeof cls);
        BVR_LOG("[b2r] vraim weapon: key '%s'%s, holdable %p (%s), %zu profile(s), "
                "%u swap(s), baseline %s",
                ui, g_weaponKeySim ? " (SIM)" : "", hold,
                !rigKnown ? "rig unknown" : (clsOk ? cls : "no class"),
                g_weaponProfiles.size(), g_weaponSwaps,
                g_presetBaselineValid ? "captured" : "NOT captured (resolver idle)");
        BVR_LOG("[b2r]   live R: aim trim %.1f/%.1f pos %.1f/%.1f/%.1f, model trim "
                "%.1f/%.1f/%.1f off %.1f/%.1f/%.1f scale %.2f, wscale %.2f",
                g_trimPitch[1].load(std::memory_order_relaxed),
                g_trimYaw[1].load(std::memory_order_relaxed),
                g_posFwdCm[1].load(std::memory_order_relaxed),
                g_posRightCm[1].load(std::memory_order_relaxed),
                g_posUpCm[1].load(std::memory_order_relaxed), hands::trim_pitch(1),
                hands::trim_yaw(1), hands::trim_roll(1), hands::off_fwd_cm(1),
                hands::off_right_cm(1), hands::off_up_cm(1), bones::scale_of(1),
                bones::weapon_scale());
        return true;
    }
    if (token(args, "wsave", &rest)) {
        save_weapon_profiles();
        return true;
    }
    if (token(args, "wkey", &rest)) {
        char name[48] = {};
        if (token(rest, "real")) {
            g_weaponKeySim = false;
            g_weaponKeyActor = nullptr; // force a fresh resolve
            BVR_LOG("[b2r] command: vraim wkey real (resolver re-armed)");
        } else if (sscanf_s(rest, "sim %47s", name, (unsigned)sizeof name) == 1) {
            // Flat lane: pin a key without switching weapons (BS1's wkey sim).
            g_weaponKeySim = true;
            char key[48];
            narrow_key(name, key, sizeof key);
            apply_weapon_key(std::string(key), "wkey sim");
            BVR_LOG("[b2r] command: vraim wkey sim %s", key);
        } else {
            BVR_LOG("[b2r] vraim wkey real | sim <ClassName>");
        }
        return true;
    }
    // Session 41 derivation probe: `vraim oclass hands|weapon|<hexptr>`.
    if (token(args, "oclass", &rest)) {
        void* obj = nullptr;
        const char* label = "ptr";
        if (token(rest, "hands")) {
            obj = bones::hands_actor();
            label = "hands";
            if (!obj)
                BVR_LOG("[b2r] vraim oclass: AHands not resolved (enter gameplay, "
                        "vrhands drives it)");
        } else if (token(rest, "weapon")) {
            obj = g_lastWeaponThis.load(std::memory_order_relaxed);
            label = "weapon";
            if (!obj)
                BVR_LOG("[b2r] vraim oclass: no weapon seen at the seam yet - fire a "
                        "gun (vrinput test trig r 255 300)");
        } else {
            unsigned v = 0;
            if (sscanf_s(rest, "%x", &v) == 1) obj = reinterpret_cast<void*>(
                static_cast<uintptr_t>(v));
        }
        if (obj) patterns::probe_object_identity(obj, label);
        return true;
    }
    BVR_LOG("[b2r] vraim: status | on|off | handray on|off | laser [l|r] on|off | "
            "dot [l|r] on|off|<distM> | trim l|r <p> <y> | pos l|r <f> <r> <u> | "
            "origin on|off|max <uu> | pose aim|grip | seam weapon|ability on|off | "
            "test r <yaw> <pitch> [ms]|off | sublog [n] | probe on|off|clear|dump | "
            "weapon | wsave | wkey real|sim <Class> | oclass hands|weapon|<hex>");
    return true;
}

void* last_weapon_this() {
    return g_lastWeaponThis.load(std::memory_order_relaxed);
}

bool last_ray(int hand, FVector* origin, FRotator* rot) {
    return ray_for(hand, origin, rot);
}

float trim_pitch(int h) { return g_trimPitch[h & 1].load(std::memory_order_relaxed); }
float trim_yaw(int h) { return g_trimYaw[h & 1].load(std::memory_order_relaxed); }
void set_trim(int h, float pitchDeg, float yawDeg) {
    h &= 1;
    g_trimPitch[h].store(pitchDeg, std::memory_order_relaxed);
    g_trimYaw[h].store(yawDeg, std::memory_order_relaxed);
}

float pos_fwd_cm(int h) { return g_posFwdCm[h & 1].load(std::memory_order_relaxed); }
float pos_right_cm(int h) { return g_posRightCm[h & 1].load(std::memory_order_relaxed); }
float pos_up_cm(int h) { return g_posUpCm[h & 1].load(std::memory_order_relaxed); }
void set_pos(int h, float fwdCm, float rightCm, float upCm) {
    h &= 1;
    g_posFwdCm[h].store(fwdCm, std::memory_order_relaxed);
    g_posRightCm[h].store(rightCm, std::memory_order_relaxed);
    g_posUpCm[h].store(upCm, std::memory_order_relaxed);
}

bool origin_on() { return g_handOrigin.load(std::memory_order_relaxed); }
void set_origin(bool on) { g_handOrigin.store(on, std::memory_order_relaxed); }

float dot_dist_m() { return g_dotDistM.load(std::memory_order_relaxed); }
void set_dot_dist_m(float m) {
    if (m > 0.2f && m < 20.0f) g_dotDistM.store(m, std::memory_order_relaxed);
}

// Per-hand laser/dot toggles (session 41 round 3, user ask: the commands
// existed but nothing in F10 could switch the beam or the dot). The master
// g_laserOn/g_dotOn stay implied-ON; these are the user-facing per-hand
// enables, preset-persisted.
bool laser_hand(int h) { return g_laserHandOn[h & 1].load(std::memory_order_relaxed); }
void set_laser_hand(int h, bool on) {
    g_laserHandOn[h & 1].store(on, std::memory_order_relaxed);
}
bool dot_hand(int h) { return g_dotHandOn[h & 1].load(std::memory_order_relaxed); }
void set_dot_hand(int h, bool on) {
    g_dotHandOn[h & 1].store(on, std::memory_order_relaxed);
}

} // namespace bvr::b2r::aim
