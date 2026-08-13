#include "game/bioshockinf/hide.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/cine.h"
#include "game/bioshockinf/melee.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/profiles.h"
#include "game/bioshockinf/reflect.h"

#include <imgui.h>

namespace bvr::bsi::hide {
namespace {

using bvr::pattern_scan::is_memory_valid;

// ---- levers -----------------------------------------------------------------
enum Lever : int {
    kLeverOwner = 0, // SetOwnerNoSee(UBOOL) on the component
    kLeverComp = 1,  // SetHidden(UBOOL) on the component
    kLeverActor = 2, // bHidden bit write on the attachment instance
    kLeverBone = 3,  // HideBoneByName per side (the only per-hand lever)
};
const char* lever_name(int l) {
    switch (l) {
        case kLeverOwner: return "owner (SetOwnerNoSee)";
        case kLeverComp: return "comp (SetHidden)";
        case kLeverActor: return "actor (bHidden bit)";
        case kLeverBone: return "bone (HideBoneByName)";
    }
    return "?";
}

// ---- derived levers (one-shot per boot, refused-latch: the fidget pattern) --
bool g_derived = false;
bool g_deriveRefused = false;
uint32_t g_actorHiddenOff = 0, g_actorHiddenMask = 0;   // attachment bHidden
uint32_t g_hiddenGameOff = 0, g_hiddenGameMask = 0;     // component HiddenGame
uint32_t g_ownerNoSeeOff = 0, g_ownerNoSeeMask = 0;     // component bOwnerNoSee
uint32_t g_onlyOwnerOff = 0, g_onlyOwnerMask = 0;       // component bOnlyOwnerSee
int32_t g_idxSetHidden = -1, g_idxSetHiddenGame = -1, g_idxSetOwnerNoSee = -1;
int32_t g_idxHideBone = -1, g_idxUnHideBone = -1;
int32_t g_idxIsBoneHidden = -1, g_idxMatchRefBone = -1;

// ---- cine policy ------------------------------------------------------------
// s53 sim measurement (rowboat save): the game MANAGES the attachment's
// bHidden itself - 1 through the no-hands phases of a scripted scene, 0 for
// its authored hand moments and gameplay. So the cine scope is a POLICY
// question the headset must judge:
//  - game:  trust the game's own management, we touch nothing during holds
//    (risk: the round-4 doubles were our STALE driven bones becoming visible
//    when the game unhides for an authored moment);
//  - force: assert the hide through the whole hold, watchdog re-asserting
//    against the game's unhide (risk: authored hand moments - the box
//    handoff - lose their hands too);
//  - off:   cine scope disabled entirely (bisect lever).
// s59 added kCineAuto (APPEND ONLY - the values persist in vr.ini): per-hold
// own-rig discrimination. The s59 paired trace (door vs vigor drink, both
// flat-measured with the hold probe): every cheap binary signal reads
// IDENTICAL across the two classes (bHidden=0, who=SAME on both - the
// spawned door rig is never reachable through GetFirstPersonAttachment), but
// the ROTATION channel separates them: the door only plays ForceUnequip
// through our rig (51-67 deg peak), the drink vignette articulates it ~180
// deg both hands. Rotation is also immune to our own hide's grip-scale
// artifact (~99 UU translation, ~6 deg rotation). So auto = hide-first, then
// SHOW for the rest of the hold once either grip rotates past the threshold
// from its hold-open pose while bHidden stays 0; bHidden=1 (the game parking
// the rig - the death/respawn class) hides immediately and clears the latch.
// Signal failure degrades to force behavior - auto can regress into "always
// hide", never into doubles.
enum CineMode : int { kCineGame = 0, kCineForce = 1, kCineOff = 2, kCineAuto = 3 };
const char* cine_mode_name(int m) {
    switch (m) {
        case kCineGame: return "game";
        case kCineForce: return "force";
        case kCineOff: return "off";
        case kCineAuto: return "auto";
    }
    return "?";
}

// ---- the auto gate ----------------------------------------------------------
// Latches are plain bools: every reader/writer is the game thread (tick and
// the command pump share it). Only the F10 checkbox/radio cross threads, and
// those are the atomics.
std::atomic<bool> g_auto{true}; // the s53 feature ships armed; F10 + bsihide auto off
// s54b tried game-managed as the default (the raffle stalled inside a
// force-hidden hold, and s53 measured 15 reasserts there) - FALSIFIED the
// same day: the raffle stalls identically with game-managed, so the hide
// gate is EXONERATED for the scene wedge and the default returns to the
// user-directed force. The stall predates the hide gate entirely (user
// report: camera + stereo era) - the open investigation lives in STATUS.
std::atomic<int> g_cineMode{kCineAuto}; // s59 default: per-hold discrimination
// s53 lever verdicts, sim + HEADSET: actor bHidden is INEFFECTIVE (bit set,
// hands+pistol keep rendering; headset-confirmed "the old behavior"); bone
// grip+arm hides left the bare HANDS floating in the headset (the rig is
// parent-flat - no cascade; the composite now includes the cluster); comp
// SetHidden and owner SetOwnerNoSee both fully hide the mesh (headset-
// confirmed) but leave an attached WEAPON floating (sim) - so the OWNER
// rig-wide path also bone-hides the two grips to take the holdable down
// with it. Owner is the production default.
std::atomic<int> g_lever{kLeverOwner};
int g_activeLever = kLeverOwner;       // the lever the current latches used
bool g_rigHidden = false;
bool g_handHidden[2] = {};
void* g_appliedAttach = nullptr;
bool g_faultLatched = false;
uint32_t g_applies = 0, g_reasserts = 0, g_failStreak = 0;
uint64_t g_lastWatchMs = 0;

// ---- primitives -------------------------------------------------------------

// -1 unknown, else the bit's current value. Direct gated read - cadence-safe.
int read_bit(const void* obj, uint32_t off, uint32_t mask) {
    if (!obj || !mask) return -1;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    if (!is_memory_valid(p, 4)) return -1;
    return (*reinterpret_cast<const uint32_t*>(p) & mask) ? 1 : 0;
}

bool write_bit(void* obj, uint32_t off, uint32_t mask, bool set) {
    if (!obj || !mask) return false;
    uint8_t* p = static_cast<uint8_t*>(obj) + off;
    if (!is_memory_valid(p, 4)) return false;
    if (set)
        *reinterpret_cast<uint32_t*>(p) |= mask;
    else
        *reinterpret_cast<uint32_t*>(p) &= ~mask;
    return true;
}

// One UBOOL argument at parms+0. call_on_object_by_index carries the whole
// gate stack (game thread, vtable RVA interlock, SEH isolation).
bool dispatch_bool(void* obj, int32_t idx, bool v) {
    if (!obj || idx < 0) return false;
    alignas(16) uint8_t parms[64] = {};
    const uint32_t u = v ? 1u : 0u;
    memcpy(parms, &u, sizeof u);
    return reflect::call_on_object_by_index(obj, idx, parms);
}

// HideBoneByName(FName, PhysBodyOption) / UnHideBoneByName(FName): the FName
// {Index, Number=0} sits at parms+0; everything past it stays zero, which is
// PBO_None under either enum marshalling (the s45b bsicallat recipe).
bool dispatch_bone(void* comp, int32_t fnIdx, int32_t boneFname, bool logFail,
                   const char* boneName) {
    if (!comp || fnIdx < 0 || boneFname < 0) return false;
    alignas(16) uint8_t parms[64] = {};
    memcpy(parms, &boneFname, sizeof boneFname);
    const bool ok = reflect::call_on_object_by_index(comp, fnIdx, parms);
    if (!ok && logFail)
        BVR_LOG("[bsi] hide: bone dispatch FAILED for '%s' (fname %d)",
                boneName ? boneName : "?", boneFname);
    return ok;
}

// Whole-limb per side: grip + cluster (palm/digits) + arm chain ("whole limb
// gone" - the user's s53 call; the cluster is explicit because the flat rig
// does not cascade grip hides, headset-measured).
bool bone_side(void* comp, int hand, bool hideIt, bool verbose) {
    int32_t fn[32];
    const char* nm[32];
    const int n = bones::side_bones(hand, fn, nm, 32);
    if (n <= 0) return false;
    bool ok = true;
    for (int i = 0; i < n; ++i)
        ok &= dispatch_bone(comp, hideIt ? g_idxHideBone : g_idxUnHideBone, fn[i],
                            verbose, nm[i]);
    if (verbose)
        BVR_LOG("[bsi] hide: bone %s %c - %d bones (%s first)%s",
                hideIt ? "HIDE" : "unhide", hand ? 'R' : 'L', n, n ? nm[0] : "?",
                ok ? "" : " - SOME DISPATCHES FAILED");
    return ok;
}

// -1 unknown, 0 visible, 1 hidden - MatchRefBone then IsBoneHidden, both by
// cached index (the probe readback and the bone-lever watchdog).
int bone_hidden_state(void* comp, int32_t boneFname) {
    if (!comp || g_idxMatchRefBone < 0 || g_idxIsBoneHidden < 0 || boneFname < 0)
        return -1;
    alignas(16) uint8_t parms[64] = {};
    memcpy(parms, &boneFname, sizeof boneFname);
    if (!reflect::call_on_object_by_index(comp, g_idxMatchRefBone, parms)) return -1;
    int32_t boneIdx = -1;
    memcpy(&boneIdx, parms + 8, sizeof boneIdx); // return slot past the FName
    if (boneIdx < 0 || boneIdx >= 128) return -1;
    memset(parms, 0, sizeof parms);
    memcpy(parms, &boneIdx, sizeof boneIdx);
    if (!reflect::call_on_object_by_index(comp, g_idxIsBoneHidden, parms)) return -1;
    uint32_t r = 0;
    memcpy(&r, parms + 4, sizeof r); // UBOOL return past the int
    return r ? 1 : 0;
}

// ---- derivation -------------------------------------------------------------

bool derive(bool verbose) {
    if (g_derived) return true;
    if (g_deriveRefused) return false;
    void* attach = bones::attachment();
    void* comp = bones::component();
    if (!attach || !comp) {
        if (verbose)
            BVR_LOG("[bsi] hide: derive waiting - rig not resolved (load a save; "
                    "bones resolves at 1 Hz)");
        return false;
    }
    // Property walks are one-shot (hundreds of gated reads - never a cadence).
    const bool haveActor =
        reflect::find_bool_property_bit(attach, "bHidden", &g_actorHiddenOff,
                                        &g_actorHiddenMask);
    const bool haveHG = reflect::find_bool_property_bit(comp, "HiddenGame",
                                                        &g_hiddenGameOff,
                                                        &g_hiddenGameMask);
    const bool haveONS = reflect::find_bool_property_bit(comp, "bOwnerNoSee",
                                                         &g_ownerNoSeeOff,
                                                         &g_ownerNoSeeMask);
    const bool haveOOS = reflect::find_bool_property_bit(comp, "bOnlyOwnerSee",
                                                         &g_onlyOwnerOff,
                                                         &g_onlyOwnerMask);
    // Pool scans - one shot each, indices stable per boot. A hit only proves
    // the NAME exists; FindFunction at dispatch time decides per object.
    g_idxSetHidden = reflect::find_function_index("SetHidden");
    g_idxSetHiddenGame = reflect::find_function_index("SetHiddenGame");
    g_idxSetOwnerNoSee = reflect::find_function_index("SetOwnerNoSee");
    g_idxHideBone = reflect::find_function_index("HideBoneByName");
    g_idxUnHideBone = reflect::find_function_index("UnHideBoneByName");
    g_idxIsBoneHidden = reflect::find_function_index("IsBoneHidden");
    g_idxMatchRefBone = reflect::find_function_index("MatchRefBone");

    g_derived = haveActor || haveHG || haveONS || g_idxSetHidden >= 0 ||
                g_idxHideBone >= 0;
    if (!g_derived) {
        g_deriveRefused = true; // a 600+-field walk must not spin at a cadence
        BVR_LOG("[bsi] hide: derive REFUSED - no lever found (no bHidden/HiddenGame/"
                "bOwnerNoSee property, no SetHidden/HideBoneByName name). The lane "
                "stays off this boot.");
        return false;
    }
    BVR_LOG("[bsi] hide: derived - actor.bHidden %s(+0x%X mask 0x%X) | "
            "comp.HiddenGame %s(+0x%X mask 0x%X) | comp.bOwnerNoSee %s(+0x%X mask "
            "0x%X) | comp.bOnlyOwnerSee %s(+0x%X mask 0x%X)",
            haveActor ? "" : "MISSING ", g_actorHiddenOff, g_actorHiddenMask,
            haveHG ? "" : "MISSING ", g_hiddenGameOff, g_hiddenGameMask,
            haveONS ? "" : "MISSING ", g_ownerNoSeeOff, g_ownerNoSeeMask,
            haveOOS ? "" : "MISSING ", g_onlyOwnerOff, g_onlyOwnerMask);
    BVR_LOG("[bsi] hide: fname indices - SetHidden %d SetHiddenGame %d SetOwnerNoSee "
            "%d HideBoneByName %d UnHideBoneByName %d IsBoneHidden %d MatchRefBone %d",
            g_idxSetHidden, g_idxSetHiddenGame, g_idxSetOwnerNoSee, g_idxHideBone,
            g_idxUnHideBone, g_idxIsBoneHidden, g_idxMatchRefBone);
    return true;
}

// ---- lever application ------------------------------------------------------

// The two grips alone - the OWNER composite's weapon leg: SetOwnerNoSee hides
// the arms MESH but an attached holdable is its own component and keeps
// rendering (sim s53); zero-scaling the grips takes it down with them.
bool grips_only(void* comp, bool hideIt) {
    bool ok = true;
    for (int h = 0; h < 2; ++h) {
        int32_t fn[2];
        if (bones::side_bones(h, fn, nullptr, 2) < 1) return false;
        ok &= dispatch_bone(comp, hideIt ? g_idxHideBone : g_idxUnHideBone, fn[0],
                            false, nullptr);
    }
    return ok;
}

// The rig-wide levers. The lever is an explicit argument so restores always
// run through the lever that APPLIED the state, even after the selector moved
// on. The production default (owner) is a COMPOSITE: SetOwnerNoSee for the
// mesh + both grips bone-hidden for any attached holdable.
bool apply_rig(bool hideIt, int lever, bool verbose) {
    void* attach = bones::attachment();
    void* comp = bones::component();
    if (!attach || !comp) return false;
    bool ok = false;
    switch (lever) {
        case kLeverOwner:
            ok = dispatch_bool(comp, g_idxSetOwnerNoSee, hideIt);
            ok = grips_only(comp, hideIt) && ok;
            break;
        case kLeverComp: ok = dispatch_bool(comp, g_idxSetHidden, hideIt); break;
        case kLeverActor: ok = write_bit(attach, g_actorHiddenOff, g_actorHiddenMask,
                                         hideIt); break;
        case kLeverBone:
            ok = bone_side(comp, 0, hideIt, verbose);
            ok = bone_side(comp, 1, hideIt, verbose) && ok;
            break;
    }
    if (verbose)
        BVR_LOG("[bsi] hide: rig %s via %s -> %s", hideIt ? "HIDE" : "unhide",
                lever_name(lever), ok ? "dispatched" : "FAILED");
    return ok;
}

// The watchdog's cheap read of the rig-wide lever's current state.
int rig_state(int lever) {
    void* attach = bones::attachment();
    void* comp = bones::component();
    switch (lever) {
        case kLeverOwner: return read_bit(comp, g_ownerNoSeeOff, g_ownerNoSeeMask);
        case kLeverComp: return read_bit(comp, g_hiddenGameOff, g_hiddenGameMask);
        case kLeverActor: return read_bit(attach, g_actorHiddenOff, g_actorHiddenMask);
        case kLeverBone: {
            int32_t fn[2];
            if (bones::side_bones(1, fn, nullptr, 2) < 1) return -1;
            return bone_hidden_state(comp, fn[0]); // R grip stands in for the rig
        }
    }
    return -1;
}

void clear_latches() {
    g_rigHidden = false;
    g_handHidden[0] = g_handHidden[1] = false;
}

// Best-effort unhide of everything we latched, while pointers are live -
// always through the lever that applied it.
void unhide_all(const char* why) {
    if (!g_rigHidden && !g_handHidden[0] && !g_handHidden[1]) return;
    void* comp = bones::component();
    if (comp) {
        if (g_rigHidden) apply_rig(false, g_activeLever, false);
        for (int h = 0; h < 2; ++h)
            if (g_handHidden[h]) bone_side(comp, h, false, false);
        BVR_LOG("[bsi] hide: unhide-all (%s)", why);
    }
    clear_latches();
}

// ---- probes -----------------------------------------------------------------

void* live_pawn() {
    void* pc = camera::last_player_controller();
    if (!pc || !is_memory_valid(pc, patterns::kPcPawnOffset + 4)) return nullptr;
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn || !is_memory_valid(pawn, 0x40)) return nullptr;
    return pawn;
}

// Silent core of the who probe: the game's own GetFirstPersonAttachment and
// its mesh comp, by cached index (the s59 probe samples this on a cadence).
// Returns false when the pawn or the dispatch is unavailable.
bool who_probe(void** outFpa, void** outFcomp) {
    *outFpa = nullptr;
    *outFcomp = nullptr;
    void* pawn = live_pawn();
    if (!pawn) return false;
    static int32_t s_idxGetFpa = -2; // -2 unresolved, -1 refused (pool miss)
    if (s_idxGetFpa == -2)
        s_idxGetFpa = reflect::find_function_index("GetFirstPersonAttachment");
    if (s_idxGetFpa < 0) return false;
    alignas(16) uint8_t parms[64] = {};
    if (!reflect::call_on_object_by_index(pawn, s_idxGetFpa, parms)) return false;
    void* fpa = nullptr;
    memcpy(&fpa, parms, sizeof fpa);
    if (fpa && is_memory_valid(static_cast<uint8_t*>(fpa) +
                                   patterns::kFpAttachMeshCompOffset,
                               4))
        *outFcomp = *reinterpret_cast<void**>(static_cast<uint8_t*>(fpa) +
                                              patterns::kFpAttachMeshCompOffset);
    *outFpa = fpa;
    return true;
}

// The doubles-identity probe: does the game's OWN GetFirstPersonAttachment
// still return the rig we drive, or did a scripted scene swap it? A swap
// means the visible doubles are our STALE rig and the fix pivots (STATUS s52
// round 4 risk list).
void cmd_who() {
    void* fpa = nullptr;
    void* fcomp = nullptr;
    if (!who_probe(&fpa, &fcomp)) {
        BVR_LOG("[bsi] hide: who REFUSED - no pawn or GetFirstPersonAttachment "
                "dispatch failed (load a save first)");
        return;
    }
    char cls[64] = {};
    reflect::class_name_of(fpa, cls, sizeof cls);
    void* ourA = bones::attachment();
    void* ourC = bones::component();
    BVR_LOG("[bsi] hide: who - game says attachment %p (%s) comp %p | ours %p / %p "
            "-> %s",
            fpa, cls[0] ? cls : "?", fcomp, ourA, ourC,
            (fpa == ourA && fcomp == ourC) ? "SAME (the doubles are NOT a stale rig)"
                                           : "DIFFERENT - the doubles ARE our stale "
                                             "rig; the fix is hide-stale + re-resolve");
    if (g_derived) {
        BVR_LOG("[bsi] hide: who - bits: game rig bHidden=%d HiddenGame=%d "
                "bOwnerNoSee=%d | our rig bHidden=%d HiddenGame=%d bOwnerNoSee=%d",
                read_bit(fpa, g_actorHiddenOff, g_actorHiddenMask),
                read_bit(fcomp, g_hiddenGameOff, g_hiddenGameMask),
                read_bit(fcomp, g_ownerNoSeeOff, g_ownerNoSeeMask),
                read_bit(ourA, g_actorHiddenOff, g_actorHiddenMask),
                read_bit(ourC, g_hiddenGameOff, g_hiddenGameMask),
                read_bit(ourC, g_ownerNoSeeOff, g_ownerNoSeeMask));
    }
}

// The pawn-body probe (s53): if the headset doubles survive an FP-rig hide,
// they are the PAWN's own third-person body - visible only because the VR
// camera sits off the authored head point. A/B lever: SetOwnerNoSee on the
// pawn's Mesh component. Probe-only until a headset verdict makes it policy.
void cmd_pawn(bool hideIt) {
    void* pawn = live_pawn();
    if (!pawn) {
        BVR_LOG("[bsi] hide: pawn REFUSED - no pawn");
        return;
    }
    static uint32_t s_meshOff = 0;
    static bool s_refused = false;
    if (!s_meshOff && !s_refused) {
        if (!reflect::find_property_offset(pawn, "Mesh", "ObjectProperty", &s_meshOff)) {
            s_refused = true;
            BVR_LOG("[bsi] hide: pawn - 'Mesh' did not derive on the pawn's chain");
            return;
        }
        BVR_LOG("[bsi] hide: pawn - derived Mesh at pawn+0x%X", s_meshOff);
    }
    if (!s_meshOff || !is_memory_valid(static_cast<uint8_t*>(pawn) + s_meshOff, 4))
        return;
    void* mesh = *reinterpret_cast<void**>(static_cast<uint8_t*>(pawn) + s_meshOff);
    char cls[64] = {};
    if (mesh) reflect::class_name_of(mesh, cls, sizeof cls);
    if (!mesh) {
        BVR_LOG("[bsi] hide: pawn - Mesh slot is null");
        return;
    }
    const bool ok = dispatch_bool(mesh, g_idxSetOwnerNoSee, hideIt);
    BVR_LOG("[bsi] hide: pawn mesh %p (%s) SetOwnerNoSee(%d) -> %s | bOwnerNoSee now %d",
            mesh, cls[0] ? cls : "?", hideIt ? 1 : 0, ok ? "dispatched" : "FAILED",
            read_bit(mesh, g_ownerNoSeeOff, g_ownerNoSeeMask));
}

// Arm reflect's bsidiff over the flag regions - run once to snapshot, cross
// the scene edge, run the SAME subcommand again to name the writer.
void cmd_diff(const char* which) {
    if (!derive(true)) return;
    void* base = nullptr;
    uint32_t off = 0;
    if (strcmp(which, "actor") == 0) {
        base = bones::attachment();
        off = g_actorHiddenOff;
    } else if (strcmp(which, "comp") == 0) {
        base = bones::component();
        off = g_hiddenGameOff; // bOwnerNoSee lives within the same 8-dword window
    } else {
        BVR_LOG("[bsi] hide: diff - usage: bsihide diff actor|comp");
        return;
    }
    if (!base || !off) {
        BVR_LOG("[bsi] hide: diff REFUSED - %s flag not derived or rig not resolved",
                which);
        return;
    }
    char args[32];
    const uint32_t start = (off & ~3u) >= 4 ? (off & ~3u) - 4 : 0;
    const uint32_t addr = reinterpret_cast<uint32_t>(base) + start;
    snprintf(args, sizeof args, "0x%X 8", addr);
    reflect::handle_command("bsidiff", args);
    BVR_LOG("[bsi] hide: diff armed over %s+0x%X (8 dwords). Cross the scene edge, "
            "then run `bsihide diff %s` again - changed dwords name the writer.",
            which, off & ~3u, which);
}

// ---- the s59 hold probe -----------------------------------------------------
// Per-hold discriminator instrumentation (spawned-rig vs own-rig scripted
// beats, ENGINE_NOTES s58b): during a cine hold, is the game parking OUR rig
// (spawned cutscene hands - the hide is correct) or animating it (own-rig
// beat - the hide eats the authored hands)? Candidate signals sampled here:
//  1. our attachment's stock bHidden bit - the game's own scene-state
//     correlate (s53: 1 through no-hands phases, 1->0 at the authored hand
//     moment, 0 in gameplay). Free per-tick read; edge-logged.
//  2. who-identity by cached index every 750 ms - DIFFERENT means the game
//     repointed GetFirstPersonAttachment at a distinct cutscene rig.
//  3. hold class (camera vs scripted) + melee release state, for the per-beat
//     table. Read-only; never gates anything. Armed by `bsihide probe on`.
bool g_probe = false;
bool g_probeTravel = false; // auto-arm `bsibones travel all` at hold-open
bool s_probePrevHold = false;
uint64_t s_probeOpenMs = 0;
uint64_t s_probeLastWhoMs = 0;
int s_probePrevBits[4] = {-2, -2, -2, -2}; // bHidden HiddenGame bOwnerNoSee bOnlyOwnerSee
int s_probePrevWhoSame = -2;               // -2 unknown, 0 DIFFERENT, 1 SAME
uint64_t s_probeBhFirst1 = UINT64_MAX, s_probeBhFirst0 = UINT64_MAX;
uint32_t s_probeBhFlips = 0;
bool s_probeWhoDiffSeen = false;
uint32_t s_probeReassertsAtOpen = 0;
// Articulation sample (s59 round 2): grip-anchor peak motion vs the hold-open
// snapshot. During a hold the drives are released (s52), so any motion is the
// game's authored anim through OUR rig = the own-rig discriminator. The door
// leg parks the rig (expected ~0); the drink animates it (expected tens of
// deg/UU). Detect-edge thresholds are log-only markers, not policy.
constexpr float kArtDetectDeg = 15.0f;
constexpr float kArtDetectUu = 8.0f;
bool s_artStartValid[2] = {};
float s_artStartQ[2][4], s_artStartT[2][3];
float s_artPeakDeg[2] = {}, s_artPeakUu[2] = {};
uint64_t s_artFirstMoveMs = UINT64_MAX;

float quat_angle_deg(const float a[4], const float b[4]) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.29578f;
}

