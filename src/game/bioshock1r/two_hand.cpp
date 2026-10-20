#include "two_hand.h"
#include "two_hand_math.h"
#include "hand_compose.h"
#include "manual_reload.h"
#include "patterns.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cwchar>
#include <limits>

namespace bvr::b1r::two_hand {
namespace {
using S = menu::Setting;

std::atomic<bool> g_on{true};
std::atomic<float> g_grabCm{12.0f};       // BRVR TwoHandGrabRadius; release = grab + 8 (BRVR 20)
constexpr float kReleaseExtraCm = 8.0f;
constexpr float kBuzzAmp = 0.35f;         // BRVR hapticGrabAmp - the hand-over grab's buzz
constexpr int kBuzzMs = 60;               // hapticGrabMs
constexpr uint64_t kBuzzEveryMs = 50;     // hapticGrabEveryMs
constexpr uint64_t kStaleMs = 250;        // no compose this long: no grab point
constexpr float kInMs = 120.0f, kOutMs = 150.0f;
std::atomic<bool> g_shots{true};
std::atomic<float> g_shotAmp{1.0f};       // BRVR HapticRecoilAmp
constexpr float kOffHandShare = 0.6f;     // the front hand's share of a kick while two-handing

// Game thread -> XInput thread: this frame's numbers, as one packed snapshot.
struct Snap {
    float distCm = -1.0f, slideCm = -1.0f;
    float toward[3] = {};
    bool paused = false;
    uint64_t ms = 0;
};
std::atomic<uint32_t> g_snapSeq{0};
Snap g_snap;
void put_snap(const Snap& s) {
    g_snapSeq.fetch_add(1, std::memory_order_acq_rel);
    g_snap = s;
    g_snapSeq.fetch_add(1, std::memory_order_acq_rel);
}
Snap get_snap() {
    for (int i = 0; i < 4; ++i) {
        const uint32_t a = g_snapSeq.load(std::memory_order_acquire);
        if (a & 1) continue;
        const Snap s = g_snap;
        if (g_snapSeq.load(std::memory_order_acquire) == a) return s;
    }
    return {};
}

// XInput thread.
Grip g_grip;
bool g_suppressUntilUp = false;
uint64_t g_lastBuzzMs = 0, g_lastProbeMs = 0;
std::atomic<bool> g_held{false};
std::atomic<int> g_grabs{0};
std::atomic<float> g_liveDistCm{-1.0f};

// Game thread.
const void* g_classOf = nullptr;
bool g_classTwoHanded = false;
float g_weight = 0.0f;
bool g_weightHeld = false;
uint64_t g_weightMs = 0;
float g_weightFrom = 0.0f;
Kick g_kick;

bool two_handed_class(const wchar_t* c) {
    if (!c) return false;
    static const wchar_t* const kGuns[] = {L"MachineGun", L"Shotgun", L"GrenadeLauncher", L"ChemicalThrower",
                                           L"Crossbow"};
    for (const wchar_t* g : kGuns)
        if (wcscmp(c, g) == 0) return true;
    return false;
}
} // namespace

bool applicable(const void* holdable) {
    if (!g_on.load(std::memory_order_relaxed) || !holdable) return false;
    if (holdable != g_classOf) {
        g_classOf = holdable;
        const wchar_t* c = patterns::object_class_name(holdable);
        g_classTwoHanded = two_handed_class(c);
        BVR_LOG("[twohand] %S %s", c ? c : L"(no class)",
                g_classTwoHanded ? "is two-handed: the grip near where the game has the other hand takes hold"
                                 : "is one-handed");
    }
    return g_classTwoHanded;
}

bool melee(const void* holdable) {
    static const void* s_of = nullptr;
    static bool s_melee = false;
    if (holdable != s_of) {
        s_of = holdable;
        const wchar_t* c = holdable ? patterns::object_class_name(holdable) : nullptr;
        s_melee = c && wcscmp(c, L"Wrench") == 0;
    }
    return s_melee;
}

void publish(float distCm, float slideCm, bool paused, const float towardCm[3], uint64_t nowMs) {
    Snap s;
    s.distCm = distCm;
    s.slideCm = slideCm;
    for (int i = 0; i < 3; ++i) s.toward[i] = towardCm[i];
    s.paused = paused;
    s.ms = nowMs;
    put_snap(s);
}
void publish_none(uint64_t nowMs) {
    Snap s;
    s.ms = nowMs;
    put_snap(s);
}

float hold_weight(uint64_t nowMs) {
    const bool h = g_held.load(std::memory_order_relaxed);
    if (h != g_weightHeld) {
        g_weightHeld = h;
        g_weightFrom = g_weight;
        g_weightMs = nowMs;
    }
    const float span = h ? kInMs : kOutMs;
    const float t = std::fmin(1.0f, static_cast<float>(nowMs - g_weightMs) / span);
    const float e = hand_compose::smootherstep(t);
    g_weight = g_weightFrom + ((h ? 1.0f : 0.0f) - g_weightFrom) * e;
    return g_weight;
}

void shot(int gunPhys, int offPhys, bool shotEvent, uint64_t nowMs) {
    if (!g_shots.load(std::memory_order_relaxed)) return;
    const KickConfig c;
    if (!g_kick.update(shotEvent, nowMs, c)) return;
    const float amp = g_shotAmp.load(std::memory_order_relaxed);
    bvr::vr::pulse_hand(gunPhys, amp, c.pulseMs);
    const bool both = g_held.load(std::memory_order_relaxed);
    if (both) bvr::vr::pulse_hand(offPhys, amp * kOffHandShare, c.pulseMs);
    // A count, not a line per shot: at most one line a second while shooting.
    static int s_kicks = 0, s_both = 0;
    static uint64_t s_logMs = 0;
    ++s_kicks;
    if (both) ++s_both;
    if (nowMs - s_logMs >= 1000) {
        s_logMs = nowMs;
        BVR_LOG("[twohand] shot kicks: %d (%d with the front hand too)", s_kicks, s_both);
    }
}

float reach(bool busy) {
    if (g_held.load(std::memory_order_relaxed)) return 0.0f;
    const uint64_t now = GetTickCount64();
    const Snap s = get_snap();
    if (!s.ms || now - s.ms >= kStaleMs || busy || s.paused || !(s.distCm >= 0.0f)) return -1.0f;
    const float r = g_grabCm.load(std::memory_order_relaxed);
    return r > 0.0f ? s.distCm / r : -1.0f;
}

bool pad(bvr::input::PadOverride* pad, int gunPhys, bool busy, bool* changed, bool locked) {
    constexpr uint16_t kLB = 0x0100; // the hand without the gun is always the plasmid role: LB
    // A squeeze another grab owns reads as up: nothing is grabbed, and it is not stripped here.
    const bool grip = !locked && (pad->buttons & kLB) != 0;
    const uint64_t now = GetTickCount64();
    const Snap s = get_snap();
    const bool fresh = s.ms && now - s.ms < kStaleMs && !busy;
    GripInput in;
    in.grip = grip;
    if (fresh && s.paused) {
        in.distCm = -1.0f; // a clip owns the hands: a hold waits it out, nothing new is grabbed
        in.slideCm = 0.0f;
    } else if (fresh) {
        in.distCm = s.distCm;
        in.slideCm = s.slideCm;
    }
    GripConfig c;
    c.grabCm = g_grabCm.load(std::memory_order_relaxed);
    c.releaseCm = c.grabCm + kReleaseExtraCm;
    const GripOutput o = g_grip.update(in, c);
    g_held.store(o.held, std::memory_order_relaxed);
    g_liveDistCm.store(fresh && !s.paused ? s.distCm : -1.0f, std::memory_order_relaxed);
    const int freePhys = 1 - gunPhys;
    if (o.grabbed) {
        g_grabs.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("[twohand] the %s hand took hold of the gun %.1f cm from where the game has it (grab %.0f, "
                "release %.0f along the gun)",
                freePhys ? "right" : "left", s.distCm, c.grabCm, c.releaseCm);
    }
    if (o.letGo)
        BVR_LOG("[twohand] let go - %s", o.slidOff ? "the hand slid off along the gun" : "the grip came up");
    // The grip that grabbed, or that let go by sliding off, stays the grip's until it comes up:
    // a bumper appearing mid-squeeze would raise the plasmid wheel.
    if (o.eligible && grip) g_suppressUntilUp = true;
    if (!grip) g_suppressUntilUp = false;
    const bool claimed = o.eligible || g_suppressUntilUp;
    if (claimed && grip) {
        pad->buttons = static_cast<uint16_t>(pad->buttons & ~kLB);
        *changed = true;
    }
    // The buzz means "you could grab here", so it stops once you have (BRVR).
    if (o.eligible && !o.held && now - g_lastBuzzMs >= kBuzzEveryMs) {
        g_lastBuzzMs = now;
        bvr::vr::pulse_hand(freePhys, kBuzzAmp, kBuzzMs);
    }
    // BRVR's probe, twice a second while the hand is near the grab point.
    if (fresh && !s.paused && s.distCm >= 0.0f && s.distCm < 25.0f && now - g_lastProbeMs >= 500) {
        g_lastProbeMs = now;
        BVR_LOG("[twohand] off hand %.1f cm from where the game has it on the gun (fwd %+.1f right %+.1f up %+.1f) "
                "(grab %.0f) %s%s",
                s.distCm, s.toward[0], s.toward[1], s.toward[2], c.grabCm, o.eligible ? "ELIGIBLE" : "far",
                o.held ? " - HOLDING" : "");
        if (o.held)
            BVR_LOG("[twohand]   holding: slid %.1f cm along the gun (lets go past %.0f)", s.slideCm, c.releaseCm);
    }
    return claimed;
}

bool held() { return g_held.load(std::memory_order_relaxed); }

float menu_read(menu::Setting id) {
    switch (id) {
        case S::TwoHandEnabled: return g_on.load() ? 1.0f : 0.0f;
        case S::TwoHandDistance: return g_grabCm.load();
        case S::ShotHaptics: return g_shots.load() ? 1.0f : 0.0f;
        case S::ShotHapticStrength: return g_shotAmp.load();
        default: return manual_reload::menu_read(id);
    }
}

bool menu_write(menu::Setting id, float value) {
    switch (id) {
        case S::TwoHandEnabled:
            if (g_on.exchange(value != 0) != (value != 0))
                BVR_LOG("[twohand] two-handed grip %s", value != 0 ? "ON" : "off");
            return true;
        case S::TwoHandDistance: g_grabCm.store(value); return true;
        case S::ShotHaptics:
            if (g_shots.exchange(value != 0) != (value != 0))
                BVR_LOG("[twohand] gunshot haptics %s", value != 0 ? "ON" : "off");
            return true;
        case S::ShotHapticStrength: g_shotAmp.store(value); return true;
        default: return manual_reload::menu_write(id, value);
    }
}

void draw_status() {
    const float d = g_liveDistCm.load(std::memory_order_relaxed);
    if (g_held.load(std::memory_order_relaxed))
        ImGui::Text("Two hands on the gun (%d grab(s) this session).", g_grabs.load(std::memory_order_relaxed));
    else if (d >= 0.0f)
        ImGui::Text("Your other hand is %.0f cm from where the gun wants it.", d);
    else
        ImGui::TextDisabled("Two-handed grip: hold a two-handed gun to use it.");
}

} // namespace bvr::b1r::two_hand
