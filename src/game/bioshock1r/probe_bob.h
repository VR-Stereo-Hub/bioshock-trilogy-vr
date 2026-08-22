// BioShock 1: WHERE DOES THE WEAPON'S WALK BOB COME FROM?
//
// Read-only, self-limiting, and meant to be DELETED once it has answered.
//
// The bob is not in the hand skeleton: walking probe deltas measured 1.0-1.6 UU
// / 2-5.6 deg against a 6 UU / 12 deg gate, so the reference freeze holds and
// nothing animated reaches the driven rig. It is not in the weapon's own
// skeleton either: BRVR never touches that and has no bob. The hands do not
// bob. So it enters in the chain that composes the WEAPON ACTOR from the
// AHands actor, and there are two candidates left:
//
//   A. The AHands actor's ROTATION bobs. A rotational bob about the actor
//      origin barely moves hands sitting at the pivot and swings a long gun
//      visibly at the muzzle, which is exactly "hands fine, gun bobs". BRVR
//      pins actor rotation and has to re-apply it from Present because the
//      game tick erases a CalcView-time write - the write this repo never
//      makes.
//
//   B. The attach path composes the weapon from a PRE-WRITE snapshot, so the
//      weapon actor lags into the bobbing pose while the hands render
//      correctly from the bones we wrote.
//
// The discriminator is peak-to-peak oscillation per second, bucketed by
// whether the player is walking, of four quantities:
//
//   actorZ-pawnZ     the actor's height above the pawn      -> actor LOCATION bobs
//   actor pitch/roll                                        -> actor ROTATION bobs  (A)
//   weapon-vs-actor  the weapon's offset from the actor     -> the ATTACH chain bobs (B)
//   weapon pitch/roll                                       -> the weapon's own orientation bobs
//
// A quantity whose moving peak-to-peak is much larger than its standing one is
// the bob. If none of them separate, the bob is downstream of all of this, in
// the foreground render pass, and the whole actor/attach line of attack is
// wrong.
//
// See docs/bioshock1/VIEWMODEL-RIG-DIFFERENCES.md for how the candidates were
// narrowed and what each verdict implies for the fix.

#pragma once

namespace bvr::b1r::probe_bob {

// Called from the CalcView hook with the player controller and the view actor
// (the pawn during gameplay). Self-throttling; costs a handful of reads.
void on_calcview(void* pc, void* viewActor);

// `vrprobe bob on|off|status`. Default ON until the question is answered.
void handle_command(const char* args);

} // namespace bvr::b1r::probe_bob
