// bvr_xrsim32: frame pacing, the per-frame snapshot, xrLocateViews, and the
// xrEndFrame layer capture.
//
// Threading, as the mod actually calls us (openxr_runtime.cpp):
//   xrWaitFrame   - the mod's dedicated pace thread, or the present thread when
//                   `vrpace thread off`
//   xrBeginFrame  - always the present thread, possibly long after its wait
//   xrEndFrame    - the present thread, sometimes on the NEXT present, sometimes
//                   never for a frame the mod deliberately holds open
// So nothing here may assume same-thread ordering, and no lock is held across a
// block. THE invariant:
//
//   NO WAIT IN THE SIM IS EVER UNBOUNDED.
//
// That is what makes step mode safe. The mod's present thread already abandons
// a pace wait after 200 ms (kPaceDeadlineFocusedMs), so an agent that walks away
// mid-step gates XR submission only - the game keeps running.

#include "xrsim_internal.h"

#include <cstring>

namespace xrsim {

FrameGate g_gate;

namespace {

std::mutex g_snapMutex;
FrameSnapshot g_snapshot;

std::mutex g_paceMutex;
std::condition_variable g_paceCv;
uint64_t g_frameIndex = 0;
XrTime g_nextDisplay = 0;
bool g_paceAbort = false;

// The LOCAL space origin in the sim's world frame. `recenter` moves it onto the
// current head pose, which is what a real runtime does.
Pose g_localOrigin = pose_identity();

XrDuration period_ns() {
    const double hz = (g.pacing.hz > 1.0) ? g.pacing.hz : 1.0;
    return static_cast<XrDuration>(1000000000.0 / hz);
}

// Build every world pose this frame will be answered from, once.
void compose_snapshot(FrameSnapshot& snap, const Rig& rig) {
    snap.rig = rig;
    snap.localOrigin = g_localOrigin;
    snap.headWorld = rig.head;

    // Last line of defence on the optics. A zero-extent fov produces a degenerate
    // projection: xrLocateViews hands the mod nonsense and every capture comes
    // back black, which reads as a mod bug rather than a sim one. Repair and say
    // so, once, rather than silently rendering nothing.
    for (int e = 0; e < 2; ++e) {
        if (!fov_is_degenerate(snap.rig.fov[e])) continue;
        XRSIM_LOG_ONCE("xrsim: eye %d had a zero-extent fov - restoring the Quest 3 default", e);
        Rig fresh;
        rig_defaults(fresh);
        snap.rig.fov[0] = fresh.fov[0];
        snap.rig.fov[1] = fresh.fov[1];
        break;
    }

    for (int h = 0; h < 2; ++h) {
        if (rig.handFollowsHead[h]) {
            // Parked relative to the head, so a hand stays in frame while the
            // agent sweeps the view. Orientation tracks the head, which makes
            // "point where I look" the default and keeps aim tests simple.
            Pose grip;
            grip.q = snap.headWorld.q;
            grip.p = v3_add(snap.headWorld.p, quat_rotate(snap.headWorld.q, rig.handOffset[h]));
            snap.gripWorld[h] = grip;
        } else {
            snap.gripWorld[h] = rig.grip[h];
        }

        // The aim pose runs along the pointing direction; the grip pose runs
        // along the controller handle. On Touch those differ by tens of degrees
        // (ENGINE_NOTES, M6), so the sim applies the same trim rather than
        // handing the mod two identical poses it could never tell apart.
        const Quat trim = quat_from_ypr(deg2rad(rig.aimTrimYaw[h]),
                                        deg2rad(rig.aimTrimPitch[h]), 0.0f);
        Pose aim = snap.gripWorld[h];
        aim.q = quat_norm(quat_mul(aim.q, trim));
        snap.aimWorld[h] = aim;
    }
    snap.state = current_session_state();
}

} // namespace

const FrameSnapshot& snapshot() { return g_snapshot; }

void snapshot_copy(FrameSnapshot& out) {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    out = g_snapshot;
}

const Rig& committed_rig() { return g_snapshot.rig; }

// The single write of the rig, from the control channel's commit point.
void snapshot_set_rig(const Rig& rig) {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    g_snapshot.rig = rig;
}

void recenter_local_space() {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    // Yaw and position only. Carrying pitch/roll into the origin would tilt the
    // whole world, which no runtime's recenter does.
    float yaw, pitch, roll;
    quat_to_ypr(g_snapshot.headWorld.q, yaw, pitch, roll);
    g_localOrigin.q = quat_from_ypr(yaw, 0.0f, 0.0f);
    g_localOrigin.p = g_snapshot.headWorld.p;
    XRSIM_LOG("xrsim: recentered LOCAL onto the head (yaw %.1f deg, y %.2f m)", rad2deg(yaw),
              g_localOrigin.p.y);

    XrEventDataBuffer buf{};
    auto* ev = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&buf);
    ev->type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
    ev->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ev->changeTime = now_xr_time();
    ev->poseValid = XR_TRUE;
    ev->poseInPreviousSpace = to_xr(pose_identity());
    queue_event(buf);
}

