#include "core/input/xinput_bridge.h"

#include "core/hooks/pattern_scan.h"
#include "core/input/swing.h"
#include "core/util/log.h"

#include <windows.h>
#include <Xinput.h> // layout only (XINPUT_STATE); no XInput functions are called

#include <imgui.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace bvr::input {
namespace {

static_assert(sizeof(Gamepad) == sizeof(XINPUT_GAMEPAD),
              "Gamepad must mirror XINPUT_GAMEPAD");

// The proxy's seam signature (xinput_proxy.cpp): called after the real
// XInputGetState/GetStateEx with the real result; may rewrite state + result.
using PostGetStateHook = void(WINAPI*)(DWORD userIndex, void* state, DWORD* result);
using SetPostGetStateFn = void(WINAPI*)(PostGetStateHook hook);

std::atomic<bool> g_registered{false};
std::atomic<bool> g_enabled{false};
std::atomic<float> g_deadzone{0.10f};

// Everything the composer touches lives under one mutex: the hook can run on
// any game thread while the render thread publishes XR state and the game
// thread applies test commands. Copies are 12 bytes - contention is trivial.
std::mutex g_mutex;
Gamepad g_xrPad{};
bool g_xrActive = false;
uint64_t g_xrLastMs = 0;          // publish staleness guard (session teardown
                                  // stops publishing entirely - expire the slot)
constexpr uint64_t kXrStaleMs = 500;

// Stick-pitch kill (session 19): while the VR camera drives a real gameplay
// view, the composed right-stick Y is zeroed - the HMD owns pitch there, and
// a stick-pitched body drags the viewmodel with it and aims the wrench's
// melee phantom at the body pitch instead of the hand. The game layer
// publishes the gate once per CalcView; the slot self-expires like the XR pad
// so a stopped publisher (world unload, drive off) fails open to stock
// behavior. Menus/cutscenes publish false and keep stick pitch.
std::atomic<bool> g_pitchKill{true};
std::atomic<bool> g_vrGameplay{false};
std::atomic<uint64_t> g_vrGameplayLastMs{0};
constexpr uint64_t kVrGameplayStaleMs = 500;

// ---- Session 30: the pitch SERVO, and why zeroing the stick was not enough ---
//
// Killing the stick's pitch stops it fighting the HMD, but it also means the
// engine's OWN view pitch can never change again - and camera.cpp writes
// rot->pitch ABSOLUTELY from the head, so nothing ever reads the engine's value
// either. It therefore freezes at whatever it last held and stays there for the
// session. Measured in-headset: frozen at 49350 units, which is -89 degrees,
// straight down, unmoving for fifty seconds.
//
// That is invisible until something the engine owns aims with it. The wrench
// does: the comment above already said melee "aims the wrench's melee phantom at
// the body pitch instead of the hand", which is why the kill exists - but a
// frozen pitch is just a differently-wrong pitch. In-headset the swings landed
// on the FLOOR, and the occasional kill was catching a leg on the way down.
// Also explains the reports of misses on the opening rocks with no combat at
// all: rocks are on the floor, so you look down at them.
//
// So instead of zeroing the stick, DRIVE it: feed a proportional term that
// steers the engine's pitch toward the head's. This goes through the game's own
// input path, so it writes no engine memory (none of the session-29 world-change
// hazards apply), it inherits the game's own pitch clamps, and it is invisible -
// the rendered pitch is overwritten from the head either way. The head keeps
// owning what you SEE; the engine finally learns where you are looking.
//
// Fails open: the game layer publishes the error each CalcView and a stale
// publisher reverts to the plain kill (ry = 0), which is the old behaviour.
std::atomic<bool> g_pitchServo{true};
std::atomic<bool> g_pitchServoInvert{false}; // if a build's look axis is flipped
std::atomic<float> g_pitchErrDeg{0.0f};      // head pitch - engine pitch
std::atomic<uint64_t> g_pitchErrMs{0};
std::atomic<int16_t> g_pitchServoLast{0}; // last stick value, for `vrinput status`
// Deadzone stops the stick chattering once it has converged - a permanently
// nonzero look axis is the kind of thing a game can read as "the player is
// looking around". 1.5 deg is well inside melee tolerance.
constexpr float kPitchServoDeadDeg = 1.5f;
// Proportional gain in stick-units per degree, and a deliberate ceiling well
// under full deflection: this must never out-run a real player's own look, and
// a wrong SIGN must saturate at something recoverable rather than slam the view
// to the clamp. Worst case with the sign inverted is the pitch we already had.
constexpr float kPitchServoGain = 900.0f;
constexpr int16_t kPitchServoMax = 8000; // ~24% deflection

// Session 22 turn controls. Smooth scale multiplies the composed stick X
// (turn speed); snap mode instead consumes stick-X edges into queued steps
// the camera adapter applies to the recenter composite (the M7.5 transfer
// then carries the body). Both respect the same gates as the pitch kill
// (vr-gameplay fresh, lifted while a grip/bumper holds a radial open).
std::atomic<float> g_turnScale{1.0f};
std::atomic<bool> g_snapTurn{false};
std::atomic<int> g_ammoMod{1}; // AmmoMod::Thumbrest (user's call, session 23)
std::atomic<float> g_snapAngleDeg{45.0f};
std::atomic<int> g_snapPending{0}; // +right/-left, drained by take_snap_steps
bool g_snapArmed = true;           // edge re-arm state; g_mutex holds it
// Session 22 movement instrumentation: rate-limited composed-stick log
// ("vrinput sticklog on|off") - the wonkiness investigation pairs it with
// the vrbody probe's resid line.
std::atomic<bool> g_stickLog{false};
uint64_t g_lastStickLogMs = 0; // g_mutex

// Self-expiring test slots: the command seam polls at 1 Hz, so a "hold" must
// outlive its command inside the DLL. deadline == 0 means empty.
struct TimedStick { int16_t x = 0, y = 0; uint64_t deadline = 0; };
struct TimedTrig  { uint8_t v = 0;        uint64_t deadline = 0; };
TimedStick g_testStickL, g_testStickR;
TimedTrig  g_testTrigL, g_testTrigR;
uint64_t g_testBtnDeadline[16] = {}; // per XINPUT button bit index

Gamepad g_lastComposed{};
// Last composed triggers (lt low byte, rt high byte) for the M6 aim path -
// an atomic rather than a peek at g_lastComposed so a hooked engine native on
// the game thread never has to take g_mutex.
std::atomic<uint16_t> g_lastTriggers{0};
std::atomic<uint16_t> g_lastButtons{0}; // composed button bits, same contract
uint32_t g_packet = 0;            // bridge-owned monotonic dwPacketNumber
bool g_packetBump = false;        // forced bump on enable/disable edges

// Telemetry (lock-free for the overlay).
std::atomic<uint32_t> g_calls[4] = {};
std::atomic<uint32_t> g_lastRealResult{ERROR_DEVICE_NOT_CONNECTED};
std::atomic<bool> g_loggedFirstCompose{false};

struct ButtonName { const char* name; uint16_t bit; };
constexpr ButtonName kButtons[] = {
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"DU", XINPUT_GAMEPAD_DPAD_UP}, {"DD", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DL", XINPUT_GAMEPAD_DPAD_LEFT}, {"DR", XINPUT_GAMEPAD_DPAD_RIGHT},
};

int bit_index(uint16_t bit) {
    for (int i = 0; i < 16; ++i)
        if (bit == (1u << i)) return i;
    return 0;
}

// The game probes XInput exactly ONCE at boot (6 GetState calls for index 0,
// 1 each for 1-3) and never re-polls a slot that reported disconnected - not
// on WM_DEVICECHANGE, not on an interval (verified live 2026-07-24). Session
// 22 retires the old vrinput.on marker-file workaround: compose_over now
// answers a FAILED slot-0 query with a neutral CONNECTED pad even while
// vrinput is off, so the probe latches "connected" on every install and a
// later `vrinput on` engages with no restart (the first-boot-restart fix,
// ROADMAP M9). Slots 1-3 keep reporting disconnected; a real pad on slot 0
// passes through untouched.

// Merge b over a: buttons OR, triggers max, stick axes per-axis larger
// magnitude wins with ties going to `a` - deterministic and order-free.
int16_t merge_axis(int16_t a, int16_t b) {
    int absA = a < 0 ? -static_cast<int>(a) : a;
    int absB = b < 0 ? -static_cast<int>(b) : b;
    return absB > absA ? b : a;
}

Gamepad merge(const Gamepad& a, const Gamepad& b) {
    Gamepad out;
    out.buttons = a.buttons | b.buttons;
    out.lt = a.lt > b.lt ? a.lt : b.lt;
    out.rt = a.rt > b.rt ? a.rt : b.rt;
    out.lx = merge_axis(a.lx, b.lx);
    out.ly = merge_axis(a.ly, b.ly);
    out.rx = merge_axis(a.rx, b.rx);
    out.ry = merge_axis(a.ry, b.ry);
    return out;
}

// Caller holds g_mutex.
Gamepad compose_synthetic(uint64_t now) {
    Gamepad syn{};
    if (g_xrActive && now - g_xrLastMs <= kXrStaleMs) syn = g_xrPad;

    Gamepad test{};
    if (now < g_testStickL.deadline) { test.lx = g_testStickL.x; test.ly = g_testStickL.y; }
    if (now < g_testStickR.deadline) { test.rx = g_testStickR.x; test.ry = g_testStickR.y; }
    if (now < g_testTrigL.deadline) test.lt = g_testTrigL.v;
    if (now < g_testTrigR.deadline) test.rt = g_testTrigR.v;
    for (int i = 0; i < 16; ++i)
        if (now < g_testBtnDeadline[i]) test.buttons |= static_cast<uint16_t>(1u << i);

    // Session 31: a physical wrench swing composes as a full right trigger, on
    // the same self-expiring-slot principle as the test injections above. It
    // ADDS to the trigger rather than replacing it - merge() takes the trigger
    // maximum, so pulling RT during a swing is indistinguishable from pulling it
    // on its own, and the gesture is invisible while the wrench is stowed
    // (swing.cpp's gate is closed for every other holdable).
    if (bvr::input::swing::rt_pulse(now)) test.rt = 255;

    return merge(syn, test);
}

// The stick value the pitch kill substitutes for a hard zero. See the block
// comment at g_pitchServo. Returns 0 whenever it should behave exactly as the
// old kill did: servo off, no fresh error published, or already converged.
int16_t pitch_servo_stick(uint64_t now) {
    if (!g_pitchServo.load(std::memory_order_relaxed)) return 0;
    uint64_t stamp = g_pitchErrMs.load(std::memory_order_relaxed);
    // A stopped publisher (world unload, drive off, menu) fails OPEN to the
    // plain kill rather than holding the last error and steering blind.
    if (!stamp || now - stamp > kVrGameplayStaleMs) return 0;
    float errDeg = g_pitchErrDeg.load(std::memory_order_relaxed);
    if (errDeg > -kPitchServoDeadDeg && errDeg < kPitchServoDeadDeg) return 0;
    if (g_pitchServoInvert.load(std::memory_order_relaxed)) errDeg = -errDeg;
    float v = errDeg * kPitchServoGain;
    if (v > kPitchServoMax) v = kPitchServoMax;
    if (v < -kPitchServoMax) v = -kPitchServoMax;
    return static_cast<int16_t>(lroundf(v));
}

// Compose synthetic state over a completed GetState-shaped call. May run on
// any thread; no allocation, no logging except the one-shot first-compose
// line. userIndex 0 is the only slot the game plays on; other indices pass
// through untouched.
void compose_over(DWORD userIndex, XINPUT_STATE* xs, DWORD* result) {
    if (userIndex != 0) return;
    // The swing detector's flat test seam. Live samples arrive from the XR
    // frame loop, which does not run without a headset, so `vrinput swing sim`
    // needs some in-process clock to advance it - and this is the one path the
    // game itself drives at a high rate whether or not the bridge is enabled.
    // No-op (one relaxed load) while no sim is armed.
    bvr::input::swing::sim_tick(GetTickCount64());
    g_lastRealResult.store(*result, std::memory_order_relaxed);
    if (!g_enabled.load(std::memory_order_relaxed)) {
        // Publish the real pad's triggers anyway: the M6 aim path reads them to
        // tell weapon fire from plasmid fire, and that has to work for a player
        // on a physical pad too.
        uint16_t t = 0;
        uint16_t b = 0;
        if (*result == ERROR_SUCCESS) {
            t = static_cast<uint16_t>(xs->Gamepad.bLeftTrigger) |
                static_cast<uint16_t>(static_cast<uint16_t>(xs->Gamepad.bRightTrigger) << 8);
            b = xs->Gamepad.wButtons;
        }
        g_lastTriggers.store(t, std::memory_order_relaxed);
        g_lastButtons.store(b, std::memory_order_relaxed);
        // Session 22 first-boot fix: a failed slot-0 query answers as a
        // neutral CONNECTED pad (constant packet number = "no new input"),
        // so the game's one-shot boot probe never latches slot 0 dead.
        if (*result != ERROR_SUCCESS) {
            memset(xs, 0, sizeof(XINPUT_STATE));
            xs->dwPacketNumber = 1;
            *result = ERROR_SUCCESS;
        }
        return;
    }

    Gamepad real{};
    if (*result == ERROR_SUCCESS) {
        memcpy(&real, &xs->Gamepad, sizeof real);
    } else {
        // Failed real call leaves the buffer unspecified - never merge it.
        memset(xs, 0, sizeof(XINPUT_STATE));
    }

    uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    Gamepad out = merge(real, compose_synthetic(now));
    // Stick-pitch kill: yaw (rx) deliberately stays - stick turn composes
    // with the M7.5 body transfer; only pitch belongs to the HMD. The kill
    // LIFTS while a grip/bumper is held: the radial wheels read stick Y for
    // selection (session 19 part 2 - the wheel was unselectable). The game
    // side snapshots the PC pitch at bumper-down and restores it at release,
    // so the look-pitch the wheel state also accumulates cannot stick.
    bool turnGate = g_vrGameplay.load(std::memory_order_relaxed) &&
                    now - g_vrGameplayLastMs.load(std::memory_order_relaxed) <=
                        kVrGameplayStaleMs &&
                    !(out.buttons &
                      (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER));
    if (g_pitchKill.load(std::memory_order_relaxed) && turnGate) {
        out.ry = pitch_servo_stick(now);
        g_pitchServoLast.store(out.ry, std::memory_order_relaxed);
    }

    // Session 22 turn controls (same gate cluster as the pitch kill; radial
    // states keep the raw stick).
    if (turnGate) {
        if (g_snapTurn.load(std::memory_order_relaxed)) {
            // Snap owns the turn axis: edge-detect with the proven 0.65/0.30
            // hysteresis, queue a step, and zero rx to the game.
            constexpr int16_t kOn = 21299, kOff = 9830;
            int16_t rx = out.rx;
            if (g_snapArmed) {
                if (rx >= kOn) {
                    g_snapPending.fetch_add(1, std::memory_order_relaxed);
                    g_snapArmed = false;
                } else if (rx <= -kOn) {
                    g_snapPending.fetch_sub(1, std::memory_order_relaxed);
                    g_snapArmed = false;
                }
            } else if (rx > -kOff && rx < kOff) {
                g_snapArmed = true;
            }
            out.rx = 0;
        } else {
            float s = g_turnScale.load(std::memory_order_relaxed);
            if (s != 1.0f) {
                float v = static_cast<float>(out.rx) * s;
                out.rx = static_cast<int16_t>(v > 32767.0f    ? 32767
                                              : v < -32768.0f ? -32768
                                                              : lroundf(v));
            }
        }
    }
    if (g_packetBump || memcmp(&out, &g_lastComposed, sizeof out) != 0) {
        ++g_packet;
        g_packetBump = false;
        g_lastComposed = out;
    }
    memcpy(&xs->Gamepad, &out, sizeof out);
    xs->dwPacketNumber = g_packet;
    *result = ERROR_SUCCESS;
    g_lastTriggers.store(static_cast<uint16_t>(out.lt) |
                             static_cast<uint16_t>(static_cast<uint16_t>(out.rt) << 8),
                         std::memory_order_relaxed);
    g_lastButtons.store(out.buttons, std::memory_order_relaxed);

    // Session 22 wonkiness instrumentation: the FINAL composed pad, post
    // merge/pitchkill/turn - what the game actually consumes.
    if (g_stickLog.load(std::memory_order_relaxed) && now - g_lastStickLogMs >= 100) {
        g_lastStickLogMs = now;
        BVR_LOG("[input] stick composed lx=%d ly=%d rx=%d ry=%d pkt=%u",
                out.lx, out.ly, out.rx, out.ry, g_packet);
    }

    if (!g_loggedFirstCompose.exchange(true, std::memory_order_relaxed))
        BVR_LOG("input: first synthetic compose served (packet %u)", g_packet);
}

// The proxy seam (import lane: the game's static xinput1_3 ordinals).
void WINAPI PostGetState(DWORD userIndex, void* state, DWORD* result) {
    if (userIndex < 4) g_calls[userIndex].fetch_add(1, std::memory_order_relaxed);
    compose_over(userIndex, static_cast<XINPUT_STATE*>(state), result);
}

// ---- direct system-DLL hooks -----------------------------------------------
// The proxy only sees the game's static xinput1_3 imports (ordinals 2/3).
// Detection does not have to flow through them: xinput1_4.dll is loaded
// in-process, and XInputGetCapabilities can be bound dynamically against
// either system DLL. MinHook the real DLLs so detection AND polling see the
// synthetic pad no matter which lane the game (or a middleware layer) uses;
// per-lane counters tell us which lane is live (record in ENGINE_NOTES).

using XiGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XiGetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

XiGetStateFn g_orig14GetState = nullptr;
XiGetStateFn g_orig14GetStateEx = nullptr;
XiGetCapsFn g_orig14GetCaps = nullptr;
XiGetCapsFn g_orig13GetCaps = nullptr;
std::atomic<uint32_t> g_calls14State{0}, g_calls14Caps{0}, g_calls13Caps{0};
std::atomic<bool> g_dllHooksTried{false};

void fill_caps(XINPUT_CAPABILITIES* caps) {
    memset(caps, 0, sizeof *caps);
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF; // every standard-gamepad button bit
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = 0x7FFF;
    caps->Gamepad.sThumbLY = 0x7FFF;
    caps->Gamepad.sThumbRX = 0x7FFF;
    caps->Gamepad.sThumbRY = 0x7FFF;
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
}

DWORD WINAPI Hook14GetState(DWORD userIndex, XINPUT_STATE* state) {
    g_calls14State.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_orig14GetState ? g_orig14GetState(userIndex, state)
                               : ERROR_DEVICE_NOT_CONNECTED;
    compose_over(userIndex, state, &r);
    return r;
}

DWORD WINAPI Hook14GetStateEx(DWORD userIndex, XINPUT_STATE* state) {
    g_calls14State.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_orig14GetStateEx ? g_orig14GetStateEx(userIndex, state)
                                 : ERROR_DEVICE_NOT_CONNECTED;
    compose_over(userIndex, state, &r);
    return r;
}

DWORD serve_caps(DWORD userIndex, XINPUT_CAPABILITIES* caps, DWORD realResult) {
    if (userIndex != 0 || !g_enabled.load(std::memory_order_relaxed))
        return realResult;
    fill_caps(caps);
    return ERROR_SUCCESS;
}

DWORD WINAPI Hook14GetCaps(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps) {
    g_calls14Caps.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_orig14GetCaps ? g_orig14GetCaps(userIndex, flags, caps)
                              : ERROR_DEVICE_NOT_CONNECTED;
    return serve_caps(userIndex, caps, r);
}

DWORD WINAPI Hook13GetCaps(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps) {
    g_calls13Caps.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_orig13GetCaps ? g_orig13GetCaps(userIndex, flags, caps)
                              : ERROR_DEVICE_NOT_CONNECTED;
    return serve_caps(userIndex, caps, r);
}

// ---- game-IAT wrapper -------------------------------------------------------
// The game calls XInputGetState through its IAT. The Steam overlay E9-hooks
// the export THUNK our proxy exposes, so calls die inside Steam Input before
// the proxy body (and its post-hook) ever runs. Re-pointing the IAT slot at
// this wrapper keeps whatever chain the slot held (Steam included - real pads
// keep working through it) and composes synthetic state on the way out.
XiGetStateFn g_iatOriginal = nullptr;
std::atomic<uint32_t> g_callsIat{0};

DWORD WINAPI IatGetState(DWORD userIndex, XINPUT_STATE* state) {
    g_callsIat.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_iatOriginal ? g_iatOriginal(userIndex, state)
                            : ERROR_DEVICE_NOT_CONNECTED;
    compose_over(userIndex, state, &r);
    return r;
}

template <typename Fn>
bool hook_export(HMODULE mod, const char* name, void* detour, Fn* orig) {
    if (!mod || *orig) return false;
    void* target = name ? reinterpret_cast<void*>(GetProcAddress(mod, name))
                        : nullptr;
    if (!target) return false;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(orig)) != MH_OK)
        return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        *orig = nullptr;
        return false;
    }
    return true;
}

