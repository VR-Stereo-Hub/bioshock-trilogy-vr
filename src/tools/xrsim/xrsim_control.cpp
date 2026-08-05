// bvr_xrsim32: the control channel.
//
//   <dir>\command.txt   agent -> sim, one command per line
//   <dir>\ack.txt       sim -> agent, the last applied batch and the frame it landed on
//   <dir>\state.json    sim -> agent, written atomically
//
// Two deliberate differences from the mod's own command.txt poller
// (game/bioshock1r/camera.cpp poll_command_file), each for a reason:
//
//  1. It polls at 50 Hz on a DEDICATED THREAD, not once per frame. In step mode
//     xrWaitFrame is blocked, so a frame-path poller could never receive the
//     `step` that unblocks it - the sim would deadlock by construction.
//
//  2. It is PARSE-THEN-COMMIT. Lines are staged under a lock and swapped in at
//     one point inside xrWaitFrame, so a multi-line file lands as a single
//     instantaneous rig change and never tears across a frame.
//
// It also discards a command.txt older than its own start time. That is the same
// stale-file trap tools/launch-game.ps1 guard 2 exists for: a leftover
// `head rot 90 0 0` re-applying at boot looks exactly like a camera bug.

#include "xrsim_internal.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <share.h>
#include <string>
#include <thread>

namespace xrsim {
namespace {

std::mutex g_pendingMutex;
Rig g_staging;
bool g_dirty = false;

std::thread g_thread;
std::atomic<bool> g_running{false};
FILETIME g_lastWrite{};
uint64_t g_startMs = 0;
uint64_t g_lastStateWriteMs = 0;
uint32_t g_appliedSeq = 0;
uint32_t g_ackFrame = 0;
char g_ackText[512] = {};

// Timed holds. Evaluated at the commit point so a `btn a press 150` releases on
// a frame boundary rather than whenever the control thread happens to tick.
struct Hold {
    bool active = false;
    bool byFrames = false;
    uint64_t untilMs = 0;
    uint64_t untilFrame = 0;
};
Hold g_holds[VC_COUNT];

// Smooth motion for `head to` / `hand to`.
//
// Smooth hand motion is not a convenience: the mod derives wrench-swing SPEED
// from successive XR hand samples (openxr_input.cpp publish_sample), so a hand
// that teleports between poses cannot produce a swing a user would recognise.
// Driving a real gesture needs interpolation across frames.
struct Motion {
    bool active = false;
    Pose from = pose_identity();
    Pose to = pose_identity();
    uint64_t startMs = 0;
    uint32_t durMs = 0;
    bool linear = false;
    bool isAim = false;   // hand motions only: which pose slot is being driven
};
Motion g_headMotion;
Motion g_handMotion[2];

// `head orbit`
struct Orbit {
    bool active = false;
    float degPerSec = 0.0f;
    uint64_t startMs = 0;
    uint32_t durMs = 0;
    float baseYaw = 0.0f;
};
Orbit g_orbit;

void path_in_dir(wchar_t* out, size_t n, const wchar_t* leaf) {
    swprintf_s(out, n, L"%s\\%s", log::dir(), leaf);
}

// Escape a string for a JSON value. Windows paths are the reason this exists:
// an unescaped "C:\Users\..." makes the whole file invalid JSON, and every
// script that reads state.json then fails with a parse error that looks like a
// torn read rather than what it is. Caught by the first PowerShell reader.
const char* json_escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        switch (c) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += sprintf_s(out + o, cap - o, "\\u%04X", c);
            } else {
                out[o++] = static_cast<char>(c);
            }
            break;
        }
    }
    out[o] = '\0';
    return out;
}

void set_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(g.lastCmdError, _TRUNCATE, fmt, args);
    va_end(args);
    g.errors.fetch_add(1);
    XRSIM_LOG("xrsim: command error - %s", g.lastCmdError);
}

// --- tiny tokenizer --------------------------------------------------------
struct Args {
    char buf[256];
    char* tok[12];
    int n = 0;

    void parse(const char* line) {
        strncpy_s(buf, line, _TRUNCATE);
        n = 0;
        char* ctx = nullptr;
        char* t = strtok_s(buf, " \t\r\n", &ctx);
        while (t && n < 12) {
            tok[n++] = t;
            t = strtok_s(nullptr, " \t\r\n", &ctx);
        }
    }
    const char* s(int i) const { return (i < n) ? tok[i] : ""; }
    bool is(int i, const char* v) const { return i < n && _stricmp(tok[i], v) == 0; }
    float f(int i, float def = 0.0f) const { return (i < n) ? static_cast<float>(atof(tok[i])) : def; }
    uint32_t u(int i, uint32_t def = 0) const {
        return (i < n) ? static_cast<uint32_t>(strtoul(tok[i], nullptr, 10)) : def;
    }
};

