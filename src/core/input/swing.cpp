#include "core/input/swing.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::input::swing {
namespace {

// The published gate expires on the same 500 ms budget the pitch-kill gate
// uses: a publisher that stops (world unload, the camera drive going away)
// must close the gesture, not leave it latched on the last value it saw.
constexpr uint64_t kGateStaleMs = 500;

// Sample sanity. Below the floor a dt is noise amplification (a 1 mm jitter
// over 1 ms reads as 1 m/s); above the ceiling the two positions belong to
// different situations entirely - an alt-tab, a level load, a session hiccup -
// and differencing them fabricates a swing. Both cases re-seed instead.
constexpr double kMinDtMs = 4.0;
constexpr double kMaxDtMs = 100.0;

// Defaults. The FIRE THRESHOLD is no longer a guess: 3.6 m/s is the user's own
// in-headset call after the first live run (2026-07-31, verdict "it's perfect"),
// replacing the 2.2 that shipped to that run. It sits well above a walk, a body
// turn or a reach, so the gesture stays quiet during ordinary play, and a real
// swing clears it comfortably. The gesture is ON by default off the back of the
// same verdict.
// The pulse width follows the two synthetic pulses already proven against this
// game's per-tick edge detection - the ammo flick and the START pulse are both
// 150 ms - with a little taken off because a melee swing should not hold fire
// across two ticks; flat-measured, 120 ms does fire the weapon.
constexpr float kDefaultThreshold = 3.6f; // m/s, hand speed that counts as a swing
constexpr float kDefaultRearm = 1.0f;     // m/s, must fall below this to re-arm
constexpr uint32_t kDefaultCooldownMs = 300;
constexpr uint32_t kDefaultPulseMs = 120;
constexpr uint32_t kDefaultDelayMs = 0;

std::atomic<bool> g_enabled{true}; // ON by default - accepted in-headset, session 31
std::atomic<float> g_threshold{kDefaultThreshold};
std::atomic<float> g_rearm{kDefaultRearm};
std::atomic<uint32_t> g_cooldownMs{kDefaultCooldownMs};
std::atomic<uint32_t> g_pulseMs{kDefaultPulseMs};
std::atomic<uint32_t> g_delayMs{kDefaultDelayMs};
std::atomic<bool> g_headRel{true};
std::atomic<bool> g_log{false};

// Gate published by the game layer (wrench equipped + strict gameplay view).
std::atomic<bool> g_gate{false};
std::atomic<uint64_t> g_gateMs{0};

// Detector state. `armed` is the hysteresis latch: one swing accelerates and
// decelerates through the threshold, so without it a single motion fires twice.
std::atomic<bool> g_armed{true};
std::atomic<uint64_t> g_lastFireMs{0};
std::atomic<uint64_t> g_pulseStartMs{0};
std::atomic<uint64_t> g_pulseEndMs{0};
// One verdict per swing, not one per sample. A swing spends tens of samples
// above the threshold, so without this latch a single blocked swing wrote tens
// of identical log lines and counted as tens of blocks - measured flat on the
// first run: 106 "blocked" for ONE simulated swing. Cleared by the same
// slow-down that re-arms the detector, and set by a fire too, so the tail of a
// swing that DID fire stays quiet instead of reporting "not re-armed".
std::atomic<bool> g_blockLatched{false};

// Live sample history. Written only by the sample publisher (one thread).
bool g_haveLast = false;
bool g_lastHeadValid = false;
float g_lastHand[3] = {0.0f, 0.0f, 0.0f};
float g_lastHead[3] = {0.0f, 0.0f, 0.0f};
int64_t g_lastTimeNs = 0;
uint64_t g_lastSampleMs = 0;

// `swing sim <peak> [ms] [reps]`: half-sine speed humps pushed through the real
// decision path. This is the whole flat test - live samples need an XR session,
// so without it nothing about the detector could be checked without a headset.
// Each hump is followed by an equal gap at zero speed, which is what re-arms the
// detector; `reps` therefore makes the COOLDOWN testable, which one hump cannot
// do (a single hump crosses the threshold once, and the command seam polls at
// 1 Hz so two commands can never land inside a sub-second cooldown).
std::atomic<float> g_simPeak{0.0f};
std::atomic<uint64_t> g_simHumpMs{0};
std::atomic<uint32_t> g_simReps{1};
std::atomic<uint64_t> g_simStartMs{0};
std::atomic<uint64_t> g_simEndMs{0};

// Telemetry. Racy by construction (two possible decision-path callers) and
// deliberately so - these numbers are for the log and the overlay, never for a
// decision.
std::atomic<uint32_t> g_fires{0};
std::atomic<uint32_t> g_blocked{0};
std::atomic<uint32_t> g_samples{0};
std::atomic<float> g_lastSpeed{0.0f};
std::atomic<float> g_peakSpeed{0.0f}; // since the last `status`
uint64_t g_lastSpeedLogMs = 0;

enum Reject {
    kRejectNone = 0,
    kRejectDisabled,
    kRejectGate,
    kRejectWheel,
    kRejectRearm,
    kRejectCooldown,
};
std::atomic<int> g_lastReject{kRejectNone};

const char* reject_name(int r) {
    switch (r) {
        case kRejectDisabled: return "gesture off";
        case kRejectGate: return "gate closed (not the wrench, or not a gameplay view)";
        case kRejectWheel: return "a grip is held (the selection wheel is open)";
        case kRejectRearm: return "not re-armed yet (the hand never slowed down)";
        case kRejectCooldown: return "cooldown";
        default: return "-";
    }
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The re-arm level only means anything below the fire level. Enforced here
// rather than in the setters so the two can be typed in either order.
float effective_rearm() {
    float t = g_threshold.load(std::memory_order_relaxed);
    float r = g_rearm.load(std::memory_order_relaxed);
    float cap = t * 0.9f;
    return r < cap ? r : cap;
}

void note_block(int reason, float speed, const char* src) {
    g_lastReject.store(reason, std::memory_order_relaxed);
    if (g_blockLatched.exchange(true, std::memory_order_relaxed)) return; // same swing
    g_blocked.fetch_add(1, std::memory_order_relaxed);
    if (g_log.load(std::memory_order_relaxed))
        BVR_LOG("[swing] BLOCKED %.2f m/s (%s): %s", speed, src, reject_name(reason));
}

// The decision core. Both the live sample path and `sim` land here, which is
// what makes the flat test meaningful: it exercises the same thresholds, the
// same hysteresis, the same gates and the same pulse the headset will.
void on_speed(float speed, uint64_t now, const char* src) {
    g_samples.fetch_add(1, std::memory_order_relaxed);
    g_lastSpeed.store(speed, std::memory_order_relaxed);
    if (speed > g_peakSpeed.load(std::memory_order_relaxed))
        g_peakSpeed.store(speed, std::memory_order_relaxed);

    if (g_log.load(std::memory_order_relaxed) && now - g_lastSpeedLogMs >= 100) {
        g_lastSpeedLogMs = now;
        BVR_LOG("[swing] speed %.2f m/s (%s) armed=%d gate=%d", speed, src,
                g_armed.load(std::memory_order_relaxed) ? 1 : 0,
                g_gate.load(std::memory_order_relaxed) ? 1 : 0);
    }

    // Re-arm on the way down, unconditionally: a gate that closes mid-swing
    // must not leave the latch stuck and eat the NEXT swing too.
    if (speed < effective_rearm()) {
        g_armed.store(true, std::memory_order_relaxed);
        g_blockLatched.store(false, std::memory_order_relaxed);
    }

    // Everything below only matters once the player has actually swung. Staying
    // silent under the threshold is what keeps `log on` readable - one line per
    // real swing rather than one per frame.
    if (speed < g_threshold.load(std::memory_order_relaxed)) return;

    if (!g_enabled.load(std::memory_order_relaxed)) {
        note_block(kRejectDisabled, speed, src);
        return;
    }
    uint64_t stamp = g_gateMs.load(std::memory_order_relaxed);
    if (!g_gate.load(std::memory_order_relaxed) || !stamp || now - stamp > kGateStaleMs) {
        note_block(kRejectGate, speed, src);
        return;
    }
    // A grip holds a selection wheel open, and picking a weapon off a wheel
    // moves the hand fast. Same suppression the pitch kill and the turn
    // controls take, for the same reason.
    bool lb = false, rb = false;
    bvr::input::last_composed_bumpers(&lb, &rb);
    if (lb || rb) {
        note_block(kRejectWheel, speed, src);
        return;
    }
    if (!g_armed.load(std::memory_order_relaxed)) {
        note_block(kRejectRearm, speed, src);
        return;
    }
    uint64_t last = g_lastFireMs.load(std::memory_order_relaxed);
    if (last && now - last < g_cooldownMs.load(std::memory_order_relaxed)) {
        note_block(kRejectCooldown, speed, src);
        return;
    }

    g_armed.store(false, std::memory_order_relaxed);
    g_blockLatched.store(true, std::memory_order_relaxed); // the rest of this swing is silent
    g_lastFireMs.store(now, std::memory_order_relaxed);
    g_lastReject.store(kRejectNone, std::memory_order_relaxed);
    uint64_t start = now + g_delayMs.load(std::memory_order_relaxed);
    g_pulseStartMs.store(start, std::memory_order_relaxed);
    g_pulseEndMs.store(start + g_pulseMs.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    uint32_t n = g_fires.fetch_add(1, std::memory_order_relaxed) + 1;
    BVR_LOG("[swing] FIRE #%u at %.2f m/s (%s) - RT %u ms%s", n, speed, src,
            g_pulseMs.load(std::memory_order_relaxed),
            g_delayMs.load(std::memory_order_relaxed)
                ? " after the fire delay"
                : "");
}

} // namespace

void publish_gate(bool armed) {
    g_gate.store(armed, std::memory_order_relaxed);
    g_gateMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void publish_sample(const float handPos3[3], const float headPos3[3], bool handValid,
                    bool headValid, int64_t displayTimeNs) {
    uint64_t nowMs = GetTickCount64();
    if (!handValid || !handPos3) {
        g_haveLast = false;
        return;
    }

    if (g_haveLast) {
        // Prefer the runtime's own frame clock: it is the instant these poses
        // were predicted for, where the wall clock is only when we got around
        // to reading them.
        double dtMs = 0.0;
        if (displayTimeNs > 0 && g_lastTimeNs > 0)
            dtMs = static_cast<double>(displayTimeNs - g_lastTimeNs) / 1.0e6;
        else
            dtMs = static_cast<double>(nowMs - g_lastSampleMs);

        if (dtMs >= kMinDtMs && dtMs <= kMaxDtMs) {
            float d[3] = {handPos3[0] - g_lastHand[0], handPos3[1] - g_lastHand[1],
                          handPos3[2] - g_lastHand[2]};
            // Head-relative: a swing is the hand moving relative to YOU. Without
            // this, turning your body on the spot with the wrench out sweeps the
            // hand through the app space fast enough to read as a swing.
            if (g_headRel.load(std::memory_order_relaxed) && headValid && headPos3 &&
                g_lastHeadValid) {
                d[0] -= headPos3[0] - g_lastHead[0];
                d[1] -= headPos3[1] - g_lastHead[1];
                d[2] -= headPos3[2] - g_lastHead[2];
            }
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            on_speed(dist / static_cast<float>(dtMs / 1000.0), nowMs, "live");
        }
    }

    g_lastHand[0] = handPos3[0];
    g_lastHand[1] = handPos3[1];
    g_lastHand[2] = handPos3[2];
    g_lastHeadValid = headValid && headPos3 != nullptr;
    if (g_lastHeadValid) {
        g_lastHead[0] = headPos3[0];
        g_lastHead[1] = headPos3[1];
        g_lastHead[2] = headPos3[2];
    }
    g_lastTimeNs = displayTimeNs;
    g_lastSampleMs = nowMs;
    g_haveLast = true;
}

bool rt_pulse(uint64_t nowMs) {
    uint64_t end = g_pulseEndMs.load(std::memory_order_relaxed);
    if (!end || nowMs >= end) return false;
    return nowMs >= g_pulseStartMs.load(std::memory_order_relaxed);
}

void sim_tick(uint64_t nowMs) {
    uint64_t end = g_simEndMs.load(std::memory_order_relaxed);
    if (!end) return;
    uint64_t start = g_simStartMs.load(std::memory_order_relaxed);
    if (nowMs >= end) {
        g_simEndMs.store(0, std::memory_order_relaxed);
        BVR_LOG("[swing] sim window finished: %u swing(s) simulated, fires now %u, "
                "last block: %s",
                g_simReps.load(std::memory_order_relaxed),
                g_fires.load(std::memory_order_relaxed),
                reject_name(g_lastReject.load(std::memory_order_relaxed)));
        return;
    }
    if (nowMs < start) return;
    // Hump, then an equal gap at zero: the gap is what lets the next hump be a
    // NEW swing rather than a continuation of the previous one.
    const uint64_t hump = g_simHumpMs.load(std::memory_order_relaxed);
    if (!hump) return;
    const uint64_t phase = (nowMs - start) % (hump * 2);
    float speed = 0.0f;
    if (phase < hump)
        speed = g_simPeak.load(std::memory_order_relaxed) *
                sinf(3.14159265f * static_cast<float>(phase) / static_cast<float>(hump));
    on_speed(speed, nowMs, "sim");
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool on) {
    bool was = g_enabled.exchange(on, std::memory_order_relaxed);
    if (was == on) return;
    if (!on) {
        // Never leave a pulse open across the off edge.
        g_pulseEndMs.store(0, std::memory_order_relaxed);
        g_pulseStartMs.store(0, std::memory_order_relaxed);
    }
    BVR_LOG("[swing] swing-to-attack %s%s", on ? "ON" : "off",
            on ? " - a fast right-hand motion swings the wrench (the trigger still works)"
               : " - the trigger is the only way to swing");
}

float threshold_ms() { return g_threshold.load(std::memory_order_relaxed); }
void set_threshold_ms(float v) { g_threshold.store(clampf(v, 0.3f, 10.0f), std::memory_order_relaxed); }
float rearm_ms() { return g_rearm.load(std::memory_order_relaxed); }
void set_rearm_ms(float v) { g_rearm.store(clampf(v, 0.05f, 9.0f), std::memory_order_relaxed); }
uint32_t cooldown_ms() { return g_cooldownMs.load(std::memory_order_relaxed); }
void set_cooldown_ms(uint32_t v) { g_cooldownMs.store(clampu(v, 0, 2000), std::memory_order_relaxed); }
uint32_t pulse_ms() { return g_pulseMs.load(std::memory_order_relaxed); }
void set_pulse_ms(uint32_t v) { g_pulseMs.store(clampu(v, 20, 500), std::memory_order_relaxed); }
uint32_t delay_ms() { return g_delayMs.load(std::memory_order_relaxed); }
void set_delay_ms(uint32_t v) { g_delayMs.store(clampu(v, 0, 400), std::memory_order_relaxed); }
bool head_relative() { return g_headRel.load(std::memory_order_relaxed); }
void set_head_relative(bool on) { g_headRel.store(on, std::memory_order_relaxed); }

void handle_command(const char* rest) {
    char verb[16] = {};
    int consumed = 0;
    if (!rest || sscanf_s(rest, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[swing] vrinput swing on|off|status|threshold <m/s>|rearm <m/s>|"
                "cooldown <ms>|pulse <ms>|delay <ms>|rel on|off|log on|off|"
                "sim <peak> [humpMs] [reps]");
        return;
    }
    const char* p = rest + consumed;
    while (*p == ' ' || *p == '\t') ++p;

    if (strcmp(verb, "on") == 0) {
        set_enabled(true);
    } else if (strcmp(verb, "off") == 0) {
        set_enabled(false);
    } else if (strcmp(verb, "threshold") == 0) {
        float v = 0.0f;
        if (sscanf_s(p, "%f", &v) == 1) set_threshold_ms(v);
        BVR_LOG("[swing] fire threshold %.2f m/s (re-arm below %.2f)", threshold_ms(),
                effective_rearm());
    } else if (strcmp(verb, "rearm") == 0) {
        float v = 0.0f;
        if (sscanf_s(p, "%f", &v) == 1) set_rearm_ms(v);
        BVR_LOG("[swing] re-arm level %.2f m/s (effective %.2f - it is capped under the "
                "fire threshold)", rearm_ms(), effective_rearm());
    } else if (strcmp(verb, "cooldown") == 0) {
        unsigned v = 0;
        if (sscanf_s(p, "%u", &v) == 1) set_cooldown_ms(v);
        BVR_LOG("[swing] cooldown %u ms", cooldown_ms());
    } else if (strcmp(verb, "pulse") == 0) {
        unsigned v = 0;
        if (sscanf_s(p, "%u", &v) == 1) set_pulse_ms(v);
        BVR_LOG("[swing] trigger pulse %u ms", pulse_ms());
    } else if (strcmp(verb, "delay") == 0) {
        unsigned v = 0;
        if (sscanf_s(p, "%u", &v) == 1) set_delay_ms(v);
        BVR_LOG("[swing] fire delay %u ms (0 = fire the instant the swing starts)", delay_ms());
    } else if (strcmp(verb, "rel") == 0) {
        set_head_relative(strncmp(p, "off", 3) != 0);
        BVR_LOG("[swing] head-relative velocity %s - %s", head_relative() ? "ON" : "off",
                head_relative()
                    ? "the head's own motion is subtracted, so turning your body is not a swing"
                    : "raw app-space hand speed (turning your body can trigger it)");
    } else if (strcmp(verb, "log") == 0) {
        bool on = strncmp(p, "off", 3) != 0;
        g_log.store(on, std::memory_order_relaxed);
        BVR_LOG("[swing] log %s (speed at 10 Hz, plus every fire and every blocked swing)",
                on ? "ON" : "off");
    } else if (strcmp(verb, "sim") == 0) {
        float peak = 0.0f;
        unsigned ms = 0, reps = 0;
        int got = sscanf_s(p, "%f %u %u", &peak, &ms, &reps);
        if (got < 1) {
            BVR_LOG("[swing] usage: vrinput swing sim <peak m/s> [humpMs] [reps]");
            return;
        }
        if (ms == 0) ms = 200;
        ms = clampu(ms, 20, 2000);
        if (reps == 0) reps = 1;
        reps = clampu(reps, 1, 10);
        uint64_t now = GetTickCount64();
        g_simPeak.store(clampf(peak, 0.0f, 30.0f), std::memory_order_relaxed);
        g_simHumpMs.store(ms, std::memory_order_relaxed);
        g_simReps.store(reps, std::memory_order_relaxed);
        g_simStartMs.store(now, std::memory_order_relaxed);
        g_simEndMs.store(now + static_cast<uint64_t>(ms) * 2 * reps, std::memory_order_relaxed);
        BVR_LOG("[swing] sim armed: %u swing(s), half-sine to %.2f m/s over %u ms each "
                "(equal gap between), through the real decision path (fires so far %u)",
                reps, g_simPeak.load(std::memory_order_relaxed), ms,
                g_fires.load(std::memory_order_relaxed));
    } else { // status
        uint64_t now = GetTickCount64();
        uint64_t stamp = g_gateMs.load(std::memory_order_relaxed);
        uint64_t lastFire = g_lastFireMs.load(std::memory_order_relaxed);
        float peak = g_peakSpeed.exchange(0.0f, std::memory_order_relaxed);
        BVR_LOG("[swing] %s | gate %s (published %llu ms ago) | armed=%d | threshold %.2f "
                "re-arm %.2f m/s | cooldown %u pulse %u delay %u ms | headRel %d",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_gate.load(std::memory_order_relaxed) &&
                        stamp && now - stamp <= kGateStaleMs
                    ? "OPEN"
                    : "closed",
                static_cast<unsigned long long>(stamp ? now - stamp : 0),
                g_armed.load(std::memory_order_relaxed) ? 1 : 0, threshold_ms(),
                effective_rearm(), cooldown_ms(), pulse_ms(), delay_ms(),
                head_relative() ? 1 : 0);
        BVR_LOG("[swing] samples %u | fires %u (last %llu ms ago) | blocked %u (last: %s) | "
                "last %.2f m/s | PEAK SINCE LAST STATUS %.2f m/s <- tune the threshold from this",
                g_samples.load(std::memory_order_relaxed),
                g_fires.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(lastFire ? now - lastFire : 0),
                g_blocked.load(std::memory_order_relaxed),
                reject_name(g_lastReject.load(std::memory_order_relaxed)),
                g_lastSpeed.load(std::memory_order_relaxed), peak);
    }
}

void draw_debug_ui() {
    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Swing the wrench to attack", &on)) set_enabled(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A fast right-hand motion swings the wrench, in addition to the\n"
                          "trigger. Only while the wrench is equipped. It changes WHEN the\n"
                          "attack fires, not where it lands - the game aims melee itself.");

    float t = g_threshold.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Swing speed needed (m/s)", &t, 0.5f, 8.0f, "%.2f"))
        set_threshold_ms(t);
    int cd = static_cast<int>(g_cooldownMs.load(std::memory_order_relaxed));
    if (ImGui::SliderInt("Swing cooldown (ms)", &cd, 0, 1000))
        set_cooldown_ms(static_cast<uint32_t>(cd));
    int dl = static_cast<int>(g_delayMs.load(std::memory_order_relaxed));
    if (ImGui::SliderInt("Fire delay (ms)", &dl, 0, 300))
        set_delay_ms(static_cast<uint32_t>(dl));

    uint64_t now = GetTickCount64();
    uint64_t stamp = g_gateMs.load(std::memory_order_relaxed);
    bool gateOpen = g_gate.load(std::memory_order_relaxed) && stamp &&
                    now - stamp <= kGateStaleMs;
    ImGui::Text("swing: gate %s | armed %d | last %.2f m/s | fires %u | blocked %u (%s)",
                gateOpen ? "open (wrench)" : "closed",
                g_armed.load(std::memory_order_relaxed) ? 1 : 0,
                g_lastSpeed.load(std::memory_order_relaxed),
                g_fires.load(std::memory_order_relaxed),
                g_blocked.load(std::memory_order_relaxed),
                reject_name(g_lastReject.load(std::memory_order_relaxed)));
}

} // namespace bvr::input::swing