void install_dll_hooks() {
    if (g_dllHooksTried.load(std::memory_order_relaxed)) return;

    HMODULE h14 = GetModuleHandleW(L"xinput1_4.dll");
    // Real xinput1_3 by explicit system-dir path (WOW64-redirected for us) -
    // the basename resolves to our own proxy in the game folder.
    wchar_t sysPath[MAX_PATH];
    HMODULE h13 = nullptr;
    UINT len = GetSystemDirectoryW(sysPath, MAX_PATH);
    if (len > 0 && len < MAX_PATH - 16) {
        lstrcatW(sysPath, L"\\xinput1_3.dll");
        h13 = GetModuleHandleW(sysPath);
    }
    if (!h14 && !h13) return; // neither loaded yet - retry from the next command

    g_dllHooksTried.store(true, std::memory_order_relaxed);
    int hooked = 0;
    if (hook_export(h14, "XInputGetState", &Hook14GetState, &g_orig14GetState)) ++hooked;
    if (hook_export(h14, reinterpret_cast<const char*>(MAKEINTRESOURCEA(100)),
                    &Hook14GetStateEx, &g_orig14GetStateEx)) ++hooked;
    if (hook_export(h14, "XInputGetCapabilities", &Hook14GetCaps, &g_orig14GetCaps)) ++hooked;
    if (hook_export(h13, "XInputGetCapabilities", &Hook13GetCaps, &g_orig13GetCaps)) ++hooked;
    BVR_LOG("input: system-DLL hooks installed (%d: xinput1_4 %p, real xinput1_3 %p)",
            hooked, h14, h13);
}