int hand_arg(const Args& a, int i) {
    if (a.is(i, "l") || a.is(i, "left")) return 0;
    if (a.is(i, "r") || a.is(i, "right")) return 1;
    return -1;
}

VirtualControl button_by_name(const char* s) {
    if (_stricmp(s, "a") == 0) return VC_BTN_A;
    if (_stricmp(s, "b") == 0) return VC_BTN_B;
    if (_stricmp(s, "x") == 0) return VC_BTN_X;
    if (_stricmp(s, "y") == 0) return VC_BTN_Y;
    if (_stricmp(s, "menu") == 0) return VC_MENU;
    return VC_NONE;
}

void set_control(Rig& rig, VirtualControl c, bool down) {
    switch (c) {
    case VC_BTN_A: rig.btnA = down; break;
    case VC_BTN_B: rig.btnB = down; break;
    case VC_BTN_X: rig.btnX = down; break;
    case VC_BTN_Y: rig.btnY = down; break;
    case VC_MENU: rig.menu = down; break;
    case VC_CLICK_L: rig.click[0] = down; break;
    case VC_CLICK_R: rig.click[1] = down; break;
    case VC_REST_L: rig.rest[0] = down; break;
    case VC_REST_R: rig.rest[1] = down; break;
    default: break;
    }
}

