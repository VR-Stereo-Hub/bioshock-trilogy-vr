// s88: the hand-back - see handback.h.
#include "game/bioshock1r/handback.h"

#include "core/input/swing.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hand_compose.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/manual_reload.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::handback {
namespace {

namespace hc = hand_compose;
using hands_state::State;

// Levers (Dishonored's defaults: melee by trigger and scripted on; swing and fire off). The
// reload plays at the gun (handback.h); the gun's own motion in the clip is off, as the sibling mod ships it.
std::atomic<bool> g_melee{true}, g_swing{false}, g_scripted{true}, g_fire{false};
std::atomic<int> g_reload{static_cast<int>(Place::AtGun)};
std::atomic<float> g_gunMotion{0.0f};
// 2026-10-24: a reload played at the gun whose clip works the gun's own parts (the launcher's breech and box, the
// crossbow's lever and box) layers ALL of the clip's motion of the gun hand, whatever g_gunMotion says. Latched while
// the gun hand's at-gun weight is out, so the blend back to the controller is not cut short.
std::atomic<bool> g_fullGunMotion{false};
// The free hand joins the gun while the clip has its palm within this of the gun hand's (cm,
// drawn size). A guess until a reload's log line ("came within") measures BS1's clips.
std::atomic<float> g_joinCm{35.0f};
// Recoil: how much of a gun's firing clip moves the gun hand on its controller (handback.h).
std::atomic<float> g_recoil{1.0f};
std::atomic<int> g_force{0}; // bit 0 left, bit 1 right: a test hand-back with no animation trigger

hc::Handoff g_hand[2];
hc::Handoff g_carry[2]; // "game" = carried by the weapon hand (the free hand of an at-gun animation)
hc::Handoff g_gun[2];   // "game" = the clip's own motion on the controller (the weapon hand)
// "game" = a gun's firing clip moves the weapon hand (recoil). In at once: the kick is the first
// few frames of the clip, and the motion is measured from the pose just before the shot, so it
// starts from zero and cannot jump. Out over the usual 350 ms after the usual release.
hc::Handoff make_kick() {
    hc::Handoff h;
    h.inMs = 0;
    return h;
}
hc::Handoff g_kick[2] = {make_kick(), make_kick()};
int g_mask = 0;           // the hands the game owns right now (before release hysteresis)
int g_atGun = 0;          // an animation playing at the gun: bit per hand (free hand carried, gun hand moved)
State g_last = State::Unknown;
bool g_attackTrigger = false; // the current melee attack's verdict, latched at state entry
uint64_t g_lastPulseMs = 0;   // the physical swing's RT pulse, last seen open
const char* g_reason = "none";
float g_lastWeight[2] = {1.0f, 1.0f};

bool is_attack(State s) { return s == State::Firing || s == State::PostFiring; }

} // namespace

