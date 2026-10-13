// arm-ik-audit.cpp - do BS1's arm solver (src/game/bioshock1r/arm_ik.h) and the Dishonored
// VR mod's (its src/game/dishonored/hands/arm_rig.h, compiled straight from that repo)
// put the arm in the same place for the same pose? Both run on their own game's real
// rig, driven by the SAME body-relative wrist targets, scaled to each arm's length.
//
//   .\tools\arm-ik-audit.ps1        # builds this against the Dishonored repo, runs it, renders
//   arm-ik-audit.exe <bs1-rig.txt> <dishonored_vr_arm_rig.bin> <out.json>
//
// What can and cannot match. The two rigs have different proportions (BS1's upper arm
// is 1.44x its forearm, Dishonored's 0.91x), so the elbow BEND differs for the same reach
// by geometry, not by solver. What must match is everything the solver decides: which
// way the elbow points around the shoulder-wrist line, how far the shoulder slides at
// the reach limits, how the wrist roll is tracked through 0..225 deg and where the
// elbow swivel engages. Exit 1 when any of those disagree past the stated tolerance.
#include "game/bioshock1r/arm_ik.h"
#include "game/dishonored/hands/arm_rig.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace b = bvr::b1r::arm_ik;
namespace d = dvr::ik;

struct BsBone {
    std::string name;
    int parent;
    b::Bone bone;
};
static bool load_bs(const char* path, std::vector<BsBone>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        BsBone r{};
        int idx = 0;
        char name[128] = {};
        float* p = r.bone.p;
        float* q = r.bone.q;
        float* s = r.bone.s;
        if (sscanf_s(line, "%d %127s %d %f %f %f %f %f %f %f %f %f %f", &idx, name, (unsigned)sizeof name,
                     &r.parent, &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3], &s[0], &s[1], &s[2]) != 13)
            break;
        r.name = name;
        out.push_back(r);
    }
    fclose(f);
    return !out.empty();
}
static int bs_find(const std::vector<BsBone>& r, const std::string& n) {
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i].name == n) return (int)i;
    return -1;
}
static bool bs_descends(const std::vector<BsBone>& r, int x, int root) {
    for (size_t n = 0; n <= r.size() && x >= 0; ++n) {
        if (x == root) return true;
        x = r[x].parent;
    }
    return false;
}

// Body axes per rig, from its own skeleton (see the audit's report header).
struct Frame {
    b::Vec F, R, U;
};
static const Frame kBs{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}};  // Havok space: spine +X, right +Y
static const Frame kDh{{0, 0, 1}, {-1, 0, 0}, {0, -1, 0}}; // feet y=0, head y=-180, toes +Z

struct PoseDef {
    const char* name;
    float f, r, u;   // wrist offset from the shoulder, in arm lengths (r is mirrored per side)
    float roll = 0;  // degrees about the forearm (mirrored per side)
};
static const PoseDef kPoses[] = {
    {"Forward, bent", 0.70f, 0.00f, -0.20f},
    {"Reach forward", 0.95f, 0.00f, 0.00f},
    {"Low, relaxed", 0.30f, 0.05f, -0.75f},
    {"Raised", 0.40f, 0.00f, 0.60f},
    {"Crossed", 0.55f, -0.45f, -0.10f},
    {"Wide", 0.30f, 0.75f, 0.00f},
    {"Close to the chest", 0.30f, -0.05f, -0.15f},
    {"Over-reach", 1.30f, 0.00f, 0.00f},
    {"Roll 0", 0.70f, 0.00f, -0.20f, 0},
    {"Roll 45", 0.70f, 0.00f, -0.20f, 45},
    {"Roll 90", 0.70f, 0.00f, -0.20f, 90},
    {"Roll 135", 0.70f, 0.00f, -0.20f, 135},
    {"Roll 180", 0.70f, 0.00f, -0.20f, 180},
    {"Roll 225", 0.70f, 0.00f, -0.20f, 225},
    {"Roll back to 0", 0.70f, 0.00f, -0.20f, 0},
};

