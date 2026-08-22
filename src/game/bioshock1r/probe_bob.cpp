#include "probe_bob.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/util/log.h"
#include "game/bioshock1r/hands.h"
#include "patterns.h"

namespace bvr::b1r::probe_bob {
namespace {

std::atomic<bool> g_on{true};

// Bounded: two lines a second would drown the log over a long session, and the
// question is answerable in about ten seconds of walking.
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

struct Bucket {
    P2P actorZ;      // actor height above the pawn
    P2P actorPitch;  // degrees
    P2P actorRoll;
    P2P wpnOffX;     // weapon position relative to the actor, world axes
    P2P wpnOffY;
    P2P wpnOffZ;
    P2P wpnPitch;
    P2P wpnRoll;
    int samples = 0;
    void reset() {
        actorZ.reset();
        actorPitch.reset();
        actorRoll.reset();
        wpnOffX.reset();
        wpnOffY.reset();
        wpnOffZ.reset();
        wpnPitch.reset();
        wpnRoll.reset();
        samples = 0;
    }
};

Bucket g_stand;
Bucket g_move;
uint64_t g_lastLogMs = 0;
bool g_loggedNoWeapon = false;

// Ground speed from successive pawn Locations, horizontal only: a lift or a
// fall is not walking.
float g_prevPawn[2] = {0.0f, 0.0f};
uint64_t g_prevPawnMs = 0;
float g_speed = 0.0f;

void log_bucket(const char* tag, const Bucket& b) {
    if (!b.samples) return;
    BVR_LOG("[b1r] bobsrc %-8s actorZ %5.2f UU | actor pitch %5.2f roll %5.2f deg | "
            "wpn-vs-actor %5.2f %5.2f %5.2f UU | wpn pitch %5.2f roll %5.2f deg (n=%d)",
            tag, b.actorZ.span(), b.actorPitch.span(), b.actorRoll.span(), b.wpnOffX.span(),
            b.wpnOffY.span(), b.wpnOffZ.span(), b.wpnPitch.span(), b.wpnRoll.span(),
            b.samples);
}

} // namespace

void on_calcview(void* pc, void* viewActor) {
    (void)pc;
    if (!g_on.load(std::memory_order_relaxed) || !viewActor) return;
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
    // why this is read and logged as integers converted by 182.0444.
    if (!read_at(actor, patterns::kActorViewDirOffset, actorRot, sizeof actorRot)) return;

    Bucket& b = (g_speed > kMovingUu) ? g_move : g_stand;
    b.actorZ.add(actorLoc[2] - pawnLoc[2]);
    b.actorPitch.add(static_cast<float>(actorRot[0]) / kUnitsPerDeg);
    b.actorRoll.add(static_cast<float>(actorRot[2]) / kUnitsPerDeg);

    // The weapon RELATIVE to the actor. Subtracting the actor removes both the
    // player's walk and any actor-level bob, so a residual here is the attach
    // chain moving the gun under a steady actor - candidate B.
    void* wpn = hands::weapon_actor();
    if (wpn) {
        float wLoc[3];
        int32_t wRot[3];
        if (read_at(wpn, patterns::kActorLocOffset, wLoc, sizeof wLoc) &&
            read_at(wpn, patterns::kActorViewDirOffset, wRot, sizeof wRot)) {
            b.wpnOffX.add(wLoc[0] - actorLoc[0]);
            b.wpnOffY.add(wLoc[1] - actorLoc[1]);
            b.wpnOffZ.add(wLoc[2] - actorLoc[2]);
            b.wpnPitch.add(static_cast<float>(wRot[0]) / kUnitsPerDeg);
            b.wpnRoll.add(static_cast<float>(wRot[2]) / kUnitsPerDeg);
        }
    } else if (!g_loggedNoWeapon) {
        g_loggedNoWeapon = true;
        BVR_LOG("[b1r] bobsrc: no weapon actor cached yet - the wpn columns stay 0 "
                "until one is learned (fire once)");
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
        BVR_LOG("[b1r] bobsrc: ON, counters reset");
    } else {
        BVR_LOG("[b1r] bobsrc: %s, %d/%d windows logged, speed %.0f UU/s",
                g_on.load(std::memory_order_relaxed) ? "ON" : "off", g_lines, kMaxLines,
                g_speed);
        BVR_LOG("[b1r] bobsrc: usage: vrprobe bob on|off");
    }
}

} // namespace bvr::b1r::probe_bob
