#pragma once
// The hand pose engine, pure (2026-10-21, docs/bioshock1/HELD_GUN_MATRIX.md section 10; step 3b of the
// HELD_GUN_AUDIT.md order). Host-tested by tools\tests\hand-pose-tests.cpp.
//
// WHAT THE FREE HAND IS DRAWN FROM is one decision per frame, read off the gun's state record and the hands'
// state: a WRIST source (the controller; the gun's rest point; the part the clip's hand grips) and a FINGER
// source (the clip's own; the gun's rest pose; a designed grip for a carried part; a relaxed hand). The six
// places that used to substitute hand bones on their own - the kept rest, the grip pose, the designed grip,
// the start-clip hold, the bolt lock's ease, the hand-back's manual-reload case - become one resolver and
// one builder (bones.cpp), each source blended in and out over its own ease against the game's pose.
//
// THE RULES, in priority order (the first that applies decides):
//   1. a clip the hand-back gives the whole hand to (a scripted animation, the melee attack): the clip's
//   2. two hands on the gun: the wrist AT THE GUN'S REST POINT, the rest pose's fingers - whatever any clip
//      does: a shot, an empty idle, a reload, the lowering of an ammunition switch (the tester's rule)
//   3. a part carried with a designed grip: the controller, the designed fingers
//   4. the clip's hand locked on a part the hand works (the Tommy's bolt): on the part, the clip's fingers
//   5. a part being pulled or dragged (the reload's hand on it): the controller, the clip's fingers
//   6. the hand holding nothing of the gun (idle one-handed, waiting for a part or at the pouch, the drop,
//      the seating, the clip finishing): the controller, a RELAXED hand - the idle clip's grip on nothing and
//      the reload clip's reach to the pouch no longer show on a hand that is on its controller (gap P1)
//   7. anything else (no gun, the hands lowered, an ability in the hand): the clip's
// The gun hand is not resolved here: it is the controller plus its recoil window and the two-handed aim
// (bones.cpp, manual_reload::gun_motion_keep), unchanged by this step.

#include "game/bioshock1r/gun_state.h"

#include <cstdint>

namespace bvr::b1r::hand_pose {

enum class Wrist : uint8_t { Controller, OnGun, OnPart };
enum class Fingers : uint8_t { Clip, Rest, Designed, Relaxed };

struct Inputs {
    gun_state::State gun;
    bool clipOwnsHand = false;   // the hand-back gives the whole hand to a clip (in view)
    bool held = false;           // two hands on the gun (the grip holds)
    bool hasRestPose = false;    // the gun has a rest pose table (a two-handed gun)
    bool designedGrip = false;   // a carried part has a designed grip table
    bool partLocked = false;     // the reload asks for the hand on the part (the bolt)
    bool dragging = false;       // the reload's hand is pulling or sliding a part (waiting, taken hold)
    bool hasRelaxed = false;     // the relaxed finger table is available
    bool weaponInHand = false;   // a holdable with a program or a fore-grip; false: a plasmid, nothing
};

struct Decision {
    Wrist wrist = Wrist::Controller;
    Fingers fingers = Fingers::Clip;
    int rule = 0; // which rule decided (for the log and the tests)
};

inline Decision resolve_free(const Inputs& in) {
    Decision d;
    if (in.clipOwnsHand) {
        d.rule = 1;
        return d;
    }
    if (in.held && in.hasRestPose) {
        d.wrist = Wrist::OnGun;
        d.fingers = Fingers::Rest;
        d.rule = 2;
        return d;
    }
    if (in.gun.carryingPart && in.designedGrip) {
        d.fingers = Fingers::Designed;
        d.rule = 3;
        return d;
    }
    if (in.partLocked) {
        d.wrist = Wrist::OnPart;
        d.rule = 4;
        return d;
    }
    if (in.gun.carryingPart || in.dragging) {
        d.rule = 5; // the clip's hand on the part it moves
        return d;
    }
    if (in.weaponInHand && in.hasRelaxed) {
        d.fingers = Fingers::Relaxed;
        d.rule = 6;
        return d;
    }
    d.rule = 7;
    return d;
}

// One source's weight, eased in and out (smootherstep) against the game's pose. `on` is whether the
// resolver chose it this frame; the weight follows over inMs / outMs.
struct Ease {
    float w = 0.0f;      // 0..1, the eased weight
    float from = 0.0f;   // where the current transition started
    bool on = false;
    uint64_t ms = 0;     // when it started
};
inline float smootherstep(float t) {
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
inline float ease(Ease& e, bool on, uint64_t nowMs, float inMs, float outMs) {
    if (on != e.on || e.ms == 0) {
        e.on = on;
        e.from = e.w;
        e.ms = nowMs;
    }
    const float span = on ? inMs : outMs;
    const float t = span > 0.0f ? static_cast<float>(nowMs - e.ms) / span : 1.0f;
    e.w = e.from + ((on ? 1.0f : 0.0f) - e.from) * smootherstep(t);
    return e.w;
}

// The eases per source (ms): in, out.
inline constexpr float kOnGunInMs = 120.0f, kOnGunOutMs = 150.0f;       // the two-handed hold's own
inline constexpr float kOnPartInMs = 100.0f, kOnPartOutMs = 200.0f;     // the bolt lock's own
inline constexpr float kFingersInMs = 150.0f, kFingersOutMs = 150.0f;   // rest / designed / relaxed fingers

inline const char* wrist_name(Wrist w) {
    static const char* const n[] = {"controller", "on the gun", "on the part"};
    return n[static_cast<int>(w)];
}
inline const char* fingers_name(Fingers f) {
    static const char* const n[] = {"the clip's", "the rest pose", "a designed grip", "relaxed"};
    return n[static_cast<int>(f)];
}

} // namespace bvr::b1r::hand_pose
