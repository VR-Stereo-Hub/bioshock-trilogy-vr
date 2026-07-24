#include "core/input/xinput_bridge.h"

#include "core/util/log.h"

#include <windows.h>
#include <Xinput.h> // layout only (XINPUT_STATE); no XInput functions are called

#include <imgui.h>

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

// The proxy seam. May run on any thread; no allocation, no logging except the
// one-shot first-compose line. userIndex 0 is the only slot the game plays on;
// other indices pass through untouched (counted for telemetry).
void WINAPI PostGetState(DWORD userIndex, void* state, DWORD* result) {
    if (userIndex < 4) g_calls[userIndex].fetch_add(1, std::memory_order_relaxed);
    if (userIndex != 0) return;
    g_lastRealResult.store(*result, std::memory_order_relaxed);
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    auto* xs = static_cast<XINPUT_STATE*>(state);
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

    if (!g_loggedFirstCompose.exchange(true, std::memory_order_relaxed))
        BVR_LOG("input: first synthetic compose served (packet %u)", g_packet);
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
    BVR_LOG("input: status - %s, seam %s, xr %s, getstate[0] %u total (%u/s since "
            "last status, idx1-3 %u/%u/%u), last real result %u, packet %u, "
            "test slots live %d",
            g_enabled.load(std::memory_order_relaxed) ? "ENABLED" : "disabled",
            g_registered.load(std::memory_order_relaxed) ? "registered" : "MISSING",
            xrFresh ? "active" : "idle",
            calls0, rate,
            g_calls[1].load(std::memory_order_relaxed),
            g_calls[2].load(std::memory_order_relaxed),
            g_calls[3].load(std::memory_order_relaxed),
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

float stick_deadzone() { return g_deadzone.load(std::memory_order_relaxed); }

void handle_command(const char* args) {
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
    ImGui::Text("last real result: %u%s",
                g_lastRealResult.load(std::memory_order_relaxed),
                g_lastRealResult.load(std::memory_order_relaxed) ==
                        ERROR_DEVICE_NOT_CONNECTED
                    ? " (no physical pad)"
                    : "");
}

} // namespace bvr::input
