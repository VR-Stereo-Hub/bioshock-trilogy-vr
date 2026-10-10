// hand-compose-sweep.cpp - replay REAL BS1 animation through the Dishonored hand model.
//
// Every frame of each clip (from `bs2gltf.py --clip-poses`) is composed by the production
// header src/game/bioshock1r/hand_compose.h against a controller held STILL at the clip's
// first wrist pose. The arms are then solved to the composed wrists by arm_ik.h. What
// must come out:
//   - the wrist never leaves the controller (the game's own wrist travel is reported
//     beside it, so the size of what is being cancelled is on record)
//   - every finger, the weapon attach (43) and its tip (44) keep their pose relative to
//     the wrist - the animation still plays inside the hand
//   - the arm meets the composed wrist on every frame
// Writes two sweeps for tools\blender\arm_ik_bake.py - the game's own pose and the
// composed one - so both can be skinned and rendered.
//
//   .\tools\hand-compose-sweep.ps1
//   hand-compose-sweep.exe <rig.txt> <clips.txt> <native.json> <composed.json>
#include "game/bioshock1r/arm_ik.h"
#include "game/bioshock1r/hand_compose.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace bvr::b1r;
namespace ik = arm_ik;
namespace hc = hand_compose;

struct RigBone {
    std::string name;
    int parent = -1;
    ik::Bone b;
};
static bool read_bone(const char* s, ik::Bone& b) {
    return sscanf_s(s, "%f %f %f %f %f %f %f %f %f %f", &b.p[0], &b.p[1], &b.p[2], &b.q[0], &b.q[1], &b.q[2], &b.q[3],
                    &b.s[0], &b.s[1], &b.s[2]) == 10;
}
static bool load_rig(const char* path, std::vector<RigBone>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        RigBone r;
        int idx = 0, used = 0;
        char name[128] = {};
        if (sscanf_s(line, "%d %127s %d%n", &idx, name, (unsigned)sizeof name, &r.parent, &used) != 3 ||
            !read_bone(line + used, r.b))
            break;
        r.name = name;
        out.push_back(r);
    }
    fclose(f);
    return !out.empty();
}
struct Clip {
    std::string name;
    float fps = 30;
    std::vector<std::vector<ik::Bone>> frames;
};
static bool load_clips(const char* path, size_t nb, std::vector<Clip>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return false;
    std::vector<char> line(1 << 16);
    Clip* cur = nullptr;
    while (fgets(line.data(), (int)line.size(), f)) {
        if (line[0] == '#' || !strncmp(line.data(), "bones", 5)) continue;
        if (!strncmp(line.data(), "clip ", 5)) {
            char name[256] = {};
            int n = 0;
            float fps = 30;
            if (sscanf_s(line.data() + 5, "%255s %d %f", name, (unsigned)sizeof name, &n, &fps) != 3) return false;
            out.push_back(Clip{name, fps, {}});
            cur = &out.back();
            continue;
        }
        if (!cur) continue;
        std::vector<ik::Bone> pose(nb);
        const char* s = line.data();
        for (size_t i = 0; i < nb; ++i) {
            float* v[10] = {&pose[i].p[0], &pose[i].p[1], &pose[i].p[2], &pose[i].q[0], &pose[i].q[1],
                            &pose[i].q[2], &pose[i].q[3], &pose[i].s[0], &pose[i].s[1], &pose[i].s[2]};
            for (int k = 0; k < 10; ++k) {
                char* end = nullptr;
                *v[k] = strtof(s, &end);
                if (end == s) return false;
                s = end;
            }
        }
        cur->frames.push_back(pose);
    }
    fclose(f);
    return !out.empty();
}
static int find(const std::vector<RigBone>& rig, const std::string& n) {
    for (size_t i = 0; i < rig.size(); ++i)
        if (rig[i].name == n) return (int)i;
    return -1;
}
static bool descends(const std::vector<RigBone>& rig, int b, int root) {
    for (size_t n = 0; n <= rig.size() && b >= 0; ++n) {
        if (b == root) return true;
        b = rig[b].parent;
    }
    return false;
}
static float qdiff(const ik::Quat& a, const ik::Quat& b) {
    const ik::Quat d = ik::mul(a, ik::conj(b));
    const float s = sqrtf(d.v[0] * d.v[0] + d.v[1] * d.v[1] + d.v[2] * d.v[2]);
    return 2 * atan2f(s, fabsf(d.v[3]));
}