// Test-slot bookkeeping shared by handle_command. Caller holds g_mutex.
int live_test_slots(uint64_t now) {
    int live = 0;
    if (now < g_testStickL.deadline) ++live;
    if (now < g_testStickR.deadline) ++live;
    if (now < g_testTrigL.deadline) ++live;
    if (now < g_testTrigR.deadline) ++live;
    for (uint64_t d : g_testBtnDeadline)
        if (now < d) ++live;
    return live;
}

uint64_t clamp_hold(unsigned holdMs, unsigned fallback) {
    if (holdMs == 0) holdMs = fallback;
    if (holdMs > 10000) holdMs = 10000;
    return GetTickCount64() + holdMs;
}

void log_status() {
    // Rates since the previous status call (game thread only).
    static uint64_t lastMs = 0;
    static uint32_t lastCalls0 = 0;
    uint64_t now = GetTickCount64();
    uint32_t calls0 = g_calls[0].load(std::memory_order_relaxed);
    uint32_t rate = 0;
    if (lastMs && now > lastMs)
        rate = static_cast<uint32_t>((calls0 - lastCalls0) * 1000ull / (now - lastMs));
    lastMs = now;
    lastCalls0 = calls0;

    int live = 0;
    bool xrFresh = false;
    uint32_t packet = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        live = live_test_slots(now);
        xrFresh = g_xrActive && now - g_xrLastMs <= kXrStaleMs;
        packet = g_packet;
    }
    BVR_LOG("input: status - %s, seam %s, xr %s, iat %u, getstate[0] %u total "
            "(%u/s since last status, idx1-3 %u/%u/%u), xi14 state %u caps %u, "
            "xi13 caps %u, last real result %u, packet %u, test slots live %d",
            g_enabled.load(std::memory_order_relaxed) ? "ENABLED" : "disabled",
            g_registered.load(std::memory_order_relaxed) ? "registered" : "MISSING",
            xrFresh ? "active" : "idle",
            g_callsIat.load(std::memory_order_relaxed),
            calls0, rate,
            g_calls[1].load(std::memory_order_relaxed),
            g_calls[2].load(std::memory_order_relaxed),
            g_calls[3].load(std::memory_order_relaxed),
            g_calls14State.load(std::memory_order_relaxed),
            g_calls14Caps.load(std::memory_order_relaxed),
            g_calls13Caps.load(std::memory_order_relaxed),
            g_lastRealResult.load(std::memory_order_relaxed), packet, live);
}

} // namespace

