#include "game/bioshockinf/edgelog.h"

#include "core/hooks/d3d11_hook.h"
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/frame_context.h"
#include "game/bioshockinf/hands.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/scenedraw.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <vector>

namespace bvr::bsi::edgelog {
namespace {

// ~30 Hz sampling: the headset test motion is a ~5 s hand traverse, so the
// signal lives well under 1 Hz; 30 Hz gives dense curves without the ring
// growing past ~13 MB. 32768 samples = ~18 minutes before wrap.
constexpr uint64_t kSampleIntervalMs = 33;
constexpr size_t kMaxSamples = 32768;

struct Sample {
    uint64_t tMs;          // GetTickCount64 at sample
    uint64_t present;      // d3d11 present counter
    uint32_t seq2;         // scenedraw second-pass sequence
    uint64_t snapStampMs;  // core snapshot fill time (0 = no snapshot)
    // Core EdgeViewSnapshot (located views + submitted tags + claim).
    float locPos[2][3], locQuat[2][4], locFov[2][4];
    float tagPos[2][3], tagQuat[2][4];
    float claimTanH, claimTanV;
    uint8_t snapValid;
    // Consumed head pose (XR space).
    float hp[7]; // px py pz qx qy qz qw
    uint8_t hpValid;
    // Frame context (written camera base) + final camera + per-eye cameras.
    float engineLoc[3], writtenLoc[3];
    int32_t gameYawUnits;
    float worldScale;
    uint8_t fcValid;
    float finalLoc[3];
    int32_t finalRot[3]; // pitch yaw roll
    uint8_t finalValid;
    float eyeLoc[2][3];
    uint8_t eyeValid[2];
    // The right hand's XR grip pose (the test hand).
    float grip[7];
    uint8_t gripValid;
    // Composed model targets, both hands.
    float gpLoc[2][3];
    int32_t gpRot[2][3];
    uint8_t gpValid[2];
    // Driven component LocalToWorld rows + translation (row-vector convention).
    float l2w[12]; // r0 r1 r2 t
    uint8_t l2wValid;
};

std::atomic<bool> g_on{false};
uint64_t g_lastSampleMs = 0; // game thread only
std::vector<Sample> g_buf;
size_t g_next = 0;
bool g_wrapped = false;
uint64_t g_armedAtMs = 0;

void write_row(FILE* f, const Sample& s) {
    fprintf(f, "%llu\t%llu\t%u\t%llu\t%u", static_cast<unsigned long long>(s.tMs),
            static_cast<unsigned long long>(s.present), s.seq2,
            static_cast<unsigned long long>(s.snapStampMs), s.snapValid);
    for (int e = 0; e < 2; ++e) {
        fprintf(f, "\t%.6f\t%.6f\t%.6f", s.locPos[e][0], s.locPos[e][1], s.locPos[e][2]);
        fprintf(f, "\t%.6f\t%.6f\t%.6f\t%.6f", s.locQuat[e][0], s.locQuat[e][1],
                s.locQuat[e][2], s.locQuat[e][3]);
        fprintf(f, "\t%.6f\t%.6f\t%.6f\t%.6f", s.locFov[e][0], s.locFov[e][1],
                s.locFov[e][2], s.locFov[e][3]);
    }
    for (int e = 0; e < 2; ++e) {
        fprintf(f, "\t%.6f\t%.6f\t%.6f", s.tagPos[e][0], s.tagPos[e][1], s.tagPos[e][2]);
        fprintf(f, "\t%.6f\t%.6f\t%.6f\t%.6f", s.tagQuat[e][0], s.tagQuat[e][1],
                s.tagQuat[e][2], s.tagQuat[e][3]);
    }
    fprintf(f, "\t%.6f\t%.6f", s.claimTanH, s.claimTanV);
    fprintf(f, "\t%u", s.hpValid);
    for (int k = 0; k < 7; ++k) fprintf(f, "\t%.6f", s.hp[k]);
    fprintf(f, "\t%u\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%.4f", s.fcValid,
            s.engineLoc[0], s.engineLoc[1], s.engineLoc[2], s.writtenLoc[0],
            s.writtenLoc[1], s.writtenLoc[2], s.gameYawUnits, s.worldScale);
    fprintf(f, "\t%u\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d", s.finalValid, s.finalLoc[0],
            s.finalLoc[1], s.finalLoc[2], s.finalRot[0], s.finalRot[1], s.finalRot[2]);
    for (int e = 0; e < 2; ++e)
        fprintf(f, "\t%u\t%.3f\t%.3f\t%.3f", s.eyeValid[e], s.eyeLoc[e][0], s.eyeLoc[e][1],
                s.eyeLoc[e][2]);
    fprintf(f, "\t%u", s.gripValid);
    for (int k = 0; k < 7; ++k) fprintf(f, "\t%.6f", s.grip[k]);
    for (int h = 0; h < 2; ++h)
        fprintf(f, "\t%u\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d", s.gpValid[h], s.gpLoc[h][0],
                s.gpLoc[h][1], s.gpLoc[h][2], s.gpRot[h][0], s.gpRot[h][1], s.gpRot[h][2]);
    fprintf(f, "\t%u", s.l2wValid);
    for (int k = 0; k < 12; ++k) fprintf(f, "\t%.4f", s.l2w[k]);
    fputc('\n', f);
}

void flush_to_file() {
    const size_t count = g_wrapped ? g_buf.size() : g_next;
    if (count == 0) {
        BVR_LOG("[bsi] edgelog: nothing to flush (0 samples)");
        return;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\edgelog-%llu.tsv", bvr::log::data_dir(),
                 static_cast<unsigned long long>(g_armedAtMs));
    FILE* f = _wfsopen(path, L"w", _SH_DENYWR);
    if (!f) {
        BVR_LOG("[bsi] edgelog: could not open the dump file - samples kept in memory");
        return;
    }
    fprintf(f,
            "tMs\tpresent\tseq2\tsnapStampMs\tsnapValid"
            "\tlocPosLx\tlocPosLy\tlocPosLz\tlocQuatLx\tlocQuatLy\tlocQuatLz\tlocQuatLw"
            "\tlocFovLl\tlocFovLr\tlocFovLu\tlocFovLd"
            "\tlocPosRx\tlocPosRy\tlocPosRz\tlocQuatRx\tlocQuatRy\tlocQuatRz\tlocQuatRw"
            "\tlocFovRl\tlocFovRr\tlocFovRu\tlocFovRd"
            "\ttagPosLx\ttagPosLy\ttagPosLz\ttagQuatLx\ttagQuatLy\ttagQuatLz\ttagQuatLw"
            "\ttagPosRx\ttagPosRy\ttagPosRz\ttagQuatRx\ttagQuatRy\ttagQuatRz\ttagQuatRw"
            "\tclaimTanH\tclaimTanV"
            "\thpValid\thpPx\thpPy\thpPz\thpQx\thpQy\thpQz\thpQw"
            "\tfcValid\tengineLocX\tengineLocY\tengineLocZ\twrittenLocX\twrittenLocY"
            "\twrittenLocZ\tgameYawUnits\tworldScale"
            "\tfinalValid\tfinalLocX\tfinalLocY\tfinalLocZ\tfinalPitch\tfinalYaw\tfinalRoll"
            "\teyeLValid\teyeLx\teyeLy\teyeLz\teyeRValid\teyeRx\teyeRy\teyeRz"
            "\tgripValid\tgripPx\tgripPy\tgripPz\tgripQx\tgripQy\tgripQz\tgripQw"
            "\tgpLValid\tgpLLocX\tgpLLocY\tgpLLocZ\tgpLPitch\tgpLYaw\tgpLRoll"
            "\tgpRValid\tgpRLocX\tgpRLocY\tgpRLocZ\tgpRPitch\tgpRYaw\tgpRRoll"
            "\tl2wValid\tl2wR0x\tl2wR0y\tl2wR0z\tl2wR1x\tl2wR1y\tl2wR1z"
            "\tl2wR2x\tl2wR2y\tl2wR2z\tl2wTx\tl2wTy\tl2wTz\n");
    // Chronological order: the oldest sample is at g_next once wrapped.
    if (g_wrapped)
        for (size_t i = g_next; i < g_buf.size(); ++i) write_row(f, g_buf[i]);
    for (size_t i = 0; i < g_next; ++i) write_row(f, g_buf[i]);
    fclose(f);
    BVR_LOG("[bsi] edgelog: flushed %zu samples (%s) to %ls", count,
            g_wrapped ? "ring wrapped - oldest dropped" : "no wrap", path);
}

} // namespace

void tick(uint64_t nowMs) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    if (nowMs - g_lastSampleMs < kSampleIntervalMs) return;
    g_lastSampleMs = nowMs;

