#include "probe_bob.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/util/log.h"
#include "game/bioshock1r/hands.h"
#include "game/shared/ue_math.h"
#include "patterns.h"

namespace bvr::b1r::probe_bob {
namespace {

// *** TEMPORARILY ARMED AT BOOT - REVERT TO false BEFORE THIS BRANCH MERGES ***
// User directive (2026-08-22): they are not opening the console to type
// `vrprobe bob on`, so the measurement has to happen by just playing. This is
// safe to auto-arm ONLY because the probe is read-only, bounded to kMaxLines
// windows, and sits inside the gameplay-gated viewmodel block - it cannot
// sweep, cannot write, and goes quiet on its own after ~80 windows.
//
// The standing rule this suspends, and why it exists: a build handed to the
// user must behave exactly like the verified one until they arm the diagnostic.
// Two instrumented builds this session each cost a play session - one to a
// repeating heap sweep, one to a gun that desynced between the eyes - and
// neither was something the user had asked to test. Restore `false` (and delete
// the boot log below) as soon as the bob's carrier is identified.
std::atomic<bool> g_on{true};

// Bounded: the question is answerable in about ten seconds of walking, and a
// line a second would drown the log over a long session.
constexpr int kMaxLines = 80;
int g_lines = 0;

// Above this ground speed the player is walking rather than drifting.
constexpr float kMovingUu = 20.0f;
constexpr float kUnitsPerDeg = 182.0444f;

bool read_at(const void* base, size_t off, void* out, size_t n) {
    __try {
        memcpy(out, static_cast<const uint8_t*>(base) + off, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Peak-to-peak of one scalar over a window, kept per bucket.
struct P2P {
    float lo = 1e9f, hi = -1e9f;
    void add(float v) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    float span() const { return hi >= lo ? hi - lo : 0.0f; }
    void reset() {
        lo = 1e9f;
        hi = -1e9f;
    }
};

// Everything is a HEIGHT ABOVE THE PAWN, so the player's own walking, the
// stairs and the slopes all cancel and what is left is the thing we are
// hunting. The bone heights are the bone's COMPONENT-space translation carried
// out to world by the actor's own transform, which is what the renderer does.
struct Bucket {
    P2P actorZ;     // the engine's placement of the rig
    P2P boneAPre;   // bone 43, array A, before the drive writes it
    P2P boneAPost;  // bone 43, array A, after   (flat = our write landed)
    P2P boneB;      // bone 43, array B - the by-name array, never ours
    P2P wpnZ;       // the symptom
    P2P boneACompPre; // component-space Z of the same bone, unrotated
    // Session 63: every column above is a height ABOVE THE PAWN, so a moving
    // pawn under a still bone and a moving bone over a still pawn are the SAME
    // number. These two are ABSOLUTE world Z, which is what tells them apart:
    //   pawnZabs spans ~3, postAbs flat -> the pawn moves under a fixed bone
    //   postAbs spans ~3, pawnZabs flat -> what WE write is what is moving
    P2P pawnZabs;   // the pawn's own world Z
    P2P postAbs;    // bone 43 after our write, in world Z, not relative
    P2P speed;      // so a window straddling the standing/moving line shows it
    int samples = 0;
    int postSamples = 0;
    void reset() {
        actorZ.reset();
        boneAPre.reset();
        boneAPost.reset();
        boneB.reset();
        wpnZ.reset();
        boneACompPre.reset();
        pawnZabs.reset();
        postAbs.reset();
        speed.reset();
        samples = 0;
        postSamples = 0;
    }
};

Bucket g_stand;
Bucket g_move;
uint64_t g_lastLogMs = 0;
bool g_loggedNoWeapon = false;
bool g_loggedNoSkel = false;
bool g_loggedNoB = false;
bool g_loggedLayout = false;

// Ground speed from successive pawn Locations, horizontal only: a lift or a
// fall is not walking.
float g_prevPawn[2] = {0.0f, 0.0f};
uint64_t g_prevPawnMs = 0;
float g_speed = 0.0f;

// Carried from the pre sample to the post sample of the SAME frame, so the
// "after" reading is charged to the same bucket and the same pawn height.
Bucket* g_frameBucket = nullptr;
void* g_frameActor = nullptr;
float g_framePawnZ = 0.0f;
float g_frameFwd[3] = {0.0f, 0.0f, 0.0f};
float g_frameRight[3] = {0.0f, 0.0f, 0.0f};
float g_frameUp[3] = {0.0f, 0.0f, 0.0f};
float g_frameActorLoc[3] = {0.0f, 0.0f, 0.0f};

// The evaluated skeleton hangs off the actor; both arrays are 48-byte
// hkQsTransforms whose translation is the first three floats (patterns.h
// "Skeleton / bone internals"). Returns the bone's COMPONENT-space translation.
bool bone_translation(void* actor, uint32_t arrayOff, uint32_t countOff, int bone,
                      float out[3]) {
    void* skel = nullptr;
    if (!read_at(actor, patterns::kActorSkelInstOffset, &skel, sizeof skel) || !skel)
        return false;
    void* bones = nullptr;
    int32_t count = 0;
    if (!read_at(skel, arrayOff, &bones, sizeof bones) || !bones) return false;
    if (!read_at(skel, countOff, &count, sizeof count)) return false;
    if (bone < 0 || bone >= count || count > 512) return false;
    return read_at(bones, static_cast<size_t>(bone) * 48u, out, sizeof(float) * 3);
}

// Component space is the actor's own frame, so a height only means something
// once it has been carried out by the actor's rotation - which is exactly what
// the renderer composes.
float bone_world_z(const float comp[3]) {
    return g_frameActorLoc[2] + g_frameFwd[2] * comp[0] + g_frameRight[2] * comp[1] +
           g_frameUp[2] * comp[2];
}

void log_bucket(const char* tag, const Bucket& b) {
    if (!b.samples) return;
    BVR_LOG("[b1r] bobsrc %-8s actor %5.2f | bone43 A pre %5.2f post %5.2f | bone43 B %5.2f "
            "| weapon %5.2f  (UU above the pawn, peak-to-peak)",
            tag, b.actorZ.span(), b.boneAPre.span(), b.boneAPost.span(), b.boneB.span(),
            b.wpnZ.span());
    // THE DISCRIMINATOR. Same window, ABSOLUTE world Z - see the bucket fields.
    BVR_LOG("[b1r] bobsrc %-8s   WORLD Z: pawn %5.2f | bone43 post %5.2f  "
            "(peak-to-peak, absolute - relative post was %5.2f)",
            tag, b.pawnZabs.span(), b.postAbs.span(), b.boneAPost.span());
    BVR_LOG("[b1r] bobsrc %-8s   bone43 A component Z %5.2f | n=%d/%d | speed %.0f-%.0f UU/s",
            tag, b.boneACompPre.span(), b.samples, b.postSamples, b.speed.lo, b.speed.hi);
}

} // namespace

void on_calcview_pre(const FrameContext& ctx) {
    g_frameBucket = nullptr;
    void* viewActor = ctx.viewActor;
    if (!g_on.load(std::memory_order_relaxed) || !viewActor) return;

    // TEMPORARY (see g_on): announce once, so the log proves which build is
    // running and that the probe is live without anyone typing a command.
    static bool announced = false;
    if (!announced) {
        announced = true;
        BVR_LOG("[b1r] bobsrc: ARMED AT BOOT (temporary) - walk with a weapon for "
                "~15 s; %d windows then it stops. `vrprobe bob off` to silence.",
                kMaxLines);
    }

    if (g_lines >= kMaxLines) return;

    float pawnLoc[3];
    if (!read_at(viewActor, patterns::kActorLocOffset, pawnLoc, sizeof pawnLoc)) return;

    const uint64_t now = GetTickCount64();
    if (g_prevPawnMs && now > g_prevPawnMs) {
        const float dt = static_cast<float>(now - g_prevPawnMs) * 0.001f;
        const float dx = pawnLoc[0] - g_prevPawn[0];
        const float dy = pawnLoc[1] - g_prevPawn[1];
        const float inst = sqrtf(dx * dx + dy * dy) / dt;
        g_speed += (inst - g_speed) * 0.25f;
    }
    g_prevPawn[0] = pawnLoc[0];
    g_prevPawn[1] = pawnLoc[1];
    g_prevPawnMs = now;

    void* actor = hands::hands_actor();
    if (!actor) return;

    float actorLoc[3];
    int32_t actorRot[3];
    if (!read_at(actor, patterns::kActorLocOffset, actorLoc, sizeof actorLoc)) return;
    // Rotators are int32 units. Reading one as float gives a denormal, which is
    // why this is read as integers and converted by 182.0444.
    if (!read_at(actor, patterns::kActorViewDirOffset, actorRot, sizeof actorRot)) return;

    Bucket& b = (g_speed > kMovingUu) ? g_move : g_stand;
    b.speed.add(g_speed);
    b.actorZ.add(actorLoc[2] - pawnLoc[2]);
    b.pawnZabs.add(pawnLoc[2]);

    // Stash the frame's basis so the post sample lands in the same frame's
    // terms rather than re-reading a rig the drive has just moved.
    FRotator r{actorRot[0], actorRot[1], actorRot[2]};
    ue_rot_basis(r, g_frameFwd, g_frameRight, g_frameUp);
    memcpy(g_frameActorLoc, actorLoc, sizeof g_frameActorLoc);
    g_frameBucket = &b;
    g_frameActor = actor;
    g_framePawnZ = pawnLoc[2];

    float comp[3];
    if (bone_translation(actor, patterns::kSkelInstBonesOffset,
                         patterns::kSkelInstBoneCountOffset, patterns::kBoneWeaponAttach,
                         comp)) {
        b.boneACompPre.add(comp[2]);
        b.boneAPre.add(bone_world_z(comp) - pawnLoc[2]);
        if (!g_loggedLayout) {
            g_loggedLayout = true;
            BVR_LOG("[b1r] bobsrc: bone %d array A component pos (%.2f %.2f %.2f)",
                    patterns::kBoneWeaponAttach, comp[0], comp[1], comp[2]);
        }
    } else if (!g_loggedNoSkel) {
        g_loggedNoSkel = true;
        BVR_LOG("[b1r] bobsrc: no readable SkeletonInstance array A - the bone columns "
                "stay 0");
    }

    // Array B, the lazily-filled by-name path. Often null, and that is itself
    // an answer: an attachment resolved by name cannot be reading an array the
    // engine never filled.
    if (bone_translation(actor, patterns::kSkelInstBonesBOffset,
                         patterns::kSkelInstBoneCountBOffset, patterns::kBoneWeaponAttach,
                         comp)) {
        b.boneB.add(bone_world_z(comp) - pawnLoc[2]);
    } else if (!g_loggedNoB) {
        g_loggedNoB = true;
        BVR_LOG("[b1r] bobsrc: array B (by-name) is empty or unreadable on this rig - it "
                "cannot be what the gun renders from");
    }

    // The equipped holdable read straight off the rig (hands+0x45C), which is
    // class-agnostic and costs one pointer load - the wrench resolves through
    // it exactly like a gun.
    void* wpn = nullptr;
    (void)hands::current_holdable(&wpn);
    if (wpn) {
        float wLoc[3];
        if (read_at(wpn, patterns::kActorLocOffset, wLoc, sizeof wLoc))
            b.wpnZ.add(wLoc[2] - pawnLoc[2]);
    } else if (!g_loggedNoWeapon) {
        g_loggedNoWeapon = true;
        BVR_LOG("[b1r] bobsrc: nothing in the rig's CurrentHoldable - the weapon column "
                "stays 0 until something is equipped");
    }
    ++b.samples;

    if (now - g_lastLogMs < 1000) return;
    g_lastLogMs = now;
    if (!g_stand.samples && !g_move.samples) return;

    ++g_lines;
    BVR_LOG("[b1r] bobsrc --- speed %.0f UU/s ---", g_speed);
    log_bucket("standing", g_stand);
    log_bucket("MOVING", g_move);
    g_stand.reset();
    g_move.reset();
    if (g_lines == kMaxLines)
        BVR_LOG("[b1r] bobsrc: capped at %d windows - `vrprobe bob on` to re-arm", kMaxLines);
}

void on_calcview_post(const FrameContext& ctx) {
    (void)ctx;
    Bucket* b = g_frameBucket;
    g_frameBucket = nullptr; // one post per pre, always
    if (!b || !g_frameActor) return;
    float comp[3];
    if (!bone_translation(g_frameActor, patterns::kSkelInstBonesOffset,
                          patterns::kSkelInstBoneCountOffset, patterns::kBoneWeaponAttach,
                          comp))
        return;
    const float postWorldZ = bone_world_z(comp);
    b->boneAPost.add(postWorldZ - g_framePawnZ);
    b->postAbs.add(postWorldZ);
    ++b->postSamples;
}

void handle_command(const char* args) {
    const char* v = args;
    while (*v == ' ') ++v;
    if (strncmp(v, "off", 3) == 0) {
        g_on.store(false, std::memory_order_relaxed);
        BVR_LOG("[b1r] bobsrc: OFF");
    } else if (strncmp(v, "on", 2) == 0) {
        g_on.store(true, std::memory_order_relaxed);
        g_lines = 0;
        g_stand.reset();
        g_move.reset();
        g_loggedNoWeapon = false;
        g_loggedNoSkel = false;
        g_loggedNoB = false;
        g_loggedLayout = false;
        BVR_LOG("[b1r] bobsrc: ON, counters reset");
    } else {
        BVR_LOG("[b1r] bobsrc: %s, %d/%d windows logged, speed %.0f UU/s",
                g_on.load(std::memory_order_relaxed) ? "ON" : "off", g_lines, kMaxLines,
                g_speed);
        BVR_LOG("[b1r] bobsrc: usage: vrprobe bob on|off");
    }
}

} // namespace bvr::b1r::probe_bob
