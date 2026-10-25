#include "game/bioshock1r/manual_reload.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/dropped_part.h"
#include "game/bioshock1r/grip_pistol_cylinder.h"
#include "game/bioshock1r/grip_shotgun_shell.h"
#include "game/bioshock1r/grip_crossbow_box.h"
#include "game/bioshock1r/grip_tommy_drum.h"
#include "game/bioshock1r/gun_geometry.h"
#include "game/bioshock1r/gun_suppress.h"
#include "game/bioshock1r/gun_sounds.h"
#include "game/bioshock1r/two_hand.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/muzzle.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/reload_rig.h"
#include "game/bioshock1r/script_call.h"
#include "game/bioshock1r/sound_engine.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <atomic>
#include <chrono>
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
// 2026-10-21 (HELD_GUN_MATRIX.md G2, G3): a reload may be ended early - the shotgun's by a pump stroke while it waits
// for a shell, the pistol's by the reload button while it waits for a cylinder - through the game's own interrupt
// (abort_reload below). F10 "End a reload early"; off if the hands ever stay down after one.
std::atomic<bool> g_abortOn{true};
rr::Machine g_machine;
rr::Config g_cfg;
rr::Pump g_pump;          // the shotgun's pump by hand
// 2026-10-21 (the tester: "you can always cock it", never by itself; the cocking sound only when the player cocks):
// between shots the Fire clip waits after its recoil (0.45 s) - before the pump and its sound - for the hands,
// held or not, and plays on through them only as a stroke begins.
rr::PumpConfig shot_pump_cfg() {
    rr::PumpConfig c;
    c.waitT = 0.45f;
    c.waitForHands = true;
    return c;
}
rr::PumpConfig g_pumpCfg = shot_pump_cfg();
// 2026-10-21 (step 5): ONE pump. The pump between shots, the reload's last pump, the dry cock (the shotgun loaded
// from empty and not yet pumped: the trigger fires nothing until the hands pump it) and the loop-ending pump are
// one ratchet (g_pump above) on the front hand's travel along the barrel; the use chooses the clip and config.
// 2026-10-21 (HELD_GUN_AUDIT.md 2.1): THE GUN'S STATE IS ONE RECORD, assembled once per tick (gun_state.h), and the
// trigger block, the dry click and the two-handed grip's permission are functions of it. The shotgun left uncocked
// is remembered per weapon (a reload begun empty, cut short or not yet pumped by hand) and survives a switch; it
// used to be one bool cleared by any other gun in hand.
gun_state::State g_state;
void* g_uncockedGun = nullptr;
bool g_pumpHolding = false;    // the shot pump holds the Fire clip for the hands this tick (a stroke or its wait)
std::atomic<float> g_pumpBack{0.0f}; // game thread -> bones: the pump's position, 0 forward .. 1 at the rear stop
int g_rounds = -1, g_magSize = -1, g_spareRounds = -1; // the game's counts, as last read (can_reload, 4 Hz)
int g_roundsAtStart = -1;      // the rounds the gun had as its reload's clip began (read fresh then)
std::atomic<bool> g_fireAllowed{true}; // gun_state::fire_allowed(g_state), for the XInput thread's trigger
std::atomic<bool> g_handsReload{false}; // the gun in hand has a manual reload: the reload button is never the game's
std::atomic<bool> g_handFree{false}; // the reload waits for a hand that holds nothing of it (Machine::hand_free)
// The shotgun reload's last pump (reload_rig.h Kind::PumpHand): the same Pump over the end clip's own pump
// times, waiting for the hands instead of pumping itself.
rr::PumpConfig end_pump_cfg() {
    rr::PumpConfig c;
    c.backT0 = 0.409f;
    c.backT1 = 0.487f;
    c.fwdT0 = 0.532f;
    c.fwdT1 = 0.676f;
    c.earlyCm = 2.5f; // a stroke begun moves it off the wait (a fresh grab settles first: settleS)
    c.waitForHands = true;
    return c;
}
bool g_endPumpDone = false;
std::atomic<bool> g_pumpWait{false}; // game thread -> bones/two-hand: the grip is the two-handed grip's now
std::atomic<uint64_t> g_fireClipMs{0}; // the shotgun's Fire clip was last seen playing

// XInput thread <-> game thread.
std::atomic<bool> g_gripDown{false};
std::atomic<uint32_t> g_gripPresses{0};
std::atomic<bool> g_claim{false};
std::atomic<float> g_reach{-1.0f}; // Output::reach, for the grip arbiter
std::atomic<float> g_spareReach{-1.0f}; // Output::spareReach
std::atomic<uint64_t> g_claimMs{0};
std::atomic<bool> g_active{false};
std::atomic<bool> g_handles{false}; // handles_reloads()
std::atomic<bool> g_sticky{false};  // sticky_grip(): the shotgun
std::atomic<uint64_t> g_reloadButtonMs{0}; // the last time the reload button was down
// 2026-10-20: with the manual reload on, the reload button ejects instead (the gun in hand has a
// program that fetches a new magazine): game thread -> XInput thread, and the press back.
std::atomic<bool> g_ejectable{false};
std::atomic<bool> g_ejectReq{false};
bool g_xDown = false; // XInput thread
bool g_suppressUntilUp = false; // XInput thread
// The shotgun's pump: the trigger's pulls (XInput thread -> game thread), and whether a stroke is in
// progress (game thread -> the two-handed grip, which must not let go while the hand works the pump).
std::atomic<uint32_t> g_triggerPulls{0};
bool g_triggerWas = false; // XInput thread
uint32_t g_seenPulls = 0;  // game thread
std::atomic<bool> g_pumping{false};
float g_gunKeep = 1.0f;    // game thread: gun_motion_keep

// Game thread.
uint32_t g_seenPresses = 0;
void* g_weapon = nullptr;
wchar_t g_cls[64] = L"";
int32_t g_lastHandle = 0;
bool g_paused = false;
int32_t g_heldW = 0, g_heldH = 0;
uint64_t g_lastBuzzMs = 0, g_canMs = 0;
uint64_t g_switchMs = 0; // the last ammunition switch seen (a reload begun soon after it is the switch's)
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

// Every hands clip a hold must cover: the one the weapon names (PlayerWeapon.HandsAnimationHandle) and,
// when it is another, the Hands actor's own (Hands.HandsAnimationHandle). 2026-10-20: holding only the
// first, the shotgun's hands kept looping their pump after a shot, and the pistol's carried cylinder
// oscillated with Stereo too - a hand still animating under a part placed against it.
int32_t actor_hands_handle(void* hands) {
    int32_t h = 0;
    const int32_t off = hands ? script::property_offset(hands, L"HandsAnimationHandle") : -1;
    if (off >= 0) read_n(static_cast<uint8_t*>(hands) + off, &h, sizeof h);
    return h;
}
template <typename Fn>
void each_hands(void* hands, int32_t hh, Fn fn) {
    if (!hands) return;
    if (hh > 0) fn(hh);
    const int32_t h2 = actor_hands_handle(hands);
    if (h2 > 0 && h2 != hh) fn(h2);
}

// The clips' own rate before a chase sped them up (restored when the chase ends).
float g_rateW = 1.0f, g_rateH = 1.0f;
bool g_chasing = false;
void chase_rate(void* weapon, void* hands, int32_t hw, int32_t hh, float rate) {
    if (!g_chasing) {
        if (!script::anim_rate(weapon, hw, &g_rateW) || !(g_rateW > 0.0f)) g_rateW = 1.0f;
        if (!(hh > 0 && hands && script::anim_rate(hands, hh, &g_rateH)) || !(g_rateH > 0.0f)) g_rateH = 1.0f;
        g_chasing = true;
    }
    script::set_anim_rate(weapon, hw, rate);
    each_hands(hands, hh, [&](int32_t h) { script::set_anim_rate(hands, h, rate); });
}
void end_chase(void* weapon, void* hands, int32_t hw, int32_t hh) {
    if (!g_chasing) return;
    script::set_anim_rate(weapon, hw, g_rateW);
    each_hands(hands, hh, [&](int32_t h) { script::set_anim_rate(hands, h, g_rateH); });
    g_chasing = false;
}