static void write_frame(FILE* out, bool first, unsigned frame, const std::string& label, bool key,
                        const ik::Vec joints[2][3], const std::vector<ik::Bone>& pose, const std::vector<ik::Bone>& ref,
                        bool multipliers) {
    fprintf(out, "%s{\"frame\":%u,\"label\":\"%s\",\"key\":%s,\"scale\":1,\"roll\":[0,0],\"swivel\":[0,0],\"joints\":[",
            first ? "" : ",\n", frame, label.c_str(), key ? "true" : "false");
    for (int h = 0; h < 2; ++h) {
        fprintf(out, "%s[", h ? "," : "");
        for (int j = 0; j < 3; ++j)
            fprintf(out, "%s[%.5g,%.5g,%.5g]", j ? "," : "", joints[h][j].x, joints[h][j].y, joints[h][j].z);
        fprintf(out, "]");
    }
    fprintf(out, "],\"pose\":[");
    for (size_t i = 0; i < pose.size(); ++i) {
        const ik::Bone& b = pose[i];
        // The bake multiplies the reference scale by this; clip scales are absolute.
        float s[3];
        for (int k = 0; k < 3; ++k) s[k] = multipliers ? b.s[k] : b.s[k] / (ref[i].s[k] ? ref[i].s[k] : 1);
        fprintf(out, "%s[%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.7g,%.5g,%.5g,%.5g]", i ? "," : "", b.p[0], b.p[1], b.p[2], b.q[0],
                b.q[1], b.q[2], b.q[3], s[0], s[1], s[2]);
    }
    fprintf(out, "]}");
}