    Sample s{};
    s.tMs = nowMs;
    s.present = bvr::d3d11_hook::present_count();
    s.seq2 = scenedraw::second_pass_seq();

    bvr::vr::EdgeViewSnapshot snap;
    if (bvr::vr::get_edge_snapshot(snap)) {
        s.snapValid = 1;
        s.snapStampMs = snap.stampMs;
        memcpy(s.locPos, snap.locPos, sizeof s.locPos);
        memcpy(s.locQuat, snap.locQuat, sizeof s.locQuat);
        memcpy(s.locFov, snap.locFov, sizeof s.locFov);
        memcpy(s.tagPos, snap.tagPos, sizeof s.tagPos);
        memcpy(s.tagQuat, snap.tagQuat, sizeof s.tagQuat);
        s.claimTanH = snap.claimTanH;
        s.claimTanV = snap.claimTanV;
    }

    bvr::vr::HeadPose hp;
    if (camera::last_head_pose(hp)) {
        s.hpValid = 1;
        s.hp[0] = hp.px; s.hp[1] = hp.py; s.hp[2] = hp.pz;
        s.hp[3] = hp.qx; s.hp[4] = hp.qy; s.hp[5] = hp.qz; s.hp[6] = hp.qw;
    }

    const FrameContext& fc = camera::frame_context();
    if (fc.valid) {
        s.fcValid = 1;
        s.engineLoc[0] = fc.engineLocX; s.engineLoc[1] = fc.engineLocY;
        s.engineLoc[2] = fc.engineLocZ;
        s.writtenLoc[0] = fc.writtenLocX; s.writtenLoc[1] = fc.writtenLocY;
        s.writtenLoc[2] = fc.writtenLocZ;
        s.gameYawUnits = fc.gameYawUnits;
        s.worldScale = fc.worldScale;
    }