void probe_sample_articulation(uint64_t sinceMs) {
    for (int h = 0; h < 2; ++h) {
        float q[4], t[3];
        if (!bones::anchor_atoms(h, q, t)) continue;
        if (!s_artStartValid[h]) {
            memcpy(s_artStartQ[h], q, sizeof q);
            memcpy(s_artStartT[h], t, sizeof t);
            s_artStartValid[h] = true;
            continue;
        }
        const float dx = t[0] - s_artStartT[h][0];
        const float dy = t[1] - s_artStartT[h][1];
        const float dz = t[2] - s_artStartT[h][2];
        const float uu = sqrtf(dx * dx + dy * dy + dz * dz);
        const float deg = quat_angle_deg(q, s_artStartQ[h]);
        if (uu > s_artPeakUu[h]) s_artPeakUu[h] = uu;
        if (deg > s_artPeakDeg[h]) s_artPeakDeg[h] = deg;
        if (s_artFirstMoveMs == UINT64_MAX &&
            (deg >= kArtDetectDeg || uu >= kArtDetectUu)) {
            s_artFirstMoveMs = sinceMs;
            BVR_LOG("[bsi] hide: PROBE +%llums ARTICULATION detected hand=%c "
                    "(%.1f deg, %.1f UU)",
                    static_cast<unsigned long long>(sinceMs), h ? 'R' : 'L', deg,
                    uu);
        }
    }
}