int main(int argc, char** argv) {
    if (argc != 5) {
        puts("usage: hand-compose-sweep <rig.txt> <clips.txt> <native.json> <composed.json>");
        return 2;
    }
    std::vector<RigBone> rig;
    std::vector<Clip> clips;
    if (!load_rig(argv[1], rig) || !load_clips(argv[2], rig.size(), clips)) {
        puts("*** cannot read the rig or the clips");
        return 3;
    }
    std::vector<ik::Bone> ref(rig.size());
    for (size_t i = 0; i < rig.size(); ++i) ref[i] = rig[i].b;

    struct Hand {
        int wrist, sleeve[5];
        ik::Vec palmLocal;
        std::vector<int> cluster;
    } hands[2];
    const char* side[2] = {"L", "R"};
    for (int h = 0; h < 2; ++h) {
        const std::string s = side[h];
        Hand& H = hands[h];
        H.wrist = find(rig, "Bip01_" + s + "_Hand");
        const char* parts[5] = {"Clavicle", "UpperArm", "Forearm", "ForeTwist", "ForeTwist1"};
        for (int k = 0; k < 5; ++k)
            if ((H.sleeve[k] = find(rig, "Bip01_" + s + "_" + parts[k])) < 0) return 4;
        if (H.wrist < 0) return 4;
        for (size_t b = 0; b < rig.size(); ++b)
            if (descends(rig, (int)b, H.wrist)) H.cluster.push_back((int)b);
        // The palm: the right hand's R_grip (where the weapon is held); the left hand's
        // middle knuckle, halfway in. Fixed in the wrist's frame.
        const int palmBone = h ? find(rig, "R_grip") : find(rig, "kBone_L_Middle1");
        const hc::Rigid w0 = hc::frame_of(ref[H.wrist]);
        ik::Vec p = ik::vec(ref[palmBone].p);
        if (!h) p = ik::vec(ref[H.wrist].p) + (p - ik::vec(ref[H.wrist].p)) * 0.5f;
        H.palmLocal = hc::apply(hc::inverse(w0), p);
    }

    FILE* nat = nullptr;
    FILE* cmp = nullptr;
    if (fopen_s(&nat, argv[3], "w") || fopen_s(&cmp, argv[4], "w") || !nat || !cmp) return 5;
    for (FILE* f : {nat, cmp}) {
        fprintf(f, "{\"bones\":[");
        for (size_t i = 0; i < rig.size(); ++i) fprintf(f, "%s\"%s\"", i ? "," : "", rig[i].name.c_str());
        fprintf(f, "],\"frames\":[\n");
    }
    unsigned frame = 0;
    int armFails = 0;
    float worstWrist = 0, worstWristRot = 0, worstFinger = 0, worstJoin = 0;
    printf("%-34s %6s  %-26s  %-26s\n", "clip", "frames", "game moves the wrist up to", "composed wrist off by");
    for (const Clip& c : clips) {
        float travel[2] = {}, travelRot[2] = {}, off[2] = {}, offRot[2] = {};
        hc::Rigid target[2];
        ik::Vec prior[2], shoulder0[2];
        float priorTwist[2] = {};
        for (size_t fi = 0; fi < c.frames.size(); ++fi) {
            const std::vector<ik::Bone>& A = c.frames[fi];
            std::vector<ik::Bone> B = A;
            ik::Vec jn[2][3], jc[2][3];
            for (int h = 0; h < 2; ++h) {
                const Hand& H = hands[h];
                const hc::Rigid wA = hc::frame_of(A[H.wrist]);
                if (fi == 0) // the controller, held still where the clip starts
                    target[h] = {wA.q, hc::palm_of(A[H.wrist], H.palmLocal)};
                const hc::Rigid w0 = {target[h].q, target[h].t};
                travel[h] = std::max(travel[h], ik::length(hc::palm_of(A[H.wrist], H.palmLocal) - w0.t));
                travelRot[h] = std::max(travelRot[h], qdiff(wA.q, w0.q));
                const hc::Rigid D = hc::delta(target[h], A[H.wrist], H.palmLocal);
                for (int b : H.cluster) B[b] = hc::carry(D, A[b]);
                off[h] = std::max(off[h], ik::length(hc::palm_of(B[H.wrist], H.palmLocal) - target[h].t));
                offRot[h] = std::max(offRot[h], qdiff(hc::frame_of(B[H.wrist]).q, target[h].q));
                for (int b : H.cluster) {
                    const hc::Rigid r0 = hc::mul(hc::inverse(hc::frame_of(A[H.wrist])), hc::frame_of(A[b]));
                    const hc::Rigid r1 = hc::mul(hc::inverse(hc::frame_of(B[H.wrist])), hc::frame_of(B[b]));
                    worstFinger = std::max(worstFinger, ik::length(r0.t - r1.t));
                }
                // The arm, to the composed wrist, from a nominal shoulder: where this clip
                // puts the shoulder on its first frame (the body as the game posed it;
                // the reference pose's shoulders sit elsewhere in the clip's space).
                if (fi == 0) shoulder0[h] = ik::vec(A[H.sleeve[1]].p);
                ik::Ref ar;
                ik::Bone* rb[5] = {&ar.clavicle, &ar.upper, &ar.fore, &ar.twist[0], &ar.twist[1]};
                for (int k = 0; k < 5; ++k) *rb[k] = ref[H.sleeve[k]];
                memcpy(ar.wristP, ref[H.wrist].p, 12);
                memcpy(ar.wristQ, ref[H.wrist].q, 16);
                ik::Input in;
                in.shoulder = shoulder0[h];
                in.wrist = ik::vec(B[H.wrist].p);
                memcpy(in.wristQ, B[H.wrist].q, 16);
                const float sd = h ? 1.0f : -1.0f;
                const ik::Vec F{0, 0, -1}, R{0, 1, 0}, U{1, 0, 0};
                in.pole = U * -1 + R * (0.35f * sd) + F * -0.3f;
                in.outward = R * sd;
                in.priorPole = prior[h];
                in.priorTwist = priorTwist[h];
                in.fresh = fi > 0;
                ik::Output o;
                if (!ik::pose(ar, in, o)) {
                    ++armFails;
                } else {
                    prior[h] = o.basePole;
                    priorTwist[h] = o.trackedTwist;
                    const ik::Bone* ob[5] = {&o.clavicle, &o.upper, &o.fore, &o.twist[0], &o.twist[1]};
                    for (int k = 0; k < 5; ++k) {
                        B[H.sleeve[k]] = *ob[k];
                        for (int a = 0; a < 3; ++a) B[H.sleeve[k]].s[a] *= ref[H.sleeve[k]].s[a];
                    }
                    worstJoin = std::max(worstJoin, ik::length(ik::posed_wrist(ar, o) - in.wrist));
                    jc[h][0] = o.joints.shoulder;
                    jc[h][1] = o.joints.elbow;
                    jc[h][2] = in.wrist;
                }
                jn[h][0] = ik::vec(A[H.sleeve[1]].p);
                jn[h][1] = ik::vec(A[H.sleeve[2]].p);
                jn[h][2] = ik::vec(A[H.wrist].p);
            }
            const bool key = fi % 6 == 0 || fi + 1 == c.frames.size();
            const std::string label = c.name + " f" + std::to_string(fi);
            write_frame(nat, frame == 0, frame, label, key, jn, A, ref, false);
            write_frame(cmp, frame == 0, frame, label, key, jc, B, ref, false);
            ++frame;
        }
        printf("%-34s %6zu  L %5.1f UU %5.1f deg   R %5.1f UU %5.1f deg  L %.4f/%.4f  R %.4f/%.4f\n", c.name.c_str(),
               c.frames.size(), travel[0], travelRot[0] / ik::kDeg, travel[1], travelRot[1] / ik::kDeg, off[0],
               offRot[0] / ik::kDeg, off[1], offRot[1] / ik::kDeg);
        worstWrist = std::max(worstWrist, std::max(off[0], off[1]));
        worstWristRot = std::max(worstWristRot, std::max(offRot[0], offRot[1]));
    }
    for (FILE* f : {nat, cmp}) {
        fprintf(f, "],\"validation\":{\"frames\":%u}}\n", frame);
        fclose(f);
    }
    printf("composed: wrist off the controller by at most %.5f UU / %.4f deg; fingers, weapon attach and tip keep "
           "their pose in the wrist to %.5f UU; %d arm solve failures, arm-to-wrist join %.5f UU\n",
           worstWrist, worstWristRot / ik::kDeg, worstFinger, armFails, worstJoin);
    return worstWrist > 0.01f || worstWristRot > 0.05f * ik::kDeg || worstFinger > 0.01f || armFails ? 1 : 0;
}
