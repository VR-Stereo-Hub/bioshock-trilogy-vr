#include "game/bioshock1r/manual_reload.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/muzzle.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/reload_rig.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bvr::b1r::manual_reload {
namespace {
using S = menu::Setting;
namespace rr = reload_rig;
using arm_ik::Vec;

bool read_n(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<bool> g_on{true};
rr::Machine g_machine;
rr::Config g_cfg;

// XInput thread <-> game thread.
std::atomic<bool> g_gripDown{false};
std::atomic<uint32_t> g_gripPresses{0};
std::atomic<bool> g_claim{false};
std::atomic<uint64_t> g_claimMs{0};
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_reloadButtonMs{0}; // the last time the reload button was down
bool g_suppressUntilUp = false; // XInput thread

// Game thread.
uint32_t g_seenPresses = 0;
void* g_weapon = nullptr;
wchar_t g_cls[64] = L"";
int32_t g_lastHandle = 0;
bool g_paused = false;
int32_t g_heldW = 0, g_heldH = 0;
uint64_t g_lastBuzzMs = 0, g_canMs = 0;
bool g_can = true;
int32_t g_lenName = -1;
float g_len = 0;
uint32_t g_starts = 0, g_done = 0, g_aborts = 0;
std::atomic<bool> g_watch{false};
uint64_t g_watchMs = 0;

constexpr int kBuzzMs = 60;            // the grab buzz (two_hand.cpp / dial_input.cpp): 60 ms at 0.35
constexpr uint64_t kBuzzEveryMs = 50;  // re-fired every 50 ms
constexpr float kBuzzAmp = 0.35f;

void hands_handle(void* weapon, int32_t* out) {
    *out = 0;
    read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponHandsAnimationHandleOffset, out, sizeof *out);
}

// Hold both reload clips (the weapon's and the hands') at `t`, paused.
void hold(void* weapon, void* hands, int32_t hw, int32_t hh, float t) {
    if (!g_paused || hw != g_heldW || hh != g_heldH) {
        script::pause_anim(weapon, hw);
        if (hh > 0 && hands) script::pause_anim(hands, hh);
        g_paused = true;
        g_heldW = hw;
        g_heldH = hh;
    }
    script::set_anim_time(weapon, hw, t);
    if (hh > 0 && hands) script::set_anim_time(hands, hh, t);
}
void run(void* weapon, void* hands, int32_t hw, int32_t hh, bool jump, float t) {
    if (jump) {
        script::set_anim_time(weapon, hw, t);
        if (hh > 0 && hands) script::set_anim_time(hands, hh, t);
    }
    if (g_paused || jump) {
        script::resume_anim(weapon, hw);
        if (hh > 0 && hands) script::resume_anim(hands, hh);
    }
    g_paused = false;
}
// Let go of whatever is held (the reload ended, was refused, or the feature was switched off).
void let_go(void* weapon, void* hands) {
    if (g_paused && weapon) {
        script::resume_anim(weapon, g_heldW);
        if (g_heldH > 0 && hands) script::resume_anim(hands, g_heldH);
    }
    g_paused = false;
    bones::clear_weapon_part_offsets();
}

// The gun's own space against the world, from two of its bones as the engine places them now: a
// weapon-space offset d moves a bone by M d s in the world. Returns world -> weapon space.
bool weapon_space(void* weapon, const char* exclude, float m[3][3], float* s, float o0Out[3] = nullptr,
                  float p0Out[3] = nullptr) {
    const int n = bones::weapon_bone_count();
    if (n < 2) return false;
    float p0[3], q0[4];
    if (!bones::weapon_bone_pose(0, p0, q0)) return false;
    int ref = -1;
    float best = 0;
    for (int i = 1; i < n; ++i) {
        const char* nm = bones::weapon_bone_name(i);
        if (exclude && strstr(exclude, nm) && nm[0]) continue; // a part we move is no reference
        float p[3], q[4];
        if (!bones::weapon_bone_pose(i, p, q)) continue;
        const float d = std::sqrt((p[0] - p0[0]) * (p[0] - p0[0]) + (p[1] - p0[1]) * (p[1] - p0[1]) +
                                  (p[2] - p0[2]) * (p[2] - p0[2]));
        if (d > best) best = d, ref = i;
    }
    if (ref < 0 || best < 1.0f) return false;
    const int32_t n0 = script::name_index(bones::weapon_bone_name(0));
    const int32_t nr = script::name_index(bones::weapon_bone_name(ref));
    float o0[3], a0[3][3], orf[3], arf[3][3];
    if (!muzzle::bone_coords(weapon, n0, o0, a0) || !muzzle::bone_coords(weapon, nr, orf, arf)) return false;
    float pr[3], qr[4];
    bones::weapon_bone_pose(ref, pr, qr);
    const float dw = std::sqrt((orf[0] - o0[0]) * (orf[0] - o0[0]) + (orf[1] - o0[1]) * (orf[1] - o0[1]) +
                               (orf[2] - o0[2]) * (orf[2] - o0[2]));
    if (!(dw > 1e-3f)) return false;
    *s = dw / best;
    if (o0Out) memcpy(o0Out, o0, 12);
    if (p0Out) memcpy(p0Out, p0, 12);
    // world = M * weapon; the bone's world axes are M * R(q0), so M^T = R(q0) * A0^T: row a of the
    // result is R(q0) applied to the a-th column of A0^T ... built column by column below.
    const arm_ik::Quat q = arm_ik::normalized(arm_ik::quat(q0));
    for (int c = 0; c < 3; ++c) {
        // world basis vector e_c expressed in bone0's frame, then turned into weapon space
        const Vec inBone{a0[0][c], a0[1][c], a0[2][c]};
        const Vec w = arm_ik::rotate(q, inBone);
        m[0][c] = w.x;
        m[1][c] = w.y;
        m[2][c] = w.z;
    }
    return true;
}

// What the free hand carries this frame (set by tick, applied by carry() from the hand's placement).
int g_carryMode = 0;            // 0 nothing, 1 the part rides the hand, 2 the part is gone
const char* g_carryPart = nullptr;
void* g_carryWeapon = nullptr;
bool g_carryAtPalm = false;

template <typename Fn>
void for_parts(const char* part, Fn fn) {
    char names[96];
    strncpy_s(names, part, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(names, "+", &ctx); tok; tok = strtok_s(nullptr, "+", &ctx)) {
        const int i = bones::weapon_bone_index(tok);
        if (i >= 0) fn(i);
    }
}

// Every frame: what the reload asks of the parts. A dropped part goes far below (out of sight); a
// carried one is placed by carry() once the free hand is.
void apply_parts(const rr::Output& o, const Frame& f) {
    bones::clear_weapon_part_offsets();
    g_carryMode = o.part ? o.carry : 0;
    g_carryPart = o.part;
    g_carryAtPalm = o.atPalm;
    g_carryWeapon = f.weapon;
    if (g_carryMode != 2) return;
    float m[3][3], s = 1;
    if (!weapon_space(f.weapon, o.part, m, &s) || !(s > 1e-6f)) return;
    const Vec w{0, 0, -4000.0f};
    const float dw[3] = {(m[0][0] * w.x + m[0][1] * w.y + m[0][2] * w.z) / s,
                         (m[1][0] * w.x + m[1][1] * w.y + m[1][2] * w.z) / s,
                         (m[2][0] * w.x + m[2][1] * w.y + m[2][2] * w.z) / s};
    for_parts(o.part, [&](int i) { bones::set_weapon_part_offset(i, dw); });
}

void haptic(rr::Haptic h, const Frame& f) {
    switch (h) {
        case rr::Haptic::Grab: bvr::vr::pulse_hand(f.freePhys, 0.5f, 30); break;
        case rr::Haptic::Tick: bvr::vr::pulse_hand(f.freePhys, 0.25f, 15); break;
        case rr::Haptic::Seat:
            bvr::vr::pulse_hand(f.freePhys, 1.0f, 60);
            bvr::vr::pulse_hand(f.gunPhys, 0.6f, 40);
            break;
        case rr::Haptic::Deny: bvr::vr::pulse_hand(f.freePhys, 0.7f, 120); break;
        case rr::Haptic::Drop: bvr::vr::pulse_hand(f.freePhys, 0.2f, 20); break;
        default: break;
    }
}

const char* kind_name(rr::Kind k) {
    static const char* const n[] = {"pull", "carry", "fetch", "insert", "flick", "play", "finish"};
    return n[static_cast<int>(k)];
}

// Not full? The weapon's own answer (PlayerWeapon.GetRoundsRemaining / GetMagazineSize), at most
// four times a second; unknown counts as yes - the game refuses a reload it cannot do anyway.
bool can_reload(void* weapon, uint64_t now) {
    if (now - g_canMs < 250 && weapon == g_weapon) return g_can;
    g_canMs = now;
    struct { int32_t ret; int32_t pad[3]; } r{}, m{};
    if (script::call(weapon, "GetRoundsRemaining", &r) && script::call(weapon, "GetMagazineSize", &m) && m.ret > 0)
        g_can = r.ret < m.ret;
    else
        g_can = true;
    return g_can;
}
} // namespace