void probe_sample_bits(uint64_t sinceMs) {
    void* attach = bones::attachment();
    void* comp = bones::component();
    if (!attach || !comp) return;
    const int bits[4] = {
        read_bit(attach, g_actorHiddenOff, g_actorHiddenMask),
        read_bit(comp, g_hiddenGameOff, g_hiddenGameMask),
        read_bit(comp, g_ownerNoSeeOff, g_ownerNoSeeMask),
        read_bit(comp, g_onlyOwnerOff, g_onlyOwnerMask)};
    static const char* kNames[4] = {"bHidden", "HiddenGame", "bOwnerNoSee",
                                    "bOnlyOwnerSee"};
    for (int i = 0; i < 4; ++i) {
        if (bits[i] == s_probePrevBits[i]) continue;
        BVR_LOG("[bsi] hide: PROBE +%llums %s %d -> %d",
                static_cast<unsigned long long>(sinceMs), kNames[i],
                s_probePrevBits[i], bits[i]);
        if (i == 0 && s_probePrevBits[0] != -2) ++s_probeBhFlips;
        s_probePrevBits[i] = bits[i];
    }
    if (bits[0] == 1 && s_probeBhFirst1 == UINT64_MAX) s_probeBhFirst1 = sinceMs;
    if (bits[0] == 0 && s_probeBhFirst0 == UINT64_MAX) s_probeBhFirst0 = sinceMs;
}