void init() {
    // Two modules named xinput1_3.dll are in-process: our proxy (game folder,
    // loaded via the game's static import) and the real one (SysWOW64, loaded
    // by the proxy). Resolve the proxy deterministically by exe-dir full path;
    // the named seam export is the identity check either way.
    wchar_t path[MAX_PATH];
    HMODULE proxy = nullptr;
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (slash - path) + 16 < MAX_PATH) {
            lstrcpyW(slash + 1, L"xinput1_3.dll");
            proxy = GetModuleHandleW(path);
        }
    }
    if (!proxy) proxy = GetModuleHandleW(L"xinput1_3.dll");

    auto setter = proxy ? reinterpret_cast<SetPostGetStateFn>(
                              GetProcAddress(proxy, "BVR_SetPostGetStateHook"))
                        : nullptr;
    if (!setter) {
        BVR_LOG("input: proxy seam not found - synthetic input disabled "
                "(proxy module %p)", proxy);
        return;
    }
    setter(&PostGetState);
    g_registered.store(true, std::memory_order_relaxed);
    BVR_LOG("input: bridge registered with proxy seam (module %p)", proxy);

    install_dll_hooks(); // retried lazily from commands if the DLLs load later
}

bool hijack_import_slot(void** slot) {
    if (!slot || !bvr::pattern_scan::is_memory_valid(slot, sizeof(void*)))
        return false;
    if (*slot == reinterpret_cast<void*>(&IatGetState)) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        BVR_LOG("input: import-slot hijack failed (VirtualProtect err %u)",
                GetLastError());
        return false;
    }
    g_iatOriginal = reinterpret_cast<XiGetStateFn>(*slot);
    *slot = reinterpret_cast<void*>(&IatGetState);
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    BVR_LOG("input: game import slot %p hijacked (original %p -> bridge wrapper)",
            slot, g_iatOriginal);
    return true;
}

