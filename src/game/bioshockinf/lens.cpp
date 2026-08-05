// I6 rung 2: the live lens decoder. See lens.h for the contract and the vote.

#include "game/bioshockinf/lens.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"

#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::bsi::lens {
namespace {

// The stride handed to core's tap. 251 UpdateSubresource cb uploads per frame
// were measured in a single dumped frame (ENGINE_NOTES s36); sampling every
// 16th keeps the armed cost to ~a QueryInterface+GetDesc pair per handful of
// draws while still filling a 64-sample round comfortably every second.
constexpr uint32_t kTapStride = 16;
constexpr uint32_t kRoundMs = 1000;
constexpr uint32_t kMaxDrain = bvr::frame_inspector::kCbTapSlots;
constexpr uint32_t kMaxClusters = 8;
// The vote thresholds (ROADMAP I6: refuse rather than publish a confident
// wrong value). A round needs enough evidence AND a clear winner.
constexpr uint32_t kMinValid = 16;
constexpr float kMajority = 0.60f;
constexpr float kRunnerUpMin = 0.10f;
// Structural gates, matching tools/decode-framedump.ps1 Decode-Matrix exactly
// (same instrument, two implementations - keep them aligned).
constexpr float kOrthoTol = 1e-3f;
constexpr float kTanMin = 0.05f;
constexpr float kTanMax = 4.0f;
// The load-bearing aspect gate: tanH/tanV must equal the backbuffer aspect
// (1 % relative). Letterboxed scenes and degenerate matrices both fail it -
// by design; those rounds refuse.
constexpr float kAspectTol = 0.01f;

std::atomic<bool> g_armed{false};
std::atomic<bool> g_track{false};
// F10 -> game thread: -1 none, 0 off, 1 on (the g_vrstereoPending shape).
std::atomic<int> g_armPending{-1};

// Game-thread state (tick runs on the camera thread only).
uint32_t g_cursor = 0;
uint64_t g_lastRoundMs = 0;

// Published lens 1 (majority winner) + lens 2 (named runner-up, telemetry
// only). Atomics so the render-thread overlay reads without a lock.
std::atomic<float> g_lens1TanH{0.0f}, g_lens1TanV{0.0f};
std::atomic<uint64_t> g_lens1StampMs{0};
std::atomic<uint32_t> g_lens1Support{0}; // percent of valid samples
std::atomic<float> g_lens2TanH{0.0f}, g_lens2TanV{0.0f};
std::atomic<uint32_t> g_lens2Support{0}; // percent; 0 = none this round
// Round telemetry.
std::atomic<uint32_t> g_rounds{0}, g_publishedRounds{0}, g_refusedRounds{0};
std::atomic<uint32_t> g_samplesSeen{0}, g_samplesValid{0};
std::atomic<float> g_lastClaimDeltaPct{0.0f};
uint64_t g_lastMismatchLogMs = 0;

// The matrix law, aligned with decode-framedump.ps1's Decode-Matrix: columns
// c0/c1/c3 of the row-major 4x4's upper 3 rows, mutually orthogonal within
// 1e-3, tangents in (0.05, 4.0). Returns false on any gate.
bool decode_sample(const float* f, float* outTanH, float* outTanV) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(f[i])) return false;
    const float c0[3] = {f[0], f[4], f[8]};
    const float c1[3] = {f[1], f[5], f[9]};
    const float c3[3] = {f[3], f[7], f[11]};
    const float n0 = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    const float n1 = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    const float n3 = sqrtf(c3[0] * c3[0] + c3[1] * c3[1] + c3[2] * c3[2]);
    if (n0 < 1e-6f || n1 < 1e-6f || n3 < 1e-6f) return false;
    const float d03 = (c0[0] * c3[0] + c0[1] * c3[1] + c0[2] * c3[2]) / (n0 * n3);
    const float d13 = (c1[0] * c3[0] + c1[1] * c3[1] + c1[2] * c3[2]) / (n1 * n3);
    const float d01 = (c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]) / (n0 * n1);
    if (fabsf(d03) > kOrthoTol || fabsf(d13) > kOrthoTol || fabsf(d01) > kOrthoTol)
        return false;
    const float tanH = n3 / n0;
    const float tanV = n3 / n1;
    if (tanH <= kTanMin || tanH >= kTanMax || tanV <= kTanMin || tanV >= kTanMax)
        return false;
    *outTanH = tanH;
    *outTanV = tanV;
    return true;
}