void probe_sample_who(uint64_t sinceMs) {
    void* fpa = nullptr;
    void* fcomp = nullptr;
    if (!who_probe(&fpa, &fcomp)) {
        BVR_LOG("[bsi] hide: PROBE +%llums who FAILED (no pawn/dispatch)",
                static_cast<unsigned long long>(sinceMs));
        return;
    }
    const int same = (fpa == bones::attachment() && fcomp == bones::component())
                         ? 1
                         : 0;
    if (!same) s_probeWhoDiffSeen = true;
    if (same != s_probePrevWhoSame) {
        char cls[64] = {};
        reflect::class_name_of(fpa, cls, sizeof cls);
        BVR_LOG("[bsi] hide: PROBE +%llums who=%s game fpa %p (%s) comp %p | "
                "gameBits bH=%d HG=%d ONS=%d",
                static_cast<unsigned long long>(sinceMs),
                same ? "SAME" : "DIFFERENT", fpa, cls[0] ? cls : "?", fcomp,
                read_bit(fpa, g_actorHiddenOff, g_actorHiddenMask),
                read_bit(fcomp, g_hiddenGameOff, g_hiddenGameMask),
                read_bit(fcomp, g_ownerNoSeeOff, g_ownerNoSeeMask));
        s_probePrevWhoSame = same;
    }
}