void set_enabled(bool on) {
    bool was = g_enabled.exchange(on, std::memory_order_relaxed);
    if (was == on) return;
    {
        // Force a packet change so the game notices the state edge.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_packetBump = true;
    }
    BVR_LOG("input: vrinput %s", on ? "ON (synthetic gamepad live)" : "off (passthrough)");
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void publish_xr_state(const Gamepad& pad, bool active) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_xrPad = pad;
    g_xrActive = active;
    g_xrLastMs = GetTickCount64();
}

void last_xr_pad(Gamepad* pad, bool* active) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (pad) *pad = g_xrPad;
    if (active) *active = g_xrActive;
}

void publish_vr_gameplay(bool on) {
    g_vrGameplay.store(on, std::memory_order_relaxed);
    g_vrGameplayLastMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void publish_pitch_error(float headMinusEngineDeg) {
    g_pitchErrDeg.store(headMinusEngineDeg, std::memory_order_relaxed);
    g_pitchErrMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void set_pitch_kill(bool on) {
    bool was = g_pitchKill.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("input: pitchkill %s (right-stick Y %s while the VR camera "
                "drives gameplay)",
                on ? "ON" : "off", on ? "zeroed" : "passes through");
}

bool pitch_kill() { return g_pitchKill.load(std::memory_order_relaxed); }

float stick_deadzone() { return g_deadzone.load(std::memory_order_relaxed); }

void last_composed_triggers(uint8_t* lt, uint8_t* rt) {
    uint16_t t = g_lastTriggers.load(std::memory_order_relaxed);
    if (lt) *lt = static_cast<uint8_t>(t & 0xFF);
    if (rt) *rt = static_cast<uint8_t>(t >> 8);
}

void last_composed_bumpers(bool* lb, bool* rb) {
    uint16_t b = g_lastButtons.load(std::memory_order_relaxed);
    if (lb) *lb = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    if (rb) *rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
}

void last_composed_buttons(uint16_t* buttons) {
    // The whole wButtons word as the game last saw it - stored on both the
    // composed (VR) path and the disabled/real-pad path, so a consumer reading
    // edges here covers a physical pad too. Session 42: BS2's menukey lane.
    if (buttons) *buttons = g_lastButtons.load(std::memory_order_relaxed);
}

void last_composed_sticks(int16_t* lx, int16_t* ly, int16_t* rx, int16_t* ry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lx) *lx = g_lastComposed.lx;
    if (ly) *ly = g_lastComposed.ly;
    if (rx) *rx = g_lastComposed.rx;
    if (ry) *ry = g_lastComposed.ry;
}

AmmoMod ammo_mod() {
    return static_cast<AmmoMod>(g_ammoMod.load(std::memory_order_relaxed));
}
void set_ammo_mod(AmmoMod m) {
    g_ammoMod.store(static_cast<int>(m), std::memory_order_relaxed);
}

float turn_scale() { return g_turnScale.load(std::memory_order_relaxed); }
void set_turn_scale(float s) {
    if (s < 0.1f) s = 0.1f;
    if (s > 4.0f) s = 4.0f;
    g_turnScale.store(s, std::memory_order_relaxed);
}

bool snap_turn() { return g_snapTurn.load(std::memory_order_relaxed); }
void set_snap_turn(bool on) {
    bool was = g_snapTurn.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("input: snap turn %s (stick X %s)", on ? "ON" : "off",
                on ? "edges queue discrete steps" : "smooth");
}

float snap_angle_deg() { return g_snapAngleDeg.load(std::memory_order_relaxed); }
void set_snap_angle_deg(float d) {
    if (d < 5.0f) d = 5.0f;
    if (d > 180.0f) d = 180.0f;
    g_snapAngleDeg.store(d, std::memory_order_relaxed);
}

int take_snap_steps() { return g_snapPending.exchange(0, std::memory_order_relaxed); }

void handle_command(const char* args) {
    install_dll_hooks(); // lazy retry in case xinput1_4 loaded after init

    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1)
        return;
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        set_enabled(true);
    } else if (strcmp(verb, "off") == 0) {
        set_enabled(false);
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else if (strcmp(verb, "pitchkill") == 0) {
        if (strncmp(rest, "on", 2) == 0) {
            set_pitch_kill(true);
        } else if (strncmp(rest, "off", 3) == 0) {
            set_pitch_kill(false);
        } else {
            uint64_t age = GetTickCount64() -
                           g_vrGameplayLastMs.load(std::memory_order_relaxed);
            BVR_LOG("input: pitchkill %s, vr-gameplay gate %s (published %llu ms ago)",
                    g_pitchKill.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_vrGameplay.load(std::memory_order_relaxed) ? "ACTIVE" : "inactive",
                    static_cast<unsigned long long>(age));
        }
    } else if (strcmp(verb, "pitchservo") == 0) {
        // "pitchservo on|off|invert|status"
        if (strncmp(rest, "on", 2) == 0 || strncmp(rest, "off", 3) == 0) {
            bool on = strncmp(rest, "on", 2) == 0;
            g_pitchServo.store(on, std::memory_order_relaxed);
            BVR_LOG("input: pitch servo %s - %s", on ? "ON" : "off",
                    on ? "the stick steers the ENGINE's own view pitch toward your head, so "
                         "melee stops swinging at a frozen pitch"
                       : "back to the plain kill (ry=0): the engine's pitch freezes where it "
                         "is and melee aims there");
        } else if (strncmp(rest, "invert", 6) == 0) {
            bool inv = !g_pitchServoInvert.load(std::memory_order_relaxed);
            g_pitchServoInvert.store(inv, std::memory_order_relaxed);
            BVR_LOG("input: pitch servo sign %s - flip this if the engine pitch runs AWAY from "
                    "your head instead of toward it (an inverted look axis)",
                    inv ? "INVERTED" : "normal");
        } else {
            uint64_t age = GetTickCount64() - g_pitchErrMs.load(std::memory_order_relaxed);
            BVR_LOG("input: pitch servo %s%s | err=%.1f deg (published %llu ms ago) stick=%d "
                    "| deadzone %.1f deg gain %.0f max %d "
                    "(vrinput pitchservo on|off|invert|status)",
                    g_pitchServo.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_pitchServoInvert.load(std::memory_order_relaxed) ? " INVERTED" : "",
                    g_pitchErrDeg.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(age),
                    g_pitchServoLast.load(std::memory_order_relaxed), kPitchServoDeadDeg,
                    kPitchServoGain, kPitchServoMax);
        }
    } else if (strcmp(verb, "turnscale") == 0) {
        float s = 0.0f;
        if (sscanf_s(rest, "%f", &s) == 1) {
            set_turn_scale(s);
            BVR_LOG("input: turn scale %.2f", turn_scale());
        } else {
            BVR_LOG("input: turn scale %.2f (vrinput turnscale <0.1..4>)", turn_scale());
        }
    } else if (strcmp(verb, "snap") == 0) {
        if (strncmp(rest, "on", 2) == 0) set_snap_turn(true);
        else if (strncmp(rest, "off", 3) == 0) set_snap_turn(false);
        else
            BVR_LOG("input: snap %s angle %.0f deg pending %d "
                    "(vrinput snap on|off, vrinput snapangle <deg>)",
                    snap_turn() ? "ON" : "off", snap_angle_deg(),
                    g_snapPending.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "snapangle") == 0) {
        float d = 0.0f;
        if (sscanf_s(rest, "%f", &d) == 1) {
            set_snap_angle_deg(d);
            BVR_LOG("input: snap angle %.0f deg", snap_angle_deg());
        }
    } else if (strcmp(verb, "ammomod") == 0) {
        if (strncmp(rest, "click", 5) == 0) set_ammo_mod(AmmoMod::Click);
        else if (strncmp(rest, "thumbrest", 9) == 0) set_ammo_mod(AmmoMod::Thumbrest);
        else if (strncmp(rest, "both", 4) == 0) set_ammo_mod(AmmoMod::Both);
        const AmmoMod m = ammo_mod();
        BVR_LOG("input: ammo modifier = %s (vrinput ammomod click|thumbrest|both) - "
                "thumbrest is the LEFT one; the right stick still picks the slot",
                m == AmmoMod::Click       ? "CLICK (hold right stick click)"
                : m == AmmoMod::Thumbrest ? "THUMBREST (rest left thumb)"
                                          : "BOTH (either)");
    } else if (strcmp(verb, "swing") == 0) {
        bvr::input::swing::handle_command(rest); // logs its own echoes
    } else if (strcmp(verb, "sticklog") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_stickLog.store(on, std::memory_order_relaxed);
        BVR_LOG("input: stick log %s (composed pad @10 Hz)", on ? "ON" : "off");
    } else if (strcmp(verb, "test") == 0) {
        char what[16] = {};
        consumed = 0;
        if (sscanf_s(rest, "%15s%n", what, static_cast<unsigned>(sizeof what), &consumed) != 1)
            return;
        const char* p = rest + consumed;

        if (strcmp(what, "clear") == 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_testStickL = {};
            g_testStickR = {};
            g_testTrigL = {};
            g_testTrigR = {};
            memset(g_testBtnDeadline, 0, sizeof g_testBtnDeadline);
            BVR_LOG("input: test slots cleared");
        } else if (strcmp(what, "stick") == 0) {
            char side[4] = {};
            int x = 0, y = 0;
            unsigned hold = 0;
            int got = sscanf_s(p, "%3s %d %d %u", side,
                               static_cast<unsigned>(sizeof side), &x, &y, &hold);
            if (got < 3) return;
            if (x < -32768) x = -32768; if (x > 32767) x = 32767;
            if (y < -32768) y = -32768; if (y > 32767) y = 32767;
            uint64_t deadline = clamp_hold(hold, 2000);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                TimedStick& s = (side[0] == 'r') ? g_testStickR : g_testStickL;
                s.x = static_cast<int16_t>(x);
                s.y = static_cast<int16_t>(y);
                s.deadline = deadline;
            }
            BVR_LOG("input: test stick %c %d %d (%llu ms)", side[0], x, y,
                    static_cast<unsigned long long>(deadline - GetTickCount64()));
        } else if (strcmp(what, "trig") == 0) {
            char side[4] = {};
            unsigned v = 0, hold = 0;
            int got = sscanf_s(p, "%3s %u %u", side,
                               static_cast<unsigned>(sizeof side), &v, &hold);
            if (got < 2) return;
            if (v > 255) v = 255;
            uint64_t deadline = clamp_hold(hold, 2000);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                TimedTrig& t = (side[0] == 'r') ? g_testTrigR : g_testTrigL;
                t.v = static_cast<uint8_t>(v);
                t.deadline = deadline;
            }
            BVR_LOG("input: test trig %c %u (%u ms)", side[0], v, hold ? hold : 2000);
        } else if (strcmp(what, "press") == 0) {
            char name[8] = {};
            unsigned hold = 0;
            int got = sscanf_s(p, "%7s %u", name,
                               static_cast<unsigned>(sizeof name), &hold);
            if (got < 1) return;
            for (const auto& b : kButtons) {
                if (_stricmp(name, b.name) == 0) {
                    uint64_t deadline = clamp_hold(hold, 300);
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_testBtnDeadline[bit_index(b.bit)] = deadline;
                    }
                    BVR_LOG("input: test press %s (%u ms)", b.name, hold ? hold : 300);
                    return;
                }
            }
            BVR_LOG("input: unknown button '%s'", name);
        }
    }
}

