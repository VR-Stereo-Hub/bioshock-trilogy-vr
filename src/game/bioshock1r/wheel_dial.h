#pragma once
// BS1: the weapon and plasmid wheels chosen by moving the hand (the Dishonored VR
// mod's hand dial, docs/bioshock1/WEAPON_WHEEL.md). PURE: no engine, no globals -
// the DLL (wheel_dial.cpp) and tools/tests both use it.
//
// Per grip press: the dial's centre is the grip where it was pressed and its
// orientation faces the opening eye position, upright (optionally the head's tilt
// or yaw) - both frozen for the press. The hand's movement across that frozen plane
// picks a direction; depth does not count. The direction is snapped to the nearest
// FILLED slot, so every item gets an equal share of the circle in every stage, and
// that slot's canonical stick vector goes to the game, inside the movie's own wedge.
//
// Fail closed: no steering unless the wheel is really up (wheel_mirror), the hand
// and head are tracked, and the press seeded cleanly. A tracking loss neutralises
// the dial until the grip is released.

#include "core/util/xr_math.h"
#include <cmath>
#include <cstdint>

namespace bvr::b1r::dial {

constexpr int kSlots = 8;

// Slot index -> direction on the wheel (HUDRadial: 0 up, 1 down, 2 left, 3 right,
// 4 up-left, 5 up-right, 6 down-right, 7 down-left), degrees counter-clockwise from right.
inline float slot_angle_deg(int i) {
    static const float k[kSlots] = {90, 270, 180, 0, 135, 45, 315, 225};
    return k[i & 7];
}
// The stick vector the dial sends for a slot: the centre of the movie's wedge.
inline void slot_vector(int i, float* x, float* y) {
    const float r = slot_angle_deg(i) * 0.01745329252f;
    const bool diag = i >= 4;
    const float m = diag ? 0.70710678f : 1.0f;
    *x = diag ? (std::cos(r) >= 0 ? m : -m) : std::round(std::cos(r));
    *y = diag ? (std::sin(r) >= 0 ? m : -m) : std::round(std::sin(r));
}

// HUDRadial's UpdateRadialAxis, ported for the tests: which slot a stick (x, y) selects
// (-1 = none). Diagonals first (both axes past 0.2, both inside 0.9), then cardinals
// (past 0.901 with the other axis inside 0.5). The movie checks the weapon wheel's
// diagonals before its cardinals; the plasmid wheel interleaves them, same wedges.
inline int movie_slot(float x, float y) {
    const auto mid = [&] { return y > -0.9f && y < 0.9f && x > -0.9f && x < 0.9f; };
    const bool right = x > 0.2f && mid(), left = x < -0.2f && mid();
    const bool up = y > 0.2f && mid(), down = y < -0.2f && mid();
    const bool cx = x > -0.5f && x < 0.5f, cy = y > -0.5f && y < 0.5f;
    if (x == 0 && y == 0) return -1;
    if (right && up) return 5;
    if (right && down) return 6;
    if (left && down) return 7;
    if (left && up) return 4;
    if (cx && y > 0.901f) return 0;
    if (cx && y < -0.901f) return 1;
    if (x < -0.901f && cy) return 2;
    if (x > 0.901f && cy) return 3;
    return -1;
}

// The filled slot nearest to angleDeg among the first `filled` indices. `current`
// keeps its place unless another is nearer by more than hysteresisDeg.
inline int nearest_slot(float angleDeg, int filled, int current, float hysteresisDeg) {
    if (filled <= 0) return -1;
    if (filled > kSlots) filled = kSlots;
    auto dist = [&](int i) {
        float d = std::fabs(std::fmod(angleDeg - slot_angle_deg(i) + 540.0f, 360.0f) - 180.0f);
        return d;
    };
    int best = 0;
    for (int i = 1; i < filled; ++i)
        if (dist(i) < dist(best)) best = i;
    if (current >= 0 && current < filled && best != current && dist(current) <= dist(best) + hysteresisDeg)
        return current;
    return best;
}

struct Config {
    bool directionOnly = true; // any movement past deadM picks (Dishonored's default)
    float deadM = 0.002f;      // neutral radius, metres
    float travelM = 0.04f;     // analog mode: hand travel for full input
    bool snap = true;          // snap to the nearest filled slot (equal shares)
    float hysteresisDeg = 6.0f;
    bool entryTilt = false;    // take the head's pitch/roll at the opening
    bool entryYaw = false;     // take the head's yaw instead of facing the eye position
    // The movie ignores the stick until its open animation reaches frame 5
    // (weaponScrollCycling / abilityScrollCycling = false, ~210 ms at 24 fps) and only
    // acts when the stick CROSSES into a wedge. So the dial holds neutral this long after
    // the wheel shows; the step from neutral to the slot then counts as the crossing.
    unsigned armMs = 250;
};

struct Input {
    bool gripDown = false;   // this dial's grip (the bumper the pad composes from it)
    bool wheelOpen = false;  // the game's wheel for this hand is on screen (wheel_mirror)
    int filled = 0;          // its filled slots
    bool handOk = false;
    float hand[3] = {};      // grip, XR LOCAL
    bool headOk = false;
    float head[3] = {};      // centre eye, XR LOCAL
    float headQ[4] = {0, 0, 0, 1};
    uint64_t nowMs = 0;
};

struct Output {
    bool steer = false;      // replace the sticks with (x, y), the other stick at 0
    float x = 0, y = 0;
    int slot = -1;           // the slot (x, y) points at, -1 = none yet
};

struct Dial {
    bool held = false, valid = false, lost = false;
    float center[3] = {};
    float q[4] = {0, 0, 0, 1}; // frozen dial orientation (+Z toward the viewer)
    int slot = -1;
    bool shown = false;        // the game's wheel has been up during this press
    uint64_t shownMs = 0;