void close_round(uint64_t now) {
    float raw[kMaxDrain * bvr::frame_inspector::kCbTapFloats];
    const uint32_t n =
        bvr::frame_inspector::drain_cb_upload_samples(raw, kMaxDrain, &g_cursor);
    g_rounds.fetch_add(1, std::memory_order_relaxed);
    g_samplesSeen.fetch_add(n, std::memory_order_relaxed);
    if (n == 0) {
        g_refusedRounds.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    float aspect = 0.0f;
    unsigned w = 0, h = 0;
    if (bvr::hud::backbuffer_dims(&w, &h) && w > 0 && h > 0)
        aspect = static_cast<float>(w) / static_cast<float>(h);

    struct Cluster {
        int key = 0; // tanV quantized to 0.001
        uint32_t count = 0;
        float sumH = 0.0f, sumV = 0.0f;
    } clusters[kMaxClusters];
    uint32_t clusterCount = 0, valid = 0;

    for (uint32_t i = 0; i < n; ++i) {
        const float* f = raw + i * bvr::frame_inspector::kCbTapFloats +
                         patterns::kLensFloatIndex;
        float tanH = 0.0f, tanV = 0.0f;
        if (!decode_sample(f, &tanH, &tanV)) continue;
        if (aspect > 0.0f && fabsf(tanH / tanV - aspect) > aspect * kAspectTol)
            continue; // the load-bearing gate: frustum aspect must match the target
        ++valid;
        const int key = static_cast<int>(tanV * 1000.0f + 0.5f);
        uint32_t c = 0;
        for (; c < clusterCount; ++c)
            if (clusters[c].key == key) break;
        if (c == clusterCount) {
            if (clusterCount == kMaxClusters) continue; // overflow: drop, never merge
            clusters[clusterCount++].key = key;
        }
        clusters[c].count++;
        clusters[c].sumH += tanH;
        clusters[c].sumV += tanV;
    }
    g_samplesValid.fetch_add(valid, std::memory_order_relaxed);

    if (valid < kMinValid) {
        g_refusedRounds.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t top = 0, second = kMaxClusters;
    for (uint32_t c = 1; c < clusterCount; ++c)
        if (clusters[c].count > clusters[top].count) top = c;
    for (uint32_t c = 0; c < clusterCount; ++c) {
        if (c == top) continue;
        if (second == kMaxClusters || clusters[c].count > clusters[second].count)
            second = c;
    }
    const float topShare = static_cast<float>(clusters[top].count) / valid;
    if (topShare < kMajority) {
        g_refusedRounds.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const float tanH = clusters[top].sumH / clusters[top].count;
    const float tanV = clusters[top].sumV / clusters[top].count;
    g_lens1TanH.store(tanH, std::memory_order_relaxed);
    g_lens1TanV.store(tanV, std::memory_order_relaxed);
    g_lens1Support.store(static_cast<uint32_t>(topShare * 100.0f),
                         std::memory_order_relaxed);
    g_lens1StampMs.store(now, std::memory_order_relaxed);
    g_publishedRounds.fetch_add(1, std::memory_order_relaxed);

    if (second != kMaxClusters &&
        clusters[second].count >= static_cast<uint32_t>(valid * kRunnerUpMin)) {
        g_lens2TanH.store(clusters[second].sumH / clusters[second].count,
                          std::memory_order_relaxed);
        g_lens2TanV.store(clusters[second].sumV / clusters[second].count,
                          std::memory_order_relaxed);
        g_lens2Support.store(
            static_cast<uint32_t>(clusters[second].count * 100.0f / valid),
            std::memory_order_relaxed);
    } else {
        g_lens2Support.store(0, std::memory_order_relaxed);
    }

    // The audit: decoded truth vs the published claim. Track mode writes the
    // claim from the majority (the FOV lever still wins while armed - the
    // claim publish runs after this and derives from the lever).
    const float claim = camera::claim_tan_v();
    if (claim > 0.0f) {
        const float deltaPct = fabsf(tanV - claim) / claim * 100.0f;
        g_lastClaimDeltaPct.store(deltaPct, std::memory_order_relaxed);
        if (g_track.load(std::memory_order_relaxed)) {
            camera::set_claim_tan_v(tanV);
        } else if (deltaPct > 2.0f && now - g_lastMismatchLogMs >= 10000) {
            g_lastMismatchLogMs = now;
            BVR_LOG("[bsi][lens] CLAIM MISMATCH: decoded tanV=%.4f (support %u%%) vs "
                    "claim %.4f (%.1f%% off) - bsifov set/tanv to correct, or bsilens "
                    "track on",
                    tanV, g_lens1Support.load(std::memory_order_relaxed), claim, deltaPct);
        }
    }
}

void apply_arm(bool on) {
    if (on == g_armed.load(std::memory_order_relaxed)) return;
    g_armed.store(on, std::memory_order_relaxed);
    if (on) {
        bvr::frame_inspector::set_cb_upload_tap(patterns::kLensCbBytes, kTapStride);
        BVR_LOG("[bsi][lens] decoder ON: %u-byte tier, stride %u, %u ms rounds, "
                "majority %u%% of >=%u valid, aspect gate %.1f%%",
                patterns::kLensCbBytes, kTapStride, kRoundMs,
                static_cast<unsigned>(kMajority * 100), kMinValid, kAspectTol * 100.0f);
    } else {
        bvr::frame_inspector::set_cb_upload_tap(0, kTapStride);
        BVR_LOG("[bsi][lens] decoder off (rounds=%u published=%u refused=%u)",
                g_rounds.load(std::memory_order_relaxed),
                g_publishedRounds.load(std::memory_order_relaxed),
                g_refusedRounds.load(std::memory_order_relaxed));
    }
}

void log_status() {
    const uint64_t stamp = g_lens1StampMs.load(std::memory_order_relaxed);
    BVR_LOG("[bsi][lens] armed=%d track=%d rounds=%u published=%u refused=%u "
            "samples=%u valid=%u | lens1 tanH=%.4f tanV=%.4f support=%u%% age=%llu ms | "
            "lens2 tanH=%.4f tanV=%.4f support=%u%% | claim delta %.1f%%",
            g_armed.load(std::memory_order_relaxed) ? 1 : 0,
            g_track.load(std::memory_order_relaxed) ? 1 : 0,
            g_rounds.load(std::memory_order_relaxed),
            g_publishedRounds.load(std::memory_order_relaxed),
            g_refusedRounds.load(std::memory_order_relaxed),
            g_samplesSeen.load(std::memory_order_relaxed),
            g_samplesValid.load(std::memory_order_relaxed),
            g_lens1TanH.load(std::memory_order_relaxed),
            g_lens1TanV.load(std::memory_order_relaxed),
            g_lens1Support.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(stamp ? GetTickCount64() - stamp : 0),
            g_lens2TanH.load(std::memory_order_relaxed),
            g_lens2TanV.load(std::memory_order_relaxed),
            g_lens2Support.load(std::memory_order_relaxed),
            g_lastClaimDeltaPct.load(std::memory_order_relaxed));
}

} // namespace

void tick(uint64_t nowMs) {
    const int pending = g_armPending.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) apply_arm(pending != 0);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (nowMs - g_lastRoundMs < kRoundMs) return;
    g_lastRoundMs = nowMs;
    close_round(nowMs);
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsilens") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (strncmp(args, "on", 2) == 0) {
        // Seam commands already run on the game thread; apply directly.
        apply_arm(true);
    } else if (strncmp(args, "off", 3) == 0) {
        apply_arm(false);
    } else if (strncmp(args, "track", 5) == 0) {
        const char* sub = args + 5;
        while (*sub == ' ') ++sub;
        const bool on = strncmp(sub, "on", 2) == 0;
        g_track.store(on, std::memory_order_relaxed);
        BVR_LOG("[bsi][lens] track %s (%s)", on ? "ON" : "off",
                on ? "majority rounds write the claim; the FOV lever still wins while armed"
                   : "audit only - mismatches log, nothing is written");
    } else {
        log_status();
    }
    return true;
}