bool active() { return g_active.load(std::memory_order_relaxed); }

// The part the free hand carries moves with the hand exactly as the clip's hand would carry it: the
// clip has the part in the clip's hand (paused at this phase's time), and the free hand is that clip
// hand moved onto the controller - so the same move, applied to the part, keeps it in the drawn hand,
// turning with it. Weapon space -> world (the gun's two bones), world -> the hands' component space,
// the hand's move, and back.
void carry(const HandPlacement& hp) {
    if (g_carryMode != 1 || !g_carryPart || !g_carryWeapon || !g_machine.active()) return;
    float m[3][3], s = 1, o0[3], p0[3];
    if (!weapon_space(g_carryWeapon, g_carryPart, m, &s, o0, p0) || !(s > 1e-6f) || !(hp.actorScale > 1e-6f)) return;
    // m is world -> weapon (M^T); its quaternion is conj(M).
    const float mf[9] = {m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
    const arm_ik::Quat mT = arm_ik::from_matrix(mf), mq = arm_ik::conj(mT);
    const arm_ik::Quat qa = arm_ik::normalized(arm_ik::quat(hp.actorQ)), qh = arm_ik::normalized(arm_ik::quat(hp.q));
    const Vec aLoc = arm_ik::vec(hp.actorLoc), palm = arm_ik::vec(hp.palm), palmAt = arm_ik::vec(hp.palmAt);
    const arm_ik::Quat qg = arm_ik::normalized(arm_ik::quat(hp.gunQ));
    const Vec gunPalm = arm_ik::vec(hp.gunPalm), gunPalmAt = arm_ik::vec(hp.gunPalmAt);
    if (!(hp.gunS > 1e-6f)) return;
    // The part's turn as a world rotation: back through the gun hand, on through the free hand,
    // qa qh qg^-1 qa^-1.
    const arm_ik::Quat moveW =
        arm_ik::normalized(arm_ik::mul(arm_ik::mul(qa, arm_ik::mul(qh, arm_ik::conj(qg))), arm_ik::conj(qa)));
    for_parts(g_carryPart, [&](int i) {
        float p[3], q[4];
        if (!bones::weapon_bone_pose(i, p, q)) return;
        const Vec d = Vec{p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]};
        const Vec world = arm_ik::vec(o0) + arm_ik::rotate(mq, d) * s;
        const Vec drawn = arm_ik::rotate(arm_ik::conj(qa), world - aLoc) * (1.0f / hp.actorScale);
        // Back through the gun hand's placement to where the clip has it ...
        const Vec comp = gunPalm + arm_ik::rotate(arm_ik::conj(qg), drawn - gunPalmAt) * (1.0f / hp.gunS);
        // ... and on through the free hand's, which carries it. At the palm: the part's pivot in the
        // palm itself (the clip's hand is not holding it now).
        const Vec moved = g_carryAtPalm ? palmAt : palmAt + arm_ik::rotate(qh, (comp - palm) * hp.s);
        const Vec world2 = aLoc + arm_ik::rotate(qa, moved * hp.actorScale);
        const Vec back = arm_ik::rotate(mT, world2 - arm_ik::vec(o0)) * (1.0f / s);
        const float np[3] = {p0[0] + back.x, p0[1] + back.y, p0[2] + back.z};
        if (g_watch.load(std::memory_order_relaxed)) {
            static uint64_t s_ms = 0;
            const uint64_t nowMs = GetTickCount64();
            if (nowMs - s_ms >= 500) {
                s_ms = nowMs;
                const Vec palmW = aLoc + arm_ik::rotate(qa, palm * hp.actorScale);
                const Vec palmAtW = aLoc + arm_ik::rotate(qa, palmAt * hp.actorScale);
                BVR_LOG("[reload] carry %s: clip part %.1f %.1f %.1f (palm %.1f %.1f %.1f) -> part %.1f %.1f %.1f (palm %.1f "
                        "%.1f %.1f) | anim p %.1f %.1f %.1f -> %.1f %.1f %.1f, s %.3f",
                        bones::weapon_bone_name(i), world.x, world.y, world.z, palmW.x, palmW.y, palmW.z, world2.x,
                        world2.y, world2.z, palmAtW.x, palmAtW.y, palmAtW.z, p[0], p[1], p[2], np[0], np[1], np[2], s);
            }
        }
        // Turned with the hand: q' = M^-1 (move) M q.
        const arm_ik::Quat nq =
            arm_ik::normalized(arm_ik::mul(mT, arm_ik::mul(moveW, arm_ik::mul(mq, arm_ik::normalized(arm_ik::quat(q))))));
        bones::set_weapon_part_pose(i, np, nq.v);
    });
}