void pacing_grant(uint32_t frames) {
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        g.pacing.credits += frames;
    }
    g_paceCv.notify_all();
}

// Wake any waiter so it re-evaluates. This is for CONFIG changes (a new pace
// mode, a new refresh rate) and must NOT signal an error.
//
// Conflating the two produced a self-sustaining teardown loop: `pace step` set
// an abort flag, xrWaitFrame returned SESSION_LOST, the mod tore the session
// down, teardown called this again, and the next session died the same way at
// birth - forever. Waking and aborting are different things.
void pacing_wake() { g_paceCv.notify_all(); }

// Abort an in-flight wait because the session is going away. Scoped to one
// session: cleared at create, so it can never leak into the next one.
void pacing_abort() {
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        g_paceAbort = true;
    }
    g_paceCv.notify_all();
}

void pacing_reset_for_new_session() {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    g_paceAbort = false;
    g_nextDisplay = 0;
}

// ---------------------------------------------------------------------------
// xrWaitFrame - the pacing core and the commit point
// ---------------------------------------------------------------------------

static XrResult impl_WaitFrame(XrSession session, const XrFrameWaitInfo*,
                               XrFrameState* state) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;

    session_pump_state();
    if (!session_is_running()) return XR_ERROR_SESSION_NOT_RUNNING;

    if (g.hazards.waitFail > 0) {
        --g.hazards.waitFail;
        XRSIM_LOG("xrsim: hazard - xrWaitFrame returning SESSION_LOST on purpose");
        return XR_ERROR_SESSION_LOST;
    }

    // `idle on <ms>` stands in for a headset that has gone to sleep. Bounded by
    // idleMaxMs, because an unbounded stall on the present thread wedged this
    // project's game twice in the field and a test tool must not be able to
    // repeat that.
    if (g.pacing.idleBlockMs > 0) {
        const uint32_t ms = (g.pacing.idleBlockMs > g.pacing.idleMaxMs) ? g.pacing.idleMaxMs
                                                                       : g.pacing.idleBlockMs;
        std::unique_lock<std::mutex> lock(g_paceMutex);
        g_paceCv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_paceAbort; });
    }

    const XrDuration period = period_ns();

    switch (g.pacing.mode) {
    case PaceMode::Turbo:
        break;
    case PaceMode::Step: {
        std::unique_lock<std::mutex> lock(g_paceMutex);
        if (g.pacing.credits == 0) {
            const uint32_t timeout = g.pacing.starveMs ? g.pacing.starveMs : kStepStarveMsDefault;
            const bool got = g_paceCv.wait_for(lock, std::chrono::milliseconds(timeout),
                                               [] { return g.pacing.credits > 0 || g_paceAbort; });
            if (!got && !g_paceAbort) {
                if (g.pacing.starveAdvance) {
                    // The walk-away guarantee. An agent that stops sending steps
                    // leaves a slow game, never a hung one.
                    XRSIM_LOG("xrsim: STEP starved for %u ms - granting one frame "
                              "(step onstarve hold to disable)", timeout);
                    g.pacing.credits = 1;
                } else {
                    return XR_ERROR_SESSION_LOST;
                }
            }
        }
        // Do NOT clear the flag here: it is cleared at session create, so every
        // waiter parked on this gate exits rather than only the first one.
        if (g_paceAbort) return XR_ERROR_SESSION_LOST;
        if (g.pacing.credits > 0) --g.pacing.credits;
        break;
    }
    case PaceMode::Free:
    default: {
        const XrTime now = now_xr_time();
        if (g_nextDisplay == 0 || g_nextDisplay < now - 10 * period) {
            g_nextDisplay = now + period; // first frame, or we fell far behind
        } else {
            const XrTime waitNs = g_nextDisplay - now;
            if (waitNs > 0) {
                std::unique_lock<std::mutex> lock(g_paceMutex);
                g_paceCv.wait_for(lock, std::chrono::nanoseconds(waitNs),
                                  [] { return g_paceAbort; });
            }
            g_nextDisplay += period;
        }
        break;
    }
    }

    // THE COMMIT POINT. Everything the agent staged since the last frame lands
    // here, atomically, so a multi-line command file (head pose + trigger + step)
    // is one instantaneous rig change that never tears across a frame.
    control_apply_pending();

    {
        std::lock_guard<std::mutex> lock(g_snapMutex);
        g_snapshot.index = ++g_frameIndex;
        g_snapshot.displayTime = (g.pacing.mode == PaceMode::Free && g_nextDisplay != 0)
                                     ? g_nextDisplay
                                     : now_xr_time() + period;
        g_snapshot.displayPeriod = period;
        compose_snapshot(g_snapshot, g_snapshot.rig);
        state->type = XR_TYPE_FRAME_STATE;
        state->predictedDisplayTime = g_snapshot.displayTime;
        state->predictedDisplayPeriod = period;
        state->shouldRender = XR_TRUE;
    }

    g_gate.waited.fetch_add(1);
    control_write_state();
    return XR_SUCCESS;
}