// Hold both reload clips (the weapon's and the hands') at `t`, paused. `chase` (2026-10-20): a time
// the hand moved FORWARD is reached by PLAYING the clips there (1-4x their speed), not by setting it,
// so the clips' own sound cues (AnimNotify_EffectEvent: the drum seating, the bolt, the pump) fire -
// a set time skips them ("no reload or slide sounds"). Backward or still: set, as before.
void hold(void* weapon, void* hands, int32_t hw, int32_t hh, float t, bool chase = false) {
    float cur = t;
    if (chase && script::anim_time(weapon, hw, &cur) && t > cur + 0.004f) {
        const float rate = (t - cur) / 0.05f; // there within about 50 ms
        chase_rate(weapon, hands, hw, hh, rate < 1.0f ? 1.0f : rate > 4.0f ? 4.0f : rate);
        if (g_paused) {
            script::resume_anim(weapon, hw);
            each_hands(hands, hh, [&](int32_t h) { script::resume_anim(hands, h); });
            g_paused = false;
        }
        g_heldW = hw;
        g_heldH = hh;
        return;
    }
    end_chase(weapon, hands, hw, hh);
    // Paused EVERY frame, not once: a hands clip the game starts on its own handle meanwhile is held too.
    script::pause_anim(weapon, hw);
    each_hands(hands, hh, [&](int32_t h) { script::pause_anim(hands, h); });
    g_paused = true;
    g_heldW = hw;
    g_heldH = hh;
    script::set_anim_time(weapon, hw, t);
    each_hands(hands, hh, [&](int32_t h) { script::set_anim_time(hands, h, t); });
}
void run(void* weapon, void* hands, int32_t hw, int32_t hh, bool jump, float t) {
    end_chase(weapon, hands, hw, hh);
    if (jump) {
        script::set_anim_time(weapon, hw, t);
        each_hands(hands, hh, [&](int32_t h) { script::set_anim_time(hands, h, t); });
    }
    if (g_paused || jump) {
        script::resume_anim(weapon, hw);
        each_hands(hands, hh, [&](int32_t h) { script::resume_anim(hands, h); });
    }
    g_paused = false;
}
// Let go of whatever is held (the reload ended, was refused, or the feature was switched off).
void let_go(void* weapon, void* hands) {
    if (weapon) end_chase(weapon, hands, g_heldW, g_heldH);
    if (g_paused && weapon) {
        script::resume_anim(weapon, g_heldW);
        each_hands(hands, g_heldH, [&](int32_t h) { script::resume_anim(hands, h); });
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
    // 2026-10-20 (15:33 run): the measured factor flipped frame to frame between DrawScale (0.800) and
    // DrawScale x weapon size (0.708) - GetBoneCoords reads the skeleton's matrices, which land before or
    // after the lane writes the scaled pose - and the pistol's carried cylinder jumped between two places
    // (with Stereo too). The drawn gun is always the written one: use that factor; the measurement only
    // checks it.
    float ds = 0.0f;
    if (read_n(static_cast<uint8_t*>(weapon) + patterns::kActorDrawScaleOffset, &ds, sizeof ds) && ds > 0.05f &&
        ds < 20.0f) {
        const float drawn = ds * bones::weapon_written_scale();
        static uint32_t s_flips = 0;
        static uint64_t s_ms = 0;
        if (std::fabs(*s / drawn - 1.0f) > 0.02f) ++s_flips;
        const uint64_t now = GetTickCount64();
        if (s_flips && now - s_ms >= 5000) {
            s_ms = now;
            BVR_LOG("[reload] the gun's space: %u bone read(s) disagreed with the drawn scale %.3f (this one %.3f) - "
                    "the drawn one is used",
                    s_flips, drawn, *s);
            s_flips = 0;
        }
        *s = drawn;
    }
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
bool g_havokCopy = false;       // this eject's spent part fell as a Havok copy (dropped_part.h)
bool g_copyRounds = false;      // ...and that copy is the pistol's rounds, not the part itself
bool g_handLock = false;        // Output::handLock this frame (the bolt held)
float g_lockW = 0.0f;           // ...eased (hand_lock)
uint64_t g_lockMs = 0;
// The latch: the spent part's first bone where it sat in the gun (weapon space, at the eject), and
// the carried new one's distance from there (carry(), read by the next tick).
bool g_seatValid = false;
int g_seatBone = -1;
float g_seatP[3] = {};
// 2026-10-21 (the 20:54 run: no drum would go in at all): the seat was read only while an ejected drum falls, and a
// reload begun at the pouch (no drum in the gun) never has one. The Tommy Gun's drum sits where it sits, open or
// not: its seat is the part bone's REST pose in the weapon skeleton (bones::weapon_rest_bone_pose), a constant of
// the gun - it was read from idle frames every 0.5 s, keyed by nothing (HELD_GUN_AUDIT.md 1.4).
float g_partSeatCm = -1.0f;
uint64_t g_partSeatMs = 0;
float g_uuPerCm = 1.0f;
bool g_fallInit = false;        // the gun's own part falling: where it let go (world), and the floor
Vec g_fallStart{};
float g_floorZ = -1e9f;
const char* g_carryPart = nullptr;
void* g_carryWeapon = nullptr;
bool g_carryAtPalm = false;
// 2026-10-20 THE SPARE DRUM (reload_rig Program::spare): a prop of the gun's own magazine in the free hand,
// taken from the pouch whenever the Tommy Gun is held; let go it is thrown, and a thrown one can be caught
// in the open well.
dropped_part::Prop g_spare;
bool g_spareAmmo = true;        // the player carries rounds beyond the gun's (can_reload)
// The drum against the clip's left wrist (Bip01_L_Hand) where the reload's hand grips it at 0.30 s -
// TG_AmmoClip, Unreal axes, clip units (tools\blender\game_frames.py, the hold the carry uses too).
constexpr float kSpareWristP[3] = {20.8412f, -14.7071f, -8.0871f};
constexpr float kSpareWristQ[4] = {0.6109275f, 0.3447002f, 0.2079246f, 0.6817014f};
// The visible magazine (AmmoModel) against its bone, measured while the gun is idle and steady.
bool g_amRelValid = false;
Vec g_amRelP{};
arm_ik::Quat g_amRelQ{};
float g_amGunS = 1.0f;
uint64_t g_amRelMs = 0;
struct Flying { dropped_part::Prop p; uint64_t ms; };
Flying g_flying[4] = {};
constexpr uint64_t kCatchMs = 4000;   // a thrown drum can be caught this long
constexpr float kCatchCm = 15.0f;     // ... this near where the drum sits in the open gun
// The carried part's recent world positions, for a throw (Half-Life: Alyx's way, ENGINE_NOTES): the
// velocity of the frame next to the fastest of the last few, not of the release frame itself.
constexpr int kThrowFrames = 10;
constexpr uint64_t kThrowGapUs = 100000; // older than this: no throw history
struct ThrowSample { float p[3]; uint64_t us; };
ThrowSample g_throw[kThrowFrames] = {};
int g_throwN = 0, g_throwAt = 0;
uint64_t g_throwFedUs = 0;
uint64_t now_us() { // GetTickCount64's 16 ms steps are too coarse for a hand's speed
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
void throw_feed(const Vec& p) {
    const uint64_t us = now_us();
    if (g_throwN && us - g_throwFedUs < 2000) return; // the same frame (carry runs twice a frame)
    if (g_throwN && us - g_throwFedUs > kThrowGapUs) g_throwN = 0; // a gap: start over
    g_throwFedUs = us;
    g_throw[g_throwAt] = ThrowSample{{p.x, p.y, p.z}, us};
    g_throwAt = (g_throwAt + 1) % kThrowFrames;
    if (g_throwN < kThrowFrames) ++g_throwN;
}
// World units per second; zero when there is no fresh history.
Vec throw_velocity(float* peakOut) {
    *peakOut = 0.0f;
    if (g_throwN < 3 || now_us() - g_throwFedUs > kThrowGapUs) return Vec{0, 0, 0};
    Vec v[kThrowFrames];
    float sp[kThrowFrames];
    int n = 0;
    for (int i = g_throwN - 1; i >= 1; --i) { // oldest pair first
        const ThrowSample& a = g_throw[(g_throwAt - i - 1 + 2 * kThrowFrames) % kThrowFrames];
        const ThrowSample& b = g_throw[(g_throwAt - i + 2 * kThrowFrames) % kThrowFrames];
        const float dt = static_cast<float>(b.us - a.us) * 1e-6f;
        if (!(dt > 1e-4f)) continue;
        v[n] = Vec{(b.p[0] - a.p[0]) / dt, (b.p[1] - a.p[1]) / dt, (b.p[2] - a.p[2]) / dt};
        sp[n] = arm_ik::length(v[n]);
        ++n;
    }
    if (!n) return Vec{0, 0, 0};
    int peak = 0;
    for (int i = 1; i < n; ++i)
        if (sp[i] > sp[peak]) peak = i;
    *peakOut = sp[peak];
    // The neighbour of the peak, toward the middle of the window (Alyx: a peak at either end uses the
    // frame next to it) - one frame of tracking noise never sets the throw.
    const int use = peak == 0 ? (n > 1 ? 1 : 0) : peak == n - 1 ? peak - 1 : peak;
    return v[use];
}
bool g_ejectedThis = false;     // this manual reload ejected its magazine
void* g_showAmmoFor = nullptr;  // ...and it is over: show the magazine on this weapon once it is in hand
// 2026-10-21 (a drum appeared in the Tommy Gun on switching back to it after leaving it with none): a
// Tommy Gun left mid-reload with its drum out stays without one - hidden when it is back in hand, its reload begun
// again at the pouch, and no shot until a drum is in.
void* g_magOut = nullptr;
uint64_t g_magOutAskMs = 0, g_magOutHideMs = 0;
int g_magOutAsks = 0;

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
    // 2026-10-20 the eject: the spent part falls out of the gun under gravity (world down), unless a
    // Havok copy already took its place (then it is simply gone).
    // 12:49 run: the gun's own part lying on the floor flickered and floated - it is drawn in the hands'
    // foreground layer (depth and motion vectors for things near the hands), never the world's. So it
    // only falls out of the gun briefly, then is gone; a world prop (dropped_part) is what stays.
    // The Tommy Gun's drum IS its own world copy: gone at once. The pistol's copy is its rounds: the
    // cylinder still falls out first.
    // 2026-10-22 late night (the tester: the launcher's box "disappeared the second it released rather than
    // dropping"): a part that IS the gun's own bones, with no copy to take over, falls for half a second before it
    // goes - long enough to see it drop, short of the floor it cannot lie on.
    const gun_state::Kind k = gun_state::kind_of(g_cls);
    const float kOwnFallS = (k == gun_state::Kind::Launcher || k == gun_state::Kind::Crossbow) ? 0.5f : 0.12f;
    const bool copyIsPart = g_havokCopy && !g_copyRounds;
    const bool falling = o.carry == 3 && !copyIsPart && o.fallS < kOwnFallS;
    if (o.carry == 3 && !falling) g_carryMode = 2;
    if (g_carryMode != 2 && !falling) return;
    float m[3][3], s = 1, o0[3], p0[3];
    if (!weapon_space(f.weapon, o.part, m, &s, o0, p0) || !(s > 1e-6f)) return;
    const float uu = f.uuPerCm > 1e-4f ? f.uuPerCm : 1.0f;
    Vec w{0, 0, -4000.0f};
    if (falling) {
        g_carryMode = 0; // the clip still has it; it is only moved
        // Fixed in the WORLD, not riding the gun: where the part was when it let go, falling, and
        // stopped at the floor under the player (12:21 headset run: it fell through the floor).
        int first = -1;
        for_parts(o.part, [&](int i) { if (first < 0) first = i; });
        float p[3], q[4];
        if (first < 0 || !bones::weapon_bone_pose(first, p, q)) return;
        const Vec d{p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]};
        // weapon -> world is m's transpose
        const Vec cur{o0[0] + (m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z) * s,
                      o0[1] + (m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z) * s,
                      o0[2] + (m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z) * s};
        if (!g_fallInit) {
            g_fallStart = cur;
            g_fallInit = true;
        }
        Vec target = g_fallStart;
        target.z -= 0.5f * 980.0f * uu * o.fallS * o.fallS;
        if (g_floorZ > -1e8f && target.z < g_floorZ + 2.0f * uu) target.z = g_floorZ + 2.0f * uu;
        w = target - cur;
    }
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
    static const char* const n[] = {"pull", "carry", "fetch", "insert", "flick", "play", "finish", "slide", "pump"};
    return n[static_cast<int>(k)];
}

// Not full? The weapon's own answer (PlayerWeapon.GetRoundsRemaining / GetMagazineSize), at most
// four times a second; unknown counts as yes - the game refuses a reload it cannot do anyway.
bool can_reload(void* weapon, uint64_t now) {
    if (now - g_canMs < 250 && weapon == g_weapon) return g_can;
    g_canMs = now;
    struct { int32_t ret; int32_t pad[3]; } r{}, m{};
    g_rounds = g_magSize = -1;
    if (script::call(weapon, "GetRoundsRemaining", &r)) g_rounds = r.ret;
    if (script::call(weapon, "GetMagazineSize", &m)) g_magSize = m.ret;
    if (g_rounds >= 0 && g_magSize > 0)
        g_can = r.ret < m.ret;
    else
        g_can = true;
    // 2026-10-20: and rounds to reload WITH. PlayerWeapon.Reload reloads only when the rounds in the gun
    // differ from both the magazine's size and the rounds the player carries of the pending ammunition
    // (Holder.GetNumberOfItems) - the headset run's pistol had none spare, and every eject was refused.
    if (g_can && script::properties_ready(weapon)) {
        const int32_t ammoOff = script::property_offset(weapon, L"PendingAmmoSelection");
        void* ammo = nullptr;
        void* holder = nullptr;
        if (ammoOff >= 0 && read_n(static_cast<uint8_t*>(weapon) + ammoOff, &ammo, sizeof ammo) && ammo &&
            read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponOwnerOffset, &holder, sizeof holder) && holder) {
            struct { void* cls; int32_t ret; int32_t pad[2]; } n{ammo, 0, {}};
            if (script::call(holder, "GetNumberOfItems", &n)) g_can = r.ret != n.ret;
        }
    }
    // A spare magazine is there to take while the player carries more rounds than the gun holds (unknown: yes).
    g_spareAmmo = true;
    g_spareRounds = -1;
    if (script::properties_ready(weapon)) {
        const int32_t ammoOff = script::property_offset(weapon, L"PendingAmmoSelection");
        void* ammo = nullptr;
        void* holder = nullptr;
        struct { int32_t ret; int32_t pad[3]; } rr2{};
        if (ammoOff >= 0 && read_n(static_cast<uint8_t*>(weapon) + ammoOff, &ammo, sizeof ammo) && ammo &&
            read_n(static_cast<uint8_t*>(weapon) + patterns::kWeaponOwnerOffset, &holder, sizeof holder) && holder &&
            script::call(weapon, "GetRoundsRemaining", &rr2)) {
            struct { void* cls; int32_t ret; int32_t pad[2]; } n{ammo, 0, {}};
            if (script::call(holder, "GetNumberOfItems", &n)) {
                g_spareAmmo = n.ret > rr2.ret;
                g_spareRounds = n.ret - rr2.ret; // the count includes the rounds in the gun
            }
        }
    }
    return g_can;
}
} // namespace

const gun_state::State& state() { return g_state; }

bool active() { return g_active.load(std::memory_order_relaxed); }
bool handles_reloads() { return g_handles.load(std::memory_order_relaxed); }
bool start_clip(void* weapon) {
    if (!weapon) return false;
    const int32_t hw = weapon_clip_handle(weapon);
    int32_t nm = -1;
    if (hw <= 0 || !script::anim_name(weapon, hw, &nm) || nm < 0) return false;
    const wchar_t* cn = patterns::fname_text(nm);
    return cn && !_wcsicmp(cn, L"SingleFrame");
}
bool pumping() {
    return g_pumping.load(std::memory_order_relaxed) || GetTickCount64() - g_fireClipMs.load(std::memory_order_relaxed) < 400;
}
bool pump_wait() { return g_pumpWait.load(std::memory_order_relaxed); }
float pump_back() { return g_pumpBack.load(std::memory_order_relaxed); }