void arm_hold(VirtualControl c, const Args& a, int msArgIndex, uint32_t defaultMs) {
    Hold& h = g_holds[c];
    h.active = true;
    const char* spec = a.s(msArgIndex);
    if (spec[0]) {
        const size_t len = strlen(spec);
        // A trailing 'f' means FRAMES, which is the deterministic form: under
        // `pace step` a millisecond duration has no fixed relationship to how
        // many frames the game actually got.
        if (spec[len - 1] == 'f' || spec[len - 1] == 'F') {
            h.byFrames = true;
            h.untilFrame = snapshot().index + strtoul(spec, nullptr, 10);
            return;
        }
    }
    h.byFrames = false;
    h.untilMs = now_ms() + (spec[0] ? strtoul(spec, nullptr, 10) : defaultMs);
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------

void apply_line(const char* line) {
    Args a;
    a.parse(line);
    if (a.n == 0 || a.s(0)[0] == '#') return;

    strncpy_s(g.lastCmd, line, _TRUNCATE);
    Rig& rig = g_staging;
    g_dirty = true;

    // --- head ---------------------------------------------------------------
    if (a.is(0, "head")) {
        if (a.is(1, "pos")) {
            rig.head.p = v3(a.f(2), a.f(3), a.f(4));
        } else if (a.is(1, "rot")) {
            rig.head.q = quat_from_ypr(deg2rad(a.f(2)), deg2rad(a.f(3)), deg2rad(a.f(4)));
        } else if (a.is(1, "pose")) {
            rig.head.p = v3(a.f(2), a.f(3), a.f(4));
            rig.head.q = quat_from_ypr(deg2rad(a.f(5)), deg2rad(a.f(6)), deg2rad(a.f(7)));
        } else if (a.is(1, "move")) {
            rig.head.p = v3_add(rig.head.p, v3(a.f(2), a.f(3), a.f(4)));
        } else if (a.is(1, "movelocal")) {
            // forward, right, up in the head's own frame
            const Vec3 local = v3(a.f(3), a.f(4), -a.f(2));
            rig.head.p = v3_add(rig.head.p, quat_rotate(rig.head.q, local));
        } else if (a.is(1, "turn")) {
            float y, p, r;
            quat_to_ypr(rig.head.q, y, p, r);
            rig.head.q = quat_from_ypr(y + deg2rad(a.f(2)), p + deg2rad(a.f(3)), r + deg2rad(a.f(4)));
        } else if (a.is(1, "height")) {
            rig.head.p.y = a.f(2, 1.6f);
        } else if (a.is(1, "valid")) {
            rig.headValid = !a.is(2, "off");
        } else if (a.is(1, "to")) {
            g_headMotion.active = true;
            g_headMotion.from = rig.head;
            g_headMotion.to.p = v3(a.f(2), a.f(3), a.f(4));
            g_headMotion.to.q = quat_from_ypr(deg2rad(a.f(5)), deg2rad(a.f(6)), deg2rad(a.f(7)));
            g_headMotion.startMs = now_ms();
            g_headMotion.durMs = a.u(8, 500);
        } else if (a.is(1, "orbit")) {
            g_orbit.active = true;
            g_orbit.degPerSec = a.f(2, 30.0f);
            g_orbit.durMs = a.u(3, 4000);
            g_orbit.startMs = now_ms();
            float y, p, r;
            quat_to_ypr(rig.head.q, y, p, r);
            g_orbit.baseYaw = rad2deg(y);
        } else {
            set_error("unknown head subcommand '%s'", a.s(1));
        }
        return;
    }

    // --- hands --------------------------------------------------------------
    if (a.is(0, "hand")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("hand needs l or r, got '%s'", a.s(1)); return; }

        if (a.is(2, "grip") || a.is(2, "aim")) {
            const bool isAim = a.is(2, "aim");
            Pose& target = isAim ? rig.aim[h] : rig.grip[h];
            if (a.is(3, "pos")) {
                target.p = v3(a.f(4), a.f(5), a.f(6));
                rig.handFollowsHead[h] = false;
            } else if (a.is(3, "rot")) {
                target.q = quat_from_ypr(deg2rad(a.f(4)), deg2rad(a.f(5)), deg2rad(a.f(6)));
                rig.handFollowsHead[h] = false;
            } else if (a.is(3, "pose")) {
                target.p = v3(a.f(4), a.f(5), a.f(6));
                target.q = quat_from_ypr(deg2rad(a.f(7)), deg2rad(a.f(8)), deg2rad(a.f(9)));
                rig.handFollowsHead[h] = false;
            } else {
                set_error("hand %s needs pos, rot or pose", a.s(2));
            }
        } else if (a.is(2, "follow")) {
            rig.handFollowsHead[h] = !a.is(4, "off") && !a.is(3, "off");
        } else if (a.is(2, "offset")) {
            // forward, right, up - the order the mod's own vrhands pos uses
            rig.handOffset[h] = v3(a.f(4), a.f(5), -a.f(3));
        } else if (a.is(2, "aimtrim")) {
            rig.aimTrimPitch[h] = a.f(3);
            rig.aimTrimYaw[h] = a.f(4);
        } else if (a.is(2, "point")) {
            // Convenience: aim relative to where the head is looking.
            rig.handFollowsHead[h] = true;
            rig.aimTrimYaw[h] = a.f(3);
            rig.aimTrimPitch[h] = a.f(4);
        } else if (a.is(2, "valid")) {
            rig.handValid[h] = !a.is(3, "off");
        } else if (a.is(2, "to")) {
            // hand <h> to grip|aim <x> <y> <z> <yaw> <pitch> <roll> <ms>
            //
            // A smooth sweep, which is what the coupling tests need: to tell
            // whether a hand or weapon MODEL is following the controller you
            // have to move the controller continuously and watch the model over
            // several frames. A teleport between two poses cannot show that.
            const bool isAim = a.is(3, "aim");
            Motion& m = g_handMotion[h];
            m.active = true;
            m.isAim = isAim;
            m.from = isAim ? rig.aim[h] : rig.grip[h];
            if (rig.handFollowsHead[h]) {
                // Detach from the head first, or the follow logic would
                // overwrite the interpolated pose every frame.
                Pose parked;
                parked.q = rig.head.q;
                parked.p = v3_add(rig.head.p, quat_rotate(rig.head.q, rig.handOffset[h]));
                m.from = parked;
                rig.grip[h] = parked;
                rig.aim[h] = parked;
                rig.handFollowsHead[h] = false;
            }
            m.to.p = v3(a.f(4), a.f(5), a.f(6));
            m.to.q = quat_from_ypr(deg2rad(a.f(7)), deg2rad(a.f(8)), deg2rad(a.f(9)));
            m.startMs = now_ms();
            m.durMs = a.u(10, 500);
        } else {
            set_error("unknown hand subcommand '%s'", a.s(2));
        }
        return;
    }
    if (a.is(0, "hands") && a.is(1, "reset")) {
        Rig fresh;
        rig_defaults(fresh);
        for (int h = 0; h < 2; ++h) {
            rig.grip[h] = fresh.grip[h];
            rig.aim[h] = fresh.aim[h];
            rig.handValid[h] = true;
            rig.handFollowsHead[h] = true;
            rig.handOffset[h] = fresh.handOffset[h];
            rig.aimTrimPitch[h] = fresh.aimTrimPitch[h];
            rig.aimTrimYaw[h] = fresh.aimTrimYaw[h];
        }
        return;
    }

    // --- buttons and axes ---------------------------------------------------
    if (a.is(0, "btn")) {
        const VirtualControl c = button_by_name(a.s(1));
        if (c == VC_NONE) { set_error("unknown button '%s'", a.s(1)); return; }
        if (a.is(2, "down")) { set_control(rig, c, true); g_holds[c].active = false; }
        else if (a.is(2, "up")) { set_control(rig, c, false); g_holds[c].active = false; }
        else if (a.is(2, "press") || a.is(2, "click")) { set_control(rig, c, true); arm_hold(c, a, 3, 150); }
        else set_error("btn needs down, up or press");
        return;
    }
    if (a.is(0, "click")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("click needs l or r"); return; }
        const VirtualControl c = (h == 0) ? VC_CLICK_L : VC_CLICK_R;
        if (a.is(2, "down")) { set_control(rig, c, true); g_holds[c].active = false; }
        else if (a.is(2, "up")) { set_control(rig, c, false); g_holds[c].active = false; }
        else { set_control(rig, c, true); arm_hold(c, a, 3, 150); }
        return;
    }
    if (a.is(0, "thumbrest")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("thumbrest needs l or r"); return; }
        rig.rest[h] = !a.is(2, "off") && !a.is(2, "release");
        return;
    }
    if (a.is(0, "trigger") || a.is(0, "grip")) {
        const bool isGrip = a.is(0, "grip");
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("%s needs l or r", a.s(0)); return; }
        float* slot = isGrip ? &rig.squeeze[h] : &rig.trigger[h];
        if (a.is(2, "pull") || a.is(2, "squeeze")) {
            *slot = 1.0f;
            const VirtualControl c = isGrip ? (h == 0 ? VC_SQUEEZE_L : VC_SQUEEZE_R)
                                            : (h == 0 ? VC_TRIGGER_L : VC_TRIGGER_R);
            arm_hold(c, a, 3, 200);
        } else {
            float v = a.f(2);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            *slot = v;
        }
        return;
    }
    if (a.is(0, "stick")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("stick needs l or r"); return; }
        if (a.is(2, "center")) {
            rig.stick[h][0] = rig.stick[h][1] = 0.0f;
        } else {
            rig.stick[h][0] = a.f(2);
            rig.stick[h][1] = a.f(3);
        }
        return;
    }
    if (a.is(0, "input") && a.is(1, "clear")) {
        rig.btnA = rig.btnB = rig.btnX = rig.btnY = rig.menu = false;
        for (int h = 0; h < 2; ++h) {
            rig.click[h] = rig.rest[h] = false;
            rig.trigger[h] = rig.squeeze[h] = 0.0f;
            rig.stick[h][0] = rig.stick[h][1] = 0.0f;
        }
        for (auto& hold : g_holds) hold.active = false;
        return;
    }

    // --- pacing -------------------------------------------------------------
    if (a.is(0, "pace")) {
        if (a.is(1, "free")) { g.pacing.mode = PaceMode::Free; if (a.n > 2) g.pacing.hz = a.f(2, 90.0f); }
        else if (a.is(1, "step")) g.pacing.mode = PaceMode::Step;
        else if (a.is(1, "turbo")) g.pacing.mode = PaceMode::Turbo;
        else if (a.is(1, "timeout")) g.pacing.starveMs = a.u(2, kStepStarveMsDefault);
        else if (a.is(1, "onstarve")) g.pacing.starveAdvance = !a.is(2, "hold");
        else set_error("unknown pace mode '%s'", a.s(1));
        pacing_wake();
        return;
    }
    if (a.is(0, "step")) {
        if (a.is(1, "on")) { g.pacing.mode = PaceMode::Step; return; }
        if (a.is(1, "off")) { g.pacing.mode = PaceMode::Free; pacing_wake(); return; }
        if (a.is(1, "timeout")) { g.pacing.starveMs = a.u(2, kStepStarveMsDefault); return; }
        if (a.is(1, "onstarve")) { g.pacing.starveAdvance = !a.is(2, "hold"); return; }
        pacing_grant(a.n > 1 ? a.u(1, 1) : 1);
        return;
    }
    if (a.is(0, "refresh")) {
        const float hz = a.f(1, 90.0f);
        if (hz < 1.0f || hz > 1000.0f) { set_error("refresh out of range: %s", a.s(1)); return; }
        g.pacing.hz = hz;
        return;
    }
    if (a.is(0, "idle")) {
        if (a.is(1, "off")) g.pacing.idleBlockMs = 0;
        else if (a.is(1, "max")) g.pacing.idleMaxMs = a.u(2, kIdleMaxMsDefault);
        else if (a.is(1, "on")) g.pacing.idleBlockMs = a.u(2, 5000);
        else set_error("idle needs on, off or max");
        return;
    }

    // --- session state and hazards -----------------------------------------
    if (a.is(0, "state")) {
        if (a.is(1, "hz")) { g.stateWriteHz.store(a.u(2, 20)); return; }
        XrSessionState s = XR_SESSION_STATE_UNKNOWN;
        if (a.is(1, "ready")) s = XR_SESSION_STATE_READY;
        else if (a.is(1, "synchronized")) s = XR_SESSION_STATE_SYNCHRONIZED;
        else if (a.is(1, "visible")) s = XR_SESSION_STATE_VISIBLE;
        else if (a.is(1, "focused")) s = XR_SESSION_STATE_FOCUSED;
        else if (a.is(1, "stopping")) s = XR_SESSION_STATE_STOPPING;
        else if (a.is(1, "exiting")) s = XR_SESSION_STATE_EXITING;
        else if (a.is(1, "lost")) s = XR_SESSION_STATE_LOSS_PENDING;
        else if (a.is(1, "idle")) s = XR_SESSION_STATE_IDLE;
        if (s == XR_SESSION_STATE_UNKNOWN) { set_error("unknown session state '%s'", a.s(1)); return; }
        if (s == XR_SESSION_STATE_VISIBLE) session_focus_lose(0);
        else session_force_state(s);
        return;
    }
    if (a.is(0, "focus")) {
        if (a.is(1, "lose")) session_focus_lose(a.u(2, 0));
        else if (a.is(1, "regain")) session_force_state(XR_SESSION_STATE_FOCUSED);
        else if (a.is(1, "policy")) g.focusPolicy = a.is(2, "permissive") ? FocusPolicy::Permissive
                                                                         : FocusPolicy::Vdxr;
        else if (a.is(1, "frames")) g.focusFrames = a.u(2, 3);
        else set_error("focus needs lose, regain, policy or frames");
        return;
    }
    if (a.is(0, "hazard")) {
        if (a.is(1, "nosystem")) g.hazards.noSystem = !a.is(2, "off");
        else if (a.is(1, "waitfail")) g.hazards.waitFail = a.u(2, 1);
        else if (a.is(1, "beginfail")) g.hazards.beginFail = a.u(2, 1);
        else if (a.is(1, "endfail")) g.hazards.endFail = a.u(2, 1);
        else if (a.is(1, "swapchainfail")) g.hazards.swapchainFail = !a.is(2, "off");
        else if (a.is(1, "attachfail")) g.hazards.attachFail = !a.is(2, "off");
        else if (a.is(1, "clear")) g.hazards = Hazards{};
        else set_error("unknown hazard '%s'", a.s(1));
        return;
    }
    if (a.is(0, "instanceloss")) {
        XrEventDataBuffer buf{};
        auto* ev = reinterpret_cast<XrEventDataInstanceLossPending*>(&buf);
        ev->type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        ev->lossTime = now_xr_time();
        queue_event(buf);
        return;
    }

    // --- optics -------------------------------------------------------------
    if (a.is(0, "ipd")) {
        rig.ipdM = a.f(1, 63.0f) / 1000.0f;
        return;
    }
    if (a.is(0, "worldscale")) {
        rig.worldScale = a.f(1, 1.0f);
        return;
    }
    if (a.is(0, "fov")) {
        if (a.is(1, "quest3")) {
            Rig fresh;
            rig_defaults(fresh);
            rig.fov[0] = fresh.fov[0];
            rig.fov[1] = fresh.fov[1];
        } else if (a.is(1, "eye")) {
            const int h = hand_arg(a, 2);
            if (h < 0) { set_error("fov eye needs l or r"); return; }
            const Fov f{deg2rad(a.f(3)), deg2rad(a.f(4)), deg2rad(a.f(5)), deg2rad(a.f(6))};
            // Refuse a zero-extent fov rather than accept it and hand back black
            // captures that read as a mod bug.
            if (fov_is_degenerate(f)) {
                set_error("fov eye %s has zero extent (l=%s r=%s u=%s d=%s) - refused",
                          a.s(2), a.s(3), a.s(4), a.s(5));
                return;
            }
            rig.fov[h] = f;
        } else {
            // Symmetric-outer shorthand: `fov 54 55` gives the mod's own
            // half-angle log line exactly h=54.0 v=55.0. Arg fallbacks track
            // the pinned VDXR defaults in rig_defaults (session 37).
            const float hh = deg2rad(a.f(1, 54.0f));
            const float hv = deg2rad(a.f(2, 55.0f));
            const float inner = deg2rad(a.f(3, 44.0f));
            rig.fov[0] = Fov{-hh, inner, hv, -hv};
            rig.fov[1] = Fov{-inner, hh, hv, -hv};
        }
        return;
    }
    if (a.is(0, "recenter")) {
        recenter_local_space();
        return;
    }

    // --- capture ------------------------------------------------------------
    if (a.is(0, "capture")) {
        if (a.is(1, "next")) g.captureCountdown.store(a.u(2, 1));
        else if (a.is(1, "every")) g.captureEvery.store(a.u(2, 0));
        else if (a.is(1, "off")) { g.captureCountdown.store(0); g.captureEvery.store(0); }
        else if (a.is(1, "layers")) g.captureLayers.store(!a.is(2, "off"));
        else if (a.is(1, "size")) { g.captureWidth.store(a.u(2, 1032)); g.captureHeight.store(a.u(3, 1104)); }
        else if (a.is(1, "tag")) strncpy_s(g.captureTag, a.s(2), _TRUNCATE);
        else set_error("unknown capture subcommand '%s'", a.s(1));
        return;
    }
    if (a.is(0, "shot")) {
        if (a.n > 1) strncpy_s(g.captureTag, a.s(1), _TRUNCATE);
        g.captureCountdown.store(1);
        return;
    }
    if (a.is(0, "compose")) {
        g.composeAlways.store(a.is(1, "always"));
        return;
    }

    // --- misc ---------------------------------------------------------------
    if (a.is(0, "runtimename")) { strncpy_s(g.runtimeName, a.s(1), _TRUNCATE); return; }
    if (a.is(0, "systemname")) { strncpy_s(g.systemName, a.s(1), _TRUNCATE); return; }
    if (a.is(0, "profile")) { actions_set_profile(a.is(1, "simple")); return; }
    if (a.is(0, "log")) { XRSIM_LOG("xrsim: MARK %s", line + 3); return; }
    if (a.is(0, "note")) { return; }
    if (a.is(0, "reset")) {
        rig_defaults(g_staging);
        for (auto& h : g_holds) h.active = false;
        g_headMotion.active = false;
        g_handMotion[0].active = g_handMotion[1].active = false;
        g_orbit.active = false;
        g.pacing.mode = PaceMode::Free;
        g.pacing.hz = 90.0;
        g.pacing.idleBlockMs = 0;
        g.hazards = Hazards{};
        g.captureCountdown.store(0);
        g.captureEvery.store(0);
        g.captureLayers.store(true);
        pacing_wake();
        return;
    }
    if (a.is(0, "status")) {
        XRSIM_LOG("xrsim: status frame=%llu state=%s pace=%d hz=%.1f credits=%u",
                  static_cast<unsigned long long>(snapshot().index),
                  session_state_name(current_session_state()), static_cast<int>(g.pacing.mode),
                  g.pacing.hz, g.pacing.credits);
        return;
    }

    set_error("unknown command '%s'", a.s(0));
}

