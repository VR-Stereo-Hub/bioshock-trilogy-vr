#include "game/bioshock2r/aim.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <MinHook.h>

#include <cmath>
#include <cstdio>
#include <cstring>

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
std::atomic<int> g_laserHand{1};
// Per-hand trims (degrees) and ray-origin offsets (cm), BS1's tuning surface.
std::atomic<float> g_trimPitch[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_trimYaw[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_posFwdCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_posRightCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_posUpCm[2] = {{0.0f}, {0.0f}};
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

int __fastcall WeaponGpfsDetour(void* self, void* edx, FVector* outLoc, FRotator* outRot,
                                FVector* outEffect) {
    int r = g_origWeaponGpfs(self, edx, outLoc, outRot, outEffect);
    g_wepCalls.fetch_add(1, std::memory_order_relaxed);
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
        substituted_rot(outRot, 1))
        g_subs.fetch_add(1, std::memory_order_relaxed);
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
        substituted_rot(outRot, 0))
        g_subs.fetch_add(1, std::memory_order_relaxed);
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
            "pitch %.1f; laser %s hand %d, dot %s %.1f m; trim R %.1f/%.1f origin R "
            "%.1f/%.1f/%.1f",
            g_handRayOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
            g_rayValid[0].load(std::memory_order_relaxed) ? "valid" : "-",
            g_lastRayYawDeg[0].load(std::memory_order_relaxed),
            g_lastRayPitchDeg[0].load(std::memory_order_relaxed),
            g_rayValid[1].load(std::memory_order_relaxed) ? "valid" : "-",
            g_lastRayYawDeg[1].load(std::memory_order_relaxed),
            g_lastRayPitchDeg[1].load(std::memory_order_relaxed),
            g_laserOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_laserHand.load(std::memory_order_relaxed),
            g_dotOn.load(std::memory_order_relaxed) ? "ON" : "off",
            g_dotDistM.load(std::memory_order_relaxed),
            g_trimPitch[1].load(std::memory_order_relaxed),
            g_trimYaw[1].load(std::memory_order_relaxed),
            g_posFwdCm[1].load(std::memory_order_relaxed),
            g_posRightCm[1].load(std::memory_order_relaxed),
            g_posUpCm[1].load(std::memory_order_relaxed));
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

    // Publish the laser: core re-derives the ray render-side from the same
    // hand pose and the same trims, so these fields MUST mirror the ray's.
    int lh = g_laserHand.load(std::memory_order_relaxed);
    bvr::vr::LaserConfig lc{};
    lc.enabled = g_laserOn.load(std::memory_order_relaxed) && gate;
    lc.hand = lh;
    lc.pitchTrimDeg = g_trimPitch[lh].load(std::memory_order_relaxed);
    lc.yawTrimDeg = g_trimYaw[lh].load(std::memory_order_relaxed);
    lc.posFwdCm = g_posFwdCm[lh].load(std::memory_order_relaxed);
    lc.posRightCm = g_posRightCm[lh].load(std::memory_order_relaxed);
    lc.posUpCm = g_posUpCm[lh].load(std::memory_order_relaxed);
    bvr::vr::set_laser(lc);

    // Publish the aim dot FROM the final game-space ray (core's design note:
    // the dot is the point the shot starts from, mapped back into XR, so
    // "dot == shot" is exact rather than congruent).
    bvr::vr::AimDotConfig dc{};
    dc.enabled = g_dotOn.load(std::memory_order_relaxed);
    FVector origin{};
    FRotator rrot{};
    if (dc.enabled && ray_for(lh, &origin, &rrot)) {
        float dir[3];
        ue_rot_to_dir(rrot, dir);
        float distUu = g_dotDistM.load(std::memory_order_relaxed) * ctx.worldScale;
        FVector pt{origin.x + dir[0] * distUu, origin.y + dir[1] * distUu,
                   origin.z + dir[2] * distUu};
        game_point_to_xr(ctx, pt, dc.posXr);
        dc.valid = true;
        // One-shot round-trip self-check: map the point back out and compare.
        // A mismatch means the forward/inverse pair disagree - the one bug
        // class that would make the dot lie about where the shot goes.
        if (!g_loggedDotRoundTrip.exchange(true, std::memory_order_relaxed)) {
            const float identQ[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            GamePose back = xr_pose_to_game(ctx, dc.posXr, identQ);
            float dx = back.loc.x - pt.x, dy = back.loc.y - pt.y, dz = back.loc.z - pt.z;
            BVR_LOG("[b2r] aim dot round-trip: game (%.1f %.1f %.1f) -> xr (%.3f %.3f "
                    "%.3f) -> game (%.1f %.1f %.1f), error %.4f UU",
                    pt.x, pt.y, pt.z, dc.posXr[0], dc.posXr[1], dc.posXr[2], back.loc.x,
                    back.loc.y, back.loc.z, sqrtf(dx * dx + dy * dy + dz * dz));
        }
    }
    bvr::vr::set_aim_dot(dc);
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
    if (token(args, "laser", &rest)) {
        g_laserOn.store(token(rest, "on"), std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vraim laser %s",
                g_laserOn.load(std::memory_order_relaxed) ? "on" : "off");
        return true;
    }
    if (token(args, "dot", &rest)) {
        float d = 0.0f;
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
    if (token(args, "origin", &rest)) {
        char hand[4] = {};
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(rest, "%3s %f %f %f", hand, static_cast<unsigned>(sizeof hand), &f,
                     &r, &u) == 4) {
            int h = (hand[0] == 'l') ? 0 : 1;
            g_posFwdCm[h].store(f, std::memory_order_relaxed);
            g_posRightCm[h].store(r, std::memory_order_relaxed);
            g_posUpCm[h].store(u, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim origin %c fwd %.1f right %.1f up %.1f cm",
                    hand[0], f, r, u);
        } else {
            BVR_LOG("[b2r] vraim origin l|r <fwdCm> <rightCm> <upCm>");
        }
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
    BVR_LOG("[b2r] vraim: status | on|off | handray on|off | laser on|off | "
            "dot on|off|<distM> | trim l|r <p> <y> | origin l|r <f> <r> <u> | "
            "pose aim|grip | seam weapon|ability on|off | test r <yaw> <pitch> "
            "[ms]|off | sublog [n] | probe on|off|clear|dump");
    return true;
}

} // namespace bvr::b2r::aim
