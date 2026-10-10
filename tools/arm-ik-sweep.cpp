// arm-ik-sweep.cpp - run the PRODUCTION arm solver (src/game/bioshock1r/arm_ik.h) over a
// fixed set of arm poses on BS1's real rig, and write every bone's component-space
// transform per frame for tools\blender\arm_ik_bake.py to skin through the original
// weights. The Dishonored VR mod's offline arm validation (its tools/arm-ik-sweep.cpp),
// on this solver and this rig.
//
//   .\tools\arm-ik-sweep.ps1                 # builds this, runs it, bakes and renders
//   arm-ik-sweep.exe <rig.txt> <out.json> [lengthScale]
//
// The rig comes from `bs2gltf.py --rig-out` (the Havok reference pose, composed): game-
// derived, local only, and so is the output. Exit 1 when a pose fails or a length or the
// wrist join is off by more than 0.01 UU.
#include "game/bioshock1r/arm_ik.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bvr::b1r::arm_ik;

struct RigBone {
    std::string name;
    int parent = -1;
    Bone b;
};

static bool load_rig(const char* path, std::vector<RigBone>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        RigBone r;
        int idx = 0;
        char name[128] = {};
        float* p = r.b.p;
        float* q = r.b.q;
        float* s = r.b.s;
        if (sscanf_s(line, "%d %127s %d %f %f %f %f %f %f %f %f %f %f", &idx, name, (unsigned)sizeof name,
                     &r.parent, &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3], &s[0], &s[1], &s[2]) != 13 ||
            idx != (int)out.size()) {
            fclose(f);
            return false;
        }
        r.name = name;
        out.push_back(r);
    }
    fclose(f);
    return !out.empty();
}
static int find(const std::vector<RigBone>& rig, const char* name) {
    for (size_t i = 0; i < rig.size(); ++i)
        if (rig[i].name == name) return (int)i;
    return -1;
}
static bool descends(const std::vector<RigBone>& rig, int b, int root) {
    for (size_t n = 0; n <= rig.size() && b >= 0; ++n) {
        if (b == root) return true;
        b = rig[b].parent;
    }
    return false;
}

struct Arm {
    int clav, upper, fore, tw0, tw1, hand;
    Ref ref;
};