bool pad(bvr::input::PadOverride* pad, bool* changed) {
    constexpr uint16_t kLB = 0x0100; // the hand without the gun is always the plasmid role
    constexpr uint16_t kX = 0x4000;  // the pad's X: the game's reload (XENON_X)
    if (pad->buttons & kX) g_reloadButtonMs.store(GetTickCount64(), std::memory_order_relaxed);
    const bool grip = (pad->buttons & kLB) != 0;
    if (grip && !g_gripDown.exchange(true, std::memory_order_relaxed))
        g_gripPresses.fetch_add(1, std::memory_order_relaxed);
    if (!grip) g_gripDown.store(false, std::memory_order_relaxed);
    const bool claim = g_on.load(std::memory_order_relaxed) && g_claim.load(std::memory_order_relaxed) &&
                       GetTickCount64() - g_claimMs.load(std::memory_order_relaxed) < 250;
    // A squeeze the reload took stays the reload's until it lets go (a bumper appearing mid-squeeze
    // would raise the plasmid wheel).
    if (claim && grip) g_suppressUntilUp = true;
    if (!grip) g_suppressUntilUp = false;
    if ((claim || g_suppressUntilUp) && grip) {
        pad->buttons = static_cast<uint16_t>(pad->buttons & ~kLB);
        *changed = true;
        return true;
    }
    return false;
}