void draw_debug_ui() {
    ImGui::Text("Input (synthetic gamepad)");
    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("VR input (motion controllers as gamepad)", &on))
        set_enabled(on);

    float dz = g_deadzone.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Stick deadzone", &dz, 0.0f, 0.4f, "%.2f"))
        g_deadzone.store(dz, std::memory_order_relaxed);

    bool pk = g_pitchKill.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Kill right-stick pitch under VR", &pk))
        set_pitch_kill(pk);

    // Session 22 turn controls.
    float ts = g_turnScale.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Smooth turn speed", &ts, 0.2f, 3.0f, "%.2f"))
        set_turn_scale(ts);
    bool st = g_snapTurn.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Snap turn (discrete steps)", &st)) set_snap_turn(st);
    if (st) {
        float sa = g_snapAngleDeg.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Snap angle (deg)", &sa, 15.0f, 90.0f, "%.0f"))
            set_snap_angle_deg(sa);
    }

    // Session 23: how you hold the ammo-select modifier. Thumbrest is the
    // default; "Both" exists for controllers whose runtime reports no
    // thumbrest at all (Pico, some SteamVR setups) - see xinput_bridge.h.
    int am = g_ammoMod.load(std::memory_order_relaxed);
    const char* amNames[] = {"Right-stick click", "Left thumbrest", "Either"};
    if (ImGui::Combo("Ammo-select modifier", &am, amNames, 3))
        set_ammo_mod(static_cast<AmmoMod>(am));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hold this, then push the RIGHT stick up/down/left to pick an "
                          "ammo slot.\nThe thumbrest is the pad above the buttons - it is "
                          "the LEFT one,\nbecause your right thumb cannot rest and push the "
                          "right stick at once.");

    // Session 31 swing-to-attack (its own module; see core/input/swing.h).
    ImGui::Separator();
    bvr::input::swing::draw_debug_ui();
    ImGui::Separator();

    // GetState poll rate, sampled ~1/s (render thread only).
    static uint64_t lastMs = 0;
    static uint32_t lastCalls = 0;
    static uint32_t rate = 0;
    uint64_t now = GetTickCount64();
    uint32_t calls = g_calls[0].load(std::memory_order_relaxed);
    if (now - lastMs >= 1000) {
        if (lastMs && now > lastMs)
            rate = static_cast<uint32_t>((calls - lastCalls) * 1000ull / (now - lastMs));
        lastMs = now;
        lastCalls = calls;
    }
    bool xrFresh = false;
    uint32_t packet = 0;
    int live = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        xrFresh = g_xrActive && now - g_xrLastMs <= kXrStaleMs;
        packet = g_packet;
        live = live_test_slots(now);
    }
    ImGui::Text("seam %s | xr %s | getstate %u/s | packet %u | test slots %d",
                g_registered.load(std::memory_order_relaxed) ? "ok" : "MISSING",
                xrFresh ? "active" : "idle", rate, packet, live);
    ImGui::Text("lanes: iat %u | proxy %u | xi14 state %u caps %u | xi13 caps %u",
                g_callsIat.load(std::memory_order_relaxed),
                g_calls[0].load(std::memory_order_relaxed),
                g_calls14State.load(std::memory_order_relaxed),
                g_calls14Caps.load(std::memory_order_relaxed),
                g_calls13Caps.load(std::memory_order_relaxed));
    ImGui::Text("last real result: %u%s",
                g_lastRealResult.load(std::memory_order_relaxed),
                g_lastRealResult.load(std::memory_order_relaxed) ==
                        ERROR_DEVICE_NOT_CONNECTED
                    ? " (no physical pad)"
                    : "");
}

} // namespace bvr::input