bool primary(float* tanH, float* tanV, uint64_t* ageMs) {
    const uint64_t stamp = g_lens1StampMs.load(std::memory_order_relaxed);
    if (!stamp) return false;
    if (tanH) *tanH = g_lens1TanH.load(std::memory_order_relaxed);
    if (tanV) *tanV = g_lens1TanV.load(std::memory_order_relaxed);
    if (ageMs) *ageMs = GetTickCount64() - stamp;
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Lens decoder (I6)")) return;
    {
        bool armed = g_armed.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Decode the live lens (cb tap)", &armed))
            g_armPending.store(armed ? 1 : 0, std::memory_order_relaxed);
        bool track = g_track.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Track: majority writes the claim", &track))
            g_track.store(track, std::memory_order_relaxed);
    }
    ImGui::Text("rounds %u  published %u  refused %u  (samples %u, valid %u)",
                g_rounds.load(std::memory_order_relaxed),
                g_publishedRounds.load(std::memory_order_relaxed),
                g_refusedRounds.load(std::memory_order_relaxed),
                g_samplesSeen.load(std::memory_order_relaxed),
                g_samplesValid.load(std::memory_order_relaxed));
    const uint64_t stamp = g_lens1StampMs.load(std::memory_order_relaxed);
    if (stamp) {
        ImGui::Text("lens1: tanH %.4f  tanV %.4f  support %u%%  age %llu ms",
                    g_lens1TanH.load(std::memory_order_relaxed),
                    g_lens1TanV.load(std::memory_order_relaxed),
                    g_lens1Support.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(GetTickCount64() - stamp));
        if (g_lens2Support.load(std::memory_order_relaxed) > 0)
            ImGui::Text("lens2 (runner-up): tanH %.4f  tanV %.4f  support %u%%",
                        g_lens2TanH.load(std::memory_order_relaxed),
                        g_lens2TanV.load(std::memory_order_relaxed),
                        g_lens2Support.load(std::memory_order_relaxed));
        ImGui::Text("claim delta %.1f%%",
                    g_lastClaimDeltaPct.load(std::memory_order_relaxed));
    } else {
        ImGui::TextDisabled("no majority published yet");
    }
    ImGui::TextDisabled("refused rounds keep the last lens (age shows); nothing is invented");
}

} // namespace bvr::bsi::lens