void tick(const Frame& f) {
    if (!f.weapon || !g_on.load(std::memory_order_relaxed) || !script::ready()) {
        if (g_machine.active()) {
            let_go(g_weapon, f.hands);
            g_machine.reset();
        }
        g_active.store(false, std::memory_order_relaxed);
        g_claim.store(false, std::memory_order_relaxed);
        return;
    }
    if (f.weapon != g_weapon) {
        if (g_machine.active()) BVR_LOG("[reload] the weapon changed - manual reload dropped");
        let_go(nullptr, nullptr);
        g_machine.reset();
        g_weapon = f.weapon;
        const wchar_t* c = patterns::object_class_name(f.weapon);
        wcsncpy_s(g_cls, c ? c : L"", _TRUNCATE);
        g_lastHandle = 0;
        g_canMs = 0;
    }
    // The game's reload clip, when it is playing one.
    const hands_state::State st = hands_state::current(f.hands);
    rr::Input in;
    in.now = static_cast<float>(f.now) * 0.001f;
    int32_t hw = 0, hh = 0;
    const wchar_t* clipName = nullptr;
    if (st == hands_state::State::Reloading) {
        hw = weapon_clip_handle(f.weapon);
        hands_handle(f.weapon, &hh);
        int32_t nm = -1;
        if (hw > 0 && script::anim_name(f.weapon, hw, &nm) && nm >= 0) {
            clipName = patterns::fname_text(nm);
            if (nm != g_lenName) {
                g_lenName = nm;
                g_len = 0;
                script::anim_length(f.weapon, nm, &g_len);
            }
        }
    }
    in.clipLive = clipName != nullptr;
    in.clip = clipName;
    in.clipLen = g_len;
    in.clipNew = in.clipLive && hw != g_lastHandle;
    g_lastHandle = in.clipLive ? hw : 0;
    in.clipProgram = in.clipLive ? rr::find(g_cls, clipName) : nullptr;
    in.entryProgram = rr::entry(g_cls);
    in.hand = Vec{f.hand[0], f.hand[1], f.hand[2]};
    in.unitsPerCm = f.unitsPerCm;
    const uint32_t presses = g_gripPresses.load(std::memory_order_relaxed);
    in.grip = g_gripDown.load(std::memory_order_relaxed);
    in.gripRise = presses != g_seenPresses;
    g_seenPresses = presses;
    in.inPouch = f.inPouch;
    in.gunDegPerSec = f.gunDegPerSec;
    in.canReload = !g_machine.active() && in.entryProgram ? can_reload(f.weapon, f.now) : true;
    // A reload that began without the reload button (the gun ran dry): the game's own, but it waits
    // for the hands. One the button began plays as it always has.
    in.adopt = in.clipNew && f.now - g_reloadButtonMs.load(std::memory_order_relaxed) > 1500;

    const bool wasActive = g_machine.active();
    const rr::Output o = g_machine.update(in, g_cfg);

    if (o.act == rr::Act::StartReload) {
        struct { int32_t pad[4]; } none{};
        const bool ok = script::call(f.weapon, "Reload", &none);
        ++g_starts;
        BVR_LOG("[reload] %S: the game is asked to reload (%s)", g_cls, ok ? "called" : "FAILED");
    }
    if (o.act == rr::Act::Abort) ++g_aborts;
    if (in.clipLive && g_machine.active()) {
        if (o.hold)
            hold(f.weapon, f.hands, hw, hh, o.t);
        else if (o.run)
            run(f.weapon, f.hands, hw, hh, o.jump, o.t);
    }
    if (wasActive && !g_machine.active()) {
        let_go(f.weapon, f.hands);
        ++g_done;
    }
    if (g_machine.active()) apply_parts(o, f);
    else bones::clear_weapon_part_offsets();
    haptic(o.haptic, f);
    if (o.buzz && f.now - g_lastBuzzMs >= kBuzzEveryMs) {
        g_lastBuzzMs = f.now;
        bvr::vr::pulse_hand(f.freePhys, kBuzzAmp, kBuzzMs);
    }
    g_claim.store(o.claim, std::memory_order_relaxed);
    g_claimMs.store(f.now, std::memory_order_relaxed);
    g_active.store(o.active, std::memory_order_relaxed);
    // `vrreload watch on`: twice a second, where the free hand should go next - the current phase's
    // path start (or the entry's), from the hand, in the body's frame (cm).
    if (g_watch.load(std::memory_order_relaxed) && f.now - g_watchMs >= 500) {
        g_watchMs = f.now;
        const rr::Program* p = g_machine.active() ? g_machine.program() : in.entryProgram;
        const int ph = g_machine.active() ? g_machine.phase() : 0;
        const rr::Phase* phase = p && ph < p->n ? &p->phases[ph] : nullptr;
        if (phase && phase->path) {
            // Gripping, where the path leads; otherwise where it begins.
            const Vec target = in.grip && g_machine.active() ? phase->path[phase->n - 1].p : phase->path[0].p;
            Vec d = (target - in.hand) * f.gripScale;
            if (bones::mirror_hands()) { // the hand was read unmirrored; the drawn grip is mirrored
                float dd[3];
                arm_ik::put(d, dd);
                bones::unmirror_grip_dir(dd);
                d = arm_ik::vec(dd);
            }
            const Vec w = arm_ik::rotate(arm_ik::normalized(arm_ik::quat(f.gripQ)), d);
            const float uu = f.uuPerCm > 1e-4f ? f.uuPerCm : 1.0f;
            const float fwd = (w.x * f.bodyF[0] + w.y * f.bodyF[1] + w.z * f.bodyF[2]) / uu;
            const float rgt = (w.x * f.bodyR[0] + w.y * f.bodyR[1] + w.z * f.bodyR[2]) / uu;
            BVR_LOG("[reload] watch: %S %S phase %d %s - next contact fwd %+.1f right %+.1f up %+.1f cm (%.1f cm)%s%s",
                    g_cls, p->clip, ph, kind_name(phase->kind), fwd, rgt, w.z / uu, arm_ik::length(w) / uu,
                    in.grip ? " GRIP" : "", in.inPouch ? " POUCH" : "");
        } else if (phase) {
            BVR_LOG("[reload] watch: %S %S phase %d %s%s%s", g_cls, p->clip, ph, kind_name(phase->kind),
                    in.grip ? " GRIP" : "", in.inPouch ? " POUCH" : "");
        }
    }
    if (o.event) {
        const rr::Program* p = g_machine.program();
        BVR_LOG("[reload] %S %S phase %d %s at %.3f s: %s", g_cls, p ? p->clip : L"-", g_machine.phase(),
                p && g_machine.phase() < p->n ? kind_name(p->phases[g_machine.phase()].kind) : "-", o.t, o.event);
    }
}

