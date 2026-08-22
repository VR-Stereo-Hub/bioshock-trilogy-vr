// BioShock 1: WHERE DOES THE WEAPON'S WALK BOB COME FROM?
//
// Read-only, self-limiting, and meant to be DELETED once it has answered.
//
// ---------------------------------------------------------------------------
// ROUND 4, 2026-08-22. What the first three rounds established, all measured:
//
//   * The AHands ACTOR's vertical position bobs. Clean straight-walking windows
//     span 3.1 to 8.4 UU against 0.00 standing, on a gait cadence. Its rotation
//     does not (0.46 deg max, roll 0.00).
//   * The actor sits EXACTLY on the pawn's eye point (measured residual +0.00),
//     so the actor's Z and the engine's view Z are the same number.
//   * Pinning that Z from Present LANDS but does not fix the bob, and desyncs
//     the gun between the two eyes (they render one build apart). Reverted.
//     While the player is MOVING the engine re-places the actor by 2.9 to 17.2
//     UU after our write and before the draw, so the pin only ever held during
//     the one state where there was nothing to hold.
//   * The weapon's offset from the actor does not oscillate on a gait cadence.
//
// And the finding that makes this round's question the right one, from this
// repo's own ENGINE_NOTES, established by a live poke: the bones are Havok
// hkQsTransforms in COMPONENT space, and "poking a bone's pos moved that mesh
// part on screen the same frame, and the equipped WEAPON rendered at the poked
// transform of the attach bone". The renderer draws the gun from bone 43.
//
// That is what makes the bob hard to explain. bones::drive writes bone 43 every
// frame from a FROZEN reference, composed against the live actor as
// ptc = qaInv (grip - actorLoc) - so the actor cancels by construction and the
// written value carries no animation at all. The hands, drawn the same way, are
// steady. The gun is not. One of the premises in that sentence is false.
//
// ---------------------------------------------------------------------------
// The candidates this round separates, by sampling the whole chain in one frame
// (every height taken against the pawn, so the player's own walking cancels):
//
//   actor Z            the engine's placement            KNOWN to bob
//   bone 43, array A   the array the renderer reads, BEFORE our write
//   bone 43, array A   the same slot AFTER our write     proves we land
//   bone 43, array B   the lazily-filled BY-NAME array   never written by us
//   weapon actor Z     the symptom itself
//
// SkeletonInstance carries two bone arrays: A at +0x48 feeds "the by-index path
// and the renderer", B at +0x54 is "the lazily-filled by-name path". Attachment
// is by bone NAME (AttachToBone stores the FName on the weapon at +0xF0). If
// the attach resolves through B, the gun renders from an array nothing in this
// tree has ever written - which would explain a steady hand and a bobbing gun
// exactly, and would mean the fix is a second write, not a better actor pin.
//
//   bone 43 in A bobs BEFORE our write and is flat AFTER  -> we own A, and a
//       still-bobbing gun means the gun is not drawn from A
//   bone 43 in B bobs                                     -> B is the carrier
//   neither bobs, actor bobs, weapon bobs with it         -> the gun rides the
//       actor after all, and the actor write has to land between the engine's
//       placement and the draw
//
// A NOTE ON WHAT A PROBE MAY DO. Round 2 resolved the weapon through
// hands::resolve_weapon_actor, which SCANS when its cache is empty. With a
// wrench held the vtable-gated accept took nothing ("2 match(es), 0 accepted")
// and a 3.9-second sliced sweep re-ran every 8 seconds for the whole session;
// the player reported the framerate before anything else. Nothing in this file
// may scan, allocate, or write. It reads, and it reads a bounded number of
// times.
//
// See docs/bioshock1/VIEWMODEL-RIG-DIFFERENCES.md for the full falsification
// record and what each verdict implies for the fix.

#pragma once

#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::probe_bob {

// BEFORE the viewmodel drive, so the bone array still holds what the engine
// evaluated. Samples the actor, both bone arrays and the weapon.
void on_calcview_pre(const FrameContext& ctx);

// AFTER the drive, same frame: re-reads bone 43 in array A so "did our write
// land" is answered by the same instrument, not by assumption.
void on_calcview_post(const FrameContext& ctx);

// `vrprobe bob on|off|status`. DEFAULT OFF - arm it deliberately.
void handle_command(const char* args);

} // namespace bvr::b1r::probe_bob