static XrResult impl_BeginFrame(XrSession session, const XrFrameBeginInfo*) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!session_is_running()) return XR_ERROR_SESSION_NOT_RUNNING;
    if (g.hazards.beginFail > 0) {
        --g.hazards.beginFail;
        return XR_ERROR_SESSION_LOST;
    }
    g_gate.begun.fetch_add(1);
    // XR_FRAME_DISCARDED is a SUCCESS-class code, so the mod's XR_FAILED check
    // passes and it carries on - which is the correct behaviour for a second
    // begin against one wait.
    if (g_gate.open.exchange(true)) {
        g_gate.discarded.fetch_add(1);
        return XR_FRAME_DISCARDED;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrEndFrame - deep copy, then composite
// ---------------------------------------------------------------------------

static XrResult impl_EndFrame(XrSession session, const XrFrameEndInfo* info) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!session_is_running()) return XR_ERROR_SESSION_NOT_RUNNING;
    if (g.hazards.endFail > 0) {
        --g.hazards.endFail;
        return XR_ERROR_SESSION_LOST;
    }
    if (!g_gate.open.exchange(false)) {
        g_gate.outOfOrder.fetch_add(1);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (info->layerCount > kMaxLayers) return XR_ERROR_LAYER_LIMIT_EXCEEDED;

    // The mod hands us pointers into its own STACK FRAME (openxr_runtime.cpp
    // builds `layers[]` as a local). Everything must be copied out before this
    // function returns; nothing app-owned may be read afterwards.
    SimSubmission sub;
    sub.frameIndex = g_gate.ended.load() + 1;
    sub.displayTime = info->displayTime;
    sub.blend = info->environmentBlendMode;
    sub.layerCount = 0;
    snapshot_copy(sub.snap);

    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (!base) continue;
        SimLayer& dst = sub.layers[sub.layerCount];
        dst = SimLayer{};
        dst.type = base->type;
        dst.space = base->space;
        dst.flags = base->layerFlags;

        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            dst.viewCount = (p->viewCount > 2) ? 2 : p->viewCount;
            for (uint32_t v = 0; v < dst.viewCount; ++v) dst.views[v] = p->views[v];
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(base);
            dst.eyeVisibility = q->eyeVisibility;
            dst.pose = q->pose;
            dst.size = q->size;
            dst.sub = q->subImage;
        } else {
            XRSIM_LOG_ONCE("xrsim: unsupported composition layer type %d - skipped",
                           static_cast<int>(base->type));
            continue;
        }
        ++sub.layerCount;
    }

    g_gate.ended.fetch_add(1);
    session_note_submitted_frame();
    session_pump_state();

    // Decide whether this frame is a capture BEFORE compositing: the compositor
    // is off on ordinary frames (`compose oncapture`), so the steady-state cost
    // of having the sim attached is zero. This tool must never become the pacing
    // bug it was built to find.
    bool capture = false;
    uint32_t pending = g.captureCountdown.load();
    if (pending > 0) {
        capture = g.captureCountdown.compare_exchange_strong(pending, pending - 1);
    }
    const uint32_t every = g.captureEvery.load();
    if (!capture && every > 0 && (sub.frameIndex % every) == 0) capture = true;

    // The layer census is published EVERY frame, not only on capture frames, or
    // state.json reports whatever the last capture happened to see and an agent
    // polling "is the laser armed yet" reads a stale answer.
    compositor_note_layers(sub);
    if (capture || g.composeAlways.load()) compositor_on_end_frame(sub, capture);
    swapchains_begin_frame_census();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrLocateViews
// ---------------------------------------------------------------------------

static XrResult impl_LocateViews(XrSession session, const XrViewLocateInfo* info,
                                 XrViewState* viewState, uint32_t capacity,
                                 uint32_t* countOutput, XrView* views) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !viewState || !countOutput) return XR_ERROR_VALIDATION_FAILURE;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    *countOutput = 2;
    viewState->viewStateFlags = 0;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2 || !views) return XR_ERROR_SIZE_INSUFFICIENT;

    SimSpace* base = space_get(info->space);
    if (!base) return XR_ERROR_HANDLE_INVALID;

    FrameSnapshot snap;
    snapshot_copy(snap);

    Pose basePose;
    bool baseTracked = true;
    space_pose(*base, snap, basePose, baseTracked);
    const Pose baseInv = pose_inverse(basePose);

    for (uint32_t eye = 0; eye < 2; ++eye) {
        Pose offset = pose_identity();
        offset.p.x = (eye == 0 ? -0.5f : 0.5f) * snap.rig.ipdM;
        const Pose eyeWorld = pose_mul(snap.headWorld, offset);

        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = nullptr;
        views[eye].pose = to_xr(pose_mul(baseInv, eyeWorld));
        views[eye].fov = to_xr(snap.rig.fov[eye]);
    }

    if (snap.rig.headValid) {
        viewState->viewStateFlags =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_WaitFrame(XrSession s, const XrFrameWaitInfo* i,
                                               XrFrameState* st) {
    XRSIM_ENTRY(impl_WaitFrame(s, i, st), "WaitFrame")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_BeginFrame(XrSession s, const XrFrameBeginInfo* i) {
    XRSIM_ENTRY(impl_BeginFrame(s, i), "BeginFrame")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EndFrame(XrSession s, const XrFrameEndInfo* i) {
    XRSIM_ENTRY(impl_EndFrame(s, i), "EndFrame")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateViews(XrSession s, const XrViewLocateInfo* i,
                                                 XrViewState* vs, uint32_t c, uint32_t* o,
                                                 XrView* v) {
    XRSIM_ENTRY(impl_LocateViews(s, i, vs, c, o, v), "LocateViews")
}

} // namespace xrsim