// Called first from tick(), unconditionally (probe runs even with auto off or
// the lane fault-latched - it is read-only instrumentation).
void probe_tick(uint64_t nowMs) {
    if (!g_probe) return;
    const bool hold = cine::hold();
    if (hold && !s_probePrevHold) {
        s_probeOpenMs = nowMs;
        s_probeLastWhoMs = 0;
        for (int i = 0; i < 4; ++i) s_probePrevBits[i] = -2;
        s_probePrevWhoSame = -2;
        s_probeBhFirst1 = s_probeBhFirst0 = UINT64_MAX;
        s_probeBhFlips = 0;
        s_probeWhoDiffSeen = false;
        s_probeReassertsAtOpen = g_reasserts;
        s_artStartValid[0] = s_artStartValid[1] = false;
        s_artPeakDeg[0] = s_artPeakDeg[1] = 0.0f;
        s_artPeakUu[0] = s_artPeakUu[1] = 0.0f;
        s_artFirstMoveMs = UINT64_MAX;
        derive(false); // one-shot latch; bits read -1 if it refuses
        BVR_LOG("[bsi] hide: PROBE hold OPEN class=%s meleeRelease=%d rigHidden=%d "
                "(derived=%d)",
                cine::camera_hold() ? "camera" : "scripted",
                melee::hide_release() ? 1 : 0, g_rigHidden ? 1 : 0,
                g_derived ? 1 : 0);
        probe_sample_bits(0);
        probe_sample_who(0);
        s_probeLastWhoMs = nowMs;
        if (g_probeTravel) bones::handle_command("bsibones", "travel all 12");
    } else if (hold) {
        const uint64_t since = nowMs - s_probeOpenMs;
        probe_sample_bits(since); // free reads, edge-logged only
        probe_sample_articulation(since);
        if (nowMs - s_probeLastWhoMs >= 750) {
            s_probeLastWhoMs = nowMs;
            probe_sample_who(since);
        }
    } else if (s_probePrevHold) {
        const uint64_t dur = nowMs - s_probeOpenMs;
        char f1[24] = "never", f0[24] = "never";
        if (s_probeBhFirst1 != UINT64_MAX)
            snprintf(f1, sizeof f1, "+%llums",
                     static_cast<unsigned long long>(s_probeBhFirst1));
        if (s_probeBhFirst0 != UINT64_MAX)
            snprintf(f0, sizeof f0, "+%llums",
                     static_cast<unsigned long long>(s_probeBhFirst0));
        char fm[24] = "never";
        if (s_artFirstMoveMs != UINT64_MAX)
            snprintf(fm, sizeof fm, "+%llums",
                     static_cast<unsigned long long>(s_artFirstMoveMs));
        BVR_LOG("[bsi] hide: PROBE hold CLOSED dur=%llums | bHidden first1=%s "
                "first0=%s flips=%u last=%d | whoDIFFERENTseen=%d | "
                "reassertsDelta=%u | art L %.1fdeg/%.1fUU R %.1fdeg/%.1fUU "
                "firstMove=%s",
                static_cast<unsigned long long>(dur), f1, f0, s_probeBhFlips,
                s_probePrevBits[0], s_probeWhoDiffSeen ? 1 : 0,
                g_reasserts - s_probeReassertsAtOpen, s_artPeakDeg[0],
                s_artPeakUu[0], s_artPeakDeg[1], s_artPeakUu[1], fm);
    }
    s_probePrevHold = hold;
}

// ---- the s59 auto gate ------------------------------------------------------
// Game-thread statics; only the threshold crosses threads (F10 slider).
std::atomic<float> g_autoShowDeg{100.0f}; // rotation threshold, deg
bool s_autoPrevHold = false;
bool s_autoShowLatch = false;       // crossed the threshold this hold
bool s_autoSnapValid[2] = {};
float s_autoSnapQ[2][4];
bool s_autoDegradeLogged = false;   // one line per hold, not per tick
bool s_autoShowLogged = false;