    void reset() { held = valid = lost = shown = false; slot = -1; }

    // Upright, facing the opening eye position; or the head's yaw/tilt when asked.
    bool seed(const Config& c, const Input& in) {
        for (int k = 0; k < 3; ++k) center[k] = in.hand[k];
        float yaw;
        if (c.entryYaw) {
            const float f0[3] = {0, 0, -1};
            float f[3];
            xrmath::quat_rotate(in.headQ[0], in.headQ[1], in.headQ[2], in.headQ[3], f0, f);
            if (f[0] * f[0] + f[2] * f[2] < 1e-4f) return false;
            yaw = std::atan2(-f[0], -f[2]);
        } else {
            const float dx = in.head[0] - center[0], dz = in.head[2] - center[2];
            if (dx * dx + dz * dz < 1e-4f) return false;
            yaw = std::atan2(dx, dz);
        }
        const float up[4] = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
        if (!c.entryTilt) {
            for (int k = 0; k < 4; ++k) q[k] = up[k];
            return true;
        }
        // The head's pitch and roll on top: head * inverse(head's upright yaw).
        const float f0[3] = {0, 0, -1};
        float f[3];
        xrmath::quat_rotate(in.headQ[0], in.headQ[1], in.headQ[2], in.headQ[3], f0, f);
        const float hy = std::atan2(-f[0], -f[2]);
        const float inv[4] = {0, -std::sin(hy * 0.5f), 0, std::cos(hy * 0.5f)};
        float tilt[4];
        xrmath::quat_mul(inv, in.headQ, tilt);
        xrmath::quat_mul(up, tilt, q);
        return true;
    }

    Output update(const Config& c, const Input& in) {
        Output out;
        if (!in.gripDown) { reset(); return out; }
        const bool tracked = in.handOk && in.headOk && std::isfinite(in.hand[0] + in.hand[1] + in.hand[2]) &&
                             std::isfinite(in.head[0] + in.head[1] + in.head[2]);
        if (!held) {
            held = true;
            lost = false;
            slot = -1;
            valid = tracked && seed(c, in);
        }
        if (!tracked) lost = true;         // never re-seeded mid-press
        if (!in.wheelOpen || in.filled < 2) return out;   // the game's wheel is not up: hands off
        out.steer = true;                  // the wheel is up: the hand alone owns the sticks
        if (!shown) { shown = true; shownMs = in.nowMs; }
        const bool armed = in.nowMs - shownMs >= c.armMs;
        if (!valid || lost) return out;    // ... and a lost hand means neutral, not a stale pick
        const float r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0};
        float right[3], up[3];
        xrmath::quat_rotate(q[0], q[1], q[2], q[3], r0, right);
        xrmath::quat_rotate(q[0], q[1], q[2], q[3], u0, up);
        float dx = 0, dy = 0;
        for (int k = 0; k < 3; ++k) {
            dx += (in.hand[k] - center[k]) * right[k];
            dy += (in.hand[k] - center[k]) * up[k];
        }
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > c.deadM) {
            if (c.snap) {
                slot = nearest_slot(std::atan2(dy, dx) * 57.2957795f, in.filled, slot, c.hysteresisDeg);
            } else {
                float x = dx / dist, y = dy / dist;
                if (!c.directionOnly) {
                    const float m = dist >= c.travelM ? 1.0f : (dist - c.deadM) / (c.travelM - c.deadM);
                    x *= m; y *= m;
                }
                slot = movie_slot(x, y);
                if (slot >= in.filled) slot = -1;
                if (armed) { out.x = x; out.y = y; out.slot = slot; }
                return out;
            }
        }
        // Inside the neutral radius the last pick holds: a weapon commits on release,
        // and drifting back toward the centre must not lose it.
        if (armed && slot >= 0 && slot < in.filled) {
            slot_vector(slot, &out.x, &out.y);
            out.slot = slot;
        }
        return out;
    }
};

} // namespace bvr::b1r::dial
