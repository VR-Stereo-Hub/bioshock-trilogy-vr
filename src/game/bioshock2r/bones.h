#pragma once
// BS2 bone drive MECHANISM: locate the AHands rig's SkeletonInstance, capture
// its reference pose, and rigidly place a configurable bone cluster at a
// game-space target every frame. The POLICY (which controller, what trims)
// lives in hands.cpp; the mechanism/policy split is what lets the actor-mode
// fallback ship if this lane ever stalls on another build.
//
// BS1's bones.cpp is the SHAPE reference (rigid cluster about an anchor,
// composed against the ACTOR transform, reapply on the stereo second pass,
// release() as the deliberate hand-back). Two deliberate omissions:
// - NO render-lock domain (lock/lockgain/lockdgain/lockpull): session 21's
//   user-accepted verdict - the lock's own correction CAUSED the +-90 deg
//   laser-vs-gun drift. BS2 composes at true geometry, full stop.
// - No barrel_ref_axis yet (session 40, with the bone-name map).
//
// Derivation for every offset: patterns.h "the AHands rig" + ENGINE_NOTES
// session 39. The cluster is RUNTIME-CONFIGURABLE (`vrbones cluster`)
// because the per-hand bone split is not yet derived - default drives the
// whole 64-bone rig from one controller; session 40 splits it by name.

#include "game/bioshock2r/frame_context.h"

#include <cstdint>