const GripTable* carry_grip() {
    // 2026-10-20: the pistol's new cylinder, held in the palm with the fingers over its rim
    // (tools\blender\grip_solve.py --hold rim: middle, ring, pinky and thumb on it along 57-63% of their
    // length, nothing deeper than 0.3) - the clip's own hand there is open, mid-toss.
    static const GripTable kPistolCylinder{grips::kPistolCylinderGrip,
                                           static_cast<int>(sizeof grips::kPistolCylinderGrip / sizeof grips::kPistolCylinderGrip[0]),
                                           grips::kPistolCylinderGripPart,
                                           static_cast<int>(sizeof grips::kPistolCylinderGripPart /
                                                            sizeof grips::kPistolCylinderGripPart[0])};
    // 2026-10-21: the shotgun's shell, across the fingers with them closed round it (grip_solve.py --hold across
    // --part-mesh shotgun_shell.pskx: middle and ring on its side, thumb over its end, nothing deeper than 0.16).
    // It was carried at the palm centre by its bone, the model reaching out through the back of the hand -
    // "above my middle finger knuckle" (the 00:20 recording, replayed with the shell hung on its bone).
    static const GripTable kShotgunShell{grips::kShotgunShellGrip,
                                         static_cast<int>(sizeof grips::kShotgunShellGrip / sizeof grips::kShotgunShellGrip[0]),
                                         grips::kShotgunShellGripPart,
                                         static_cast<int>(sizeof grips::kShotgunShellGripPart /
                                                          sizeof grips::kShotgunShellGripPart[0])};
    if (g_carryMode == 1 && g_machine.active() && g_carryPart && !g_carryAtPalm && !_wcsicmp(g_cls, L"Pistol") &&
        strstr(g_carryPart, "DrumParent"))
        return &kPistolCylinder;
    if (g_carryMode == 1 && g_machine.active() && g_carryPart && !_wcsicmp(g_cls, L"Shotgun") && strstr(g_carryPart, "SG_Shell"))
        return &kShotgunShell;
    // 2026-10-24 (the tester: the new box's pose "pretty good but the position was off"; the 17:00 capture: the box 19
    // units out along the hand, past the fingertips): the carry put the box where the clip has it against the clip's
    // own hand - but the hands' clip is not playing while the reload holds the weapon clip at 1.70 ("hands None"), so
    // that hand was some other pose. The clip's hold at 1.70 as a table instead (tools\blender\clip_grip_emit.py): the
    // fingers round the box and the box 12.3 / -7.5 / -2.4 against the wrist, wherever the hands' clip is.
    static const GripTable kCrossbowBox{grips::kCrossbowBoxGrip,
                                        static_cast<int>(sizeof grips::kCrossbowBoxGrip / sizeof grips::kCrossbowBoxGrip[0]),
                                        grips::kCrossbowBoxGripPart,
                                        static_cast<int>(sizeof grips::kCrossbowBoxGripPart /
                                                         sizeof grips::kCrossbowBoxGripPart[0])};
    if (g_carryMode == 1 && g_machine.active() && g_carryPart && !g_carryAtPalm && !_wcsicmp(g_cls, L"Crossbow") &&
        strstr(g_carryPart, "X_AmmoBox"))
        return &kCrossbowBox;
    // 2026-10-21: a spare Tommy Gun drum in the hand - the reload clip's own hand on the drum at 0.30 s (where the
    // spare is placed against the wrist), not the grip pose ("an unneeded mag has the normal grip pose").
    static const GripTable kTommyDrum{grips::kTommyDrumGrip,
                                      static_cast<int>(sizeof grips::kTommyDrumGrip / sizeof grips::kTommyDrumGrip[0]),
                                      nullptr, 0};
    if (g_spare.actor) return &kTommyDrum;
    return nullptr;
}
// The two-handed grip may hold now: a function of the gun's state (gun_state.h), with the hold read live so a
// hold already on is never dropped by a reload beginning.
bool two_hand_allowed() {
    gun_state::State s = g_state;
    s.twoHanded = two_hand::held();
    return gun_state::two_hand_allowed(s);
}
bool sticky_grip() { return g_sticky.load(std::memory_order_relaxed); }
float gun_motion_keep() { return g_gunKeep; }

bool hand_lock_wanted() { return g_handLock && g_machine.active(); }
const char* phase_text() {
    const rr::Program* p = g_machine.program();
    if (!g_machine.active() || !p || g_machine.phase() >= p->n) return "";
    return p->phases[g_machine.phase()].say ? p->phases[g_machine.phase()].say : "";
}
bool hand_closed_on_part() {
    const rr::Program* p = g_machine.program();
    return hand_lock_wanted() && p && g_machine.phase() < p->n && p->phases[g_machine.phase()].handOnPart;
}
bool dragging() {
    // Waiting at a part and taken hold of (Pull / Slide / Flick dragging): the machine says the hand is not free.
    const rr::Program* p = g_machine.program();
    if (!g_machine.active() || !p || g_machine.phase() >= p->n || g_machine.hand_free() || g_carryMode == 1 ||
        g_state.stage == gun_state::Stage::Dropping)
        return false;
    const rr::Kind k = p->phases[g_machine.phase()].kind;
    return k == rr::Kind::Pull || k == rr::Kind::Slide || k == rr::Kind::Flick;
}
float hand_lock(uint64_t nowMs) {
    // Eased toward the machine's ask: 100 ms onto the part, 200 ms back to the controller.
    const float dt = g_lockMs && nowMs > g_lockMs ? static_cast<float>(nowMs - g_lockMs) : 0.0f;
    g_lockMs = nowMs;
    const bool on = g_handLock && g_machine.active();
    g_lockW += (on ? 1.0f : -1.0f) * dt / (on ? 100.0f : 200.0f);
    g_lockW = g_lockW < 0 ? 0 : g_lockW > 1 ? 1 : g_lockW;
    return g_lockW * g_lockW * (3.0f - 2.0f * g_lockW);
}

// The part the free hand carries moves with the hand exactly as the clip's hand would carry it: the
// clip has the part in the clip's hand (paused at this phase's time), and the free hand is that clip
// hand moved onto the controller - so the same move, applied to the part, keeps it in the drawn hand,
// turning with it. Weapon space -> world (the gun's two bones), world -> the hands' component space,
// the hand's move, and back.
// The spare drum in the free hand: where the reload's hand holds the drum (TG_AmmoClip against the wrist,
// kSpareWrist), moved with the drawn hand as carry() moves a carried part; the prop - the visible magazine
// - against that bone as it sits on the gun (g_amRel).
static void place_spare(const HandPlacement& hp) {
    if (!g_spare.actor) return;
    if (!dropped_part::alive(g_spare, g_weapon)) {
        g_spare = dropped_part::Prop{};
        return;
    }
    const arm_ik::Quat qa = arm_ik::normalized(arm_ik::quat(hp.actorQ)), qh = arm_ik::normalized(arm_ik::quat(hp.q));
    const arm_ik::Quat wq = arm_ik::normalized(arm_ik::quat(hp.wristQ));
    float relP[3] = {kSpareWristP[0], kSpareWristP[1], kSpareWristP[2]};
    float relQ[4] = {kSpareWristQ[0], kSpareWristQ[1], kSpareWristQ[2], kSpareWristQ[3]};
    bones::mirror_free_rel(kSpareWristP, kSpareWristQ, relP, relQ); // the gun in the left hand: the right wrist
    const Vec comp = arm_ik::vec(hp.wristP) + arm_ik::rotate(wq, arm_ik::vec(relP));
    const arm_ik::Quat compQ = arm_ik::normalized(arm_ik::mul(wq, arm_ik::normalized(arm_ik::quat(relQ))));
    const Vec moved = arm_ik::vec(hp.palmAt) + arm_ik::rotate(qh, (comp - arm_ik::vec(hp.palm)) * hp.s);
    const Vec boneW = arm_ik::vec(hp.actorLoc) + arm_ik::rotate(qa, moved * hp.actorScale);
    const arm_ik::Quat boneQ = arm_ik::normalized(arm_ik::mul(qa, arm_ik::mul(qh, compQ)));
    Vec amW = boneW;
    arm_ik::Quat amQ = boneQ;
    if (g_amRelValid) {
        amW = boneW + arm_ik::rotate(boneQ, g_amRelP * g_amGunS);
        amQ = arm_ik::normalized(arm_ik::mul(boneQ, g_amRelQ));
    }
    const float loc[3] = {amW.x, amW.y, amW.z};
    const ue::FRotator r = ue::ue_quat_to_rot(amQ.v);
    const int32_t rot[3] = {r.pitch, r.yaw, r.roll};
    static uint64_t s_lastUs = 0;
    const uint64_t us = now_us();
    const float dt = s_lastUs && us > s_lastUs ? static_cast<float>(us - s_lastUs) * 1e-6f : 1.0f / 90.0f;
    s_lastUs = us;
    dropped_part::move(g_spare, loc, rot, dt);
    throw_feed(amW);
}

void carry(const HandPlacement& hp) {
    place_spare(hp);
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
    bool fed = false;
    for_parts(g_carryPart, [&](int i) {
        float p[3], q[4];
        if (!bones::weapon_bone_pose(i, p, q)) return;
        const Vec d = Vec{p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]};
        const Vec world = arm_ik::vec(o0) + arm_ik::rotate(mq, d) * s;
        const Vec drawn = arm_ik::rotate(arm_ik::conj(qa), world - aLoc) * (1.0f / hp.actorScale);
        // Back through the gun hand's placement to where the clip has it ... and the drawn gun is
        // gunRatio of the clip's about its grip socket (2026-10-20, the pistol's carried cylinder ~4 cm
        // toward the grip: only the hand's scale was undone, so a part at arm's length came back 8% short).
        const float ratio = hp.gunRatio > 1e-3f ? hp.gunRatio : 1.0f;
        const Vec sock = arm_ik::vec(hp.socket);
        const Vec sockDrawn = gunPalmAt + arm_ik::rotate(qg, (sock - gunPalm) * hp.gunS);
        Vec comp = sock + arm_ik::rotate(arm_ik::conj(qg), drawn - sockDrawn) * (1.0f / (hp.gunS * ratio));
        // A designed grip (grips.h, tools\blender\grip_solve.py): the part where the solved hand holds it,
        // against the clip's wrist - not where the clip's own hand has it (the pistol's: an open hand,
        // mid-toss, the cylinder resting on the fingers).
        const grips::GripPart* gp = nullptr;
        if (const GripTable* g = carry_grip())
            for (int k = 0; k < g->partCount; ++k)
                if (!_stricmp(g->parts[k].bone, bones::weapon_bone_name(i))) gp = &g->parts[k];
        arm_ik::Quat compQ{};
        if (gp) {
            // Mirrored (the gun in the left hand), against the right wrist as the left one's mirror.
            float relP[3] = {gp->p[0], gp->p[1], gp->p[2]}, relQ[4] = {gp->q[0], gp->q[1], gp->q[2], gp->q[3]};
            bones::mirror_free_rel(gp->p, gp->q, relP, relQ);
            const arm_ik::Quat wq = arm_ik::normalized(arm_ik::quat(hp.wristQ));
            comp = arm_ik::vec(hp.wristP) + arm_ik::rotate(wq, arm_ik::vec(relP));
            compQ = arm_ik::normalized(arm_ik::mul(wq, arm_ik::normalized(arm_ik::quat(relQ))));
        }
        // ... and on through the free hand's, which carries it. At the palm: the part's pivot in the
        // palm itself (the clip's hand is not holding it now).
        const Vec moved = g_carryAtPalm && !gp ? palmAt : palmAt + arm_ik::rotate(qh, (comp - palm) * hp.s);
        const Vec world2 = aLoc + arm_ik::rotate(qa, moved * hp.actorScale);
        if (!fed) { // the part's first bone, for a throw
            fed = true;
            throw_feed(world2);
        }
        // 2026-10-20 the latch: how far the carried part is from where it sits in the gun (recorded at
        // the eject, the gun's own frame), for the Insert - the PART is measured, not the palm, so the
        // hand's orientation does not matter.
        if (i == g_seatBone && g_seatValid) {
            const Vec seatW = arm_ik::vec(o0) + arm_ik::rotate(mq, arm_ik::vec(g_seatP) - arm_ik::vec(p0)) * s;
            const float uu = g_uuPerCm > 1e-4f ? g_uuPerCm : 1.0f;
            g_partSeatCm = arm_ik::length(world2 - seatW) / uu;
            g_partSeatMs = GetTickCount64();
        }
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
        // Turned with the hand: q' = M^-1 (move) M q - or, gripped by design, the hand's own turn of it:
        // M^-1 qa qh (wrist q_rel).
        const arm_ik::Quat nq =
            gp ? arm_ik::normalized(arm_ik::mul(mT, arm_ik::mul(qa, arm_ik::mul(qh, compQ))))
               : arm_ik::normalized(
                     arm_ik::mul(mT, arm_ik::mul(moveW, arm_ik::mul(mq, arm_ik::normalized(arm_ik::quat(q))))));
        bones::set_weapon_part_pose(i, np, nq.v);
        // Held by a designed grip, the part is drawn at the HAND's size (the grip was solved at the game's own
        // 1:1, hands and gun alike; the gun is drawn smaller than the hand - the pistol's cylinder came out
        // 15% small in the simulation and no finger reached it). World units per clip unit: the hand's
        // actor scale times its placement's, against the gun's drawn factor.
        if (gp && s > 1e-6f) bones::set_weapon_part_scale(i, hp.actorScale * hp.s / s);
    });
}

