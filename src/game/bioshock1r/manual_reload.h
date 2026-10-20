#pragma once
// The manual reload (2026-10-20): the game's own reload clip, held and driven by the player's hands.
// reload_rig.h holds the programs and the state machine (pure, host-tested) and says why; this
// module feeds it from the live game and carries out what it asks for - the game's reload is
// started, its clip held or let run through Actor's animation API (script_call.h), the part a
// hand carries is offset on the gun's own skeleton (bones.h), the grip of the hand without the gun
// is claimed so it never reaches the game as a bumper, and the hands are told by haptics.
// The normal reload button is untouched: a reload the mod did not start is never held.

#include "menu_model.h"

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
};
void carry(const HandPlacement& hp);

// A manual reload is in progress (the hands stay on their controllers; the two-handed grip waits).
bool active();

// XInput thread: the free hand's grip. Records it for the game thread, and withholds its bumper while
// the reload claims it. True when claimed this frame.
bool pad(bvr::input::PadOverride* pad, bool* changed);

// The handle of the weapon's own animation (Weapon.AnimationHandle, which the reload waits on), 0
// when unreadable. Game thread.
int32_t weapon_clip_handle(void* weapon);

// `vrreload ...` (camera.cpp's command switch): status | probe | hold <s> | release | give ...
void handle_command(const char* args);

// F10 (routed through dial_input -> two_hand).
float menu_read(menu::Setting id); // NaN = not this module's
bool menu_write(menu::Setting id, float value);

} // namespace bvr::b1r::manual_reload