static d::Vec dv(b::Vec v) { return {v.x, v.y, v.z}; }
static b::Vec bv(d::Vec v) { return {v.x, v.y, v.z}; }
static b::Vec body_of(const Frame& fr, b::Vec v, float side) { // world -> (f, r*side, u)
    return {b::dot(v, fr.F), side * b::dot(v, fr.R), b::dot(v, fr.U)};
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        puts("usage: arm-ik-audit <bs1-rig.txt> <dishonored_vr_arm_rig.bin> <out.json> [upperLength foreLength]");
        return 2;
    }
    // s86: BS1's segments at their own lengths (arm_ik.h Input::upperLength/foreLength).
    // At 0.71 / 1.13 BS1's proportions are Dishonored's, and the elbow gap - the one metric
    // the s81 audit said geometry alone kept apart - should close.
    const float upperLength = argc == 6 ? static_cast<float>(atof(argv[4])) : 1.0f;
    const float foreLength = argc == 6 ? static_cast<float>(atof(argv[5])) : 1.0f;
    std::vector<BsBone> bs;
    if (!load_bs(argv[1], bs)) return 3;
    FILE* f = nullptr;
    fopen_s(&f, argv[2], "rb");
    d::Rig dh;
    const bool ok = dh.load(f);
    if (f) fclose(f);
    if (!ok) {
        puts("*** cannot read the Dishonored rig");
        return 3;
    }
    d::Chain chains[2];
    if (!chains[0].build(dh, false) || !chains[1].build(dh, true)) return 4;

    struct BsArm {
        int idx[6];
        b::Ref ref;
        float L;
    } bsArm[2];
    const char* sides[2] = {"L", "R"};
    for (int h = 0; h < 2; ++h) {
        const std::string s = sides[h];
        const char* parts[6] = {"Clavicle", "UpperArm", "Forearm", "ForeTwist", "ForeTwist1", "Hand"};
        for (int k = 0; k < 6; ++k)
            if ((bsArm[h].idx[k] = bs_find(bs, "Bip01_" + s + "_" + parts[k])) < 0) return 4;
        b::Ref& r = bsArm[h].ref;
        r.clavicle = bs[bsArm[h].idx[0]].bone;
        r.upper = bs[bsArm[h].idx[1]].bone;
        r.fore = bs[bsArm[h].idx[2]].bone;
        r.twist[0] = bs[bsArm[h].idx[3]].bone;
        r.twist[1] = bs[bsArm[h].idx[4]].bone;
        memcpy(r.wristP, bs[bsArm[h].idx[5]].bone.p, 12);
        memcpy(r.wristQ, bs[bsArm[h].idx[5]].bone.q, 16);
        bsArm[h].L = b::length(b::vec(r.fore.p) - b::vec(r.upper.p)) * upperLength +
                     b::length(b::vec(r.wristP) - b::vec(r.fore.p)) * foreLength;
    }

    FILE* out = nullptr;
    if (fopen_s(&out, argv[3], "w") || !out) return 5;
    fprintf(out, "{\"bs1Bones\":[");
    for (size_t i = 0; i < bs.size(); ++i) fprintf(out, "%s\"%s\"", i ? "," : "", bs[i].name.c_str());
    fprintf(out, "],\"dhBones\":%zu,\"frames\":[\n", dh.bones.size());

    // History per rig and hand, carried exactly the same way for both.
    b::Vec bsPrior[2];
    float bsTwist[2] = {};
    d::Vec dhPrior[2];
    float dhTwist[2] = {};
    unsigned frame = 0;
    int failures = 0;
    float worstDir = 0, worstShift = 0, worstTrack = 0, worstSwivel = 0;
    std::string worstDirAt;
    for (size_t pi = 0; pi < sizeof kPoses / sizeof kPoses[0]; ++pi) {
        const PoseDef& cur = kPoses[pi];
        const PoseDef& prev = pi ? kPoses[pi - 1] : cur;
        for (int sub = 1; sub <= 10; ++sub) {
            const float t = sub / 10.0f;
            const float pf = prev.f + (cur.f - prev.f) * t, pr = prev.r + (cur.r - prev.r) * t,
                        pu = prev.u + (cur.u - prev.u) * t, proll = prev.roll + (cur.roll - prev.roll) * t;
            std::vector<b::Bone> bsPose(bs.size());
            for (size_t i = 0; i < bs.size(); ++i) {
                bsPose[i] = bs[i].bone;
                for (float& c : bsPose[i].s) c = 1;
            }
            std::vector<d::Xform> dhSkin(dh.bones.size());
            for (auto& m : dhSkin) {
                m.r = dvr::hf::identity3();
                m.t[0] = m.t[1] = m.t[2] = 0;
            }
            float metrics[2][8] = {};
            for (int h = 0; h < 2; ++h) {
                const float side = h ? 1.0f : -1.0f;
                const float roll = proll * b::kDeg * side;
                // ---- BS1 ----
                b::Vec bsElbowBody, bsDir;
                float bsShift = 0, bsTracked = 0, bsSw = 0;
                {
                    const BsArm& a = bsArm[h];
                    const Frame& fr = kBs;
                    const b::Vec S = b::vec(a.ref.upper.p);
                    const b::Vec W = S + (fr.F * pf + fr.R * (pr * side) + fr.U * pu) * a.L;
                    b::Input in;
                    in.shoulder = S;
                    in.wrist = W;
                    in.pole = fr.U * -1 + fr.R * (0.6f * side) + fr.F * -0.3f;
                    in.outward = fr.R * side;
                    in.priorPole = bsPrior[h];
                    in.priorTwist = bsTwist[h];
                    in.fresh = frame > 0;
                    in.upperLength = upperLength;
                    in.foreLength = foreLength;
                    b::Swing sw;
                    b::Output o;
                    if (!b::swing(a.ref, in, in.pole, sw)) { ++failures; continue; }
                    const b::Quat wq = b::mul(b::mul(b::axis_angle(sw.axis, roll), sw.fore), b::quat(a.ref.wristQ));
                    for (int i = 0; i < 4; ++i) in.wristQ[i] = wq.v[i];
                    if (!b::pose(a.ref, in, o)) { ++failures; continue; }
                    bsPrior[h] = o.basePole;
                    bsTwist[h] = o.trackedTwist;
                    const b::Ref& r = a.ref;
                    const b::Bone* ob[5] = {&o.clavicle, &o.upper, &o.fore, &o.twist[0], &o.twist[1]};
                    for (int k = 0; k < 5; ++k) bsPose[a.idx[k]] = *ob[k];
                    const b::Quat dq = b::mul(wq, b::conj(b::quat(r.wristQ)));
                    for (size_t bi = 0; bi < bs.size(); ++bi) {
                        if (!bs_descends(bs, (int)bi, a.idx[5])) continue;
                        b::put(W + b::rotate(dq, b::vec(bs[bi].bone.p) - b::vec(r.wristP)), bsPose[bi].p);
                        const b::Quat q = b::mul(dq, b::quat(bs[bi].bone.q));
                        for (int i = 0; i < 4; ++i) bsPose[bi].q[i] = q.v[i];
                    }
                    b::Vec n = W - o.joints.shoulder;
                    b::unit(n);
                    bsDir = b::across(o.joints.elbow - o.joints.shoulder, n);
                    b::unit(bsDir);
                    bsDir = body_of(fr, bsDir, side);
                    bsElbowBody = body_of(fr, (o.joints.elbow - S) * (1 / a.L), side);
                    bsShift = o.joints.shoulderShift / a.L;
                    bsTracked = o.trackedTwist;
                    bsSw = o.swivel;
                }
                // ---- Dishonored ----
                b::Vec dhDir, dhElbowBody;
                float dhShift = 0, dhTracked = 0, dhSw = 0;
                {
                    const d::Chain& c = chains[h];
                    const Frame& fr = kDh;
                    const d::Vec s0 = dh.bones[c.upper].head, e0 = dh.bones[c.lower].head, w0 = dh.bones[c.wrist].head;
                    const float L = d::length(e0 - s0) + d::length(w0 - e0);
                    const d::Vec S = s0;
                    const d::Vec W = S + dv(fr.F * pf + fr.R * (pr * side) + fr.U * pu) * L;
                    const d::Vec pole = dv(fr.U * -1 + fr.R * (0.6f * side) + fr.F * -0.3f);
                    const d::Vec outward = dv(fr.R * side);
                    // The wrist with no roll of its own: the forearm swing of pose_arm's first
                    // attempt, then rolled about the forearm - the same construction as BS1's.
                    d::Vec ue = e0 - s0, ew = w0 - e0, refNormal = d::cross(ue, ew);
                    if (!d::unit(refNormal)) refNormal = d::fallback(ue);
                    d::Solution sol;
                    if (!d::solve(S, W, pole, outward, dhPrior[h], d::length(ue), d::length(ew), 0.5f, sol)) { ++failures; continue; }
                    d::Vec normal = d::cross(sol.pole, W - sol.shoulder);
                    d::unit(normal);
                    const d::Mat3 fore = d::frame_delta(ew, refNormal, W - sol.elbow, normal);
                    d::Vec axis = W - sol.elbow;
                    d::unit(axis);
                    const d::Xform wrist = d::skin(dvr::hf::mul3(d::axis_angle(axis, roll), fore), 1, w0, W);
                    d::ArmPose ap;
                    if (!d::pose_arm(dh, c, S, pole, outward, dhPrior[h], dhTwist[h], frame > 0, wrist, 1, 0.5f, ap)) { ++failures; continue; }
                    dhPrior[h] = ap.joints.pole;
                    dhTwist[h] = ap.trackedTwist;
                    for (size_t bi = 0; bi < dh.bones.size(); ++bi) {
                        if (c.region[bi] == 3) dhSkin[bi] = wrist;
                        else if (c.region[bi]) dhSkin[bi] = ap.skin[bi];
                    }
                    d::Vec n = W - ap.joints.shoulder;
                    d::unit(n);
                    d::Vec dir = d::across(ap.joints.elbow - ap.joints.shoulder, n);
                    d::unit(dir);
                    dhDir = body_of(fr, bv(dir), side);
                    dhElbowBody = body_of(fr, bv(ap.joints.elbow - S) * (1 / L), side);
                    dhShift = ap.joints.shoulderShift / L;
                    dhTracked = ap.trackedTwist;
                    if (fabsf(dhTracked) > 80 * b::kDeg)
                        dhSw = std::clamp(dhTracked - std::copysign(80 * b::kDeg, dhTracked), -70 * b::kDeg, 70 * b::kDeg);
                }
                const float dirDeg = acosf(std::clamp(b::dot(bsDir, dhDir), -1.f, 1.f)) / b::kDeg;
                if (dirDeg > worstDir) {
                    worstDir = dirDeg;
                    worstDirAt = std::string(cur.name) + (h ? " (right)" : " (left)");
                }
                worstShift = std::max(worstShift, fabsf(bsShift - dhShift));
                worstTrack = std::max(worstTrack, fabsf(bsTracked - dhTracked) / b::kDeg);
                worstSwivel = std::max(worstSwivel, fabsf(bsSw - dhSw) / b::kDeg);
                float* m = metrics[h];
                m[0] = dirDeg;
                m[1] = bsShift;
                m[2] = dhShift;
                m[3] = bsTracked / b::kDeg;
                m[4] = dhTracked / b::kDeg;
                m[5] = bsSw / b::kDeg;
                m[6] = dhSw / b::kDeg;
                m[7] = b::length(bsElbowBody - dhElbowBody);
            }
            fprintf(out, "%s{\"frame\":%u,\"label\":\"%s\",\"key\":%s,\"metrics\":[", frame ? ",\n" : "", frame,
                    cur.name, sub == 10 ? "true" : "false");
            for (int h = 0; h < 2; ++h) {
                fprintf(out, "%s{\"elbowDirDeg\":%.3f,\"shiftBs\":%.4f,\"shiftDh\":%.4f,\"trackedBs\":%.2f,"
                             "\"trackedDh\":%.2f,\"swivelBs\":%.2f,\"swivelDh\":%.2f,\"elbowGap\":%.4f}",
                        h ? "," : "", metrics[h][0], metrics[h][1], metrics[h][2], metrics[h][3], metrics[h][4],
                        metrics[h][5], metrics[h][6], metrics[h][7]);
            }
            fprintf(out, "],\"bs1\":[");
            for (size_t i = 0; i < bsPose.size(); ++i) {
                const b::Bone& x = bsPose[i];
                fprintf(out, "%s[%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.7g,%.5g,%.5g,%.5g]", i ? "," : "", x.p[0], x.p[1],
                        x.p[2], x.q[0], x.q[1], x.q[2], x.q[3], x.s[0], x.s[1], x.s[2]);
            }
            fprintf(out, "],\"dh\":[");
            for (size_t i = 0; i < dhSkin.size(); ++i) {
                const d::Xform& x = dhSkin[i];
                fprintf(out, "%s[", i ? "," : "");
                for (int k = 0; k < 9; ++k) fprintf(out, "%s%.7g", k ? "," : "", x.r.m[k]);
                fprintf(out, ",%.6g,%.6g,%.6g]", x.t[0], x.t[1], x.t[2]);
            }
            fprintf(out, "]}");
            ++frame;
        }
    }
    fprintf(out, "],\"summary\":{\"frames\":%u,\"failures\":%d,\"worstElbowDirectionDeg\":%.3f,\"worstAt\":\"%s\","
                 "\"worstShoulderShiftDiff\":%.4f,\"worstTrackedTwistDiffDeg\":%.3f,\"worstSwivelDiffDeg\":%.3f}}\n",
            frame, failures, worstDir, worstDirAt.c_str(), worstShift, worstTrack, worstSwivel);
    fclose(out);
    printf("arm IK audit, BS1 vs Dishonored: %u frames, %d failures | elbow direction differs by at most %.2f deg "
           "(%s) | shoulder slide %.4f arm lengths | tracked roll %.2f deg | swivel %.2f deg\n",
           frame, failures, worstDir, worstDirAt.c_str(), worstShift, worstTrack, worstSwivel);
    return failures || worstDir > 2.0f || worstShift > 0.01f || worstTrack > 1.0f || worstSwivel > 1.0f ? 1 : 0;
}