float reach() {
    if (!g_on.load(std::memory_order_relaxed) ||
        GetTickCount64() - g_claimMs.load(std::memory_order_relaxed) >= 250)
        return -1.0f;
    return g_reach.load(std::memory_order_relaxed);
}
float spare_reach() {
    if (!g_on.load(std::memory_order_relaxed) ||
        GetTickCount64() - g_claimMs.load(std::memory_order_relaxed) >= 250)
        return -1.0f;
    return g_spareReach.load(std::memory_order_relaxed);
}

bool pad(bvr::input::PadOverride* pad, bool* changed, bool locked) {
    constexpr uint16_t kLB = 0x0100; // the hand without the gun is always the plasmid role
    constexpr uint16_t kX = 0x4000;  // the pad's X: the game's reload (XENON_X)
    const bool x = (pad->buttons & kX) != 0;
    // Fresh only: tick() stops with no weapon in hand (a plasmid), and X is then the game's again.
    const bool fresh = GetTickCount64() - g_claimMs.load(std::memory_order_relaxed) < 250;
    const bool ejectable = g_ejectable.load(std::memory_order_relaxed) && fresh;
    // 2026-10-21 ("it still let me press x to reload which shouldnt happen"): on every gun the hands reload, the
    // game never sees the reload button - the shotgun's, which ejects nothing, too: its reload began a reload the
    // hands then had to finish ("automatically reloaded one round ... wouldnt let me fire it").
    if (g_on.load(std::memory_order_relaxed) && fresh && g_handsReload.load(std::memory_order_relaxed)) {
        // The reload button ejects: the game never sees it (its own reload would play the whole clip).
        if (x) {
            pad->buttons = static_cast<uint16_t>(pad->buttons & ~kX);
            *changed = true;
            if (!g_xDown && ejectable) g_ejectReq.store(true, std::memory_order_relaxed);
        }
    } else if (x) {
        g_reloadButtonMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
    g_xDown = x;
    const bool trig = pad->rt >= 128; // the weapon hand's trigger, half a pull
    if (trig && !g_triggerWas) g_triggerPulls.fetch_add(1, std::memory_order_relaxed);
    g_triggerWas = trig;
    // The trigger reaches the game only while the gun's state allows a shot (gun_state::fire_allowed: no manual
    // reload in progress, a magazine in, the shotgun cocked, no pump waiting for the hands); a pull it refuses is
    // counted above and clicks in tick().
    if (!g_fireAllowed.load(std::memory_order_relaxed) && fresh && pad->rt) {
        pad->rt = 0;
        *changed = true;
    }
    // A squeeze another grab owns (the hand-over, the two-handed grip) is never the reload's: it
    // is not a press here at all, so it cannot start one.
    const bool grip = !locked && (pad->buttons & kLB) != 0;
    if (grip && !g_gripDown.exchange(true, std::memory_order_relaxed))
        g_gripPresses.fetch_add(1, std::memory_order_relaxed);
    if (!grip) g_gripDown.store(false, std::memory_order_relaxed);
    const bool claim = g_on.load(std::memory_order_relaxed) && g_claim.load(std::memory_order_relaxed) &&
                       GetTickCount64() - g_claimMs.load(std::memory_order_relaxed) < 250;
    // A squeeze the reload took stays the reload's until it lets go (a bumper appearing mid-squeeze
    // would raise the plasmid wheel).
    // 2026-10-22 (the 10:14 run, 10:17:15: a squeeze at the pouch with the drum still in took the spare AND opened
    // the plasmid wheel - the spare is a prop outside the machine, so nothing claimed the press): a squeeze the
    // arbiter gave the reload (`locked` false: grip_owner::Owner::Reload or Spare) is the reload's from its first
    // frame, claimed or not - the machine's claim comes a frame after the press it counts here.
    if (grip) g_suppressUntilUp = true;
    if (!grip) g_suppressUntilUp = false;
    if ((claim || g_suppressUntilUp) && grip) {
        pad->buttons = static_cast<uint16_t>(pad->buttons & ~kLB);
        *changed = true;
        return true;
    }
    return false;
}

// A bone of the gun as drawn now, in the world (weapon_space; carry()'s own placement of a part).
static bool weapon_bone_world(void* weapon, int bone, const char* exclude, Vec* p, arm_ik::Quat* q, float* s) {
    float m[3][3], o0[3], p0[3], bp[3], bq[4];
    if (!weapon_space(weapon, exclude, m, s, o0, p0) || !bones::weapon_bone_pose(bone, bp, bq)) return false;
    const float mf[9] = {m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
    const arm_ik::Quat mq = arm_ik::conj(arm_ik::from_matrix(mf));
    *p = arm_ik::vec(o0) + arm_ik::rotate(mq, arm_ik::vec(bp) - arm_ik::vec(p0)) * *s;
    *q = arm_ik::normalized(arm_ik::mul(mq, arm_ik::normalized(arm_ik::quat(bq))));
    return true;
}

// The visible magazine against the drum's bone (the spare is drawn the same way against the hand's).
static void measure_ammo_model(void* weapon, uint64_t now) {
    g_amRelMs = now;
    const rr::Program* e = rr::entry(g_cls);
    const int bone = e && e->ejectPart ? bones::weapon_bone_index(e->ejectPart) : -1;
    float loc[3], scale = 1.0f, s = 1.0f;
    int32_t rot[3];
    Vec bp;
    arm_ik::Quat bq;
    if (bone < 0 || !dropped_part::ammo_model_pose(weapon, loc, rot, &scale) ||
        !weapon_bone_world(weapon, bone, nullptr, &bp, &bq, &s) || !(s > 1e-6f))
        return;
    float amq[4];
    ue::ue_rot_to_quat(ue::FRotator{rot[0], rot[1], rot[2]}, amq);
    const arm_ik::Quat ib = arm_ik::conj(bq);
    const Vec relP = arm_ik::rotate(ib, arm_ik::vec(loc) - bp) * (1.0f / s);
    const arm_ik::Quat relQ = arm_ik::normalized(arm_ik::mul(ib, arm_ik::normalized(arm_ik::quat(amq))));
    const bool first = !g_amRelValid || arm_ik::length(relP - g_amRelP) > 1.0f;
    g_amRelP = relP;
    g_amRelQ = relQ;
    g_amGunS = s;
    g_amRelValid = true;
    if (first)
        BVR_LOG("[reload] %S: the visible magazine against %s: %.2f %.2f %.2f (turn %.3f %.3f %.3f %.3f), gun scale %.3f",
                g_cls, e->ejectPart, relP.x, relP.y, relP.z, relQ.v[0], relQ.v[1], relQ.v[2], relQ.v[3], s);
}

// A thrown drum (g_flying) at the open well while the reload waits for a new one: it is caught (the prop goes,
// the machine seats the gun's own).
static bool catch_check(const Frame& f) {
    const rr::Program* p = g_machine.program();
    if (!g_machine.active() || !p || !g_seatValid || g_machine.phase() >= p->n ||
        p->phases[g_machine.phase()].kind != rr::Kind::Fetch)
        return false;
    float m[3][3], s = 1, o0[3], p0[3];
    if (!weapon_space(f.weapon, p->ejectPart, m, &s, o0, p0)) return false;
    const float mf[9] = {m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
    const arm_ik::Quat mq = arm_ik::conj(arm_ik::from_matrix(mf));
    const Vec seatW = arm_ik::vec(o0) + arm_ik::rotate(mq, arm_ik::vec(g_seatP) - arm_ik::vec(p0)) * s;
    const float uu = f.uuPerCm > 1e-4f ? f.uuPerCm : 1.0f;
    for (Flying& fl : g_flying) {
        if (!fl.p.actor) continue;
        if (f.now - fl.ms > kCatchMs || !dropped_part::alive(fl.p, f.weapon)) {
            fl = Flying{};
            continue;
        }
        float loc[3];
        int32_t rot[3];
        if (!dropped_part::location(fl.p, loc, rot)) continue;
        // The prop is the visible magazine; the seat is its bone's.
        Vec bone = arm_ik::vec(loc);
        if (g_amRelValid) {
            float q[4];
            ue::ue_rot_to_quat(ue::FRotator{rot[0], rot[1], rot[2]}, q);
            const arm_ik::Quat bq = arm_ik::normalized(arm_ik::mul(arm_ik::normalized(arm_ik::quat(q)), arm_ik::conj(g_amRelQ)));
            bone = bone - arm_ik::rotate(bq, g_amRelP * g_amGunS);
        }
        const float cm = arm_ik::length(bone - seatW) / uu;
        if (cm <= kCatchCm) {
            dropped_part::destroy(fl.p);
            BVR_LOG("[reload] %S: a thrown drum reached the open well (%.1f cm) - caught", g_cls, cm);
            fl = Flying{};
            return true;
        }
    }
    return false;
}

// Seconds since the two-handed hold began (a fresh grab settles before the pump counts a stroke).
static float held_seconds(uint64_t now) {
    static bool s_was = false;
    static uint64_t s_since = 0;
    const bool h = two_hand::held();
    if (h && !s_was) s_since = now;
    s_was = h;
    return h ? static_cast<float>(now - s_since) * 0.001f : 0.0f;
}

// 2026-10-21 (one cock sound played both strokes at once; each stroke should have its own half): the shotgun's cock,
// a stroke at a time. The game's PumpShotgun sound (weapons_shotgun_pump_01) has both strokes 35 ms apart - a rack
// between shots, too quick to part - so the strokes play the reload end's (weapons_shotgun_reloadEndwav), whose
// strokes stand apart. Its timeline (tools/fsb-probe.ps1 `pcm`, the whole decoded sample, 20 ms steps - the same
// timeline a channel's position uses, `seek` measured 1:1): a click at 280-300, the shell at 480, the pump BACK
// 540-680 (its clack 600-660), a dip at 700-720, FORWARD 740-900 (the slam 840-860). Each part plays as its stroke
// ENDS - the pump hitting home at the back, then at the front - so each starts near its clack, not its slide.
// Without FMOD's channel, the back stroke plays the whole PumpShotgun as before.
// (The first cut points, 130/310/900, came from the `env` mode's readData, which came back 400 ms short at the
// start: both strokes fell in the second part.)
// 2026-10-21 (step 6): one action, one row of the sound table (gun_sounds.h) - a part of the game's own sound
// fired through its effect event (sound_engine::play_part), the whole event when the part cannot be cut, nothing
// when the gun has no row for it. The Tommy's reload sound and the shotgun's stroke parts were played from phase
// numbers and a stroke-flag dance in two helpers; the table and the machine's phase kinds say it now.
void play_action(gun_state::Kind kind, gun_sounds::Action a, void* hands, void* weapon) {
    const gun_sounds::Row* r = gun_sounds::find(kind, a);
    if (!r) return;
    void* actor = r->onWeapon ? weapon : hands;
    if (!actor) return;
    if (r->toMs > r->fromMs && r->sound) {
        if (sound_engine::play_part(actor, r->event, r->sound, r->fromMs, r->toMs, r->lengthMs, r->first)) return;
        if (r->fallback) sound_engine::play(actor, r->fallback);
        return;
    }
    char tag[64];
    sound_engine::play(actor, r->event, r->onWeapon && sound_engine::ammo_tag(weapon, tag, sizeof tag) ? tag : nullptr);
}
void stroke_sound(gun_state::Kind kind, void* hands, void* weapon, const rr::Pump& p, bool wasFwd) {
    gun_sounds::Action a[1];
    if (gun_sounds::pump_actions(wasFwd, p.backDone(), p.done(), a)) play_action(kind, a[0], hands, weapon);
}

// End the game's reload early, the way a weapon switch does (Hands.uc, from the UnrealScript corpus): the hands'
// transition sequence is interrupted (`InterruptStateTransitionExecution`: the sequence index cleared, the hands to
// HandsOffscreen, which ends the latent reload code), the weapon told its reload was interrupted
// (`OnReloadInterrupted`: the three-part shotgun's current ammunition restored - it is none through the loops - and
// its ammo contexts removed), and the hands asked to equip the weapon again (`EquipWeapon`). The script calls are
// by name (script_call.h); each one's result is logged. NOT verified in a run (2026-10-21): if the hands stay down
// after one, the weapon wheel brings them back, and F10 "End a reload early" turns this off.
bool abort_reload(void* weapon, void* hands, const char* why) {
    struct { int32_t pad[4]; } none{};
    const bool a = script::call(weapon, "OnReloadInterrupted", &none);
    const bool b = hands && script::call(hands, "InterruptStateTransitionExecution", &none);
    const bool c = hands && script::call(hands, "EquipWeapon", &none);
    BVR_LOG("[reload] %S: the reload is ended early (%s): OnReloadInterrupted %s, InterruptStateTransitionExecution %s, "
            "EquipWeapon %s",
            g_cls, why, a ? "called" : "FAILED", b ? "called" : "FAILED", c ? "called" : "FAILED");
    return a && b && c;
}

// A manual reload is being dropped (the weapon changed, or put away, or the game ended its reload): with the drum
// out, the gun has none until one goes in; otherwise a magazine it ejected is shown again.
void reload_left(void* weapon, bool magOut) {
    if (magOut && weapon && !_wcsicmp(g_cls, L"MachineGun")) {
        g_magOut = weapon;
        g_magOutAsks = 0;
        g_magOutAskMs = 0;
        if (g_showAmmoFor == weapon) g_showAmmoFor = nullptr;
        BVR_LOG("[reload] %S: left with its drum out - it stays without one until a new one goes in", g_cls);
    } else if (g_ejectedThis) {
        g_showAmmoFor = weapon; // shown when it is back in hand
    }
}

void tick(const Frame& f) {
    sound_engine::tick();
    // The gun sound engine (sound_engine.h): hooked through the hands actor once; told whose events are the gun's.
    if (f.hands) sound_engine::install(f.hands);
    {
        const wchar_t* wc = f.weapon ? patterns::object_class_name(f.weapon) : nullptr;
        const gun_suppress::Record& sr = gun_suppress::record(gun_state::kind_of(wc));
        sound_engine::set_context(f.hands, f.weapon, sr.mutedEvents, f.weapon && g_on.load(std::memory_order_relaxed));
    }
    if (g_spare.actor && (!f.weapon || !g_on.load(std::memory_order_relaxed))) {
        if (f.weapon && dropped_part::alive(g_spare, f.weapon)) dropped_part::destroy(g_spare);
        g_spare = dropped_part::Prop{};
    }
    if (!f.weapon || !g_on.load(std::memory_order_relaxed) || !script::ready()) {
        if (g_machine.active()) {
            const bool out = g_machine.mag_out();
            let_go(g_weapon, f.hands);
            g_machine.reset();
            reload_left(g_weapon, out);
        }
        g_ejectedThis = false;
        g_gunKeep = 1.0f;
        g_state = gun_state::State{};
        g_pumpHolding = false;
        g_pumpBack.store(0.0f, std::memory_order_relaxed);
        g_fireAllowed.store(true, std::memory_order_relaxed);
        g_active.store(false, std::memory_order_relaxed);
        g_handles.store(false, std::memory_order_relaxed);
        g_claim.store(false, std::memory_order_relaxed);
        g_ejectable.store(false, std::memory_order_relaxed);
        g_handsReload.store(false, std::memory_order_relaxed);
        g_handFree.store(false, std::memory_order_relaxed);
        g_ejectReq.store(false, std::memory_order_relaxed);
        return;
    }
    // The script name index, built on a weapon change rather than mid-reload (the first eject's hitch).
    if (f.weapon != g_weapon) script::warm_names();
    if (f.weapon != g_weapon) {
        if (g_machine.active()) BVR_LOG("[reload] the weapon changed - manual reload dropped");
        if (g_machine.active()) reload_left(g_weapon, g_machine.mag_out());
        g_ejectedThis = false;
        let_go(nullptr, nullptr);
        g_machine.reset();
        g_weapon = f.weapon;
        const wchar_t* c = patterns::object_class_name(f.weapon);
        wcsncpy_s(g_cls, c ? c : L"", _TRUNCATE);
        g_lastHandle = 0;
        g_canMs = 0;
    }
    // What the game may not do with this gun (gun_suppress.h): one record, read below for X, the adopted reload,
    // the gun hand's clip motion, the free hand's clip, the magazine's visibility and the muted cues.
    const gun_suppress::Record& rec = gun_suppress::record(gun_state::kind_of(g_cls));
    const gun_state::Kind kind = gun_state::kind_of(g_cls);
    const bool shotgun = kind == gun_state::Kind::Shotgun;
    // (2026-10-22 late night: the crossbow's lever between shots is a two-pull program on its Fire clip,
    // reload_rig.h kCrossbowCock - the pump stays the shotgun's.)
    const bool pumpGun = shotgun;
    const rr::PumpConfig& shotCfg = g_pumpCfg;
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
    // The weapon's live FIRE clip, for the gun hand's clip motion (the suppression record's curve: a shot's
    // recoil and nothing else of a hands-reloaded gun's clips) and for the shotgun's pump below.
    bool fireClipNow = false;
    int32_t fireHandle = 0, fireHandsHandle = 0;
    float fireT = 0.0f;
    if (st == hands_state::State::Firing || st == hands_state::State::PostFiring || g_pump.working()) {
        fireHandle = weapon_clip_handle(f.weapon);
        hands_handle(f.weapon, &fireHandsHandle);
        int32_t nm = -1;
        const wchar_t* cn = nullptr;
        if (fireHandle > 0 && script::anim_name(f.weapon, fireHandle, &nm) && nm >= 0) cn = patterns::fname_text(nm);
        fireClipNow = cn && wcsstr(cn, L"Fire") && script::anim_time(f.weapon, fireHandle, &fireT);
        // 2026-10-23 (the tester: the crossbow "still automatically doing the lever in between bolts"): a Fire clip
        // with a program of its own (the crossbow's lever, kCrossbowCock) is that program's clip. The clip was only
        // ever read while the hands were Reloading, and the lever plays while they are Firing, so the program was
        // never found and the game worked the lever itself (the log: no Crossbow Fire phase line in a whole run).
        // 2026-10-23 (the tester: the bolts "make the sound and disappear" but hit nothing): adopted the moment the clip
        // began, the program refused the trigger in the very tick the shot was being made - the log: the Fired events,
        // then "rounds 5/5 ... FIRE REFUSED" and a dry click - so no bolt ever left. The clip plays on its own up to
        // just short of the program's first phase (the lever), the shot long made, and is adopted there.
        const rr::Program* fp = (!clipName && fireClipNow) ? rr::find(g_cls, cn) : nullptr;
        static int32_t s_fireName = -1;
        static float s_fireLen = 0.0f;
        if (fp && nm != s_fireName) {
            s_fireName = nm;
            s_fireLen = 0.0f;
            script::anim_length(f.weapon, nm, &s_fireLen);
        }
        const float fireScale = fp && s_fireLen > 0 && fp->clipLen > 0 ? s_fireLen / fp->clipLen : 1.0f;
        const float adoptAt = fp && fp->n > 0 ? fp->phases[0].t0 * fireScale - 0.05f : 0.0f;
        if (fp && fireT >= adoptAt) {
            clipName = cn;
            hw = fireHandle;
            hh = fireHandsHandle;
            if (nm != g_lenName) {
                g_lenName = nm;
                g_len = 0;
                script::anim_length(f.weapon, nm, &g_len);
            }
        }
    }
    g_gunKeep = gun_suppress::clip_motion_keep(rec, fireClipNow, fireT, g_rounds > 0);
    in.clipLive = clipName != nullptr;
    in.clip = clipName;
    in.clipLen = g_len;
    in.clipNew = in.clipLive && hw != g_lastHandle;
    g_lastHandle = in.clipLive ? hw : 0;
    in.clipProgram = in.clipLive ? rr::find(g_cls, clipName) : nullptr;
    in.entryProgram = rr::entry(g_cls);
    g_handles.store(rec.freeHandClip && in.entryProgram != nullptr, std::memory_order_relaxed);
    g_sticky.store(!_wcsicmp(g_cls, L"Shotgun"), std::memory_order_relaxed);
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
    // (The record says the game's own reload is adopted; X never reaches the game on such a gun, so every reload
    // it begins is its own: the old "not within 1.5 s of the button" test is a constant now.)
    in.adopt = in.clipNew && rec.adoptGameReload;
    // 2026-10-20: the reload button ejects, on a gun whose program fetches a new magazine (the
    // pistol, the Tommy Gun; the shotgun's reload starts at the pouch, and its button does nothing - pad()).
    const bool ejectable = rec.ejectOnButton && in.entryProgram && !in.entryProgram->entryAtPouch &&
                           rr::first_fetch(in.entryProgram) >= 0;
    g_ejectable.store(ejectable, std::memory_order_relaxed);
    g_handsReload.store(rec.reloadButton && in.entryProgram != nullptr, std::memory_order_relaxed);
    const bool xPressed = g_ejectReq.exchange(false, std::memory_order_relaxed) && ejectable;
    in.eject = xPressed && !g_machine.active();
    in.ejectNow = xPressed && g_machine.active(); // a reload waiting at its first pull drops the magazine
    // 2026-10-24 (the tester: the crossbow drops its box on X "only when its empty"; the 15:2x run: every X on the
    // crossbow fell while its Fire clip waited for the lever between shots - a program with nothing to eject): X on a
    // gun whose program between shots does not eject (the crossbow's lever) ends that program, runs its clip out to the
    // end (no lever animation of the game's own) and drops the box the moment the gun is idle again - the reload then
    // works the lever up on its own clip, waits at the pouch, and the lever comes down by hand after the new box.
    static void* s_ejectQueued = nullptr;
    if (s_ejectQueued && s_ejectQueued != f.weapon) s_ejectQueued = nullptr;
    if (xPressed && g_machine.active() && g_machine.program() && g_machine.program()->ejectT < 0 && in.entryProgram &&
        in.entryProgram->ejectT >= 0 && in.clipLive && clipName && wcsstr(clipName, L"Fire")) {
        in.ejectNow = false;
        if (hw > 0 && g_len > 0.0f) run(f.weapon, f.hands, hw, hh, true, g_len - 0.01f);
        let_go(f.weapon, f.hands);
        g_machine.reset();
        s_ejectQueued = f.weapon;
        BVR_LOG("[reload] %S: X while the lever waits between shots - its clip runs out, then the box drops", g_cls);
    } else if (s_ejectQueued == f.weapon && !g_machine.active() && !in.clipLive && st != hands_state::State::Firing &&
               st != hands_state::State::PostFiring) {
        in.eject = true;
        s_ejectQueued = nullptr;
    }
    // G3: the pistol waiting for a new cylinder (at the pouch, nothing in the hand) - X cancels the reload instead:
    // the gun is put away and comes back closed with the rounds it had (the game refills only at a clip's end).
    if (xPressed && g_abortOn.load(std::memory_order_relaxed) && !_wcsicmp(g_cls, L"Pistol") && g_machine.active() &&
        g_machine.hand_free() && g_machine.program() && g_machine.phase() < g_machine.program()->n &&
        g_machine.program()->phases[g_machine.phase()].kind == rr::Kind::Fetch) {
        in.ejectNow = false;
        abort_reload(f.weapon, f.hands, "the reload button at the pouch");
        let_go(f.weapon, f.hands);
        g_machine.reset();
        g_showAmmoFor = f.weapon;
        g_ejectedThis = false;
    }
    // 21:11 run: a dry Tommy Gun's reload waited at the drum and the tester switched ammunition - the game
    // took the switch (PendingAmmoSelection) but the reload went on waiting: "completely blocked". A switch
    // while it waits drops the drum as X does; the game loads the new ammunition when the clip ends.
    static bool s_switchSeen = false;
    if (!g_machine.active()) s_switchSeen = false;
    if (g_machine.active() && ejectable && !s_switchSeen && script::properties_ready(f.weapon)) {
        const int32_t po = script::property_offset(f.weapon, L"PendingAmmoSelection");
        const int32_t co = script::property_offset(f.weapon, L"CurrentAmmoSelection");
        void *pend = nullptr, *cur = nullptr;
        if (po >= 0 && co >= 0 && read_n(static_cast<uint8_t*>(f.weapon) + po, &pend, sizeof pend) &&
            read_n(static_cast<uint8_t*>(f.weapon) + co, &cur, sizeof cur) && pend && cur && pend != cur) {
            BVR_LOG("[reload] %S: the ammunition was switched while the reload waits - the drum drops", g_cls);
            in.ejectNow = true;
            s_switchSeen = true;
            g_switchMs = f.now;
        }
    }
    in.pumpDone = g_endPumpDone;
    g_endPumpDone = false;
    // And a reload the game begins itself (the gun ran dry) ejects the same way.
    in.ejectAdopted = ejectable;
    // The carried part's distance from its latch (carry(), last frame), while one is known.
    in.partSeatCm = g_seatValid && f.now - g_partSeatMs < 150 ? g_partSeatCm : -1.0f;
    in.magOut = g_magOut && g_magOut == f.weapon;

    const bool wasActive = g_machine.active();
    const rr::Program* progWas = g_machine.program();
    // The rounds the gun has as its reload's clip begins, read fresh on that frame (the game refills only at the
    // clip's end; the 4 Hz count may be a shot stale): the Tommy's bolt is skipped with a round in, and a shotgun
    // begun empty is uncocked until pumped by hand. (This was a script call every idle frame.)
    if (in.clipNew && in.entryProgram && !progWas) {
        struct { int32_t ret; int32_t pad[3]; } r{};
        g_roundsAtStart = script::call(f.weapon, "GetRoundsRemaining", &r) ? r.ret : -1;
    }
    in.chambered = g_roundsAtStart > 0;
    // 2026-10-20 (the tester): the Tommy Gun running dry does not drop its drum - the hand pulls it out
    // (the program's own first step). A reload the game begins with rounds still in the gun (switching
    // ammunition) still drops it.
    // 22:57 run: after an ammunition switch the game ended its reload and began another for the new type -
    // with the gun dry that one waited for the hand to pull the drum ("stuck", "the pull stopped before
    // doing it"). A reload begun for a switch drops the drum, as the user asked of switches.
    bool switching = false;
    if (!g_machine.active() && script::properties_ready(f.weapon)) {
        const int32_t po = script::property_offset(f.weapon, L"PendingAmmoSelection");
        const int32_t co = script::property_offset(f.weapon, L"CurrentAmmoSelection");
        void *pend = nullptr, *cur = nullptr;
        switching = po >= 0 && co >= 0 && read_n(static_cast<uint8_t*>(f.weapon) + po, &pend, sizeof pend) &&
                    read_n(static_cast<uint8_t*>(f.weapon) + co, &cur, sizeof cur) && pend && cur && pend != cur;
    }
    // 09:32 run: a switch while a dry gun waits ends that reload the same frame the drop is asked for, and the
    // game's next reload (for the new type) already has Pending == Current - it waited at the drum again. A
    // reload begun within 1.5 s of a seen switch is the switch's: it drops the drum.
    if (switching) g_switchMs = f.now;
    const bool afterSwitch = g_switchMs && f.now - g_switchMs < 1500;
    // The rounds as the clip began (read fresh above on that frame), never the 4 Hz count: the 23:57 sim run's dry
    // Tommy Gun still showed 1 round in the cache and dropped its drum instead of waiting for the hand.
    // 2026-10-22 (the 10:31 run, the tester: the pistol opened by itself as it ran dry): the pistol's dry reload
    // waits for the hand to pull the cylinder too, as the Tommy's waits for the drum.
    if ((!_wcsicmp(g_cls, L"MachineGun") || !_wcsicmp(g_cls, L"Pistol") || !_wcsicmp(g_cls, L"GrenadeLauncher") ||
         !_wcsicmp(g_cls, L"Crossbow")) &&
        !(g_roundsAtStart > 0) && !switching && !afterSwitch)
        in.ejectAdopted = false;
    // 2026-10-20 THE SPARE DRUM (the tester: "pre pull a mag, drop the current one, then slide in the new
    // one quickly", and "throw one up in the air and slap the gun into it").
    const bool spareGun = in.entryProgram && in.entryProgram->spare;
    if (g_spare.actor && (!spareGun || !dropped_part::alive(g_spare, f.weapon))) {
        if (dropped_part::alive(g_spare, f.weapon)) dropped_part::destroy(g_spare);
        g_spare = dropped_part::Prop{};
    }
    // Never with no drum in (the 20:35 run: measured off the hidden model, the seat moved under the gun and only
    // the clip's own path start would take a drum).
    if (spareGun && !g_machine.active() && st != hands_state::State::Reloading && f.now - g_amRelMs >= 500 &&
        f.gunDegPerSec < 20.0f && g_magOut != f.weapon)
        measure_ammo_model(f.weapon, f.now);
    in.spareOk = spareGun && g_spareAmmo;
    in.spareInHand = g_spare.actor != nullptr;
    in.caught = spareGun && catch_check(f);
    const int phaseWas = g_machine.active() ? g_machine.phase() : -1;
    const bool magOutWas = g_machine.mag_out();
    const rr::Output o = g_machine.update(in, g_cfg);
    if (!_wcsicmp(g_cls, L"MachineGun"))
        {
            gun_sounds::Action acts[2];
            const int n = gun_sounds::reload_actions(progWas, phaseWas, g_machine.active() ? g_machine.phase() : -1, o.ejected, acts);
            for (int i = 0; i < n; ++i) play_action(kind, acts[i], f.hands, f.weapon);
        }
    if (o.spareTake && !g_spare.actor) {
        g_spare = dropped_part::make_held(f.weapon, f.hands);
        BVR_LOG("[reload] %S: a spare from the pouch - %s", g_cls, g_spare.actor ? "in the hand" : "NO prop (nothing shows)");
    }
    if (o.spareUsed && g_spare.actor) {
        if (dropped_part::alive(g_spare, f.weapon)) dropped_part::destroy(g_spare);
        g_spare = dropped_part::Prop{};
        BVR_LOG("[reload] %S: the spare goes into the reload - the gun's own drum rides the hand", g_cls);
    }
    if (o.spareLetGo && g_spare.actor) {
        float peak = 0.0f;
        const Vec v = throw_velocity(&peak);
        const float vel[3] = {v.x, v.y, v.z};
        const bool ok = dropped_part::alive(g_spare, f.weapon) && dropped_part::release(g_spare, f.weapon, f.hands, vel);
        if (ok) {
            int slot = 0;
            for (int i = 1; i < 4; ++i)
                if (g_flying[i].ms < g_flying[slot].ms) slot = i;
            g_flying[slot] = Flying{dropped_part::last_dropped(), f.now};
        }
        const float uu = f.uuPerCm > 1e-4f ? f.uuPerCm : 1.0f;
        BVR_LOG("[reload] %S: the spare left the hand at %.2f m/s (peak %.2f) - %s", g_cls, arm_ik::length(v) / uu * 0.01f,
                peak / uu * 0.01f, ok ? "it flies (catchable in the open well)" : "it is gone");
        g_spare = dropped_part::Prop{};
    }
    if (o.ejected) {
        // A Havok copy where the magazine sits, when it has a mesh of its own (the Tommy Gun's drum);
        // otherwise the gun's own part falls (the pistol's cylinder is part of the gun's mesh).
        const float still[3] = {0.0f, 0.0f, 0.0f};
        // 2026-10-22 late (the tester: the launcher "just drops the ammo" - the copy was the grenade pack, the
        // gun's own box bones fell apart from it): a part of the gun's own bones with its ammo model riding a
        // socket on them falls as one and is gone - no Havok copy.
        const bool ownBones = kind == gun_state::Kind::Launcher || kind == gun_state::Kind::Crossbow;
        g_havokCopy = !ownBones && dropped_part::capture(f.weapon) && dropped_part::drop(f.weapon, f.hands, still);
        g_copyRounds = g_havokCopy && dropped_part::dropped_rounds_only();
        g_seatValid = false; // the latch is read below, once the clip holds the gun open
        // The floor for the gun's own part, if it falls: the player's feet (Location minus the
        // collision cylinder's half height).
        g_fallInit = false;
        g_floorZ = -1e9f;
        void* pawn = nullptr;
        float loc[3], half = 0.0f;
        if (read_n(static_cast<uint8_t*>(f.weapon) + patterns::kWeaponOwnerOffset, &pawn, sizeof pawn) && pawn &&
            read_n(static_cast<uint8_t*>(pawn) + patterns::kActorLocOffset, loc, sizeof loc)) {
            const int32_t hOff = script::property_offset(pawn, L"CollisionHeight");
            if (hOff >= 0 && read_n(static_cast<uint8_t*>(pawn) + hOff, &half, sizeof half) && half > 1.0f)
                g_floorZ = loc[2] - half;
        }
        BVR_LOG("[reload] %S: the spent %s %s", g_cls, o.part ? o.part : "part",
                g_havokCopy ? "is replaced by a Havok copy, which falls" : "falls out of the gun");
    }
    // The latch: where the spent part sits in the OPEN gun (the clip held at the eject time a few frames;
    // the pistol's cylinder seats with the barrel folded, not where it sat closed). The pose read is
    // the clip's own, without the fall's offset.
    // 2026-10-22: the launcher's pack the same way (its seat is the pack's rest on the closed gun).
    const rr::Program* seatProg = rr::entry(g_cls);
    const bool tommy = seatProg && seatProg->spare && seatProg->ejectPart && !_wcsicmp(g_cls, L"Pistol") == 0;
    if (tommy && g_machine.active() && !g_seatValid) {
        int bone = -1;
        for_parts(seatProg->ejectPart, [&](int i) { if (bone < 0) bone = i; });
        float q[4];
        if (bone >= 0 && bones::weapon_rest_bone_pose(bone, g_seatP, q)) {
            g_seatBone = bone;
            g_seatValid = true;
        }
    }
    if (o.carry == 3 && !g_seatValid && o.fallS > 0.06f && o.part) {
        g_seatBone = -1;
        for_parts(o.part, [&](int i) { if (g_seatBone < 0) g_seatBone = i; });
        float sq[4];
        g_seatValid = g_seatBone >= 0 && bones::weapon_bone_pose(g_seatBone, g_seatP, sq);
    }
    g_uuPerCm = f.uuPerCm;
    if (!g_machine.active()) {
        g_havokCopy = false;
        g_seatValid = false;
    }

    // 2026-10-20 THE VISIBLE MAGAZINE FOLLOWS THE HANDS. The clip hides and shows the gun's AmmoModel
    // itself (AnimNotify_SetAmmoVisible), and a manual reload sets the clip's time as often as it plays
    // it - backward to the pouch after a drop, forward over stretches it never plays - so the notify that
    // shows it again can be skipped: once a Tommy Gun fired with no drum on it. A new part taken from the
    // pouch shows it (the drum in the hand IS the AmmoModel, on TG_AmmoClip), and so does any manual
    // reload of a gun that ejects, once it has ended - refilled or not, the gun has rounds in it.
    const rr::Program* prog = g_machine.program();
    const bool ejects = prog && prog->ejectT >= 0;
    // The shotgun's shell too (20:35 run: "the other bullets turned invisible while I put them in").
    // 2026-10-23 (the tester: a new bolt box "causes a bolt to instantly appear in the chamber before the bolt box is
    // put in"): the crossbow's AmmoModel is not the box (the box is the gun's own bone, X_AmmoBox, carried at the
    // palm) but the bolt in the chamber, so it is shown when the reload ends, not when a box leaves the pouch.
    const bool boltNotBox = !_wcsicmp(g_cls, L"Crossbow");
    if (boltNotBox && wasActive && !g_machine.active()) g_showAmmoFor = f.weapon;
    if (rec.ammoModelVisible && !boltNotBox && (o.fetched || (o.seating && ejects))) {
        struct { int32_t pad[4]; } none{};
        const bool ok = script::call(f.weapon, "ShowAmmoModel", &none);
        BVR_LOG("[reload] %S: %s %s - its magazine shown (%s)", g_cls, o.fetched ? "a new one from the pouch:" : "seating:",
                o.part ? o.part : "part", ok ? "ShowAmmoModel" : "ShowAmmoModel FAILED");
    }
    if (wasActive && !g_machine.active()) reload_left(f.weapon, magOutWas);
    // A new drum in the hand (or seated): the gun has one again.
    if (g_magOut == f.weapon && g_machine.active() && !g_machine.mag_out()) g_magOut = nullptr;
    if (g_magOut == f.weapon) {
        struct { int32_t pad[4]; } none{};
        // Hidden again and again until a new one is in the hand: the gun's equip shows its magazine, and the reload
        // adopted at the pouch never plays the clip's own hide (the 20:35 run: empty only half the time).
        if (f.now - g_magOutHideMs >= 300) {
            g_magOutHideMs = f.now;
            script::call(f.weapon, "HideAmmoModel", &none);
        }
    }
    if (g_magOut == f.weapon && !g_machine.active()) {
        struct { int32_t pad[4]; } none{};
        // Its reload begun again, at rest: the machine adopts it at the pouch (Input::magOut).
        if (st == hands_state::State::Idling && f.now - g_magOutAskMs >= 1500) {
            g_magOutAskMs = f.now;
            if (++g_magOutAsks > 3) {
                // The game will not reload it (no rounds for it): the drum the gun still counts comes back.
                g_magOut = nullptr;
                script::call(f.weapon, "ShowAmmoModel", &none);
                BVR_LOG("[reload] %S: no drum, and the game will not reload it - its drum is shown again", g_cls);
            } else {
                const bool ok = script::call(f.weapon, "Reload", &none);
                BVR_LOG("[reload] %S: no drum in it - the game is asked to reload (%s)", g_cls, ok ? "called" : "FAILED");
            }
        }
    }
    if (!g_machine.active()) g_ejectedThis = false;
    if (o.ejected) g_ejectedThis = true;
    // A part let go away from the gun (the spent drum pulled out by hand, or a new one short of its seat)
    // leaves the hand as a Havok copy moving as the hand moved it - thrown (Half-Life: Alyx's release:
    // the velocity next to the fastest of the last ten frames). The Tommy Gun's drum only: its copy IS the
    // part; the pistol's cylinder has no mesh of its own and simply goes (carry 2).
    if (o.dropped && !_wcsicmp(g_cls, L"MachineGun")) {
        float peak = 0.0f;
        // Alyx's vr_throw_velocity_scale is 1.6; at 1.6 here "a little too powerful" (19:57 run): 1:1.
        constexpr float kThrowScale = 1.0f;
        const Vec v = throw_velocity(&peak) * kThrowScale;
        const float vel[3] = {v.x, v.y, v.z};
        const bool ok = dropped_part::capture(f.weapon) && dropped_part::drop(f.weapon, f.hands, vel);
        // A NEW drum let go short of its seat (the machine is back at its Fetch) is a full one: catchable.
        const rr::Program* dp = g_machine.program();
        if (ok && dp && g_machine.phase() < dp->n && dp->phases[g_machine.phase()].kind == rr::Kind::Fetch) {
            int slot = 0;
            for (int i = 1; i < 4; ++i)
                if (g_flying[i].ms < g_flying[slot].ms) slot = i;
            g_flying[slot] = Flying{dropped_part::last_dropped(), f.now};
        }
        const float uu = f.uuPerCm > 1e-4f ? f.uuPerCm : 1.0f;
        BVR_LOG("[reload] %S: the %s left the hand at %.2f m/s (peak %.2f) - %s", g_cls, o.part ? o.part : "part",
                arm_ik::length(v) / uu * 0.01f, peak / uu * 0.01f, ok ? "a Havok copy flies" : "no copy (it is simply gone)");
    }
    if (g_showAmmoFor == f.weapon && !g_machine.active() && st != hands_state::State::Reloading) {
        struct { int32_t pad[4]; } none{};
        const bool ok = script::call(f.weapon, "ShowAmmoModel", &none);
        BVR_LOG("[reload] %S: the reload is over - its magazine shown (%s)", g_cls,
                ok ? "ShowAmmoModel" : "ShowAmmoModel FAILED");
        g_showAmmoFor = nullptr;
    }

    if (o.act == rr::Act::StartReload) {
        struct { int32_t pad[4]; } none{};
        const bool ok = script::call(f.weapon, "Reload", &none);
        ++g_starts;
        BVR_LOG("[reload] %S: the game is asked to reload (%s)", g_cls, ok ? "called" : "FAILED");
    }
    if (o.act == rr::Act::Abort) ++g_aborts;
    if (in.clipLive && g_machine.active()) {
        if (o.hold)
            hold(f.weapon, f.hands, hw, hh, o.t, o.chase);
        else if (o.run)
            run(f.weapon, f.hands, hw, hh, o.jump, o.t);
    }
    g_pumpWait.store(o.pumpWait, std::memory_order_relaxed);
    if (wasActive && !g_machine.active()) {
        let_go(f.weapon, f.hands);
        ++g_done;
    }
    if (g_machine.active()) apply_parts(o, f);
    else bones::clear_weapon_part_offsets();
    // 2026-10-21 THE COCK. A shotgun reload begun from empty leaves the gun uncocked until its last pump is worked
    // by hand; a reload cut short before it (a weapon change, the wheel) leaves it uncocked, and then the hands
    // cock it dry - the same pump, the pump bone drawn moving with the strokes. Until then the trigger is dead.
    // Remembered per weapon (gun_state): switching away and back does not cock it.
    if (!progWas && g_machine.program() && gun_state::reload_begins_uncocked(kind, g_roundsAtStart)) {
        g_uncockedGun = f.weapon;
        BVR_LOG("[reload] Shotgun: reloading from empty - uncocked until its last pump is worked by hand");
    }
    // ---- ONE PUMP (reload_rig.h Pump; HELD_GUN_AUDIT.md 2.5, step 5) -------------------------------------------
    // The pump between shots (the Fire clip held at 0.45 s for the hands), the reload's last pump (the end clip
    // held at 0.409 s), the dry cock (no clip: the pump bone drawn with the strokes) and the loop-ending pump
    // (G2: a stroke while the reload waits for a shell ends it, cocked) are ONE ratchet on the front hand's
    // travel along the barrel from the gun hand - never the drawn gun, which the clip moves, and never the bare
    // distance between the controllers, which the gun hand's own motion and sideways motion fed as strokes.
    // The use picks the clip and the config; a change of use starts the ratchet afresh.
    // 2026-10-22 (gun_state::pump_need; the 09:31 run: the aim's drift ended an empty gun's reload three times in
    // four seconds): a pump the gun NEEDS keeps its quick stroke; the one it does not - a pump while the reload waits
    // for a shell, which ends the reload - takes a deliberate one (reload_rig.h kDeliberateStrokeCm). Every pump is
    // the player's: a shot's clip waits for the hands whatever the gun has left, and a reload may be pumped out at
    // any shell (223bbe5 let an empty gun pump itself and refused the stroke before the first shell: the 10:14 run).
    using PumpUse = gun_state::PumpUse;
    using PumpNeed = gun_state::PumpNeed;
    static PumpUse s_useWas = PumpUse::None;
    static const rr::PumpConfig s_endCfg = end_pump_cfg();
    PumpUse use = PumpUse::None;
    int32_t pw = 0, ph = 0;
    float clipT = 0.0f;
    if (pumpGun) {
        const rr::Program* lp = g_machine.program();
        const bool atFetch = shotgun && g_machine.active() && lp && g_machine.phase() < lp->n &&
                             lp->phases[g_machine.phase()].kind == rr::Kind::Fetch && g_machine.hand_free();
        if (o.pumpWait && in.clipLive) {
            use = PumpUse::ReloadEnd;
            pw = hw;
            ph = hh;
            script::anim_time(f.weapon, hw, &clipT);
        } else if (g_abortOn.load(std::memory_order_relaxed) && atFetch && two_hand::held()) {
            use = PumpUse::Loop;
            clipT = 2.0f; // past the wait: no clip, only the hands
        } else if (shotgun && g_uncockedGun == f.weapon && !g_machine.active() && st != hands_state::State::Reloading &&
                   st != hands_state::State::Firing) {
            use = PumpUse::Dry;
            clipT = 2.0f;
        } else if (fireClipNow || g_pump.working()) {
            use = PumpUse::Shot;
            pw = fireHandle;
            ph = fireHandsHandle;
            clipT = fireT;
        }
    }
    const PumpNeed need = gun_state::pump_need(use);
    if (use != PumpUse::None) {
        static const char* const kUseName[] = {"-", "pump", "reload's last pump", "dry cock", "pump while loading"};
        rr::PumpInput pi;
        pi.fireClip = use == PumpUse::Shot ? fireClipNow : true;
        static int32_t s_fireHandle = 0;
        pi.clipNew = use != s_useWas || (use == PumpUse::Shot && pw != s_fireHandle);
        s_fireHandle = use == PumpUse::Shot ? pw : 0;
        pi.clipT = clipT;
        pi.held = two_hand::held();
        pi.heldS = held_seconds(f.now);
        {
            // The controllers themselves: the free one against the gun one, along the gun controller's aim forward
            // (XR: -Z of the aim pose). get_hand_pose takes the ROLE slot; the physical hand is the slot swapped
            // when left-handed (bones.cpp m4_phys).
            const bool lh = bvr::input::left_handed();
            const int gunSlot = lh ? 1 - f.gunPhys : f.gunPhys;
            bvr::vr::HeadPose g{}, h{};
            pi.alongOk = bvr::vr::get_hand_pose(gunSlot, true, g) && bvr::vr::get_hand_pose(1 - gunSlot, true, h);
            if (pi.alongOk) {
                const arm_ik::Quat gq = arm_ik::normalized(arm_ik::Quat{{g.qx, g.qy, g.qz, g.qw}});
                // The stroke axis is the gun's (gun_geometry::strokeDir): the barrel for the shotgun's pump, the
                // lever's swing for the crossbow's.
                const gun_geometry::Gun* grow = gun_geometry::find(g_cls);
                const Vec axis = grow ? grow->strokeDir : Vec{0.0f, 0.0f, -1.0f};
                const Vec fwd = arm_ik::rotate(gq, axis);
                const float fm[3] = {h.px, h.py, h.pz}, gm[3] = {g.px, g.py, g.pz}, d[3] = {fwd.x, fwd.y, fwd.z};
                pi.alongCm = rr::along_barrel_cm(fm, gm, d);
            }
        }
        const uint32_t pulls = g_triggerPulls.load(std::memory_order_relaxed);
        pi.triggerRise = pulls != g_seenPulls;
        g_seenPulls = pulls;
        const rr::PumpConfig cfg =
            need == PumpNeed::Optional ? rr::deliberate(shotCfg) : use == PumpUse::ReloadEnd ? s_endCfg : shotCfg;
        const bool fwdWas = g_pump.backDone();
        const rr::PumpOutput po = g_pump.update(pi, cfg);
        // The sound engine: each stroke's sound as the player ends it (its clip's own cue is muted).
        stroke_sound(kind, f.hands, f.weapon, g_pump, fwdWas);
        g_pumping.store(g_pump.working(), std::memory_order_relaxed);
        // 21:11 recording: the hold let go ("slid off") ~0.1 s after each pump - the clip still returning the pump.
        // The whole Fire clip, and a moment after, is the pump's.
        if (use == PumpUse::Shot && fireClipNow) g_fireClipMs.store(f.now, std::memory_order_relaxed);
        if (use == PumpUse::Shot || use == PumpUse::ReloadEnd) {
            // hold()/run() cover the Hands actor's own clip too (each_hands): the 14:11 run held the gun's only,
            // and the hands kept looping their pump with the front hand on it.
            if (po.hold)
                hold(f.weapon, f.hands, pw, ph, po.t, po.chase);
            else if (po.run)
                run(f.weapon, f.hands, pw, ph, po.jump, po.t);
        }
        // The pump's position (0 forward .. 1 at the rear stop), for the two-handed anchor to ride (bones.cpp,
        // gun_geometry::grip_point - 2026-10-22, the tester: the hand stayed at the grip point while the slide went
        // back): with a clip, from the time it is held or played at; without one, from the ratchet's own time.
        float back = 0.0f;
        if (use == PumpUse::Shot || use == PumpUse::ReloadEnd) back = rr::pump_position(cfg, po.hold || po.run ? po.t : clipT);
        else if (g_pump.working()) back = rr::pump_position(cfg, po.t);
        g_pumpBack.store(back, std::memory_order_relaxed);
        if (use == PumpUse::Dry || use == PumpUse::Loop) {
            // No clip: the pump bone drawn along the gun as the strokes go (back over backT0..backT1, forward over
            // fwdT0..fwdT1; SG_Pump's 18-unit stroke in the Fire clip, reload_pose.py).
            const int pump = bones::weapon_bone_index("SG_Pump");
            if (pump >= 0 && back > 0.0f) {
                const float dw[3] = {-18.0f * back, 0.0f, 0.0f};
                bones::set_weapon_part_offset(pump, dw);
            }
        }
        g_pumpHolding = use == PumpUse::Shot && po.hold; // a shot's clip held for the hands: no shot until they pump
        if (po.run) {
            switch (use) {
                case PumpUse::ReloadEnd:
                    g_endPumpDone = true;
                    if (g_uncockedGun == f.weapon) g_uncockedGun = nullptr; // cocked by hand
                    break;
                case PumpUse::Loop:
                    abort_reload(f.weapon, f.hands, "a pump stroke while it waited for a shell");
                    // 2026-10-22 (the 10:31 run: a shell went into the gun on its own after every pump out of a
                    // reload): the weapon's loop clip, held at its fetch, played on from there once let go - the
                    // hands had moved on to the equip, the gun still animated the shell home. It goes to its end.
                    if (hw > 0 && g_len > 0.0f) script::set_anim_time(f.weapon, hw, g_len);
                    let_go(f.weapon, f.hands);
                    g_machine.reset();
                    g_uncockedGun = nullptr;
                    BVR_LOG("[reload] Shotgun: pumped while loading - the reload ends with the shells in, cocked");
                    break;
                case PumpUse::Dry:
                    g_uncockedGun = nullptr;
                    BVR_LOG("[reload] Shotgun: cocked dry by hand - it fires again");
                    break;
                default: break;
            }
        }
        haptic(po.haptic, f);
        // Fire is gated while the pump waits (Half-Life 2 VR's dry click): the gun hand says so; the click itself
        // is the gun state's (every refused pull, below).
        if (po.deny) {
            bvr::vr::pulse_hand(f.gunPhys, 0.6f, 25);
            BVR_LOG("[reload] %S %s: the trigger while it waits - click (%s stroke %.1f cm of %.0f)", g_cls,
                    kUseName[static_cast<int>(use)], g_pump.backDone() ? "forward" : "back", po.strokeCm, cfg.strokeCm);
        }
        if (po.event) BVR_LOG("[reload] %S %s: %s (clips %d / %d)", g_cls, kUseName[static_cast<int>(use)], po.event, pw, ph);
        // Where the stroke is, twice a second while it waits: the 15:37 run's pumps sat for seconds with nothing in
        // the log to say how far the hand had got.
        static uint64_t s_strokeMs = 0;
        if (g_pump.working() && po.strokeCm >= 0.0f && f.now - s_strokeMs >= 500) {
            s_strokeMs = f.now;
            BVR_LOG("[reload] %S %s: %s stroke %.1f cm of %.0f (along %.1f cm, clip %.3f s)%s", g_cls,
                    kUseName[static_cast<int>(use)], g_pump.backDone() ? "forward" : "back", po.strokeCm, cfg.strokeCm,
                    pi.alongCm, po.t, pi.held ? "" : " - not two-handed");
        }
        if (use == PumpUse::ReloadEnd && !pi.held && f.now - g_lastBuzzMs >= 400) { // grab the fore-end: the hand is told where
            g_lastBuzzMs = f.now;
            bvr::vr::pulse_hand(f.freePhys, 0.25f, 30);
        }
    } else {
        if (s_useWas != PumpUse::None) {
            rr::PumpInput off;
            g_pump.update(off, shotCfg); // the ratchet at rest for the next use
        }
        g_pumping.store(false, std::memory_order_relaxed);
        g_pumpHolding = false;
        g_pumpBack.store(0.0f, std::memory_order_relaxed);
    }
    if (use != s_useWas && use != PumpUse::None)
        BVR_LOG("[reload] %S %s: %s stroke (%.0f cm)", g_cls,
                use == PumpUse::Shot ? "pump" : use == PumpUse::ReloadEnd ? "reload's last pump" : use == PumpUse::Dry ? "dry cock" : "pump while loading",
                need == PumpNeed::Optional ? "a deliberate" : "the quick",
                need == PumpNeed::Optional ? rr::kDeliberateStrokeCm : use == PumpUse::ReloadEnd ? s_endCfg.strokeCm : g_pumpCfg.strokeCm);
    s_useWas = use;
    // ---- THE GUN'S STATE, assembled once (gun_state.h) ---------------------------------------------------------
    {
        gun_state::State s;
        s.kind = kind;
        s.handsReload = in.entryProgram != nullptr;
        s.rounds = g_rounds;
        s.magazineSize = g_magSize;
        s.spareRounds = g_spareRounds;
        s.switching = switching;
        const rr::Program* p = g_machine.program();
        const bool ejects = p && p->ejectT >= 0;
        if ((g_magOut && g_magOut == f.weapon) || g_machine.mag_out())
            s.magazine = gun_state::Magazine::Out;
        else if (ejects && o.carry == 1 && o.part)
            s.magazine = gun_state::Magazine::InHand;
        s.spareInHand = g_spare.actor != nullptr;
        if (g_machine.active()) {
            using K = rr::Kind;
            const int ph = g_machine.phase();
            // (A spent part LYING where it fell through the fetch reports the same carry code as the fall, so the
            // drop is the fall held in the pull's phase only - the 23:57 sim run read "dropping" through the pouch.)
            if (!p) s.stage = gun_state::Stage::Starting;
            else if (ph >= p->n) s.stage = gun_state::Stage::Finishing;
            else switch (p->phases[ph].kind) {
                case K::Pull: s.stage = o.carry == 3 ? gun_state::Stage::Dropping : gun_state::Stage::Waiting; break;
                case K::Carry: case K::Insert: s.stage = o.carry == 0 ? gun_state::Stage::Seating : gun_state::Stage::Carrying; break;
                case K::Play: s.stage = gun_state::Stage::Seating; break;
                case K::PumpHand: s.stage = gun_state::Stage::Pumping; break;
                case K::Finish: s.stage = gun_state::Stage::Finishing; break;
                default: s.stage = gun_state::Stage::Waiting; break;
            }
        }
        s.carryingPart = g_carryMode == 1 && g_carryPart != nullptr;
        s.handFree = g_machine.hand_free();
        s.pumpWait = o.pumpWait;
        s.cocked = g_uncockedGun != f.weapon;
        s.pumpHolding = g_pumpHolding;
        s.twoHanded = two_hand::held();
        static gun_state::Stage s_stageWas = gun_state::Stage::None;
        static bool s_fireWas = true;
        const bool fire = gun_state::fire_allowed(s);
        if (s.stage != s_stageWas || fire != s_fireWas)
            BVR_LOG("[reload] %S state: %s, magazine %s%s%s, rounds %d/%d (+%d), %s%s", g_cls, gun_state::stage_name(s.stage),
                    s.magazine == gun_state::Magazine::In ? "in" : s.magazine == gun_state::Magazine::Out ? "OUT" : "in hand",
                    s.spareInHand ? ", a spare in hand" : "", s.cocked ? "" : ", UNCOCKED", s.rounds, s.magazineSize,
                    s.spareRounds, fire ? "fire allowed" : "FIRE REFUSED", s.twoHanded ? ", two-handed" : "");
        s_stageWas = s.stage;
        s_fireWas = fire;
        g_state = s;
        g_fireAllowed.store(fire, std::memory_order_relaxed);
    }
    // THE DRY CLICK: every pull the gun refuses (the trigger is zeroed exactly then, so the game never sees it and
    // never clicks for it) plays the game's own DryFiring on the weapon, the ammunition's name as the tag, as the
    // game does for an empty gun. One rule, every gun, every stage - a dry Tommy Gun waiting at its drum, the
    // pistol's open frame, the shotgun's reload, its uncocked chamber and its pump between shots alike.
    {
        static uint32_t s_dryPulls = 0;
        const uint32_t pulls = g_triggerPulls.load(std::memory_order_relaxed);
        const bool rose = pulls != s_dryPulls;
        s_dryPulls = pulls;
        if (rose && gun_state::dry_click_on_pull(g_state)) play_action(kind, gun_sounds::Action::DryFire, f.hands, f.weapon);
    }
    haptic(o.haptic, f);
    if (o.buzz && f.now - g_lastBuzzMs >= kBuzzEveryMs) {
        g_lastBuzzMs = f.now;
        bvr::vr::pulse_hand(f.freePhys, kBuzzAmp, kBuzzMs);
    }
    g_handLock = o.handLock;
    g_claim.store(o.claim, std::memory_order_relaxed);
    // For the grip arbiter (dial_input): a reload in progress that wants the grip owns it outright.
    // 2026-10-21 (HELD_GUN_MATRIX.md G1, G7): a distance in every stage, never an absolute claim; the spare's own.
    g_reach.store(o.reach, std::memory_order_relaxed);
    g_spareReach.store(o.spareReach, std::memory_order_relaxed);
    g_claimMs.store(f.now, std::memory_order_relaxed);
    g_active.store(o.active, std::memory_order_relaxed);
    g_handFree.store(g_machine.hand_free(), std::memory_order_relaxed);
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
        // Both hands clips a hold covers (each_hands): the weapon's and the Hands actor's own.
        BVR_LOG("[reload] %S %S phase %d %s at %.3f s: %s (hands clips %d / %d)", g_cls, p ? p->clip : L"-",
                g_machine.phase(), p && g_machine.phase() < p->n ? kind_name(p->phases[g_machine.phase()].kind) : "-",
                o.t, o.event, hh, actor_hands_handle(f.hands));
    }
}

float menu_read(menu::Setting id) {
    switch (id) {
        case S::ManualReload: return g_on.load() ? 1.0f : 0.0f;
        case S::ReloadAbort: return g_abortOn.load() ? 1.0f : 0.0f;
        default: return std::numeric_limits<float>::quiet_NaN();
    }
}

bool menu_write(menu::Setting id, float value) {
    switch (id) {
        case S::ManualReload:
            if (g_on.exchange(value != 0) != (value != 0))
                BVR_LOG("[reload] manual reload %s", value != 0 ? "ON" : "off");
            return true;
        case S::ReloadAbort:
            if (g_abortOn.exchange(value != 0) != (value != 0))
                BVR_LOG("[reload] ending a reload early (a pump stroke / the reload button at the pouch) %s", value != 0 ? "ON" : "off");
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