// The per-hold verdict for kCineAuto. Hide-first by construction: the latch
// starts false every hold, so the rig hides at hold-open (today's accepted
// door behavior) and only releases when the game demonstrably animates OUR
// rig. Any signal failure returns true (= force behavior).
bool auto_wants_hide(bool holdEdge) {
    if (holdEdge) {
        s_autoShowLatch = false;
        s_autoSnapValid[0] = s_autoSnapValid[1] = false;
        s_autoDegradeLogged = false;
        s_autoShowLogged = false;
    }
    // Degrade to force: the bHidden read must exist and must be the game's
    // own (the actor lever WRITES that bit - mutually exclusive with auto).
    const int lever = g_lever.load(std::memory_order_relaxed);
    if (!g_actorHiddenMask || lever == kLeverActor) {
        if (!s_autoDegradeLogged) {
            s_autoDegradeLogged = true;
            BVR_LOG("[bsi] hide: auto-cine DEGRADED to force this hold (%s)",
                    !g_actorHiddenMask ? "bHidden not derived"
                                       : "actor lever owns the bit");
        }
        return true;
    }
    const int bh = read_bit(bones::attachment(), g_actorHiddenOff, g_actorHiddenMask);
    if (bh < 0) return true; // rig memory invalid this tick - stay hidden
    if (bh == 1) {
        // The game parked our rig (death/respawn class) - hide wins and the
        // show latch resets so a later phase must re-earn the release.
        s_autoShowLatch = false;
        return true;
    }
    // Rotation-only articulation vs the hold-open pose (translation is
    // contaminated by our own grip-scale hide; rotation reads ~6 deg through
    // it - s59 measured).
    for (int h = 0; h < 2 && !s_autoShowLatch; ++h) {
        float q[4], t[3];
        if (!bones::anchor_atoms(h, q, t)) continue;
        if (!s_autoSnapValid[h]) {
            memcpy(s_autoSnapQ[h], q, sizeof q);
            s_autoSnapValid[h] = true;
            continue;
        }
        const float deg = quat_angle_deg(q, s_autoSnapQ[h]);
        if (deg >= g_autoShowDeg.load(std::memory_order_relaxed)) {
            s_autoShowLatch = true;
            if (!s_autoShowLogged) {
                s_autoShowLogged = true;
                BVR_LOG("[bsi] hide: auto-cine OWN-RIG beat (hand %c rotated "
                        "%.1f deg) - rig shown for the rest of the hold",
                        h ? 'R' : 'L', deg);
            }
        }
    }
    return !s_autoShowLatch;
}

} // namespace

// ---- the production gate ----------------------------------------------------

