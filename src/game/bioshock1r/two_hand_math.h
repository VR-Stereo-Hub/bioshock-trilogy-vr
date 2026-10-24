#pragma once
// The two-handed grip and the gunshot kick, pure (2026-10-19). Host-tested by
// tools\tests\two-hand-tests.cpp; two_hand.cpp feeds them the live numbers.
//
// BRVR (CameraHook "M6-S2: THE TWO-HANDED GRIP", Config "HAPTICS") is the reference, and its
// numbers are kept: a grab radius of 12 cm and a release radius of 20 cm (hysteresis, so a hand
// resting on the boundary cannot chatter), the grip's RISING edge inside the radius takes hold,
// the grip coming up lets go. The grab point costs nothing: on the two-handed guns the game's
// own animation already puts the off hand on the fore-end, and that is where the hand goes.
// While held the gun lies along both hands - the rear hand's palm stays on its controller and
// the gun turns about it until the fore-end points at the front hand; its roll stays the rear
// hand's (BRVR substitutes pitch and yaw only; the left-hand fork aims along rear -> front the
// same way). Only sliding ALONG the gun can take the hand off it while held, since the gun
// turns to meet the hand: the fork's measure, the real span against the authored one.

#include "arm_ik.h"

#include <cmath>
#include <cstdint>

