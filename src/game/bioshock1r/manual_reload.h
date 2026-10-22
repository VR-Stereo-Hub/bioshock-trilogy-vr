#pragma once
// The manual reload (2026-10-20): the game's own reload clip, held and driven by the player's hands.
// reload_rig.h holds the programs and the state machine (pure, host-tested) and says why; this
// module feeds it from the live game and carries out what it asks for - the game's reload is
// started, its clip held or let run through Actor's animation API (script_call.h), the part a
// hand carries is offset on the gun's own skeleton (bones.h), the grip of the hand without the gun
// is claimed so it never reaches the game as a bumper, and the hands are told by haptics.
// The normal reload button is untouched: a reload the mod did not start is never held.

#include "menu_model.h"
#include "game/bioshock1r/grips.h"
#include "game/bioshock1r/gun_state.h"

#include <cstdint>

namespace bvr::input {
struct PadOverride;
}

namespace bvr::b1r::manual_reload {

// One frame, from the hands' compose (bones.cpp), game thread.
struct Frame {
    void* weapon = nullptr;     // the holdable
    void* hands = nullptr;      // the Hands actor
    uint64_t now = 0;           // ms
    float hand[3] = {};         // the free hand's palm in the grip's frame (rig units), as with the
                                // gun in the right hand (unmirrored)
    float unitsPerCm = 1;       // rig units per centimetre at the drawn size
    float gripO[3] = {};        // the drawn grip in WORLD: x_world = gripO + gripQ * (p * gripScale)
    float gripQ[4] = {0, 0, 0, 1};
    float gripScale = 1;
    bool inPouch = false;       // the free hand is at the waist
    float gunDegPerSec = 0;     // how fast the gun hand turns
    int gunPhys = 1, freePhys = 0; // physical controllers, for haptics
    float bodyF[3] = {1, 0, 0}, bodyR[3] = {0, 1, 0}; // the body's forward/right (world), the watch line
    float uuPerCm = 1;
};
void tick(const Frame& f);

// The free hand's placement this frame, from the same compose (after tick): the clip's hand is
// drawn at x' = palmAt + q * (x - palm) * s in component space, the actor at loc/rot/scale. A part
// the hand carries is moved by exactly this, so it stays in the drawn hand as the clip holds it.
// The gun is drawn by the GUN hand's placement (the same form), so a part read where it is drawn is
// first taken back through the gun hand's to where the clip has it, then through the free hand's.
struct HandPlacement {
    float actorLoc[3] = {}, actorQ[4] = {0, 0, 0, 1}, actorScale = 1;
    float palm[3] = {}, palmAt[3] = {}, q[4] = {0, 0, 0, 1}, s = 1;          // the free hand
    float gunPalm[3] = {}, gunPalmAt[3] = {}, gunQ[4] = {0, 0, 0, 1}, gunS = 1; // the gun hand
    // The gun as drawn against the clip's: gunRatio of it about the grip socket (two_hand_math.h
    // gun_ratio), the socket at `socket` in the clip's component space.
    float socket[3] = {}, gunRatio = 1;
    // The free hand's wrist bone in the clip's component space: a designed grip (grips.h) puts the part
    // against it instead of where the clip's hand has it.
    float wristP[3] = {}, wristQ[4] = {0, 0, 0, 1};
};
// Game thread: a designed grip for the part the free hand carries now (grips.h), or null - bones.cpp
// curls that hand's fingers by it, carry() places the part by it.
struct GripTable {
    const grips::GripJoint* joints;
    int jointCount;
    const grips::GripPart* parts;
    int partCount;
};
const GripTable* carry_grip();
void carry(const HandPlacement& hp);

// Game thread: the held gun's state as this tick assembled it (gun_state.h) - the one record the trigger, the
// dry click and the two-handed grip's permission are read off. A default record with no weapon in hand.
const gun_state::State& state();
// A manual reload is in progress (the hands stay on their controllers; the two-handed grip waits).
bool active();
// Any thread: the gun in hand reloads by hand (the manual reload is on and has a program for it) - so any reload
// of it is the hands', from its first frame, before active() can say so (it begins in the gun hand's tick).
bool handles_reloads();
// Any thread: the gun in hand is the shotgun - its two-handed hold lets go only when the grip comes up (never by
// sliding or reaching off it), and it can be taken hold of at any time (two_hand_allowed).
bool sticky_grip();
// Game thread: the weapon's live clip is a reload's START clip (the shotgun's 'SingleFrame'): the free hand
// holds as it was through it (it snapped there and back on every reload an ammunition switch restarted).
bool start_clip(void* weapon);
// Any thread: the shotgun's pump is mid-stroke (the front hand working it, reload_rig.h Pump). The
// two-handed grip holds on through it - the pump is the fore-end, so working it is not sliding off.
bool pumping();
// Game thread: the two-handed grip may hold now - no manual reload, the reload's last pump, or a reload
// begun while both hands were on the gun and the grip is still held (bones.cpp keeps the free hand
// where it was on the gun then).
bool two_hand_allowed();
bool pump_wait(); // the reload's last pump is waiting for (or being worked by) the two-handed grip
// Game thread, once per compose: 0..1, how much the free hand is drawn where the clip's own hand is
// (holding the Tommy Gun's bolt: the hand locked onto it in the game's grip), eased in and out.
float hand_lock(uint64_t nowMs);
// Game thread: 0..1, how much of the gun hand's own clip motion is layered on the controller. 1 but
// for the shotgun's Fire clip, which fades it out after the recoil (0.30-0.45 s): the rest of that clip
// swings the gun up to pump it one-handed ("it should just recoil", 19:57 run).
float gun_motion_keep();

// XInput thread: the free hand's grip. Records it for the game thread, and withholds its bumper while
// the reload claims it. True when claimed this frame. `locked`: the grip arbiter gave this squeeze
// to another grab - it is no press here.
bool pad(bvr::input::PadOverride* pad, bool* changed, bool locked);
// For the grip arbiter (XInput thread): how near the free hand is to what starts a reload, over the
// grab radius (<= 1 in reach); 0 while a reload in progress wants the grip; -1 nothing.
float reach();

// The handle of the weapon's own animation (Weapon.AnimationHandle, which the reload waits on), 0
// when unreadable. Game thread.
int32_t weapon_clip_handle(void* weapon);

// `vrreload ...` (camera.cpp's command switch): status | probe | hold <s> | release | give ...
void handle_command(const char* args);

// F10 (routed through dial_input -> two_hand).
float menu_read(menu::Setting id); // NaN = not this module's
bool menu_write(menu::Setting id, float value);

} // namespace bvr::b1r::manual_reload
