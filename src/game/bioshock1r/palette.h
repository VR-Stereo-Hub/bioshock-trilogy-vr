#pragma once
// s87: BS1's twin of Dishonored's palette route (DISHONORED_PIPELINE.md 1.2-1.4).
//
// BS1 skins on the CPU from a per-bone matrix array on the USkeletalMeshInstance (patterns.h
// "THE SKIN PALETTE"). This module hooks the routine that gathers that array for the skinner
// and, for the hands' mesh instance only, hands the skinner a COPY with each hand's correction
// composed on:
//
//     newPalette[i] = S_hand * palette[i]      for every bone of that hand's cluster
//
// where S_hand is the same correction mode 4 computes (hand_compose::delta, the palm on the
// controller) plus the hand size about the palm - Dishonored's D * M with scale_about. The
// engine's own array, its skeleton and every bone the game reads are never written: the
// animation keeps running underneath and only the skinned vertices move. Arm bones are not
// bound by the hierarchy here either; "hands only" collapses each sleeve bone's matrix to the
// composed wrist point (Dishonored cuts the mesh; this is the palette's nearest equivalent).

#include "game/bioshock1r/palette_math.h"

namespace bvr::b1r::palette {

// One hand's correction in the skeleton's component space (the space the evaluated pose and
// the palette share): x' = t + s * rotate(q, x - pivot).
struct HandXform {
    bool valid = false;
    float q[4] = {0, 0, 0, 1};
    float t[3] = {};
    float pivot[3] = {};
    float s = 1.0f;
    float wrist[3] = {}; // the composed wrist point, where hidden arm bones collapse
    // The engine's evaluated wrist (component space) for the space probe: if the palette and
    // the pose share a space, inverse(pose) * palette - the wrist's inverse bind - is constant.
    float poseP[3] = {};
    float poseQ[4] = {0, 0, 0, 1};
    int wristBone = -1;
    // s88: the five palm bones' evaluated positions (component space), for the rigid-anchor
    // measurement (Dishonored VR-183): their bind-space centroid is measured once from the
    // palette and carried by the hand bone alone from then on.
    float palmPoseP[5][3] = {};
    // s88: the IK arm on the palette (Dishonored arm_ik_draw.inc): per sleeve bone (patterns.h
    // kBone*Sleeve order), the affine that carries the game's bone to the solved one, column
    // form [R | t] row-major 3x4 (R may carry scale). Already blended toward identity by the
    // hand-back weight, so at weight 0 the arm is exactly the game's.
    bool armValid = false;
    float arm[5][12] = {};
};

// Hook the gather (once; fail-soft with a log line). Game thread.
bool install(const void* imageBase);

// The drive's switch. Default ON (s87: the tester's choice of route).
void set_enabled(bool on);
bool enabled();
// True once the hook is installed AND enabled: the drive then publishes instead of writing bones.
bool active();

// A similarity x' = s * rotate(q, x) + t: an actor's local-to-world (rotation, DrawScale,
// location) or a correction carried between spaces (palette_math.h).
using Sim = palette_math::Sim;

// The held weapon, Dishonored's way (DISHONORED_PIPELINE 1.5, weapon_attach.cpp): the held
// hand's correction in WORLD space, D_world = L_hand * S * inverse(L_hand), and the weapon actor
// whose instance gets  inverse(L_weapon) * D_world * L_weapon  composed onto its whole palette.
struct WeaponXform {
    bool valid = false;
    void* actor = nullptr;
    Sim dWorld;
    Sim lWeapon;
};

// L * S * inverse(L): a correction in L's local space, carried to the space L maps into.
Sim sim_world(const Sim& L, const Sim& S);

// Publish this frame's corrections for the mesh instance OWNED by this actor (the hands actor;
// patterns.h kMeshInstOwnerOffset). hideArms collapses the sleeves.
void publish(void* ownerActor, const HandXform hands[2], bool hideArms, const WeaponXform& weapon);
// Diagnostic link (s87): the hands actor and its SkeletonInstance, to find which field of the
// skinned instance points back at them.
void set_link_probe(void* actor, void* skelInst);
// Stop composing (the drive left mode 4, a menu, a stale frame).
void clear();

// One status line for the HANDS log and `vrhands palette status`.
void log_status();

// s88 (Dishonored VR-183): the palm anchor in the hand bone's frame, measured once from the bind
// pose through the palette - so finger animation can never move the point the hand is placed
// by. False until the gather has measured it (the drive then uses the live palm).
bool rigid_palm_local(int hand, float out[3]);
// The rigid anchor and the rigid wrist (VR-184: the hand's vertices keep no forearm weight)
// are levers, default ON: `vrhands palette anchor on|off`, `vrhands palette wrist on|off`.
void set_rigid_anchor(bool on);
bool rigid_anchor();
void set_rigid_wrist(bool on);

// The space probe (`vrhands palette probe on`): every 2 s, per wrist, the inverse bind implied
// by the engine's palette and pose, and how far it has drifted since the probe was armed.
void set_probe(bool on);

} // namespace bvr::b1r::palette