float menu_read(menu::Setting id) {
    switch (id) {
        case S::ManualReload: return g_on.load() ? 1.0f : 0.0f;
        default: return std::numeric_limits<float>::quiet_NaN();
    }
}

bool menu_write(menu::Setting id, float value) {
    switch (id) {
        case S::ManualReload:
            if (g_on.exchange(value != 0) != (value != 0))
                BVR_LOG("[reload] manual reload %s", value != 0 ? "ON" : "off");
            return true;
        default: return false;
    }
}

namespace {

// ---- `vrreload probe`: where does the weapon keep its reload animation handles? ----------------
// Weapon.uc stores the handles FinishAnimations waits on (AnimationHandle on the weapon,
// HolderAnimationHandle on its holder) as plain ints. Run during a reload: every int-sized slot of
// the weapon whose value the game accepts as a live handle on the weapon, the holder or the Hands
// actor is logged with the animation's name. The slot whose name is the reload clip IS the field.
void probe(void* handsActor) {
    void* weapon = nullptr;
    if (!hands::current_holdable(&weapon) || !weapon) {
        BVR_LOG("[reload] probe: no weapon in hand");
        return;
    }
    void* pawn = nullptr;
    read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponOwnerOffset, &pawn, sizeof pawn);
    void* actors[3] = {weapon, pawn, handsActor};
    const char* who[3] = {"weapon", "holder", "hands"};
    int found = 0;
    for (uint32_t off = 0x30; off < 0x900; off += 4) {
        int32_t v = 0;
        if (!read_n(static_cast<uint8_t*>(weapon) + off, &v, sizeof v) || v <= 0 || v > 1000000) continue;
        for (int a = 0; a < 3; ++a) {
            if (!actors[a] || !script::anim_valid(actors[a], v)) continue;
            int32_t nm = -1;
            float t = -1.0f, len = -1.0f;
            script::anim_name(actors[a], v, &nm);
            script::anim_time(actors[a], v, &t);
            if (nm >= 0) script::anim_length(actors[a], nm, &len);
            const wchar_t* text = nm >= 0 ? patterns::fname_text(nm) : nullptr;
            BVR_LOG("[reload] probe: weapon+0x%X = %d is a live handle on the %s: '%S' at %.2f of %.2f s", off, v, who[a],
                    text ? text : L"?", t, len);
            ++found;
        }
    }
    BVR_LOG("[reload] probe: %d live handle slot(s) (state %S, %u script call(s), %u failure(s))", found,
            hands_state::current_name(handsActor), script::calls(), script::failures());
}

} // namespace