namespace bvr::b2r::bones {

// Rigidly place one hand's cluster at the target pose (game space); hand 0 =
// left (plasmid), 1 = right (weapon). Resolves and revalidates the rig lazily
// (one-shot heap scan, dormant after misses, re-armed on view-state change).
// Game thread, CalcView tail only. False = rig not available this frame.
//
// Composition (session 40, the ~90 deg fix): the cluster's authored rotations
// are PRESERVED and rotated by the controller's rotation relative to the
// AHands actor - q_i = qtc * refQ_i, so a controller aiming where the view
// aims leaves the rig exactly where the engine drew it. The old form
// (delta = qtc * conj(refQ_anchor)) replaced the anchor's authored frame with
// the raw controller rotation and cost a constant ~81.6 deg on this rig.
bool drive(const FrameContext& ctx, const GamePose& target, int hand);

// Repaint the last drive's write - the stereo second pass replays CalcView
// and the engine may re-evaluate the skeleton over it. Cheap memcpy, no-op
// when nothing is cached.
void reapply();

// Stop driving one hand's cluster (hand < 0 = both): restores the captured
// reference pose over that cluster's bones and forgets its write, so the
// engine's own animation owns them again. A left-hand release must never
// disturb the right hand's live drive, hence the per-cluster restore.
void release(const char* why, int hand = -1);

// Per-cluster scale multiplier (1.0 = authored). Applied to the pose bank's
// scale channel AND to the anchor-relative translations, so the cluster
// scales about its anchor rather than only thinning the bones. Deliberately
// independent of worldscale (user requirement, session-40 first look).
void set_scale(int hand, float scale);
float scale_of(int hand);

// Scale the weapon-attach (pivot) bone's SCALE CHANNEL with the hand? Some
// weapon attachments inverse-decompose the bone scale (BS1 session-30 class -
// live on BS2: the rifle's ammo drum GROWS when the hand scales down). Off =
// the pivot still MOVES with the scaled hand but keeps its authored scale, so
// the weapon stays authored size. Atomic-only setter, F10-safe.
void set_scale_attach(bool on);
bool scale_attach();

// Arms mode (session 40 round 2): 0 = game (engine animates them - reads as
// FROZEN arms beside driven hands), 1 = follow (arm bones ride their hand's
// cluster rigidly), 2 = hide (collapsed to zero scale - hands + weapon only).
// Atomic-only setter; the game thread applies transitions inside drive(),
// restoring the arm bones from reference when leaving follow/hide (the scale
// channel is never restamped by animation, so a stale zero-scale would strand
// the arms invisible forever - BS1's session-29 lesson).
void set_arms_mode(int mode);
int arms_mode();

// Left-eye flicker fix (session 40 round 2): the engine's animation restamps
// the pose bank mid-draw on SOME frames, after pass 1's CalcView write -
// pass 2 is protected by reapply(), pass 1 was not. Called per ProcessEvent
// dispatch while inside a hooked draw; one 48-byte sentinel compare per
// driven hand, full repaint only when a restamp is actually seen.
// Session 41: on pass 1 the repaint ABSORBS the restamp (adopts it as the
// new animation pose) and recomposes, instead of restoring a stale write;
// pass 2 keeps the verbatim restamp so both eyes render the same frame.
// Session 42: `site` tags the catch for the flicker instrumentation - 0 = the
// PE lane (default, existing call sites unchanged), 1 = the flush point.
void pe_repaint(int site = 0);

// Animation-preserving drive (session 41): ON composes the controller frame
// on top of the engine's own freshly-evaluated pose (adopted per bone only
// when the bank stopped being our own write), so weapon/finger animations
// play in the driven hand's space; OFF is the rigid session-40 reference
// drive (also the escape hatch if the returning idle sway bothers). The
// axes-instrument 0.21 deg rest oracle is only valid with anim OFF.
void set_anim_mode(bool on);
bool anim_mode();
// Re-add the wrist's own authored travel (0 = anchor glued to the controller,
// the default; 1 = full authored travel composed into the controller frame).
void set_anim_trans(float t);
float anim_trans();
// Telemetry: engine restamps absorbed for one hand (adoption liveness).
uint32_t adopt_count(int hand);

// --- Session 42: flicker DIAGNOSIS snapshot ---------------------------------
// Cumulative counters plus the per-phase write->catch latency maxima (the
// dmax fields are a WINDOW max: drained to 0 by the snapshot, so the caller's
// cadence defines the window). Phases: 0/1 = PE repaint pass 1/2, 2/3 =
// flush-point repaint pass 1/2. catches[][0] = hands bank, [][1] = wskel.
// Invariant printed by the consumer: sum(catches) == peRepaints.
struct FlickerStats {
    uint32_t catches[4][2];
    uint32_t dmaxMs[4];
    uint32_t driveAdoptEvents[2]; // [0] hand drives w/ adoption, [1] wskel
    uint32_t adopts[2];           // raw per-hand adoption counts
    uint32_t wAdopts;
    uint32_t peRepaints;
    uint32_t worldChanges;
    uint32_t wRescans;
};
void flicker_snapshot(FlickerStats* out);

// Session 74: the pose-RACE probe. Samples the drive's sentinel bones at six
// points of the stereo pair - 0 pass-1 entry, 1 pass-1 flush (before the
// repaint), 2 pass-1 post-drain, 3 pass-2 entry, 4 pass-2 flush, 5 pass-2
// post-drain - and counts "bank is foreign here", per hands bank / weapon
// skeleton. pe_repaint(2) is the pass-ENTRY repaint (phases 4/5 in the catch
// table), armed per pass by set_entry_fix (`vrbones p1fix|p2fix`).
struct RaceStats {
    uint32_t calls[6] = {};
    uint32_t foreign[6][2] = {};
    int firstDiff[6] = {};          // last first-differing hand bone at that point
    uint32_t entryCatch[2][2] = {}; // [pass][hands/weapon]
    bool entryFix[2] = {};
    bool fullMask = false;
};
void probe_point(int p);
void race_snapshot(RaceStats* out);
void set_entry_fix(int pass, bool on);
bool entry_fix(int pass);
// s74: repaint decision scans every masked bone instead of the first one.
void set_full_mask(bool on);
bool full_mask();
// s74: hardware write-watch on the sentinel bone (`vrbones watch on|off`);
// status lists the engine writer RVAs and where in the pair they fired.
void watch_set(bool on);
void watch_status();
// s74 fix: hook the writer the watch named and repaint the driven pose right
// after it returns inside pass 1 (`vrbones wfix install|on|off|status`).
bool wfix_install();
void wfix_set(bool on);
void wfix_status();
bool wfix_enabled(); // the post-writer repaint is armed (F10 A/B checkbox)
bool wfix_hooked();  // the writer hook is installed (rig resolved at least once)
// `vrbones flick on|off` gates only the [flick] minute log line; the counters
// always count (they are a handful of relaxed atomics on actual catches).
bool flicker_log();
void set_flicker_log(bool on);

// World/view changed under us - drop every cached pointer and the reference.
void on_world_change(const char* why);

// `vrbones <args>`: status | cluster <lo> <hi> <anchor> | refcap | release.
bool handle_command(const char* args);

// Telemetry for vrhands status: one cluster's last written anchor location
// (UU) - the VERIFICATION 2.8 ground truth ("last write loc tracks the
// sweep"), now per hand so each cluster proves its own controller.
bool last_write(int hand, float* x, float* y, float* z, uint64_t* ageMs);

// The resolved AHands actor (null until the rig resolves). Diagnostic /
// identity-probe use only - consumers must treat it as revalidate-before-use.
void* hands_actor();

// The equipped holdable, read raw off the rig (kHandsCurrentHoldableOffset,
// session 41). NEVER vtable-gated (session-21 rule c) - validate the CLASS
// via patterns::object_class_name instead. false = rig unknown (no signal);
// true with *out == null = rig known, nothing equipped.
bool current_holdable(void** out);

// Uniform weapon scale (session 41): drives the HOLDABLE's own
// SkeletonInstance - scale channels AND translations, uniform about the
// component origin - so body and ammo canister scale together (the AHands
// pivot-63 path inverse-scales attachments and stays only as the
// scaleweapon fallback). Adopts the engine's trans/quat per bone (weapon
// animations keep playing while scaled); 1.0 = fully hands-off. Called per
// CalcView tail from hands::on_calcview; resolve/revalidate is internal.
void wskel_drive();
// Session 42: hand the weapon skeleton back explicitly (authored transforms
// restored through an INTACT identity only; no-op when nothing is held). The
// cine-suspend consumer: a suspended cutscene must show the authored weapon,
// and the engine never restamps the scale channel on its own.
void wskel_release(const char* why);
void set_weapon_scale(float s);
float weapon_scale();

// Weapon offset (session 41 round 2): moves the WEAPON relative to the hand
// by offsetting only the attach pivot's position (fingers/wrist and the aim
// ray are untouched). cm in the hand's trimmed basis, per-weapon profile
// field. hands.cpp converts to the game-space vector each frame via
// set_weapon_offset_game (game thread only).
void set_weapon_offset(float fwdCm, float rightCm, float upCm);
float weapon_off_fwd_cm();
float weapon_off_right_cm();
float weapon_off_up_cm();
void set_weapon_offset_game(float x, float y, float z);

} // namespace bvr::b2r::bones