void tick(uint64_t nowMs) {
    probe_tick(nowMs); // read-only; runs even with auto off / fault-latched
    if (g_faultLatched) return;
    if (!g_auto.load(std::memory_order_relaxed)) {
        unhide_all("auto off");
        return;
    }
    void* attach = bones::attachment();
    void* comp = bones::component();
    if (!attach || !comp) {
        // Rig dropped mid-hide: nothing to restore - a fresh rig spawns
        // visible, which is the fail-safe by construction.
        clear_latches();
        g_appliedAttach = nullptr;
        return;
    }
    if (attach != g_appliedAttach) {
        clear_latches(); // a new rig starts visible; restate from scratch
        g_appliedAttach = attach;
    }
    if (!derive(false)) {
        if (g_deriveRefused) g_faultLatched = true; // logged once by derive()
        return;
    }

    // A lever switch (F10 radio or command) with state still latched: restore
    // through the OLD lever first, then restate with the new one.
    const int lever = g_lever.load(std::memory_order_relaxed);
    if (lever != g_activeLever) {
        unhide_all("lever switch");
        g_activeLever = lever;
    }

    // Desired state from the s52-round-3 conditions (unchanged and correct),
    // scoped by the cine policy: during a hold the scene owns the rig - the
    // per-hand empty hides stand down so they can never fight an authored
    // hand moment (only the force policy asserts anything during a hold).
    // s53 headset call: BOTH hands empty uses the RIG-WIDE lever (the owner
    // composite - "truly hidden"); a single empty hand (skyhook-era) uses
    // that side's bone composite, best-effort.
    const bool hold = cine::hold();
    const int cineMode = g_cineMode.load(std::memory_order_relaxed);
    const bool holdEdge = hold && !s_autoPrevHold;
    s_autoPrevHold = hold;
    bool wantRig = false;
    if (hold) {
        switch (cineMode) {
            case kCineForce: wantRig = true; break;
            case kCineAuto: wantRig = auto_wants_hide(holdEdge); break;
            default: break; // kCineGame, kCineOff: touch nothing during holds
        }
    }
    bool wantHand[2] = {false, false};
    const bool perHandCapable = g_idxHideBone >= 0 && g_idxUnHideBone >= 0;
    if (!hold && profiles::hide_empty_hands()) {
        const bool e[2] = {profiles::hand_empty(0), profiles::hand_empty(1)};
        if (e[0] && e[1]) {
            wantRig = true;
        } else if (perHandCapable) {
            wantHand[0] = e[0];
            wantHand[1] = e[1];
        }
    }
    // s57 melee: the swing window and the execution hold release EVERYTHING -
    // the authored swing/execution animates the FP rig itself (the vigor-only
    // right hand reads empty and would be bone-hidden mid-swing; the
    // execution's hold would force-hide the rig the game is animating - the
    // exact s53 "authored hand moments" warning, observed as the no-hand
    // execution). Edge-driven applies below unhide, and the watchdog only
    // fires while a hide is latched, so it goes quiet for free.
    if (melee::hide_release()) {
        wantRig = false;
        wantHand[0] = wantHand[1] = false;
    }
    // s57b: during the SWING itself the gun hand hides (its authored melee
    // articulation lurches through the compose - headset verdict); never
    // during the execution (swing_hide_hand returns -1 there).
    const int swingHide = melee::swing_hide_hand();
    if (swingHide >= 0 && perHandCapable && !hold) wantHand[swingHide] = true;
    if (lever == kLeverBone) {
        // The bone lever IS per-side - fold the rig scope into both hands so
        // the two scopes never fight over the same dispatches.
        wantHand[0] = wantHand[0] || wantRig;
        wantHand[1] = wantHand[1] || wantRig;
        wantRig = false;
    } else if (wantRig) {
        wantHand[0] = wantHand[1] = false; // the rig lever covers everything
    }

    // Edge-driven applies. Unhides run first so a scope handoff (hand-hide ->
    // rig-hide) never leaves a freshly-unhidden limb.
    bool okAll = true;
    for (int h = 0; h < 2; ++h) {
        if (!wantHand[h] && g_handHidden[h]) {
            if (bone_side(comp, h, false, false)) g_handHidden[h] = false;
            else okAll = false;
        }
    }
    if (!wantRig && g_rigHidden) {
        if (apply_rig(false, lever, false)) g_rigHidden = false;
        else okAll = false;
    }
    if (wantRig && !g_rigHidden) {
        if (apply_rig(true, lever, false)) {
            g_rigHidden = true;
            ++g_applies;
            BVR_LOG("[bsi] hide: rig hidden (%s) - cine hold", lever_name(lever));
        } else {
            okAll = false;
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (wantHand[h] && !g_handHidden[h]) {
            if (bone_side(comp, h, true, false)) {
                g_handHidden[h] = true;
                ++g_applies;
                BVR_LOG("[bsi] hide: %c limb hidden (bone lever)", h ? 'R' : 'L');
            } else {
                okAll = false;
            }
        }
    }

    // Failure accounting: repeated dispatch failure on the game thread means
    // the lever is wrong for this object - latch off rather than spam.
    if (!okAll) {
        if (++g_failStreak >= 8) {
            g_faultLatched = true;
            BVR_LOG("[bsi] hide: LANE OFF - 8 consecutive apply failures on lever "
                    "%s; attempting one unhide",
                    lever_name(g_lever.load(std::memory_order_relaxed)));
            unhide_all("fault latch");
        }
        return;
    }
    g_failStreak = 0;

    // The re-assert watchdog: the game may restamp visibility on its own
    // schedule. 500 ms cadence; reads are direct gated bits (cheap), the bone
    // readback is two ProcessEvents (the cine-poll budget).
    if (nowMs - g_lastWatchMs >= 500) {
        g_lastWatchMs = nowMs;
        if (g_rigHidden && rig_state(lever) == 0) {
            if (apply_rig(true, lever, false)) ++g_reasserts;
        }
        if (g_handHidden[0] || g_handHidden[1]) {
            int32_t fn[2];
            for (int h = 0; h < 2; ++h) {
                if (!g_handHidden[h]) continue;
                if (bones::side_bones(h, fn, nullptr, 2) < 1) continue;
                if (bone_hidden_state(comp, fn[0]) == 0) {
                    if (bone_side(comp, h, true, false)) ++g_reasserts;
                }
            }
        }
    }
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsihide") != 0) return false;
    char sub[32] = {}, a1[64] = {}, a2[64] = {};
    const int n = sscanf_s(args ? args : "", "%31s %63s %63s", sub,
                           static_cast<unsigned>(sizeof sub), a1,
                           static_cast<unsigned>(sizeof a1), a2,
                           static_cast<unsigned>(sizeof a2));
    if (n < 1 || strcmp(sub, "status") == 0) {
        BVR_LOG("[bsi] hide: %s | cine %s | lever %s | derived %s%s | latches rig=%d "
                "L=%d R=%d | applies %u reasserts %u failstreak %u%s",
                g_auto.load(std::memory_order_relaxed) ? "AUTO ON" : "auto off",
                cine_mode_name(g_cineMode.load(std::memory_order_relaxed)),
                lever_name(g_lever.load(std::memory_order_relaxed)),
                g_derived ? "yes" : "no", g_deriveRefused ? " (REFUSED)" : "",
                g_rigHidden ? 1 : 0, g_handHidden[0] ? 1 : 0, g_handHidden[1] ? 1 : 0,
                g_applies, g_reasserts, g_failStreak,
                g_faultLatched ? " | FAULT-LATCHED (off this boot)" : "");
        if (g_derived)
            BVR_LOG("[bsi] hide: live bits - actor bHidden=%d comp HiddenGame=%d "
                    "bOwnerNoSee=%d bOnlyOwnerSee=%d",
                    read_bit(bones::attachment(), g_actorHiddenOff, g_actorHiddenMask),
                    read_bit(bones::component(), g_hiddenGameOff, g_hiddenGameMask),
                    read_bit(bones::component(), g_ownerNoSeeOff, g_ownerNoSeeMask),
                    read_bit(bones::component(), g_onlyOwnerOff, g_onlyOwnerMask));
        if (n < 1)
            BVR_LOG("[bsi] hide: verbs - status derive who diff actor|comp | "
                    "actor|comp|owner|pawn 0|1 | bone <Name> 0|1 | hand l|r 0|1 | "
                    "lever owner|comp|actor|bone | cine auto|always|game|off | "
                    "cine deg <n> | auto on|off | probe on|off|travel");
        return true;
    }
    if (strcmp(sub, "cine") == 0) {
        int want = -1;
        if (strcmp(a1, "auto") == 0) want = kCineAuto;
        else if (strcmp(a1, "game") == 0) want = kCineGame;
        else if (strcmp(a1, "force") == 0 || strcmp(a1, "always") == 0)
            want = kCineForce;
        else if (strcmp(a1, "off") == 0) want = kCineOff;
        else if (strcmp(a1, "deg") == 0) {
            float v = 0.0f;
            if (sscanf_s(a2, "%f", &v) == 1 && v >= 10.0f && v <= 180.0f) {
                g_autoShowDeg.store(v, std::memory_order_relaxed);
                BVR_LOG("[bsi] hide: auto-cine show threshold -> %.0f deg", v);
            } else {
                BVR_LOG("[bsi] hide: cine deg %.0f | usage: bsihide cine deg "
                        "<10..180>",
                        g_autoShowDeg.load(std::memory_order_relaxed));
            }
            return true;
        }
        if (want < 0) {
            BVR_LOG("[bsi] hide: cine %s | usage: bsihide cine "
                    "auto|always|game|off | cine deg <10..180>",
                    cine_mode_name(g_cineMode.load(std::memory_order_relaxed)));
            return true;
        }
        g_cineMode.store(want, std::memory_order_relaxed);
        BVR_LOG("[bsi] hide: cine -> %s (takes effect on the next tick edge)",
                cine_mode_name(want));
        return true;
    }
    if (strcmp(sub, "derive") == 0) {
        derive(true);
        return true;
    }
    if (strcmp(sub, "probe") == 0) {
        if (strcmp(a1, "on") == 0) {
            g_probe = true;
            s_probePrevHold = false;
            BVR_LOG("[bsi] hide: PROBE armed%s - per-hold discriminator sampling "
                    "(bit edges + who at 750 ms)",
                    g_probeTravel ? " (travel auto-arm ON)" : "");
        } else if (strcmp(a1, "off") == 0) {
            g_probe = false;
            BVR_LOG("[bsi] hide: PROBE off");
        } else if (strcmp(a1, "travel") == 0) {
            g_probeTravel = !g_probeTravel;
            BVR_LOG("[bsi] hide: PROBE travel auto-arm %s",
                    g_probeTravel ? "ON" : "off");
        } else {
            BVR_LOG("[bsi] hide: probe %s | usage: bsihide probe on|off|travel",
                    g_probe ? "ON" : "off");
        }
        return true;
    }
    if (strcmp(sub, "who") == 0) {
        cmd_who();
        return true;
    }
    if (strcmp(sub, "diff") == 0) {
        cmd_diff(n >= 2 ? a1 : "");
        return true;
    }
    if (strcmp(sub, "auto") == 0) {
        if (n >= 2 && strcmp(a1, "on") == 0) {
            g_faultLatched = false;
            g_failStreak = 0;
            g_auto.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] hide: auto ON (lever %s)",
                    lever_name(g_lever.load(std::memory_order_relaxed)));
        } else if (n >= 2 && strcmp(a1, "off") == 0) {
            g_auto.store(false, std::memory_order_relaxed);
            unhide_all("auto off (command)");
            BVR_LOG("[bsi] hide: auto off");
        } else {
            BVR_LOG("[bsi] hide: auto %s",
                    g_auto.load(std::memory_order_relaxed) ? "ON" : "off");
        }
        return true;
    }
    if (strcmp(sub, "lever") == 0) {
        int want = -1;
        if (strcmp(a1, "owner") == 0) want = kLeverOwner;
        else if (strcmp(a1, "comp") == 0) want = kLeverComp;
        else if (strcmp(a1, "actor") == 0) want = kLeverActor;
        else if (strcmp(a1, "bone") == 0) want = kLeverBone;
        if (want < 0) {
            BVR_LOG("[bsi] hide: lever %s | usage: bsihide lever owner|comp|actor|bone",
                    lever_name(g_lever.load(std::memory_order_relaxed)));
            return true;
        }
        unhide_all("lever switch"); // old lever restores before the new one arms
        g_lever.store(want, std::memory_order_relaxed);
        BVR_LOG("[bsi] hide: lever -> %s", lever_name(want));
        return true;
    }
    // Manual probes below: they need the derivation and a live rig, and they
    // deliberately bypass the latches (experiments, not policy). With auto ON
    // the watchdog will fight a manual flip within 500 ms - probe with auto
    // off.
    if (!derive(true)) return true;
    void* attach = bones::attachment();
    void* comp = bones::component();
    if (!attach || !comp) {
        BVR_LOG("[bsi] hide: REFUSED - rig not resolved");
        return true;
    }
    const bool onArg = n >= 2 && strcmp(a1, "1") == 0;
    if (strcmp(sub, "actor") == 0) {
        const int before = read_bit(attach, g_actorHiddenOff, g_actorHiddenMask);
        const bool ok = write_bit(attach, g_actorHiddenOff, g_actorHiddenMask, onArg);
        BVR_LOG("[bsi] hide: actor bHidden %d -> %d (%s; instance %p only, never the "
                "archetype)",
                before, read_bit(attach, g_actorHiddenOff, g_actorHiddenMask),
                ok ? "written" : "FAILED", attach);
        return true;
    }
    if (strcmp(sub, "comp") == 0) {
        const bool ok = dispatch_bool(comp, g_idxSetHidden, onArg);
        BVR_LOG("[bsi] hide: comp SetHidden(%d) -> %s | HiddenGame now %d", onArg ? 1 : 0,
                ok ? "dispatched" : "FAILED",
                read_bit(comp, g_hiddenGameOff, g_hiddenGameMask));
        return true;
    }
    if (strcmp(sub, "owner") == 0) {
        const bool ok = dispatch_bool(comp, g_idxSetOwnerNoSee, onArg);
        BVR_LOG("[bsi] hide: comp SetOwnerNoSee(%d) -> %s | bOwnerNoSee now %d",
                onArg ? 1 : 0, ok ? "dispatched" : "FAILED",
                read_bit(comp, g_ownerNoSeeOff, g_ownerNoSeeMask));
        return true;
    }
    if (strcmp(sub, "pawn") == 0) {
        cmd_pawn(onArg);
        return true;
    }
    if (strcmp(sub, "bone") == 0 && n >= 3) {
        const bool boneOn = strcmp(a2, "1") == 0;
        // Command-time pool scan is fine (never a cadence).
        const int32_t fn = patterns::fname_find(a1);
        if (fn < 0) {
            BVR_LOG("[bsi] hide: bone REFUSED - '%s' not in GNames", a1);
            return true;
        }
        const bool ok = dispatch_bone(comp, boneOn ? g_idxHideBone : g_idxUnHideBone,
                                      fn, true, a1);
        BVR_LOG("[bsi] hide: bone '%s' %s -> %s | IsBoneHidden=%d", a1,
                boneOn ? "HIDE" : "unhide", ok ? "dispatched" : "FAILED",
                bone_hidden_state(comp, fn));
        return true;
    }
    if (strcmp(sub, "hand") == 0 && n >= 3) {
        const int h = (a1[0] == 'r' || a1[0] == 'R') ? 1 : 0;
        const bool handOn = strcmp(a2, "1") == 0;
        bone_side(comp, h, handOn, true);
        return true;
    }
    BVR_LOG("[bsi] hide: unknown verb '%s' - run bsihide for usage", sub);
    return true;
}

