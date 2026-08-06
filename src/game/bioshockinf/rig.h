#pragma once
// Infinite viewmodel drive MECHANISM: resolve the first-person carrier from the
// pawn, validate its identity, and place it at a game-space target. The POLICY
// (which controller, what trims and offsets) lives in hands.cpp.
//
// THE TARGET, established by intervention in session 46 rather than inferred:
// `XFirstPersonAttachment`, an ACTOR, reached as
// `PC +0x1FC -> XHuman +0x0D8 -> object-pointer LIST -> list +0x004`.
// `SetHidden b1` on it removes the ENTIRE viewmodel - arm, hand and weapon -
// while hiding either XSkeletalMeshComponent on the pawn changes nothing in
// first person. So this actor is what the player sees, and the standard actor
// transform offsets (Location +0x44, Rotation +0x50) apply to it.
//
// ---- THE CONSTRAINT THAT SHAPES EVERYTHING: ONE CARRIER, TWO HANDS ----------
// There is exactly ONE such actor, with ONE transform, and hiding it takes the
// arm and the gun away TOGETHER. Per-hand independence is therefore NOT
// available from an actor-transform drive. This module writes the
// carrier-owning hand only (default RIGHT - the weapon hand, the one the
// carrier is named for). The other hand's target is still computed by the
// policy layer and still printed by `bsihands status`, marked NOT WRITTEN -
// never a silent zero and never a fake success.
//
// The second-carrier question stays OPEN with three candidates, each with the
// rung that decides it:
//   (a) another slot in the same list. The s46 walk found ONE
//       XFirstPersonAttachment among 12 entries - but no Vigor was equipped, so
//       the vigor hand was not rendering. Re-walk with one equipped.
//   (b) a mesh component UNDER the attachment. Not the pawn's two - R1 killed
//       those - so this needs the attachment's own component list walked.
//   (c) there is no second carrier, and the honest ship is a one-handed rig
//       with the milestone's "done when" amended rather than faked.
//
// ---- WHY RAW WRITES, NOT NATIVES -------------------------------------------
// `execSetLocation` is verified ABSENT from this build, so location has no
// native path at all: raw or nothing. Rotation raw for the same reason plus
// cost, with `SetRotation` kept as a fallback. And `fname_find` is an O(N)
// linear _stricmp scan of GNames (62k entries), so it can NEVER be on a
// per-frame path - hence the two-cache dispatch helper below.
//
// R2 also showed a `SetDrawScale` write reaching render with NO
// `ForceUpdateComponents` and no `ReattachComponent`, so that escalation ladder
// is not entered by default. Note DrawScale scales about a pivot at the EYE,
// which only becomes the right knob once the drive is writing Location
// absolutely every frame.

#include "game/bioshockinf/frame_context.h"

#include <cstdint>

namespace bvr::bsi::rig {

// Where the per-frame write happens. R4's A/B. drawTid == cameraTid on this
// game (measured, two boots), so the DrawDetour points are NOT cross-thread and
// may legally dispatch a native.
enum class WritePoint : int { Off = 0, DrawEntry = 1, DrawExit = 2, CameraTail = 3 };

WritePoint write_point();
void set_write_point(WritePoint p);

// Probe mode computes and counts everything and writes NOTHING - the aim lane's
// proven pattern, so a diagnostic cannot change what it measures. Default ON.
bool probe();
void set_probe(bool on);

// The master arm. Default OFF: this module ships with the write disabled and
// `bsihands status` must print a target that tracks a controller sweep before
// anything is armed.
bool armed();
void set_armed(bool on);

// Resolve + identity, for the status readout. All pointers are revalidate-
// before-use; nothing here is cached across a frame.
struct Resolved {
    void* pc = nullptr;
    void* pawn = nullptr;
    void* list = nullptr;
    void* fpa = nullptr;
    void* mesh = nullptr; // the FP mesh COMPONENT at fpa+0x218 - the drive target
    bool identityOk = false;
    uint32_t epoch = 0;
};
bool resolve(Resolved* out);

// Place the carrier at a game-space target. hand 0 = left, 1 = right; a hand
// that does not own the carrier returns false immediately and counts a refusal.
// Game thread only. False = nothing was written this frame, for any reason.
bool drive(int hand, const GamePose& target);

// Repaint the last write for the stereo second pass. The doubled draw may
// re-evaluate over pass 1's write, and an eye that disagrees is visible.
void reapply();

// Stop driving: restore the engine-authored transform captured at the start of
// this drive episode, but ONLY through an intact identity (same epoch, same
// object) - restoring a dead actor's transform into a live one is corruption,
// not a restore. Otherwise forget it and say so.
void release(const char* why);

// Ground-truth oracle, BS2's bones::last_write shape: the last transform
// actually READ BACK out of the object after writing it. A read-back rather
// than an echo, because an echo only proves we computed a number.
bool last_write(int hand, float* x, float* y, float* z, uint64_t* ageMs);

// The world changed under us - drop every cached pointer, verdict and saved
// transform. Wired to the camera's long-silence edge, which on this game is a
// level load.
void on_world_change(const char* why);

// Per-hand model scale (1.0 = authored), decoupled from worldScale by design:
// worldScale calibrates XR metres to UU, this calibrates the MODEL, and the rig
// can be the wrong size while the world is right.
void set_scale(int hand, float s);
float scale_of(int hand);
void set_weapon_scale(float s);
float weapon_scale();

// Arms mode 0 = game, 1 = follow, 2 = hide. REFUSES until the first-person mesh
// component is identified: R1 established that neither pawn
// XSkeletalMeshComponent is the arms, so the component that carries them is not
// yet named and a guessed bone list is a guess.
void set_arms_mode(int mode);
int arms_mode();

// One-shot GNames warm-up for the native slots. Command-driven, never per
// frame. GNames only ever grows, so a resolved index is valid for the process.
bool warm_names();

// `bsihands` mechanism verbs, forwarded from hands.cpp.
bool handle_command(const char* args);

// Count a call-in firing at a given point, whether or not it wrote. This is the
// R4 A/B instrument: compare hits/s at each point against the present rate
// before arming any of them.
void note_hit(WritePoint p);

void status_log();

} // namespace bvr::bsi::rig
