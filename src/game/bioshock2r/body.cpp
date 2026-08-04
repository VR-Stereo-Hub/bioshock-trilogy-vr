#include "game/bioshock2r/body.h"

#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshock2r/patterns.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::b2r::body {
namespace {

const uint8_t* g_imageBase = nullptr;

// ---- knobs (overlay thread writes, game thread reads) ----------------------
// Defaults = BS1's shipped v0.3.0 feel: instant 1:1 follow, no deadzone.
std::atomic<bool> g_armed{true};
std::atomic<float> g_ratePerSec{0.0f};   // 0 = instant 1:1
std::atomic<float> g_deadzoneDeg{0.0f};
std::atomic<float> g_maxDegPerSec{180.0f}; // safety slew cap, not feel
std::atomic<int> g_field{0};               // 0 pc, 1 pawn, 2 both
std::atomic<bool> g_probeLog{false};
std::atomic<int32_t> g_pokeUnits{0}; // one-shot raw write test

// ---- telemetry (game thread writes, overlay reads) -------------------------
std::atomic<int> g_stateTlm{0};
std::atomic<float> g_residDegTlm{0.0f};
std::atomic<int32_t> g_committedPerSec{0};
std::atomic<uint32_t> g_skipView{0}, g_skipWrite{0}, g_resets{0};
std::atomic<int32_t> g_rotOffsetTlm{-1};

// ---- game-thread-only state ------------------------------------------------
enum State { kOff, kProbe, kRun, kDisabled };
State g_state = kOff;

void* g_lastPc = nullptr;
uint32_t g_lastPresent = 0;
bool g_havePrev = false;
int32_t g_prevGameYaw = 0;
int32_t g_expect = 0;
int g_probeTries = 0;
int g_quietFrames = 0;
LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_lastQpc{};
uint64_t g_lastProbeLogMs = 0;
uint64_t g_secWindowMs = 0;
int32_t g_secAccum = 0;
int g_runFrames = 0;
int32_t g_runCommanded = 0, g_runObserved = 0;
char g_disableReason[64] = {};

// ---- the DERIVED rotation-field offset (the one BS1 constant not copied) ---
// AActor's rotation offset is shared by every actor in the build, so deriving
// it on the PC serves the pawn too. Candidates = every aligned offset in
// [0, kScanBytes) whose int32 pair equals the engine's own PRE-drive
// (pitch, yaw) this frame; frames with different yaw values intersect the set
// until exactly one survives. Exact integer matches - no tolerance.
constexpr uint32_t kScanBytes = 0x400;
constexpr uint32_t kScanSlots = kScanBytes / 4; // 256 candidate offsets
int32_t g_rotOffset = -1;                       // -1 = underived
uint64_t g_cand[kScanSlots / 64] = {};          // candidate bitmask
bool g_candValid = false;
int g_deriveFrames = 0;
int32_t g_deriveLastYaw = 0;
bool g_deriveFailedLogged = false;

constexpr int32_t kMaxStepUnits = 4096;       // 22.5 deg blast-radius cap
constexpr int32_t kProbeUnits = 200;          // 1.1 deg - invisible if it fails
constexpr int32_t kProbeTolerance = 64;
constexpr int32_t kQuietUnits = 32;
constexpr int32_t kDiscontinuityUnits = 4096; // teleport / load / scripted slew

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read_rot(const void* obj, uint32_t off, int32_t out[3]) {
    __try {
        memcpy(out, static_cast<const uint8_t*>(obj) + off, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Yaw is the SECOND int32 of an FRotator {pitch, yaw, roll}. Pitch is never
// touched (the engine clamps it; the pawn keeps pitch 0 by design).
bool write_yaw(void* obj, uint32_t rotOff, int32_t yaw) {
    __try {
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) + rotOff + 4) = yaw;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

const char* state_name(State s) {
    switch (s) {
        case kOff: return "off";
        case kProbe: return "PROBE";
        case kRun: return "RUN";
        default: return "DISABLED";
    }
}

void set_state(State s) {
    g_state = s;
    g_stateTlm.store(static_cast<int>(s), std::memory_order_relaxed);
}

void disable(const char* why) {
    _snprintf_s(g_disableReason, sizeof g_disableReason, _TRUNCATE, "%s", why);
    set_state(kDisabled);
    BVR_LOG("[vrbody] DISABLED: %s - the body write does not stick on this "
            "build. Camera and hand mapping are untouched (the probe undid "
            "itself); `vrbody status` for details.",
            why);
}

// One derivation step. Returns true once the offset is banked.
bool derive_offset(void* pc, int32_t pitchUnits, int32_t yawUnits) {
    if (g_rotOffset >= 0) return true;
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, kScanBytes + 8)) return false;

    uint64_t frame[kScanSlots / 64] = {};
    int hits = 0;
    for (uint32_t o = 0; o < kScanBytes; o += 4) {
        int32_t v[3];
        if (!read_rot(pc, o, v)) return false;
        if (v[0] == pitchUnits && v[1] == yawUnits) {
            frame[(o / 4) / 64] |= 1ull << ((o / 4) % 64);
            ++hits;
        }
    }
    if (hits == 0) {
        // No pair matched this frame - the PC does not mirror the view rot
        // right now (cutscene, transition). Keep the accumulated set.
        return false;
    }
    if (!g_candValid) {
        memcpy(g_cand, frame, sizeof g_cand);
        g_candValid = true;
        g_deriveFrames = 1;
        g_deriveLastYaw = yawUnits;
        return false;
    }
    for (size_t i = 0; i < _countof(g_cand); ++i) g_cand[i] &= frame[i];

    int remaining = 0;
    int32_t offset = -1;
    for (uint32_t s = 0; s < kScanSlots; ++s)
        if (g_cand[s / 64] & (1ull << (s % 64))) {
            ++remaining;
            offset = static_cast<int32_t>(s * 4);
        }
    if (remaining == 0) {
        // Contradiction - restart the accumulation (log once).
        if (!g_deriveFailedLogged) {
            g_deriveFailedLogged = true;
            BVR_LOG("[vrbody] rotation-offset derivation restarted (candidate set "
                    "emptied - view rot does not mirror a stable PC field yet)");
        }
        g_candValid = false;
        g_deriveFrames = 0;
        return false;
    }
    if (yawUnits != g_deriveLastYaw) {
        ++g_deriveFrames;
        g_deriveLastYaw = yawUnits;
    }
    // Accept only after 3 frames with DISTINCT yaws agree on exactly one slot.
    if (remaining == 1 && g_deriveFrames >= 3) {
        g_rotOffset = offset;
        g_rotOffsetTlm.store(offset, std::memory_order_relaxed);
        BVR_LOG("[vrbody] actor rotation offset DERIVED: +0x%X on the PC "
                "(pitch/yaw matched across %d distinct-yaw frames) - probing "
                "whether a write sticks",
                offset, g_deriveFrames);
        return true;
    }
    return false;
}

// Commit `units` to the body facing. Returns what actually landed.
int32_t commit(void* pc, void* pawn, int32_t units) {
    if (units == 0 || g_rotOffset < 0) return 0;
    uint32_t off = static_cast<uint32_t>(g_rotOffset);
    int field = g_field.load(std::memory_order_relaxed);
    bool wantPc = (field == 0 || field == 2);
    bool wantPawn = (field == 1 || field == 2);
    bool ok = false;

    if (wantPc && pc) {
        int32_t rot[3];
        if (read_rot(pc, off, rot) && write_yaw(pc, off, (rot[1] + units) & 0xFFFF))
            ok = true;
    }
    if (wantPawn && pawn) {
        int32_t rot[3];
        if (read_rot(pawn, off, rot) && write_yaw(pawn, off, (rot[1] + units) & 0xFFFF))
            ok = true;
    }
    if (!ok) {
        g_skipWrite.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return units;
}

int32_t wanted_step(int32_t residualUnits, float dt) {
    float dz = g_deadzoneDeg.load(std::memory_order_relaxed) * kRotUnitsPerDegree;
    float r = static_cast<float>(residualUnits);
    if (dz > 0.0f) {
        if (fabsf(r) <= dz) return 0;
        r -= (r > 0.0f ? dz : -dz); // no jump at the band edge
    }
    float rate = g_ratePerSec.load(std::memory_order_relaxed);
    if (rate > 0.0f) r *= (1.0f - expf(-rate * dt));

    float cap = g_maxDegPerSec.load(std::memory_order_relaxed) * kRotUnitsPerDegree * dt;
    if (cap > 0.0f) {
        if (r > cap) r = cap;
        if (r < -cap) r = -cap;
    }
    int32_t u = static_cast<int32_t>(lroundf(r));
    if (u > kMaxStepUnits) u = kMaxStepUnits;
    if (u < -kMaxStepUnits) u = -kMaxStepUnits;
    return u;
}

// STRICT gameplay-view guard. KNOWN BS2 LIMIT: the menu-attract scene also
// presents an AShockPlayer view (unlike BS1, where the attract class differs),
// so this cannot exclude it - the vrDriving gate and the PC-change reset are
// what bound the damage there (a few cosmetic attract-yaw writes at worst).
bool is_gameplay_view(void* viewActor) {
    if (!viewActor) return false;
    void* vtbl = nullptr;
    if (!read_ptr(viewActor, &vtbl)) return false;
    return to_rva(vtbl) == patterns::kShockPlayerVtableRva;
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    QueryPerformanceFrequency(&g_qpcFreq);
}

void on_reset(const char* why) {
    g_havePrev = false;
    g_expect = 0;
    g_probeTries = 0;
    g_quietFrames = 0;
    g_runFrames = 0;
    g_runCommanded = g_runObserved = 0;
    if (g_state != kDisabled)
        set_state(g_armed.load(std::memory_order_relaxed) ? kProbe : kOff);
    g_resets.fetch_add(1, std::memory_order_relaxed);
    if (why) BVR_LOG("[vrbody] state reset (%s) -> %s", why, state_name(g_state));
}

int32_t on_calcview(void* pc, void* viewActor, int32_t gameYawUnits,
                    int32_t pitchUnits, int32_t residualUnits, bool vrDriving) {
    // Once per rendered frame (CalcView fires far above frame rate).
    uint32_t present = static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    if (present == g_lastPresent) return 0;
    g_lastPresent = present;

    uint64_t nowMs = GetTickCount64();

    if (pc != g_lastPc) {
        g_lastPc = pc;
        on_reset("player controller changed");
    }

    int32_t pcRot[3] = {0, 0, 0}, pawnRot[3] = {0, 0, 0};
    bool havePc = false, havePawn = false;
    if (g_rotOffset >= 0) {
        uint32_t off = static_cast<uint32_t>(g_rotOffset);
        havePc = pc && read_rot(pc, off, pcRot);
        havePawn = viewActor && read_rot(viewActor, off, pawnRot);
    }

    int32_t dG = g_havePrev ? wrap_rot(gameYawUnits - g_prevGameYaw) : 0;

    if (g_probeLog.load(std::memory_order_relaxed) && nowMs - g_lastProbeLogMs >= 500) {
        g_lastProbeLogMs = nowMs;
        BVR_LOG("[vrbody] pc=%p G=%d (dG=%+d, expect %+d) off=%+d | PC.rot=(%d %d %d)%s "
                "| pawn=%p rot=(%d %d %d)%s | resid=%+d (%.2f deg) | %s view=%s",
                pc, gameYawUnits, dG, g_expect, g_rotOffset, pcRot[0], pcRot[1],
                pcRot[2], havePc ? "" : " -", viewActor, pawnRot[0], pawnRot[1],
                pawnRot[2], havePawn ? "" : " -", residualUnits,
                residualUnits / kRotUnitsPerDegree, state_name(g_state),
                is_gameplay_view(viewActor) ? "gameplay" : "OTHER");
    }

    g_residDegTlm.store(residualUnits / kRotUnitsPerDegree, std::memory_order_relaxed);

    // `vrbody poke <deg>`: raw additive write, deliberately NOT absorbed - a
    // camera swing that STAYS swung proves the field is the authority.
    int32_t poke = g_pokeUnits.exchange(0, std::memory_order_relaxed);
    if (poke != 0 && g_rotOffset >= 0) {
        int32_t landed = commit(pc, viewActor, poke);
        BVR_LOG("[vrbody] poke %+d units: %s (G was %d, PC.yaw was %d, pawn.yaw was "
                "%d) - G should hold at %d",
                poke, landed ? "written" : "WRITE FAILED", gameYawUnits, pcRot[1],
                pawnRot[1], wrap_rot(gameYawUnits + poke) & 0xFFFF);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        return 0;
    }

    if (nowMs - g_secWindowMs >= 1000) {
        g_secWindowMs = nowMs;
        g_committedPerSec.store(g_secAccum, std::memory_order_relaxed);
        g_secAccum = 0;
    }

    // A discontinuity we did not command must not read as "our write failed".
    if (g_havePrev && g_expect == 0 && abs(dG) > kDiscontinuityUnits) {
        g_prevGameYaw = gameYawUnits;
        on_reset("yaw discontinuity");
        return 0;
    }

    bool armed = g_armed.load(std::memory_order_relaxed);
    if (!armed) {
        if (g_state != kDisabled && g_state != kOff) set_state(kOff);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }
    if (g_state == kOff) on_reset("armed");
    if (g_state == kDisabled) {
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        return 0;
    }

    if (!vrDriving) {
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }
    if (!is_gameplay_view(viewActor)) {
        g_skipView.fetch_add(1, std::memory_order_relaxed);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }

    // The BS2-specific rung: derive the rotation offset before anything runs.
    if (g_rotOffset < 0) {
        derive_offset(pc, pitchUnits, gameYawUnits);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        return 0;
    }

    float dt = 0.016f;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    if (g_lastQpc.QuadPart && g_qpcFreq.QuadPart)
        dt = static_cast<float>(qpc.QuadPart - g_lastQpc.QuadPart) /
             static_cast<float>(g_qpcFreq.QuadPart);
    g_lastQpc = qpc;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;

    int32_t committed = 0;

    if (g_state == kProbe) {
        if (g_expect != 0) {
            // Verdict frame: did last frame's 1.1 deg actually move the body?
            if (abs(dG - g_expect) <= kProbeTolerance) {
                set_state(kRun);
                g_probeTries = 0;
                BVR_LOG("[vrbody] probe CONFIRMED (asked %+d units, body moved %+d) - "
                        "the write is the authority. Transfer live (offset +0x%X).",
                        g_expect, dG, g_rotOffset);
            } else if (++g_probeTries >= 3) {
                committed = -g_expect; // undo: shipped behaviour stays identical
                g_expect = 0;
                g_havePrev = true;
                g_prevGameYaw = gameYawUnits;
                g_secAccum += committed;
                disable("body yaw write did not stick (3 probes)");
                return committed;
            } else {
                committed = -g_expect;
                BVR_LOG("[vrbody] probe %d/3 failed (asked %+d, body moved %+d) - "
                        "retrying",
                        g_probeTries, g_expect, dG);
                g_quietFrames = 0;
            }
            g_expect = 0;
        } else {
            // Wait for the player to stop turning first.
            if (abs(dG) < kQuietUnits) ++g_quietFrames; else g_quietFrames = 0;
            if (g_quietFrames >= 2 && abs(residualUnits) > kProbeUnits) {
                int32_t step = residualUnits > 0 ? kProbeUnits : -kProbeUnits;
                committed = commit(pc, viewActor, step);
                g_expect = committed;
            }
        }
    } else if (g_state == kRun) {
        int32_t step = wanted_step(residualUnits, dt);
        committed = commit(pc, viewActor, step);
        g_expect = committed;

        g_runCommanded += abs(g_expect);
        g_runObserved += abs(dG);
        if (++g_runFrames >= 30) {
            if (g_runCommanded > 500 && g_runObserved * 4 < g_runCommanded) {
                BVR_LOG("[vrbody] commanded %d units over 30 frames but the body "
                        "moved %d - re-probing",
                        g_runCommanded, g_runObserved);
                on_reset("body stopped responding");
            }
            g_runFrames = 0;
            g_runCommanded = g_runObserved = 0;
        }
    }

    g_havePrev = true;
    g_prevGameYaw = gameYawUnits;
    g_secAccum += committed;
    return committed;
}

bool enabled() { return g_armed.load(std::memory_order_relaxed); }

float rate_per_sec() { return g_ratePerSec.load(std::memory_order_relaxed); }
float deadzone_deg() { return g_deadzoneDeg.load(std::memory_order_relaxed); }

void set_tuning(float ratePerSec, float deadzoneDeg) {
    g_ratePerSec.store(ratePerSec < 0.0f ? 0.0f : ratePerSec, std::memory_order_relaxed);
    g_deadzoneDeg.store(deadzoneDeg < 0.0f ? 0.0f : deadzoneDeg,
                        std::memory_order_relaxed);
}

void handle_command(const char* args) {
    float v = 0.0f;
    char word[16] = {};

    if (strncmp(args, "status", 6) == 0) {
        BVR_LOG("[vrbody] %s%s | offset=%+d | rate=%.2f/s (%s) deadzone=%.1f deg "
                "max=%.0f deg/s | field=%s | resid=%.2f deg | committed=%d units/s | "
                "skips: view=%u write=%u resets=%u",
                state_name(g_state), g_state == kDisabled ? g_disableReason : "",
                g_rotOffset,
                g_ratePerSec.load(std::memory_order_relaxed),
                g_ratePerSec.load(std::memory_order_relaxed) <= 0.0f ? "instant 1:1"
                                                                     : "smoothed",
                g_deadzoneDeg.load(std::memory_order_relaxed),
                g_maxDegPerSec.load(std::memory_order_relaxed),
                g_field.load(std::memory_order_relaxed) == 0   ? "pc"
                : g_field.load(std::memory_order_relaxed) == 1 ? "pawn"
                                                               : "both",
                g_residDegTlm.load(std::memory_order_relaxed),
                g_committedPerSec.load(std::memory_order_relaxed),
                g_skipView.load(std::memory_order_relaxed),
                g_skipWrite.load(std::memory_order_relaxed),
                g_resets.load(std::memory_order_relaxed));
    } else if (strncmp(args, "probe", 5) == 0) {
        bool on = strstr(args, "off") == nullptr;
        g_probeLog.store(on, std::memory_order_relaxed);
        BVR_LOG("[vrbody] probe telemetry %s", on ? "ON (2 Hz)" : "off");
    } else if (strncmp(args, "rate", 4) == 0) {
        if (sscanf_s(args + 4, "%f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            g_ratePerSec.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] rate %.2f/s (%s)", v,
                    v <= 0.0f ? "instant 1:1" : "smoothed");
        }
    } else if (strncmp(args, "deadzone", 8) == 0) {
        if (sscanf_s(args + 8, "%f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            g_deadzoneDeg.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] deadzone %.1f deg", v);
        }
    } else if (strncmp(args, "max", 3) == 0) {
        if (sscanf_s(args + 3, "%f", &v) == 1) {
            if (v < 1.0f) v = 1.0f;
            g_maxDegPerSec.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] slew cap %.0f deg/s", v);
        }
    } else if (strncmp(args, "field", 5) == 0) {
        if (sscanf_s(args + 5, "%15s", word, static_cast<unsigned>(sizeof word)) == 1) {
            int f = strcmp(word, "pawn") == 0 ? 1 : (strcmp(word, "both") == 0 ? 2 : 0);
            g_field.store(f, std::memory_order_relaxed);
            on_reset("field changed");
            BVR_LOG("[vrbody] write field = %s",
                    f == 0 ? "pc" : (f == 1 ? "pawn" : "both"));
        }
    } else if (strncmp(args, "poke", 4) == 0) {
        if (sscanf_s(args + 4, "%f", &v) == 1) {
            BVR_LOG("[vrbody] poke %+.1f deg queued (camera WILL swing - raw write "
                    "test, the recenter is deliberately not advanced)",
                    v);
            g_pokeUnits.store(static_cast<int32_t>(lroundf(v * kRotUnitsPerDegree)),
                              std::memory_order_relaxed);
        }
    } else {
        bool on = strncmp(args, "off", 3) != 0;
        g_armed.store(on, std::memory_order_relaxed);
        on_reset(on ? "armed" : "disarmed");
        BVR_LOG("[vrbody] body-follows-head yaw transfer %s", on ? "ON" : "off");
    }
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Body / locomotion (M7.5)")) return;

    bool on = g_armed.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Body follows head (stick-forward = look direction)", &on)) {
        g_armed.store(on, std::memory_order_relaxed);
        handle_command(on ? "on" : "off");
    }

    float rate = g_ratePerSec.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Follow rate (/s, 0 = instant)", &rate, 0.0f, 10.0f, "%.2f"))
        g_ratePerSec.store(rate, std::memory_order_relaxed);
    float dz = g_deadzoneDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Deadzone (deg)", &dz, 0.0f, 60.0f, "%.1f"))
        g_deadzoneDeg.store(dz, std::memory_order_relaxed);

    ImGui::Text("state %s   offset %+d   residual %.1f deg   %d units/s",
                state_name(static_cast<State>(g_stateTlm.load(std::memory_order_relaxed))),
                g_rotOffsetTlm.load(std::memory_order_relaxed),
                g_residDegTlm.load(std::memory_order_relaxed),
                g_committedPerSec.load(std::memory_order_relaxed));
}

} // namespace bvr::b2r::body