// s59 config surface (cineRigMode / cineShowDeg - the camera.cpp key table).
int cine_mode() { return g_cineMode.load(std::memory_order_relaxed); }
void set_cine_mode(int m) {
    if (m < kCineGame || m > kCineAuto) return;
    g_cineMode.store(m, std::memory_order_relaxed);
}
float cine_show_deg() { return g_autoShowDeg.load(std::memory_order_relaxed); }
void set_cine_show_deg(float d) {
    if (d >= 10.0f && d <= 180.0f)
        g_autoShowDeg.store(d, std::memory_order_relaxed);
}

void draw_debug_ui() {
    // Nested inside the HANDS + MODEL section (anything judged by eye gets an
    // F10 control, never a console verb - the alt-tab rule).
    ImGui::Separator();
    bool on = g_auto.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("hide rig (cutscenes / empty hands)", &on)) {
        g_auto.store(on, std::memory_order_relaxed);
        // The tick's unhide-all path restores on the next game dispatch.
    }
    int cineMode = g_cineMode.load(std::memory_order_relaxed);
    ImGui::Text("cutscene rig:");
    ImGui::SameLine();
    bool cchanged = ImGui::RadioButton("auto (s59)", &cineMode, kCineAuto);
    ImGui::SameLine();
    cchanged |= ImGui::RadioButton("always hide", &cineMode, kCineForce);
    ImGui::SameLine();
    cchanged |= ImGui::RadioButton("game-managed", &cineMode, kCineGame);
    ImGui::SameLine();
    cchanged |= ImGui::RadioButton("cine off", &cineMode, kCineOff);
    if (cchanged) g_cineMode.store(cineMode, std::memory_order_relaxed);
    if (cineMode == kCineAuto) {
        float deg = g_autoShowDeg.load(std::memory_order_relaxed);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("own-rig show threshold (deg)", &deg, 10.0f,
                               180.0f, "%.0f"))
            g_autoShowDeg.store(deg, std::memory_order_relaxed);
    }
    int lever = g_lever.load(std::memory_order_relaxed);
    ImGui::Text("hide lever:");
    ImGui::SameLine();
    bool changed = ImGui::RadioButton("owner+grips", &lever, kLeverOwner);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("comp", &lever, kLeverComp);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("bone", &lever, kLeverBone);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("actor", &lever, kLeverActor);
    if (changed) g_lever.store(lever, std::memory_order_relaxed);
    ImGui::TextDisabled("hide: %s, cine %s, lever %s, rig=%d L=%d R=%d, applies %u "
                        "reasserts %u%s",
                        on ? "ON" : "off", cine_mode_name(cineMode), lever_name(lever),
                        g_rigHidden ? 1 : 0, g_handHidden[0] ? 1 : 0,
                        g_handHidden[1] ? 1 : 0, g_applies, g_reasserts,
                        g_faultLatched ? " FAULT-LATCHED" : "");
}

} // namespace bvr::bsi::hide