// ---------------------------------------------------------------------------
// state.json
// ---------------------------------------------------------------------------

// Written from BOTH the frame path and the control thread, so it needs its own
// lock: two writers sharing one temp filename would interleave and the atomic
// replace would publish a mixture.
std::mutex g_stateWriteMutex;

void write_state_json() {
    std::lock_guard<std::mutex> writeLock(g_stateWriteMutex);

    wchar_t tmp[MAX_PATH], dst[MAX_PATH];
    path_in_dir(tmp, MAX_PATH, L"state.json.tmp");
    path_in_dir(dst, MAX_PATH, L"state.json");

    FILE* f = _wfsopen(tmp, L"w", _SH_DENYNO);
    if (!f) return;

    FrameSnapshot snap;
    snapshot_copy(snap);
    const Rig& r = snap.rig;
    float hy, hp, hr;
    quat_to_ypr(snap.headWorld.q, hy, hp, hr);

    char escName[512];
    fprintf(f, "{\n");
    fprintf(f, "  \"pid\": %lu,\n", GetCurrentProcessId());
    fprintf(f, "  \"runtime\": \"%s\",\n", json_escape(g.runtimeName, escName, sizeof(escName)));
    fprintf(f, "  \"system\": \"%s\",\n", json_escape(g.systemName, escName, sizeof(escName)));
    fprintf(f, "  \"uptimeMs\": %llu,\n", static_cast<unsigned long long>(now_ms() - g_startMs));
    fprintf(f, "  \"frame\": %llu,\n", static_cast<unsigned long long>(snap.index));
    fprintf(f, "  \"waitFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.waited.load()));
    fprintf(f, "  \"beginFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.begun.load()));
    fprintf(f, "  \"endFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.ended.load()));
    fprintf(f, "  \"frameOpen\": %s,\n", g_gate.open.load() ? "true" : "false");
    fprintf(f, "  \"framesDiscarded\": %u,\n", g_gate.discarded.load());
    fprintf(f, "  \"endsOutOfOrder\": %u,\n", g_gate.outOfOrder.load());
    fprintf(f, "  \"sessionState\": \"%s\",\n", session_state_name(snap.state));
    fprintf(f, "  \"sessionRunning\": %s,\n", session_is_running() ? "true" : "false");
    fprintf(f, "  \"actionsAttached\": %s,\n", actions_attached() ? "true" : "false");
    fprintf(f, "  \"focusPolicy\": \"%s\",\n",
            g.focusPolicy == FocusPolicy::Vdxr ? "vdxr" : "permissive");
    fprintf(f, "  \"paceMode\": \"%s\",\n",
            g.pacing.mode == PaceMode::Step ? "step"
                                            : (g.pacing.mode == PaceMode::Turbo ? "turbo" : "free"));
    fprintf(f, "  \"refreshHz\": %.2f,\n", g.pacing.hz);
    fprintf(f, "  \"stepsPending\": %u,\n", g.pacing.credits);
    fprintf(f, "  \"idleBlockMs\": %u,\n", g.pacing.idleBlockMs);
    fprintf(f, "  \"eventsDropped\": %u,\n", events_dropped());
    fprintf(f, "  \"ipdMm\": %.2f,\n", r.ipdM * 1000.0f);
    fprintf(f, "  \"fovDeg\": {\"l\": %.2f, \"r\": %.2f, \"u\": %.2f, \"d\": %.2f},\n",
            rad2deg(r.fov[0].angleLeft), rad2deg(r.fov[0].angleRight), rad2deg(r.fov[0].angleUp),
            rad2deg(r.fov[0].angleDown));
    fprintf(f, "  \"head\": {\"pos\": [%.4f, %.4f, %.4f], \"ypr\": [%.2f, %.2f, %.2f], \"valid\": %s},\n",
            snap.headWorld.p.x, snap.headWorld.p.y, snap.headWorld.p.z, rad2deg(hy), rad2deg(hp),
            rad2deg(hr), r.headValid ? "true" : "false");

    for (int h = 0; h < 2; ++h) {
        float y, p, rr;
        quat_to_ypr(snap.gripWorld[h].q, y, p, rr);
        fprintf(f, "  \"hand%s\": {\"pos\": [%.4f, %.4f, %.4f], \"ypr\": [%.2f, %.2f, %.2f], "
                   "\"valid\": %s, \"followsHead\": %s},\n",
                h == 0 ? "L" : "R", snap.gripWorld[h].p.x, snap.gripWorld[h].p.y,
                snap.gripWorld[h].p.z, rad2deg(y), rad2deg(p), rad2deg(rr),
                r.handValid[h] ? "true" : "false", r.handFollowsHead[h] ? "true" : "false");
    }

    fprintf(f, "  \"controls\": {\"a\": %s, \"b\": %s, \"x\": %s, \"y\": %s, \"menu\": %s, "
               "\"clickL\": %s, \"clickR\": %s, \"restL\": %s, \"restR\": %s, "
               "\"trigL\": %.3f, \"trigR\": %.3f, \"gripL\": %.3f, \"gripR\": %.3f, "
               "\"stickL\": [%.3f, %.3f], \"stickR\": [%.3f, %.3f]},\n",
            r.btnA ? "true" : "false", r.btnB ? "true" : "false", r.btnX ? "true" : "false",
            r.btnY ? "true" : "false", r.menu ? "true" : "false", r.click[0] ? "true" : "false",
            r.click[1] ? "true" : "false", r.rest[0] ? "true" : "false",
            r.rest[1] ? "true" : "false", r.trigger[0], r.trigger[1], r.squeeze[0], r.squeeze[1],
            r.stick[0][0], r.stick[0][1], r.stick[1][0], r.stick[1][1]);

    fprintf(f, "  \"layersLastFrame\": %u,\n", compositor_last_layer_count());
    fprintf(f, "  \"projectionViews\": %u,\n", compositor_last_projection_views());
    char esc[1024];
    fprintf(f, "  \"captureSeq\": %u,\n", g.captureSeq.load());
    fprintf(f, "  \"lastCapture\": \"%s\",\n", json_escape(g.lastCapturePath, esc, sizeof(esc)));
    fprintf(f, "  \"cmdSeq\": %u,\n", g.cmdSeq.load());
    fprintf(f, "  \"lastCmd\": \"%s\",\n", json_escape(g.lastCmd, esc, sizeof(esc)));
    fprintf(f, "  \"lastCmdError\": \"%s\",\n", json_escape(g.lastCmdError, esc, sizeof(esc)));
    fprintf(f, "  \"errors\": %u\n", g.errors.load());
    fprintf(f, "}\n");
    fclose(f);

    // Atomic replace, or a reader eventually catches a half-written file and the
    // script layer reports a torn-JSON error that is really a writer bug.
    MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}

void write_ack() {
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"ack.txt");
    FILE* f = _wfsopen(path, L"w", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "seq=%u frame=%u error=%s\nlast=%s\n", g.cmdSeq.load(), g_ackFrame,
            g.lastCmdError[0] ? g.lastCmdError : "-", g_ackText);
    fclose(f);
}

// ---------------------------------------------------------------------------
// The poll thread
// ---------------------------------------------------------------------------

void poll_once() {
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"command.txt");

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastWrite) == 0) return;
    g_lastWrite = fad.ftLastWriteTime;

    FILE* f = _wfsopen(path, L"rt", _SH_DENYNO);
    if (!f) return;

    char line[512];
    uint32_t applied = 0;
    g_ackText[0] = '\0';
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g.lastCmdError[0] = '\0';
        while (fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;
            apply_line(line);
            ++applied;
            if (strlen(g_ackText) + strlen(line) + 4 < sizeof(g_ackText)) {
                if (g_ackText[0]) strcat_s(g_ackText, " | ");
                strcat_s(g_ackText, line);
            }
        }
    }
    fclose(f);

    if (applied) {
        g.cmdSeq.fetch_add(applied);
        g_ackFrame = static_cast<uint32_t>(snapshot().index);
        write_ack();
        // The control thread publishes state.json too, not just the frame path.
        // Under `pace step` with no credits xrWaitFrame is BLOCKED, so a
        // frame-path-only writer could never acknowledge the very `step` command
        // that unblocks it - the ack channel would deadlock by construction, the
        // same trap the dedicated poll thread exists to avoid.
        write_state_json();
        XRSIM_LOG("xrsim: applied %u command(s): %s", applied, g_ackText);
    }
}