    FVector floc;
    FRotator frot;
    if (camera::final_camera(floc, frot)) {
        s.finalValid = 1;
        s.finalLoc[0] = floc.x; s.finalLoc[1] = floc.y; s.finalLoc[2] = floc.z;
        s.finalRot[0] = frot.pitch; s.finalRot[1] = frot.yaw; s.finalRot[2] = frot.roll;
    }
    for (int e = 0; e < 2; ++e) {
        FVector el;
        if (camera::eye_loc(e, el)) {
            s.eyeValid[e] = 1;
            s.eyeLoc[e][0] = el.x; s.eyeLoc[e][1] = el.y; s.eyeLoc[e][2] = el.z;
        }
    }

    bvr::vr::HeadPose grip;
    if (bvr::vr::get_hand_pose(1, /*aimPose=*/false, grip)) {
        s.gripValid = 1;
        s.grip[0] = grip.px; s.grip[1] = grip.py; s.grip[2] = grip.pz;
        s.grip[3] = grip.qx; s.grip[4] = grip.qy; s.grip[5] = grip.qz;
        s.grip[6] = grip.qw;
    }

    for (int h = 0; h < 2; ++h) {
        GamePose gp;
        if (hands::last_model_target(h, gp)) {
            s.gpValid[h] = 1;
            s.gpLoc[h][0] = gp.loc.x; s.gpLoc[h][1] = gp.loc.y; s.gpLoc[h][2] = gp.loc.z;
            s.gpRot[h][0] = gp.rot.pitch; s.gpRot[h][1] = gp.rot.yaw;
            s.gpRot[h][2] = gp.rot.roll;
        }
    }

    const uint8_t* comp = static_cast<const uint8_t*>(bones::component());
    if (comp && bvr::pattern_scan::is_memory_valid(
                    comp + patterns::kSkelCompLocalToWorldOffset, 16 * sizeof(float))) {
        const float* m = reinterpret_cast<const float*>(
            comp + patterns::kSkelCompLocalToWorldOffset);
        s.l2wValid = 1;
        // rows 0-2 xyz + translation row (row-vector convention, 4x4).
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) s.l2w[r * 3 + c] = m[r * 4 + c];
    }

    if (g_buf.size() < kMaxSamples) {
        g_buf.push_back(s);
        g_next = g_buf.size() % kMaxSamples;
        if (g_buf.size() == kMaxSamples) g_next = 0;
    } else {
        g_buf[g_next] = s;
        g_next = (g_next + 1) % kMaxSamples;
        g_wrapped = true;
    }
}

void handle_verb(const char* rest) {
    while (*rest == ' ') ++rest;
    // NOTE: command.txt lines arrive with the trailing newline attached (the
    // recorder.h warning) - the token terminator set must include it.
    if (strncmp(rest, "on", 2) == 0 &&
        (rest[2] == '\0' || rest[2] == ' ' || rest[2] == '\n' || rest[2] == '\r')) {
        g_buf.clear();
        g_buf.reserve(4096); // grows as needed; full reserve would be ~13 MB
        g_next = 0;
        g_wrapped = false;
        g_armedAtMs = GetTickCount64();
        g_lastSampleMs = 0;
        bvr::vr::set_edge_snapshot(true);
        g_on.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] edgelog: ARMED (~30 Hz ring, %zu sample cap ~18 min; zero "
                "per-sample I/O - safe to leave on; `bsicam edgelog off` flushes "
                "the TSV)",
                kMaxSamples);
        return;
    }
    if (strncmp(rest, "off", 3) == 0) {
        const bool was = g_on.exchange(false, std::memory_order_relaxed);
        bvr::vr::set_edge_snapshot(false);
        if (was) flush_to_file();
        else BVR_LOG("[bsi] edgelog: already off");
        return;
    }
    BVR_LOG("[bsi] edgelog: %s | samples %zu%s | usage: bsicam edgelog on|off|status",
            g_on.load(std::memory_order_relaxed) ? "ARMED" : "off",
            g_wrapped ? g_buf.size() : g_next, g_wrapped ? " (wrapped)" : "");
}

} // namespace bvr::bsi::edgelog