void tick(const void* handsActor, int weaponHand, uint64_t now) {
    const int wh = weaponHand == 0 ? 0 : 1;
    // The physical swing pulse: sampled every frame so a state entry can be dated against it.
    if (bvr::input::swing::rt_pulse(now)) g_lastPulseMs = now;
    // The state machine's offsets are derived once against the live actor (hands_state.h);
    // mode 4 never ran mode 3's locate call, so the hand-back does it itself.
    if (handsActor && !hands_state::located()) hands_state::locate(handsActor);
    const State st = handsActor ? hands_state::current(handsActor) : State::Unknown;
    const bool melee = aim::weapon_key_is("Wrench");

    // VR-220: classify each melee attack ONCE, at its entry. SWING if the swing's pulse was
    // open, or closed within 80 ms (one pad-poll gap and the state's entry delay, doubled);
    // else TRIGGER.
    if (is_attack(st) && !is_attack(g_last) && melee) {
        const uint64_t since = g_lastPulseMs ? now - g_lastPulseMs : ~0ull;
        g_attackTrigger = !(since <= 80);
        const bool takes = g_attackTrigger ? g_melee.load() : g_swing.load();
        BVR_LOG("[handback] melee attack source=%s -> hand-back %s (state %ls, swing pulse %s)",
                g_attackTrigger ? "TRIGGER" : "SWING", takes ? "ON" : "off",
                hands_state::current_name(handsActor),
                g_lastPulseMs ? (since <= 80 ? "open / just closed" : "long closed") : "never");
    }

    int mask = 0;
    g_reason = "none";
    if (is_attack(st) && melee) {
        if (g_attackTrigger ? g_melee.load() : g_swing.load()) {
            mask |= 1 << wh;
            g_reason = g_attackTrigger ? "melee attack (trigger)" : "melee attack (swing)";
        }
    } else if (is_attack(st) && g_fire.load()) {
        mask |= 1 << wh;
        g_reason = "fire";
    }
    // Recoil: a GUN's firing states (WeaponFiring and friends, never an ability cast) on a weapon
    // that may animate (its profile's animOn - 0 for the wrench) move the gun hand on its
    // controller by the clip's kick. The state leaves at the TOP of the kick (ENGINE_NOTES s68);
    // the release window carries the clip's return.
    const bool gunFire = is_attack(st) && !melee && handsActor &&
                         wcsstr(hands_state::current_name(handsActor), L"Weapon") && bones::anim_allowed();
    int kick = gunFire && !(mask & (1 << wh)) && g_recoil.load(std::memory_order_relaxed) > 0.0f ? 1 << wh : 0;
    if (kick && strcmp(g_reason, "none") == 0) g_reason = "recoil";
    int atGun = 0;
    // 2026-10-22 late (the tester: the crossbow's lever cock after a shot "doesn't animate the left hand at all"):
    // the crossbow re-cocks itself in its Fire clip (the lever, the string); the free hand is carried where that
    // clip has it, at the gun, for the clip's 2 s - until the lever is the player's (MANUAL_RELOAD_PLAN.md step 4).
    if (is_attack(st) && !melee && handsActor && !manual_reload::handles_reloads()) {
        void* w = nullptr;
        const wchar_t* cls = hands::current_holdable(&w) && w ? patterns::object_class_name(w) : nullptr;
        if (cls && !_wcsicmp(cls, L"Crossbow")) {
            atGun |= 1 << (1 - wh);
            if (strcmp(g_reason, "none") == 0 || strcmp(g_reason, "fire") == 0) g_reason = "fire (the crossbow's free hand on the lever, at the gun)";
        }
    }
    // 2026-10-20: a MANUAL reload (manual_reload.h) drives the clip from the player's hands, so the
    // hands stay on their controllers - the clip only shapes the fingers.
    // 2026-10-21 (00:45 run: the shotgun emptied, "the left wrist ends up near my left waist"): a reload of a gun
    // that reloads by hand is the hands' from its first frame. active() comes a frame late (it begins in the gun
    // hand's tick) and drops between the clips of an ammunition switch's restarted reloads - and each time the
    // free hand began to be carried "at the gun", to where the clip has it: the pouch.
    if (st == State::Reloading && (manual_reload::active() || manual_reload::handles_reloads())) {
        g_reason = "manual reload (the hands stay on the controllers)";
    } else if (st == State::Reloading) {
        Place p = static_cast<Place>(g_reload.load());
        // 2026-10-22 (the tester, the launcher with the manual reload off: the reload "doesn't work correctly" -
        // its clip turns the chamber, slides the breech open and takes the ammo box off the gun, and with the gun
        // hand on the controller and the free hand carried at the gun those parts move with no hand on them):
        // the grenade launcher's reload is the game's for BOTH hands, in view, until it has a program of its own
        // (MANUAL_RELOAD_PLAN.md).
        // 2026-10-24 (the tester, the launcher with the manual reload off: "put both of my arms far forward"): in view,
        // a clip's hands are placed against the camera as the flat game frames them - both arms out in front. Both
        // hands stay AT THE GUN instead: the gun hand on its controller with the clip's own motion of it layered on
        // in full (handback::gun_motion - the breech and the box move with the hand that works them), the free hand
        // carried where the clip has it against the gun hand. The crossbow's reload clip the same.
        if (p == Place::AtGun && handsActor) {
            void* w = nullptr;
            const wchar_t* cls = hands::current_holdable(&w) && w ? patterns::object_class_name(w) : nullptr;
            if (cls && (!_wcsicmp(cls, L"GrenadeLauncher") || !_wcsicmp(cls, L"Crossbow"))) g_fullGunMotion = true;
        }
        if (p == Place::View) {
            mask |= 3;
            g_reason = "reload (in view)";
        } else if (p == Place::AtGun) {
            atGun = 3;
            g_reason = "reload (at the gun)";
        }
    }
    if (st == State::Scripted && g_scripted.load()) {
        mask |= 3;
        g_reason = "scripted hand animation";
    }
    const int force = g_force.load(std::memory_order_relaxed);
    if (force) {
        mask |= force;
        g_reason = "forced (vrhands handback force)";
    }
    // A hand the game owns in view is not also carried: view wins.
    atGun &= ~mask;
    if (st != g_last)
        BVR_LOG("[handback] hands state %s (%ls) -> %s, owned mask %d, at-gun mask %d", hands_state::to_string(st),
                handsActor ? hands_state::current_name(handsActor) : L"?", g_reason, mask, atGun);
    g_last = st;
    g_mask = mask;
    g_atGun = atGun;
    for (int h = 0; h < 2; ++h) {
        const bool atGunHere = (atGun >> h) & 1;
        g_carry[h].update(atGunHere && h != wh, now);
        g_gun[h].update(atGunHere && h == wh, now);
        if (h == wh && !atGunHere && g_gun[h].weight(now) >= 1.0f) g_fullGunMotion = false;
        g_kick[h].update((kick >> h) & 1, now);
        g_hand[h].update((mask >> h) & 1, now);
        const float w = g_hand[h].weight(now);
        // One line at each end of a blend, so a run's log shows every hand-back's shape.
        if ((w <= 0.0f || w >= 1.0f) && w != g_lastWeight[h])
            BVR_LOG("[handback] %s hand at %s (weight %.2f)", h ? "right" : "left",
                    w <= 0.0f ? "the game's clip" : "the controller", w);
        g_lastWeight[h] = w;
    }
}