int32_t weapon_clip_handle(void* weapon) {
    int32_t h = 0;
    if (!weapon || !read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponAnimationHandleOffset, &h, sizeof h))
        return 0;
    return h;
}

void handle_command(const char* args) {
    char verb[24] = {};
    int consumed = 0;
    sscanf_s(args, "%23s%n", verb, static_cast<unsigned>(sizeof verb), &consumed);
    const char* rest = args + consumed;
    while (*rest == ' ') ++rest;
    if (strcmp(verb, "probe") == 0) {
        probe(hands::hands_actor());
        return;
    }
    if (strcmp(verb, "watch") == 0) {
        g_watch.store(strncmp(rest, "off", 3) != 0, std::memory_order_relaxed);
        BVR_LOG("[reload] watch %s", g_watch.load() ? "on" : "off");
        return;
    }
    if (strcmp(verb, "status") == 0) {
        BVR_LOG("[reload] manual %s, %s | %u started, %u ended, %u refused | script calls %u (%u failed)",
                g_on.load() ? "ON" : "off", g_machine.active() ? "ACTIVE" : "idle", g_starts, g_done, g_aborts,
                script::calls(), script::failures());
        return;
    }
    if (strcmp(verb, "hold") == 0 || strcmp(verb, "release") == 0) {
        // Test aid: pause both reload clips at a time (`hold <s>`) or let them run (`release`).
        void* weapon = nullptr;
        void* hands = hands::hands_actor();
        if (!hands::current_holdable(&weapon) || !weapon || !hands) return;
        const int32_t hw = weapon_clip_handle(weapon);
        int32_t hh = 0;
        read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponHandsAnimationHandleOffset, &hh, sizeof hh);
        if (verb[0] == 'h') {
            float t = 0.0f;
            sscanf_s(rest, "%f", &t);
            const bool a = script::pause_anim(weapon, hw), b = script::pause_anim(hands, hh);
            const bool c = script::set_anim_time(weapon, hw, t), d = script::set_anim_time(hands, hh, t);
            BVR_LOG("[reload] hold at %.2f: pause %d/%d, time %d/%d (handles %d/%d)", t, a, b, c, d, hw, hh);
        } else {
            const bool a = script::resume_anim(weapon, hw), b = script::resume_anim(hands, hh);
            BVR_LOG("[reload] release: resume %d/%d", a, b);
        }
        return;
    }
    if (strcmp(verb, "give") == 0) {
        // A test aid: ShockPlayer.GiveItem(Amount, "Package.Class"), e.g. `vrreload give 24
        // ShockGame.Pistol_Bullet`. The string is ours and only read (DynamicLoadObject); UE2's
        // ProcessEvent never destroys the caller's parameters.
        int amount = 0;
        char cls[96] = {};
        if (sscanf_s(rest, "%d %95s", &amount, cls, static_cast<unsigned>(sizeof cls)) != 2) {
            BVR_LOG("[reload] usage: vrreload give <amount> <Package.Class>");
            return;
        }
        void* weapon = nullptr;
        void* pawn = nullptr;
        if (!hands::current_holdable(&weapon) || !weapon ||
            !read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponOwnerOffset, &pawn, sizeof pawn) || !pawn) {
            BVR_LOG("[reload] give: no weapon (and so no player) in hand");
            return;
        }
        static wchar_t s_text[96];
        int n = 0;
        for (; n < 95 && cls[n]; ++n) s_text[n] = static_cast<wchar_t>(cls[n]);
        s_text[n] = 0;
        struct { int32_t amount; wchar_t* data; int32_t num, max; int32_t pad[4]; } p{amount, s_text, n + 1, n + 1, {}};
        const bool ok = script::call(pawn, "GiveItem", &p);
        BVR_LOG("[reload] give %d %s: %s", amount, cls, ok ? "called" : "FAILED");
        return;
    }
    BVR_LOG("[reload] usage: vrreload probe | give <amount> <Package.Class>");
}

} // namespace bvr::b1r::manual_reload
