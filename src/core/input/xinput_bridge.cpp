#include "core/input/xinput_bridge.h"

#include "core/hooks/pattern_scan.h"
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
// on WM_DEVICECHANGE, not on an interval (verified live 2026-07-24). So the
// enable flag must already be set when that probe runs, which is before the
// command seam's first poll. A marker file persisted across boots and read at
// DLL attach closes the gap: once the user opts in, every later boot reports
// a connected pad to the probe and the game polls at frame rate from then on.
// Mid-session "vrinput on" after a disconnected boot probe needs a relaunch.
bool marker_path(wchar_t out[MAX_PATH]) {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return false;
    swprintf_s(out, MAX_PATH, L"%s\\BioshockVR\\vrinput.on", base);
    return true;
}

void persist_enabled(bool on) {
    wchar_t path[MAX_PATH];
    if (!marker_path(path)) return;
    if (on) {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    } else {
        DeleteFileW(path);
    }
}

bool read_persisted_enabled() {
    wchar_t path[MAX_PATH];
    if (!marker_path(path)) return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

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

    return merge(syn, test);
}

// Compose synthetic state over a completed GetState-shaped call. May run on
// any thread; no allocation, no logging except the one-shot first-compose
// line. userIndex 0 is the only slot the game plays on; other indices pass
// through untouched.
void compose_over(DWORD userIndex, XINPUT_STATE* xs, DWORD* result) {
    if (userIndex != 0) return;
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

    if (read_persisted_enabled()) {
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("input: vrinput pre-armed from marker - boot probe will see a "
                "connected pad");
    }
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
    persist_enabled(on); // sticky across boots - see the boot-probe note above
    BVR_LOG("input: vrinput %s", on ? "ON (synthetic gamepad live)" : "off (passthrough)");
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void publish_xr_state(const Gamepad& pad, bool active) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_xrPad = pad;
    g_xrActive = active;
    g_xrLastMs = GetTickCount64();
}

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