float weight(int hand, uint64_t now) { return g_hand[hand ? 1 : 0].weight(now); }

float carry(int hand, uint64_t now) { return 1.0f - g_carry[hand ? 1 : 0].weight(now); }

float gun_motion(int hand, uint64_t now) {
    const int h = hand ? 1 : 0;
    const float reload =
        (1.0f - g_gun[h].weight(now)) * (g_fullGunMotion ? 1.0f : g_gunMotion.load(std::memory_order_relaxed));
    const float recoil = (1.0f - g_kick[h].weight(now)) * g_recoil.load(std::memory_order_relaxed);
    return reload > recoil ? reload : recoil;
}

float recoil_amount() { return g_recoil.load(std::memory_order_relaxed); }
void set_recoil_amount(float v) { g_recoil.store(v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v, std::memory_order_relaxed); }

float join_gate(float clipCm) {
    const float nearCm = g_joinCm.load(std::memory_order_relaxed);
    return 1.0f - hc::smootherstep((clipCm - nearCm) / 20.0f);
}

float tracked(int hand, uint64_t now) {
    return weight(hand, now) * (1.0f - carry(hand, now)) * (1.0f - gun_motion(hand, now));
}

bool active(int hand, uint64_t now) {
    const hc::Handoff& h = g_hand[hand ? 1 : 0];
    return h.game || h.weight(now) < 1.0f || carry(hand, now) > 0.0f || gun_motion(hand, now) > 0.0f;
}

bool clip_owns(int hand, uint64_t now) {
    const int i = hand ? 1 : 0;
    const hc::Handoff& h = g_hand[i];
    const float reload =
        (1.0f - g_gun[i].weight(now)) * (g_fullGunMotion ? 1.0f : g_gunMotion.load(std::memory_order_relaxed));
    return h.game || h.weight(now) < 1.0f || carry(hand, now) > 0.0f || reload > 0.0f;
}