struct PoseDef {
    const char* name;
    Vec off[2];       // wrist offset from its reference position: forward, right, up (UU)
    float roll = 0;   // wrist roll about the forearm, degrees (mirrored on the left)
    float length = 1; // arm length scale
    float scale = 1;  // hand scale
    bool beyond = false; // past a wrist's real range: continuity is not judged
};

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        puts("usage: arm-ik-sweep <rig.txt> <out.json> [lengthScale]");
        return 2;
    }
    const float multiplier = argc == 4 ? (float)atof(argv[3]) : 1.0f;
    std::vector<RigBone> rig;
    if (!load_rig(argv[1], rig)) {
        printf("*** cannot read the rig %s\n", argv[1]);
        return 3;
    }
    Arm arms[2];
    const char* side[2] = {"L", "R"};
    for (int h = 0; h < 2; ++h) {
        char n[6][64];
        snprintf(n[0], 64, "Bip01_%s_Clavicle", side[h]);
        snprintf(n[1], 64, "Bip01_%s_UpperArm", side[h]);
        snprintf(n[2], 64, "Bip01_%s_Forearm", side[h]);
        snprintf(n[3], 64, "Bip01_%s_ForeTwist", side[h]);
        snprintf(n[4], 64, "Bip01_%s_ForeTwist1", side[h]);
        snprintf(n[5], 64, "Bip01_%s_Hand", side[h]);
        Arm& a = arms[h];
        int* ix[6] = {&a.clav, &a.upper, &a.fore, &a.tw0, &a.tw1, &a.hand};
        for (int k = 0; k < 6; ++k)
            if ((*ix[k] = find(rig, n[k])) < 0) {
                printf("*** the rig has no %s\n", n[k]);
                return 4;
            }
        a.ref.clavicle = rig[a.clav].b;
        a.ref.upper = rig[a.upper].b;
        a.ref.fore = rig[a.fore].b;
        a.ref.twist[0] = rig[a.tw0].b;
        a.ref.twist[1] = rig[a.tw1].b;
        memcpy(a.ref.wristP, rig[a.hand].b.p, sizeof a.ref.wristP);
        memcpy(a.ref.wristQ, rig[a.hand].b.q, sizeof a.ref.wristQ);
    }
    // Body axes in the Havok reference space (measured on NEWPlayerHands): the spine runs
    // along +X, the right shoulder is on +Y, and the reference arms reach along -Z.
    const Vec F{0, 0, -1}, R{0, 1, 0}, U{1, 0, 0};
    auto body = [&](Vec o) { return F * o.x + R * o.y + U * o.z; };

    const PoseDef poses[] = {
        {"Reference", {{0, 0, 0}, {0, 0, 0}}},
        {"Relaxed", {{-25, 0, -25}, {-25, 0, -25}}},
        {"Forward", {{-8, 4, 5}, {-8, -4, 5}}},
        {"Close", {{-40, 6, 10}, {-40, -6, 10}}},
        {"Crossed", {{-20, 40, 5}, {-20, -40, 5}}},
        {"Raised", {{-25, 0, 40}, {-25, 0, 40}}},
        {"Wide", {{-30, -45, 0}, {-30, 45, 0}}},
        {"Down", {{-45, -5, -35}, {-45, 5, -35}}},
        {"Behind", {{-70, -10, -15}, {-70, 10, -15}}},
        {"Asymmetric", {{-25, 0, -25}, {-25, 0, 40}}},
        {"Roll 0", {{-15, 0, 0}, {-15, 0, 0}}, 0},
        {"Roll 45", {{-15, 0, 0}, {-15, 0, 0}}, 45},
        {"Roll 90", {{-15, 0, 0}, {-15, 0, 0}}, 90},
        {"Roll 135", {{-15, 0, 0}, {-15, 0, 0}}, 135},
        {"Roll 179", {{-15, 0, 0}, {-15, 0, 0}}, 179},
        {"Roll 181", {{-15, 0, 0}, {-15, 0, 0}}, 181},
        {"Roll 225", {{-15, 0, 0}, {-15, 0, 0}}, 225},
        {"Roll 270", {{-15, 0, 0}, {-15, 0, 0}}, 270, 1, 1, true},
        {"Roll back to 0", {{-15, 0, 0}, {-15, 0, 0}}, 0, 1, 1, true},
        {"Roll -90", {{-15, 0, 0}, {-15, 0, 0}}, -90},
        {"Roll -170", {{-15, 0, 0}, {-15, 0, 0}}, -170},
        {"Roll 0 again", {{-15, 0, 0}, {-15, 0, 0}}, 0},
        {"Shorter arms", {{-15, 0, 0}, {-15, 0, 0}}, 0, 0.8f},
        {"Longer arms", {{-5, 0, 0}, {-5, 0, 0}}, 0, 1.25f},
        {"Hand scale 0.8", {{-15, 0, 0}, {-15, 0, 0}}, 0, 1, 0.8f},
        {"Over-reach", {{25, 0, 0}, {25, 0, 0}}},
        {"Pulled to the shoulder", {{-70, 0, 0}, {-70, 0, 0}}},
    };
    FILE* out = nullptr;
    if (fopen_s(&out, argv[2], "w") || !out) return 5;
    fprintf(out, "{\"bones\":[");
    for (size_t i = 0; i < rig.size(); ++i) fprintf(out, "%s\"%s\"", i ? "," : "", rig[i].name.c_str());
    fprintf(out, "],\"frames\":[\n");

    int failures = 0;
    unsigned frame = 0;
    float worstLength = 0, worstJoin = 0, worstStep = 0, maxSwivel = 0;
    float priorTwist[2] = {}, lastTracked[2] = {};
    Vec priorPole[2];
    for (size_t pi = 0; pi < sizeof poses / sizeof poses[0]; ++pi) {
        const PoseDef& cur = poses[pi];
        const PoseDef& prev = pi ? poses[pi - 1] : cur;
        for (int sub = 1; sub <= 10; ++sub) {
            const float t = sub / 10.0f;
            std::vector<Bone> bones(rig.size());
            for (size_t i = 0; i < rig.size(); ++i) {
                bones[i] = rig[i].b;
                for (float& c : bones[i].s) c = 1; // multipliers: 1 = the reference
            }
            Vec joints[2][3];
            float twistOut[2] = {}, swivelOut[2] = {}, frameScale = 1;
            for (int h = 0; h < 2; ++h) {
                const Arm& a = arms[h];
                const float mirror = h ? 1.0f : -1.0f;
                Vec o = prev.off[h] + (cur.off[h] - prev.off[h]) * t;
                const Vec wrist = vec(a.ref.wristP) + body(o);
                const float roll = (prev.roll + (cur.roll - prev.roll) * t) * kDeg * mirror;
                Input in;
                in.shoulder = vec(a.ref.upper.p);
                in.wrist = wrist;
                in.pole = U * -1 + R * (0.6f * mirror) + F * -0.3f; // Dishonored's default pole
                in.outward = R * mirror;
                in.priorPole = priorPole[h];
                in.priorTwist = priorTwist[h];
                in.fresh = frame > 0;
                in.lengthScale = (prev.length + (cur.length - prev.length) * t) * multiplier;
                in.scale = prev.scale + (cur.scale - prev.scale) * t;
                // The wrist: carried by the forearm's swing with no roll of its own,
                // then rolled about the forearm - a hand that turns only at the wrist.
                Swing sw;
                if (!swing(a.ref, in, in.pole, sw)) {
                    ++failures;
                    continue;
                }
                const Quat wq = mul(mul(axis_angle(sw.axis, roll), sw.fore), quat(a.ref.wristQ));
                for (int i = 0; i < 4; ++i) in.wristQ[i] = wq.v[i];
                Output po;
                if (!pose(a.ref, in, po)) {
                    ++failures;
                    continue;
                }
                const float k = in.scale * in.lengthScale;
                const float A = length(vec(a.ref.fore.p) - vec(a.ref.upper.p)) * k;
                const float B = length(vec(a.ref.wristP) - vec(a.ref.fore.p)) * k;
                worstLength = std::max(worstLength, std::max(fabsf(length(po.joints.elbow - po.joints.shoulder) - A),
                                                             fabsf(length(wrist - po.joints.elbow) - B)));
                worstJoin = std::max(worstJoin, length(posed_wrist(a.ref, po) - wrist));
                // Continuity is judged only where a wrist can actually be. Past
                // kTrackLimit the solver stops following and, once the direct reading is
                // back inside kTrustDirect, snaps to it rather than stay wound
                // (Dishonored's and the fork's design) - the poses marked `beyond` take
                // the arm there and back on purpose.
                if (frame > 0 && !cur.beyond) worstStep = std::max(worstStep, fabsf(po.trackedTwist - lastTracked[h]));
                frameScale = in.scale;
                maxSwivel = std::max(maxSwivel, fabsf(po.swivel));
                lastTracked[h] = po.trackedTwist;
                priorTwist[h] = po.trackedTwist;
                priorPole[h] = po.basePole;
                joints[h][0] = po.joints.shoulder;
                joints[h][1] = po.joints.elbow;
                joints[h][2] = wrist;
                twistOut[h] = po.roll;
                swivelOut[h] = po.swivel;
                bones[a.clav] = po.clavicle;
                bones[a.upper] = po.upper;
                bones[a.fore] = po.fore;
                bones[a.tw0] = po.twist[0];
                bones[a.tw1] = po.twist[1];
                // The hand and its fingers move rigidly with the wrist, at the hand scale.
                const Quat dq = mul(wq, conj(quat(a.ref.wristQ)));
                for (size_t b = 0; b < rig.size(); ++b) {
                    if (!descends(rig, (int)b, a.hand)) continue;
                    const Vec off = vec(rig[b].b.p) - vec(a.ref.wristP);
                    put(wrist + rotate(dq, off * in.scale), bones[b].p);
                    const Quat q = mul(dq, quat(rig[b].b.q));
                    for (int i = 0; i < 4; ++i) bones[b].q[i] = q.v[i];
                    for (float& c : bones[b].s) c = in.scale;
                }
            }
            fprintf(out, "%s{\"frame\":%u,\"label\":\"%s\",\"key\":%s,\"scale\":%.4g,\"roll\":[%.3f,%.3f],\"swivel\":[%.3f,%.3f],\"joints\":[",
                    frame ? ",\n" : "", frame, cur.name, sub == 10 ? "true" : "false", frameScale, twistOut[0] / kDeg,
                    twistOut[1] / kDeg, swivelOut[0] / kDeg, swivelOut[1] / kDeg);
            for (int h = 0; h < 2; ++h) {
                fprintf(out, "%s[", h ? "," : "");
                for (int j = 0; j < 3; ++j)
                    fprintf(out, "%s[%.5g,%.5g,%.5g]", j ? "," : "", joints[h][j].x, joints[h][j].y, joints[h][j].z);
                fprintf(out, "]");
            }
            fprintf(out, "],\"pose\":[");
            for (size_t i = 0; i < bones.size(); ++i) {
                const Bone& b = bones[i];
                fprintf(out, "%s[%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.7g,%.6g,%.6g,%.6g]", i ? "," : "", b.p[0], b.p[1], b.p[2],
                        b.q[0], b.q[1], b.q[2], b.q[3], b.s[0], b.s[1], b.s[2]);
            }
            fprintf(out, "]}");
            ++frame;
        }
    }
    fprintf(out, "],\"validation\":{\"frames\":%u,\"solveFailures\":%d,\"maxSegmentError\":%.6g,"
                 "\"maxWristJoinError\":%.6g,\"maxTwistStepDeg\":%.4g,\"maxSwivelDeg\":%.4g}}\n",
            frame, failures, worstLength, worstJoin, worstStep / kDeg, maxSwivel / kDeg);
    fclose(out);
    printf("arm IK sweep: %u frames, %d solve failures, segment error %.6f, wrist join %.6f, "
           "largest twist step %.1f deg, largest swivel %.1f deg\n",
           frame, failures, worstLength, worstJoin, worstStep / kDeg, maxSwivel / kDeg);
    return failures || worstLength > 0.01f || worstJoin > 0.01f ? 1 : 0;
}