namespace bvr::b1r::two_hand {

using arm_ik::Quat;
using arm_ik::Vec;

struct GripConfig {
    float grabCm = 12.0f;    // BRVR TwoHandGrabRadius
    float releaseCm = 20.0f; // BRVR TwoHandReleaseRadius
};

// One frame of the off hand. `distCm` is the hand's distance from the grab point (unheld) and
// `slideCm` how far it has slid along the gun from it (held); negative = not applicable (not a
// two-handed gun, a hand untracked, the hand busy elsewhere) - that lets go and is never in.
struct GripInput {
    float distCm = -1.0f;
    float slideCm = -1.0f;
    bool grip = false;
    bool takeHeld = false; // 2026-10-20: a squeeze already down counts as the grab's press (a reload
                           // that ended under it hands the squeeze on: the hand back on the fore-end)
};

struct GripOutput {
    bool eligible = false; // inside the radius: the grip is the grab's (no bumper), the hand buzzes if not held
    bool held = false;
    bool grabbed = false;  // this frame
    bool letGo = false;    // this frame
    bool slidOff = false;  // the let-go was the hand leaving, not the grip
};

class Grip {
public:
    GripOutput update(const GripInput& in, const GripConfig& c) {
        GripOutput o;
        const bool rising = in.grip && (!gripWas_ || in.takeHeld);
        gripWas_ = in.grip;
        if (held_) {
            const bool off = !(in.slideCm >= 0.0f) || in.slideCm > c.releaseCm;
            if (!in.grip || off) {
                held_ = false;
                o.letGo = true;
                o.slidOff = in.grip;
            }
        } else if (rising && in.distCm >= 0.0f && in.distCm < c.grabCm) {
            held_ = true;
            o.grabbed = true;
        }
        o.held = held_;
        // Eligible gates the bumper: inside the grab radius, and for the whole hold (BRVR gates the
        // suppression on eligibility, not on the grip, so the press that grabs leaks no frame).
        o.eligible = held_ || (in.distCm >= 0.0f && in.distCm < c.grabCm);
        return o;
    }
    bool held() const { return held_; }
    void reset() { held_ = false; }

private:
    bool held_ = false;
    bool gripWas_ = false;
};

// The shortest rotation taking direction `from` onto `to`. Identity for a degenerate input.
inline Quat align(Vec from, Vec to) {
    const float lf = arm_ik::length(from), lt = arm_ik::length(to);
    if (!(lf > 1e-6f) || !(lt > 1e-6f)) return {};
    const Vec a = from * (1.0f / lf), b = to * (1.0f / lt);
    const float d = arm_ik::dot(a, b);
    if (d > 1.0f - 1e-7f) return {};
    if (d < -1.0f + 1e-6f) return arm_ik::axis_angle(arm_ik::fallback(a), 3.14159265f);
    return arm_ik::axis_angle(arm_ik::cross(a, b) * (1.0f / std::sqrt(1.0f - d * d)), std::acos(d));
}

// The gun's turn while two-handed: about the rear palm, from where the drawn gun has the front
// hand (`authoredFront`) to where the front hand really is (`realFront`). Hands closer than
// `minSpan` (component units) are no axis - BRVR: below ~5 cm the direction is noise and a gun
// snapping to a random heading is worse than one that does not two-hand - so identity.
inline Quat aim_turn(Vec rearPalm, Vec authoredFront, Vec realFront, float minSpan) {
    const Vec a = authoredFront - rearPalm, r = realFront - rearPalm;
    if (arm_ik::length(a) < minSpan || arm_ik::length(r) < minSpan) return {};
    return align(a, r);
}

// 2026-10-24: the turn above is the shortest swing taking the fore-grip's direction onto the front hand's. The
// fore-grip lies BELOW the barrel (the crossbow's stock: 6-7 units under), so that swing also rolls the gun about its
// barrel - the 15:22 run's left-handed hold rolled 25-35 deg in a quarter second on taking hold, onto its side, against
// the rule above (the roll stays the rear hand's). The swing-twist split: the part of `turn` about `barrel` (unit) is
// removed, the rest - the pitch and yaw that point the gun - kept. Identity in, identity out.
inline Quat without_twist(Quat turn, Vec barrel) {
    const float len = arm_ik::length(barrel);
    if (!(len > 1e-6f)) return turn;
    const Vec d = barrel * (1.0f / len);
    const float p = turn.v[0] * d.x + turn.v[1] * d.y + turn.v[2] * d.z;
    Quat twist{{d.x * p, d.y * p, d.z * p, turn.v[3]}};
    const float tl = std::sqrt(twist.v[0] * twist.v[0] + twist.v[1] * twist.v[1] + twist.v[2] * twist.v[2] +
                               twist.v[3] * twist.v[3]);
    if (!(tl > 1e-6f)) return turn; // a half turn exactly across the barrel: no twist to take out
    for (int c = 0; c < 4; ++c) twist.v[c] /= tl;
    return arm_ik::normalized(arm_ik::mul(turn, arm_ik::conj(twist)));
}

// The gun as drawn against the hands (2026-10-20). The game's clips place a hand against the gun
// at the gun's AUTHORED size beside the game's own hand (the rig's DrawScale). The mod draws the
// hands at `handSize` and the gun's skeleton scaled by `drawnWeaponScale` about its grip socket
// (1 = authored), so a point the clip has on the gun lies at `ratio` of its clip distance from
// the socket on the drawn gun. ratio 1: the clip's point is the drawn gun's.
inline float gun_ratio(float drawnWeaponScale, float rigDrawScale, float handSize) {
    if (!(handSize > 1e-4f) || !(rigDrawScale > 1e-4f) || !(drawnWeaponScale > 1e-4f)) return 1.0f;
    return drawnWeaponScale * rigDrawScale / handSize;
}
// A point the clip has against the gun (component space), moved onto the drawn gun.
inline Vec on_drawn_gun(Vec gripSocket, Vec clipPoint, float ratio) {
    return gripSocket + (clipPoint - gripSocket) * ratio;
}

// The gunshot kick. BRVR kicks the gun hand when its firing clip jerks the wrist past 6 deg in a
// frame (Config HapticRecoil*: 45 ms at 1.0), the honest "a shot happened" signal it had - it
// respects ammo, fire rate and reloads where a trigger watch would kick on a dry click. In this
// mod's simulator that signal is wrong both ways: the Tommy Gun's clip turns the wrist 1.8 deg a
// frame (one kick for a 2.5 s burst) and a wrench swing 96 deg (eight kicks for one swing). So a
// shot is an EVENT that is equally honest: the player's fire seam running (one per bullet, pellet
// volley, cast or wrench strike - aim.h player_shots). One event, one kick: a quiet period after
// each folds a seam called twice for one shot into one kick, and is short enough for automatic
// fire to rattle.
struct KickConfig {
    int pulseMs = 45;     // BRVR HapticRecoilMs
    int quietMs = 80;     // shorter than any automatic weapon's shot interval
};
class Kick {
public:
    bool update(bool shotEvent, uint64_t nowMs, const KickConfig& c) {
        if (!shotEvent || (any_ && nowMs - lastMs_ < static_cast<uint64_t>(c.quietMs))) return false;
        any_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    bool any_ = false;
    uint64_t lastMs_ = 0;
};

} // namespace bvr::b1r::two_hand