int reload_place() { return g_reload.load(std::memory_order_relaxed); }
void set_reload_place(int place) { g_reload.store(place < 0 ? 0 : place > 2 ? 2 : place, std::memory_order_relaxed); }
float gun_motion_amount() { return g_gunMotion.load(std::memory_order_relaxed); }
void set_gun_motion_amount(float v) {
    g_gunMotion.store(v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v, std::memory_order_relaxed);
}
float join_cm() { return g_joinCm.load(std::memory_order_relaxed); }
void set_join_cm(float cm) { g_joinCm.store(cm < 0.0f ? 0.0f : cm > 200.0f ? 200.0f : cm, std::memory_order_relaxed); }

void handle_command(const char* rest) {
    char a[16] = {}, b[16] = {};
    int x = 0, y = 0;
    const int n = sscanf_s(rest, "%15s %15s", a, static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
    const bool on = n >= 2 && strcmp(b, "on") == 0;
    if (n >= 2 && strcmp(a, "melee") == 0) g_melee = on;
    else if (n >= 2 && strcmp(a, "swing") == 0) g_swing = on;
    else if (n >= 2 && strcmp(a, "scripted") == 0) g_scripted = on;
    else if (n >= 2 && strcmp(a, "fire") == 0) g_fire = on;
    else if (n >= 2 && strcmp(a, "reload") == 0)
        set_reload_place(strcmp(b, "view") == 0 ? 2 : (on || strcmp(b, "hand") == 0) ? 1 : 0);
    else if (n >= 2 && strcmp(a, "gunmotion") == 0) set_gun_motion_amount(static_cast<float>(atof(b)));
    else if (n >= 2 && strcmp(a, "join") == 0) set_join_cm(static_cast<float>(atof(b)));
    else if (n >= 2 && strcmp(a, "recoil") == 0) set_recoil_amount(static_cast<float>(atof(b)));
    else if (n >= 2 && strcmp(a, "force") == 0)
        g_force = strcmp(b, "l") == 0 ? 1 : strcmp(b, "r") == 0 ? 2 : strcmp(b, "both") == 0 ? 3 : 0;
    else if (strcmp(a, "blendms") == 0 && sscanf_s(rest + 7, "%d %d", &x, &y) == 2)
        for (hc::Handoff* set : {g_hand, g_carry, g_gun, g_kick})
            for (int h = 0; h < 2; ++h) {
                if (set != g_kick) set[h].inMs = static_cast<unsigned>(x); // the kick is always in at once
                set[h].outMs = static_cast<unsigned>(y);
            }
    else if (strcmp(a, "release") == 0 && sscanf_s(rest + 7, "%d", &x) == 1)
        for (hc::Handoff* set : {g_hand, g_carry, g_gun, g_kick})
            for (int h = 0; h < 2; ++h) set[h].releaseMs = static_cast<unsigned>(x);
    log_status(GetTickCount64());
}

void log_status(uint64_t now) {
    static const char* kPlace[] = {"off", "at the gun", "in view"};
    BVR_LOG("[handback] recoil %.2f, melee %s, swing %s, scripted %s, fire %s, reload %s (gun motion %.2f, join %.0f cm), "
            "force %d | owned mask %d, at-gun mask %d (%s) | weight L %.2f R %.2f, carry L %.2f R %.2f | "
            "blend in %u out %u release %u ms",
            recoil_amount(), g_melee ? "on" : "off", g_swing ? "on" : "off", g_scripted ? "on" : "off", g_fire ? "on" : "off",
            kPlace[reload_place()], gun_motion_amount(), join_cm(), g_force.load(), g_mask, g_atGun, g_reason,
            g_hand[0].weight(now), g_hand[1].weight(now), carry(0, now), carry(1, now), g_hand[0].inMs,
            g_hand[0].outMs, g_hand[0].releaseMs);
}

} // namespace bvr::b1r::handback
