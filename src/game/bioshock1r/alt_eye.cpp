// Alternate-eye stereo methods for BioShock 1 - see alt_eye.h.
#include "game/bioshock1r/alt_eye.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scenedraw.h"
#include "game/bioshock1r/temporal.h"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace bvr::b1r::alt_eye {
namespace {

const char* const kNames[3] = {"both eyes every frame", "alternate eye (AER)", "alternate frame warp (AFW)"};

const uint8_t* g_base = nullptr;
std::atomic<int> g_want{kBoth};
std::atomic<int> g_method{kBoth};       // applied; written on the game thread only
std::atomic<bool> g_clampOn{true};      // AER: hold the world on the right frame (BRVR DeltaClamp=2)
std::atomic<bool> g_lockOn{true};       // AER: the right frame takes the left frame's cameras
bool g_srWasOn = false;                 // SequentialReentry was on when an alternate method began
uint64_t g_lastEnforceMs = 0;

// The eye sequence (game thread). A pair always opens on the LEFT: a mono frame or a
// method change restarts it there (Dishonored's rule - a right image must never open one).
int g_nextSign = -1;
uint32_t g_pair = 1;                    // the pair the coming frame belongs to
// AER: the coming frame is a pair's RIGHT frame, so the world must not advance for it.
// Set at the left frame's render view, read by the frame-delta hook of the next frame
// (the engine advances the world before it builds the scene).
std::atomic<bool> g_freezeNext{false};

// Pair lock: each CalcView call site's camera from the left frame of the current pair.
struct Site {
    void* ret = nullptr;
    uint32_t pair = 0;
    FVector loc{};
    FRotator rot{};
};
Site g_sites[8];
// AER: the head sample of the current pair.
bvr::vr::HeadPose g_pairHead{};
uint32_t g_pairHeadPair = 0;

// ---- the frame-delta clamp (BRVR's DeltaClamp, mode 2: both delta receivers) --------------
using DeltaFn = int(__fastcall*)(void* self, void* edx, void* arg1, uint32_t deltaBits);
DeltaFn g_origDelta = nullptr;
bool g_deltaTried = false;
std::atomic<bool> g_deltaLive{false};
// Two objects take an advance every frame; each keeps its own carry - sharing one would
// hand each the other's held time. A third identity means a load replaced them: start over.
uintptr_t g_objA = 0, g_objB = 0;
float g_carry[2] = {};

Telemetry g_t;          // game thread
Telemetry g_tMark;      // at the last 5 s line
uint64_t g_lastLogMs = 0;

int __fastcall DeltaDetour(void* self, void* edx, void* arg1, uint32_t deltaBits) {
    float d;
    memcpy(&d, &deltaBits, 4);
    const float out = clamp_delta(self, d);
    uint32_t pass;
    memcpy(&pass, &out, 4);
    // Nothing after the call: the original's return registers pass straight through.
    return g_origDelta(self, edx, arg1, pass);
}

bool install_delta() {
    if (g_deltaLive.load()) return true;
    if (g_deltaTried) return false;
    g_deltaTried = true;
    uint8_t* t = g_base ? const_cast<uint8_t*>(g_base) + patterns::kFrameDeltaRva : nullptr;
    const size_t need = patterns::kFrameDeltaTailOffset + sizeof patterns::kFrameDeltaTail;
    if (!t || !bvr::pattern_scan::is_memory_valid(t, need) ||
        memcmp(t, patterns::kFrameDeltaPrologue, sizeof patterns::kFrameDeltaPrologue) != 0 ||
        memcmp(t + patterns::kFrameDeltaTailOffset, patterns::kFrameDeltaTail, sizeof patterns::kFrameDeltaTail) != 0) {
        BVR_LOG("[aer] frame-delta function not found at RVA 0x%X (build changed?) - REFUSING the world clamp; "
                "AER runs, but the world moves between the two eyes of a pair", patterns::kFrameDeltaRva);
        return false;
    }
    MH_STATUS st = MH_CreateHook(t, reinterpret_cast<void*>(&DeltaDetour), reinterpret_cast<void**>(&g_origDelta));
    if (st == MH_OK) st = MH_EnableHook(t);
    if (st != MH_OK) {
        BVR_LOG("[aer] frame-delta hook failed: %s - AER runs without the world clamp", MH_StatusToString(st));
        return false;
    }
    g_deltaLive.store(true);
    g_t.clampHooked = true;
    BVR_LOG("[aer] frame-delta hook live (RVA 0x%X): the world advances once per eye pair", patterns::kFrameDeltaRva);
    return true;
}

void restart_sequence() {
    g_nextSign = -1;
    ++g_pair;
    g_freezeNext.store(false);
}

// Game thread, outside the scene build.
void apply(int m) {
    const int was = g_method.load();
    if (m == was) return;
    if (was == kAfw) temporal::set_afw(false);
    if (was == kBoth) {
        g_srWasOn = scenedraw::doubling_on();
        if (g_srWasOn) scenedraw::request_stereo_only(false);
    }
    if (m != kBoth && !scenedraw::build_hook_live()) {
        // The render view is recognised inside the scene build; the hook is pass-through
        // while the doubling is off.
        scenedraw::handle_command("hook build");
    }
    if (m == kAer) install_delta();
    // AER's tags ride core's SR ring: give it BRVR's drift resync while AER is on.
    bvr::vr::set_sr_drift_resync(m == kAer);
    if (m == kAfw) temporal::set_afw(true);
    if (m == kBoth && g_srWasOn) {
        scenedraw::request_stereo_only(true);
        g_srWasOn = false;
    }
    restart_sequence();
    g_method.store(m);
    BVR_LOG("[stereo] %s (was %s)%s", kNames[m], kNames[was],
            m == kAer ? (g_deltaLive.load() ? " - pairs locked, world held on the right frame"
                                            : " - pairs locked, world clamp NOT available")
                      : "");
}

void log_maybe() {
    const uint64_t now = GetTickCount64();
    if (now - g_lastLogMs < 5000) return;
    g_lastLogMs = now;
    const Telemetry& a = g_t;
    const Telemetry& b = g_tMark;
    const uint64_t n = a.interEyeN - b.interEyeN;
    BVR_LOG("[stereo] 5 s (%s): frames L %llu R %llu mono %llu | pair lock %llu (sites with no left %llu) | "
            "world held %llu paid %llu dropped %llu | render view moved within a pair: avg %.2f max %.2f uu",
            kNames[g_method.load()], (unsigned long long)(a.frames[0] - b.frames[0]),
            (unsigned long long)(a.frames[1] - b.frames[1]), (unsigned long long)(a.monoFrames - b.monoFrames),
            (unsigned long long)(a.locked - b.locked), (unsigned long long)(a.lockMisses - b.lockMisses),
            (unsigned long long)(a.frozen - b.frozen), (unsigned long long)(a.released - b.released),
            (unsigned long long)(a.carryDropped - b.carryDropped),
            n ? (a.interEyeMoveSum - b.interEyeMoveSum) / (double)n : 0.0, a.interEyeMoveMax);
    g_tMark = g_t;
    g_t.interEyeMoveMax = 0;
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_base = image.base;
}

float clamp_delta(void* self, float d) {
    const uintptr_t me = reinterpret_cast<uintptr_t>(self);
    if (me != g_objA && me != g_objB) {
        if (!g_objA) {
            g_objA = me;
        } else if (!g_objB) {
            g_objB = me;
        } else {
            g_objA = me;
            g_objB = 0;
            g_carry[0] = g_carry[1] = 0.0f;
            BVR_LOG("[aer] frame-delta receivers changed (a load): carries reset");
        }
    }
    float* carry = &g_carry[me == g_objB ? 1 : 0];
    if (g_freezeNext.load(std::memory_order_relaxed) && g_clampOn.load(std::memory_order_relaxed) &&
        g_method.load(std::memory_order_relaxed) == kAer) {
        *carry += d;
        // During a load the eye stops alternating and the carry could bank far more than a
        // frame; paying that out at once is a lurch, so it is dropped (BRVR's 0.1 s bound).
        if (*carry > 0.1f || !(*carry >= 0.0f)) {
            *carry = 0.0f;
            ++g_t.carryDropped;
        }
        ++g_t.frozen;
        return 0.0f;
    }
    if (*carry != 0.0f) {
        const float paid = d + *carry;
        *carry = 0.0f;
        ++g_t.released;
        return paid;
    }
    return d;
}

void request(int m, const char* who) {
    m = m < kBoth ? kBoth : m > kAfw ? kAfw : m;
    if (g_want.exchange(m) != m) BVR_LOG("[stereo] %s requested (%s)", kNames[m], who);
}

int method() {
    return g_method.load(std::memory_order_relaxed);
}

int frame_sign(bool stereoView) {
    const int want = g_want.load(std::memory_order_relaxed);
    if (want != g_method.load(std::memory_order_relaxed) && !scenedraw::in_hooked_call()) apply(want);
    const int m = g_method.load(std::memory_order_relaxed);
    if (m == kBoth) return 0;
    // An alternate method owns the stereo: keep SequentialReentry's doubling off (a VR
    // preset or the one-toggle can switch it back on behind it).
    const uint64_t now = GetTickCount64();
    if (scenedraw::doubling_on() && now - g_lastEnforceMs > 1000) {
        g_lastEnforceMs = now;
        g_srWasOn = true;   // it was wanted: give it back when the method goes
        scenedraw::request_stereo_only(false);
        BVR_LOG("[stereo] SequentialReentry was switched on again - off while %s is on", kNames[m]);
    }
    return stereoView ? g_nextSign : 0;
}

void latch_head(bvr::vr::HeadPose* hp, int sign) {
    if (!hp || sign == 0 || g_method.load(std::memory_order_relaxed) != kAer ||
        !g_lockOn.load(std::memory_order_relaxed))
        return;
    if (g_pairHeadPair != g_pair) {
        // A right frame whose left frame stored nothing (should not happen: a pair opens
        // LEFT) takes its own sample rather than an older pair's.
        g_pairHead = *hp;
        g_pairHeadPair = g_pair;
    } else {
        *hp = g_pairHead;
    }
}

void pair_lock(void* site, FVector* loc, FRotator* rot, int sign) {
    if (!loc || !rot || sign == 0 || g_method.load(std::memory_order_relaxed) != kAer ||
        !g_lockOn.load(std::memory_order_relaxed))
        return;
    if (sign < 0) {
        Site* slot = nullptr;
        for (Site& s : g_sites)
            if (s.ret == site) { slot = &s; break; }
        if (!slot)
            for (Site& s : g_sites)
                if (s.pair != g_pair) { slot = &s; break; }
        if (!slot) slot = &g_sites[0];
        slot->ret = site;
        slot->pair = g_pair;
        slot->loc = *loc;
        slot->rot = *rot;
        return;
    }
    for (const Site& s : g_sites) {
        if (s.ret != site || s.pair != g_pair) continue;
        if (scenedraw::in_gameplay_build()) {
            // How far the render view would have moved between the two eyes without the
            // lock: the quantity the lock and the world clamp exist to remove (BRVR INTEREYE).
            const double dx = loc->x - s.loc.x, dy = loc->y - s.loc.y, dz = loc->z - s.loc.z;
            const double moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            g_t.interEyeMoveSum += moved;
            ++g_t.interEyeN;
            if (moved > g_t.interEyeMoveMax) g_t.interEyeMoveMax = moved;
        }
        *loc = s.loc;
        *rot = s.rot;
        ++g_t.locked;
        return;
    }
    ++g_t.lockMisses;
}

void commit_render_view(int sign) {
    const int m = g_method.load(std::memory_order_relaxed);
    if (m == kBoth) return;
    if (sign != 0) {
        ++g_t.frames[sign > 0 ? 1 : 0];
        // AER: core pairs the two presents into one XR frame (the left holds it open, the
        // right closes it with one locate's pose) - BRVR's XR_SubmitPair.
        if (m == kAer) bvr::vr::sr_push_eye(sign);
        g_nextSign = -sign;
        if (sign > 0) ++g_pair;
    } else {
        ++g_t.monoFrames;
        restart_sequence();
    }
    g_freezeNext.store(m == kAer && sign < 0, std::memory_order_relaxed);
    log_maybe();
}

void handle_command(const char* verb, const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;
    auto is = [&](const char* w) {
        const size_t n = strlen(w);
        return strncmp(args, w, n) == 0 && (args[n] == 0 || args[n] == ' ');
    };
    if (strcmp(verb, "afw") == 0) {
        if (is("on")) request(kAfw, "seam");
        else if (is("off")) { if (g_want.load() == kAfw) request(kBoth, "seam"); }
        else temporal::handle_command(verb, args);
        return;
    }
    // aer
    if (is("on")) request(kAer, "seam");
    else if (is("off")) { if (g_want.load() == kAer) request(kBoth, "seam"); }
    else if (is("clamp")) {
        g_clampOn.store(strstr(args, "off") == nullptr);
        BVR_LOG("[aer] world clamp %s", g_clampOn.load() ? "on" : "off");
    } else if (is("lock")) {
        g_lockOn.store(strstr(args, "off") == nullptr);
        BVR_LOG("[aer] pair lock %s", g_lockOn.load() ? "on" : "off");
    } else {
        BVR_LOG("[aer] stereo %s | world clamp %s (%s) | pair lock %s | usage: aer on|off|clamp on|off|lock on|off",
                kNames[g_method.load()], g_clampOn.load() ? "on" : "off",
                g_deltaLive.load() ? "hooked" : g_deltaTried ? "REFUSED" : "not yet hooked",
                g_lockOn.load() ? "on" : "off");
    }
}

float menu_read(menu::Setting id) {
    if (id == menu::Setting::StereoMethod) return (float)g_want.load();
    return std::numeric_limits<float>::quiet_NaN();
}

bool menu_write(menu::Setting id, float value) {
    if (id != menu::Setting::StereoMethod) return false;
    request((int)value, "F10");
    return true;
}

void draw_status() {
    const int m = g_method.load();
    ImGui::Text("Stereo rendering: %s", kNames[m]);
    if (m == kAer)
        ImGui::Text("World held on the right eye: %s", g_deltaLive.load() ? (g_clampOn.load() ? "yes" : "off (aer clamp off)")
                                                                          : "not available");
}

Telemetry telemetry() {
    return g_t;
}

} // namespace bvr::b1r::alt_eye