void thread_proc() {
    // A command.txt older than this process is last session's leftovers. Adopt
    // its timestamp so it is skipped rather than replayed.
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"command.txt");
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        g_lastWrite = fad.ftLastWriteTime;
        XRSIM_LOG("xrsim: ignoring a command.txt written before this run started");
    }

    while (g_running.load()) {
        poll_once();
        Sleep(20); // 50 Hz - fast enough that `step` is never the bottleneck
    }
}

} // namespace

void rig_staging_init() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    rig_defaults(g_staging); // optics included - see the comment in rig_defaults
}

void control_start() {
    if (g_running.exchange(true)) return;
    g_startMs = now_ms();
    rig_staging_init();
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_dirty = true;
    }
    control_apply_pending();
    g_thread = std::thread(thread_proc);
    XRSIM_LOG("xrsim: control channel live at %ls", log::dir());
}

void control_stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    // One final, unconditional write. Steady-state writes are rate-limited, so
    // without this the last file on disk lags the true final frame count by
    // however long the limiter was holding - and a script reading state.json
    // after the process exits gets a stale answer.
    write_state_json();
}

// THE COMMIT POINT. Called from inside xrWaitFrame.
void control_apply_pending() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    const uint64_t nowMs = now_ms();
    const uint64_t frame = snapshot().index;

    // Expire timed holds on a frame boundary.
    for (uint32_t c = 0; c < VC_COUNT; ++c) {
        Hold& h = g_holds[c];
        if (!h.active) continue;
        const bool done = h.byFrames ? (frame >= h.untilFrame) : (nowMs >= h.untilMs);
        if (!done) continue;
        h.active = false;
        const auto vc = static_cast<VirtualControl>(c);
        set_control(g_staging, vc, false);
        if (vc == VC_TRIGGER_L) g_staging.trigger[0] = 0.0f;
        if (vc == VC_TRIGGER_R) g_staging.trigger[1] = 0.0f;
        if (vc == VC_SQUEEZE_L) g_staging.squeeze[0] = 0.0f;
        if (vc == VC_SQUEEZE_R) g_staging.squeeze[1] = 0.0f;
        g_dirty = true;
    }

    if (g_headMotion.active) {
        const uint64_t el = nowMs - g_headMotion.startMs;
        float t = g_headMotion.durMs ? static_cast<float>(el) / g_headMotion.durMs : 1.0f;
        if (t >= 1.0f) { t = 1.0f; g_headMotion.active = false; }
        g_staging.head = pose_lerp(g_headMotion.from, g_headMotion.to,
                                   g_headMotion.linear ? t : ease_smooth(t));
        g_dirty = true;
    }

    for (int h = 0; h < 2; ++h) {
        Motion& m = g_handMotion[h];
        if (!m.active) continue;
        const uint64_t el = nowMs - m.startMs;
        float t = m.durMs ? static_cast<float>(el) / m.durMs : 1.0f;
        if (t >= 1.0f) { t = 1.0f; m.active = false; }
        const Pose p = pose_lerp(m.from, m.to, m.linear ? t : ease_smooth(t));
        if (m.isAim) g_staging.aim[h] = p; else g_staging.grip[h] = p;
        g_dirty = true;
    }

    if (g_orbit.active) {
        const uint64_t el = nowMs - g_orbit.startMs;
        if (el >= g_orbit.durMs) g_orbit.active = false;
        const float yaw = g_orbit.baseYaw + g_orbit.degPerSec * (static_cast<float>(el) / 1000.0f);
        float y, p, r;
        quat_to_ypr(g_staging.head.q, y, p, r);
        g_staging.head.q = quat_from_ypr(deg2rad(yaw), p, r);
        g_dirty = true;
    }

    if (!g_dirty) return;
    g_dirty = false;
    // The frame path owns the snapshot rig; this is the single write.
    snapshot_set_rig(g_staging);
}

void control_write_state() {
    const uint32_t hz = g.stateWriteHz.load();
    const uint64_t nowMs = now_ms();
    // Every frame under `pace step` (determinism needs it), rate-limited when
    // free-running, because 90 file rewrites a second is real I/O for nothing.
    if (g.pacing.mode != PaceMode::Step && hz > 0) {
        const uint64_t minGap = 1000 / (hz ? hz : 1);
        if (nowMs - g_lastStateWriteMs < minGap) return;
    }
    g_lastStateWriteMs = nowMs;
    write_state_json();
}

} // namespace xrsim
