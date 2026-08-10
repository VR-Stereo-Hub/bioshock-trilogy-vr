#pragma once
// s53 (I8): the FP-rig VISIBILITY lane - the "what to do when hiding" the s52
// rounds left open.
//
// The problem on record (STATUS s52 round 4): in the states that must not
// show controller hands - a cinematic hold and an empty hand - RELEASING the
// bone drive freezes the rig visibly (the game never animates the normal FP
// rig there), and the round-4 whole-rig zero-scale drive was falsified in the
// headset (intro cutscene text broke, the doubles remained) while the sim
// looked clean. So this module abandons bone-bank writes entirely and drives
// the game's OWN visibility levers instead, derived fresh per boot:
//  - actor:  bHidden bit on the XFirstPersonAttachment instance (direct
//    property-bit write - console `set` is dead on this build, s46);
//  - comp:   SetHidden(UBOOL) native dispatched on the XSkeletalMeshComponent;
//  - owner:  SetOwnerNoSee(UBOOL) - UE3's canonical "hide the FP mesh from
//    its own player" lever;
//  - bone:   HideBoneByName per side (grip + arm chain - the s45b live-proven
//    lever; the grip subtree carries the hand AND the holdable), the only
//    PER-HAND scope.
// Which lever ships is an EVIDENCE decision (sim probes, then the headset
// A/B) - `bsihide lever` keeps them all reachable for the A/B.
//
// Policy: cine::hold() hides the whole rig; an empty hand (profiles) hides
// that side's limb. Fail-safes: instance-only writes (never the archetype -
// spawns must default visible), edge-driven with a 500 ms re-assert watchdog,
// a fresh/dropped rig always restarts visible, and any fault or persistent
// dispatch failure latches the lane off after one unhide attempt.

#include <cstdint>

namespace bvr::bsi::hide {

// Game thread (camera detour tail, after cine::tick so the hold verdict this
// dispatch acts on is fresh). Cheap when idle: a few relaxed loads.
void tick(uint64_t nowMs);

bool handle_command(const char* cmd, const char* args); // bsihide
void draw_debug_ui(); // nested in the HANDS + MODEL F10 section

} // namespace bvr::bsi::hide
