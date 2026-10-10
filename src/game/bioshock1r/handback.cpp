// s88: the hand-back - see handback.h.
#include "game/bioshock1r/handback.h"

#include "core/input/swing.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/hand_compose.h"
#include "game/bioshock1r/hands_state.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::handback {
namespace {

namespace hc = hand_compose;
using hands_state::State;

// Levers (Dishonored's defaults: melee by trigger and scripted on; swing, fire, reload off).
std::atomic<bool> g_melee{true}, g_swing{false}, g_scripted{true}, g_fire{false}, g_reload{false};
std::atomic<int> g_force{0}; // bit 0 left, bit 1 right: a test hand-back with no animation trigger

hc::Handoff g_hand[2];
int g_mask = 0;           // the hands the game owns right now (before release hysteresis)
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
    if (st == State::Reloading && g_reload.load()) {
        mask |= 3;
        g_reason = "reload";
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
    if (st != g_last)
        BVR_LOG("[handback] hands state %s (%ls) -> %s, owned mask %d", hands_state::to_string(st),
                handsActor ? hands_state::current_name(handsActor) : L"?", g_reason, mask);
    g_last = st;
    g_mask = mask;
    for (int h = 0; h < 2; ++h) {
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

bool active(int hand, uint64_t now) {
    const hc::Handoff& h = g_hand[hand ? 1 : 0];
    return h.game || h.weight(now) < 1.0f;
}

void handle_command(const char* rest) {
    char a[16] = {}, b[16] = {};
    int x = 0, y = 0;
    const int n = sscanf_s(rest, "%15s %15s", a, static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
    const bool on = n >= 2 && strcmp(b, "on") == 0;
    if (n >= 2 && strcmp(a, "melee") == 0) g_melee = on;
    else if (n >= 2 && strcmp(a, "swing") == 0) g_swing = on;
    else if (n >= 2 && strcmp(a, "scripted") == 0) g_scripted = on;
    else if (n >= 2 && strcmp(a, "fire") == 0) g_fire = on;
    else if (n >= 2 && strcmp(a, "reload") == 0) g_reload = on;
    else if (n >= 2 && strcmp(a, "force") == 0)
        g_force = strcmp(b, "l") == 0 ? 1 : strcmp(b, "r") == 0 ? 2 : strcmp(b, "both") == 0 ? 3 : 0;
    else if (strcmp(a, "blendms") == 0 && sscanf_s(rest + 7, "%d %d", &x, &y) == 2)
        for (hc::Handoff& h : g_hand) {
            h.inMs = static_cast<unsigned>(x);
            h.outMs = static_cast<unsigned>(y);
        }
    else if (strcmp(a, "release") == 0 && sscanf_s(rest + 7, "%d", &x) == 1)
        for (hc::Handoff& h : g_hand) h.releaseMs = static_cast<unsigned>(x);
    log_status(GetTickCount64());
}

void log_status(uint64_t now) {
    BVR_LOG("[handback] melee %s, swing %s, scripted %s, fire %s, reload %s, force %d | owned mask %d (%s) | "
            "weight L %.2f R %.2f | blend in %u out %u release %u ms",
            g_melee ? "on" : "off", g_swing ? "on" : "off", g_scripted ? "on" : "off", g_fire ? "on" : "off",
            g_reload ? "on" : "off", g_force.load(), g_mask, g_reason, g_hand[0].weight(now), g_hand[1].weight(now),
            g_hand[0].inMs, g_hand[0].outMs, g_hand[0].releaseMs);
}

} // namespace bvr::b1r::handback
