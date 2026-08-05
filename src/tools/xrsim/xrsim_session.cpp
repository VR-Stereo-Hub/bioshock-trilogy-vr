// bvr_xrsim32: session lifecycle, the session state machine, reference and
// action spaces, and swapchains.
//
// There is exactly one session at a time, which is all the mod or xr_hello32
// ever creates. Keeping that assumption explicit is what lets the state machine
// be a handful of plain fields instead of a per-session object graph.

#include "xrsim_internal.h"

#include <cstring>

namespace xrsim {
namespace {

std::mutex g_mutex;

// --- the one session -------------------------------------------------------
bool g_alive = false;
uint32_t g_gen = 0;
bool g_running = false;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
uint64_t g_createdMs = 0;
uint64_t g_framesSubmitted = 0;
uint64_t g_framesAtFocusLoss = 0;
uint64_t g_focusLostMs = 0;
uint32_t g_focusLoseHoldMs = 0;   // 0 = hold until told otherwise
bool g_focusHeldDown = false;     // an explicit `focus lose` is sticky
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;

// The mod creates a LOCAL app space and a VIEW head space; hello32 creates none.
SimSpace g_spaces[kMaxSpaces];
SimSwapchain g_swapchains[kMaxSwapchains];

// READY is not instant on real hardware, and the mod has a documented retry path
// that only runs if bring-up takes a beat. 250 ms keeps that path exercised
// without making every test run wait.
constexpr uint64_t kReadyDelayMs = 250;

XrSession make_session_handle() {
    return static_cast<XrSession>(make_handle(HT_SESSION, 0, g_gen));
}

void set_state_locked(XrSessionState next) {
    if (g_state == next) return;
    g_state = next;
    queue_session_state(make_session_handle(), next);
    XRSIM_LOG("xrsim: session state -> %s", session_state_name(next));
}

} // namespace

const char* session_state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

XrSessionState current_session_state() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

bool session_is_running() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running;
}

XrSession current_session_handle() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_alive ? make_session_handle() : XR_NULL_HANDLE;
}

bool session_valid(XrSession h) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_alive && h != XR_NULL_HANDLE && handle_type(h) == HT_SESSION &&
           handle_gen(h) == g_gen;
}

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------
// Promotion is EARNED BY SUBMITTED FRAMES, not by a timer, because session 28
// measured that VDXR will not re-grant FOCUSED to an app that submits nothing.
// That was one runtime's behaviour observed once, so `focus policy permissive`
// exists to model the other reading. Encoding only one would hand the project
// false confidence in whichever the mod happens to be tuned for.
void session_pump_state() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_alive) return;

    if (g_state == XR_SESSION_STATE_IDLE) {
        if (now_ms() - g_createdMs >= kReadyDelayMs) set_state_locked(XR_SESSION_STATE_READY);
        return;
    }
    if (!g_running) return;
    if (g_state == XR_SESSION_STATE_STOPPING || g_state == XR_SESSION_STATE_LOSS_PENDING ||
        g_state == XR_SESSION_STATE_EXITING)
        return;

    if (g_focusHeldDown) {
        if (g_focusLoseHoldMs > 0 && now_ms() - g_focusLostMs >= g_focusLoseHoldMs) {
            g_focusHeldDown = false;
            g_framesAtFocusLoss = g_framesSubmitted;
        } else {
            return; // stay wherever the operator put us
        }
    }

    switch (g_state) {
    case XR_SESSION_STATE_SYNCHRONIZED:
        if (g_framesSubmitted >= 1) set_state_locked(XR_SESSION_STATE_VISIBLE);
        break;
    case XR_SESSION_STATE_VISIBLE: {
        // Vdxr policy: FOCUSED has to be earned by actually submitting frames.
        // Permissive: it comes back as soon as nothing is holding it down.
        const bool earned = (g.focusPolicy == FocusPolicy::Permissive) ||
                            (g_framesSubmitted >= g_framesAtFocusLoss + g.focusFrames);
        if (earned) {
            if (g_focusLostMs != 0) {
                XRSIM_LOG("xrsim: FOCUSED regained after %llu ms unfocused (%llu frames submitted)",
                          static_cast<unsigned long long>(now_ms() - g_focusLostMs),
                          static_cast<unsigned long long>(g_framesSubmitted - g_framesAtFocusLoss));
                g_focusLostMs = 0;
            }
            set_state_locked(XR_SESSION_STATE_FOCUSED);
        }
        break;
    }
    default:
        break;
    }
}

void session_note_submitted_frame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_framesSubmitted;
}

void session_force_state(XrSessionState state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_alive) return;

    if (state == XR_SESSION_STATE_VISIBLE && g_state == XR_SESSION_STATE_FOCUSED) {
        // This is the session-33 reproduction: the mod's pace guard, its
        // SUBMISSION IDLE report and xrSyncActions returning NOT_FOCUSED all
        // key off exactly this transition.
        g_focusHeldDown = true;
        g_focusLoseHoldMs = 0;
        g_focusLostMs = now_ms();
        g_framesAtFocusLoss = g_framesSubmitted;
    }
    if (state == XR_SESSION_STATE_FOCUSED) {
        g_focusHeldDown = false;
        g_framesAtFocusLoss = 0;
    }
    set_state_locked(state);
}

void session_focus_lose(uint32_t holdMs) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_alive || g_state != XR_SESSION_STATE_FOCUSED) {
            // Nothing to lose. Say so rather than silently succeeding, or a
            // scripted hazard test reports a pass it never ran.
            XRSIM_LOG("xrsim: focus lose ignored - session is %s, not FOCUSED",
                      session_state_name(g_state));
            return;
        }
        g_focusLoseHoldMs = holdMs;
    }
    session_force_state(XR_SESSION_STATE_VISIBLE);
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

static XrResult impl_CreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                   XrSession* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;

    const XrGraphicsBindingD3D11KHR* binding = nullptr;
    for (const auto* next = static_cast<const XrBaseInStructure*>(info->next); next;
         next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
    }
    if (!binding || !binding->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_alive) return XR_ERROR_LIMIT_REACHED;

    ++g_gen;
    g_alive = true;
    g_running = false;
    g_framesSubmitted = 0;
    g_framesAtFocusLoss = 0;
    g_focusLostMs = 0;
    g_focusHeldDown = false;
    g_createdMs = now_ms();
    g_state = XR_SESSION_STATE_UNKNOWN;

    // We hold the APP's device, exactly as VDXR does. It stays alive as long as
    // the session does, so AddRef both it and its immediate context.
    g_device = binding->device;
    g_device->AddRef();
    g_device->GetImmediateContext(&g_context);

    compositor_init(g_device, g_context);
    actions_reset_session();
    pacing_reset_for_new_session();

    *out = make_session_handle();
    set_state_locked(XR_SESSION_STATE_IDLE);
    XRSIM_LOG("xrsim: session created on the app's D3D11 device %p", static_cast<void*>(g_device));
    return XR_SUCCESS;
}

void session_destroy_all() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_alive) return;
    g_alive = false;
    g_running = false;

    for (auto& sc : g_swapchains) {
        for (uint32_t i = 0; i < sc.imageCount; ++i)
            if (sc.images[i]) sc.images[i]->Release();
        sc = SimSwapchain{};
    }
    for (auto& sp : g_spaces) sp = SimSpace{};

    compositor_shutdown();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    XRSIM_LOG("xrsim: session torn down");
}

static XrResult impl_DestroySession(XrSession session) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    // Unblock anything parked in the pacing gate first. A destroy that arrives
    // while xrWaitFrame is waiting must make it return, not deadlock - hello32
    // destroys a still-running session, so this path is on the M2 route.
    pacing_abort();
    session_destroy_all();
    return XR_SUCCESS;
}

static XrResult impl_BeginSession(XrSession session, const XrSessionBeginInfo* info) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) return XR_ERROR_SESSION_RUNNING;
    if (g_state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    g_running = true;
    set_state_locked(XR_SESSION_STATE_SYNCHRONIZED);
    XRSIM_LOG("xrsim: session running - the app is now paced by the simulated headset");
    return XR_SUCCESS;
}

static XrResult impl_EndSession(XrSession session) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return XR_ERROR_SESSION_NOT_RUNNING;
    g_running = false;
    g_framesSubmitted = 0;
    g_framesAtFocusLoss = 0;
    g_createdMs = now_ms();
    set_state_locked(XR_SESSION_STATE_IDLE);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Spaces
// ---------------------------------------------------------------------------

SimSpace* space_get(XrSpace h) {
    if (h == XR_NULL_HANDLE || handle_type(h) != HT_SPACE) return nullptr;
    const uint32_t i = handle_index(h);
    if (i >= kMaxSpaces) return nullptr;
    SimSpace& s = g_spaces[i];
    if (!s.used || s.gen != handle_gen(h)) return nullptr;
    return &s;
}

namespace {
XrSpace alloc_space(SimSpace init) {
    for (uint32_t i = 0; i < kMaxSpaces; ++i) {
        if (g_spaces[i].used) continue;
        const uint32_t gen = g_spaces[i].gen + 1;
        g_spaces[i] = init;
        g_spaces[i].used = true;
        g_spaces[i].gen = gen;
        return static_cast<XrSpace>(make_handle(HT_SPACE, i, gen));
    }
    return XR_NULL_HANDLE;
}
} // namespace

static XrResult impl_EnumerateReferenceSpaces(XrSession session, uint32_t capacity,
                                              uint32_t* countOutput,
                                              XrReferenceSpaceType* types) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    static const XrReferenceSpaceType kTypes[] = {XR_REFERENCE_SPACE_TYPE_VIEW,
                                                  XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                  XR_REFERENCE_SPACE_TYPE_STAGE};
    const uint32_t n = 3;
    *countOutput = n;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < n || !types) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(types, kTypes, sizeof(kTypes));
    return XR_SUCCESS;
}

static XrResult impl_CreateReferenceSpace(XrSession session,
                                          const XrReferenceSpaceCreateInfo* info,
                                          XrSpace* out) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW &&
        info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE)
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;

    std::lock_guard<std::mutex> lock(g_mutex);
    SimSpace s;
    s.isAction = false;
    s.refType = info->referenceSpaceType;
    s.offset = from_xr(info->poseInReferenceSpace);
    const XrSpace h = alloc_space(s);
    if (h == XR_NULL_HANDLE) return XR_ERROR_LIMIT_REACHED;
    *out = h;
    return XR_SUCCESS;
}

static XrResult impl_CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info,
                                       XrSpace* out) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    SimAction* act = action_get(info->action);
    if (!act) return XR_ERROR_HANDLE_INVALID;

    std::lock_guard<std::mutex> lock(g_mutex);
    SimSpace s;
    s.isAction = true;
    s.offset = from_xr(info->poseInActionSpace);
    s.actionIndex = handle_index(info->action);
    // The hand comes from the action's own resolved binding; a subaction path on
    // the create info overrides it when present.
    s.hand = act->hand;
    if (info->subactionPath != XR_NULL_PATH) {
        const char* p = path_str(info->subactionPath);
        if (p && strstr(p, "/user/hand/left")) s.hand = 0;
        else if (p && strstr(p, "/user/hand/right")) s.hand = 1;
    }
    const XrSpace h = alloc_space(s);
    if (h == XR_NULL_HANDLE) return XR_ERROR_LIMIT_REACHED;
    *out = h;
    return XR_SUCCESS;
}

static XrResult impl_DestroySpace(XrSpace space) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSpace* s = space_get(space);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    s->used = false;
    return XR_SUCCESS;
}

// Resolve a space to a pose in the sim's world frame, from the published
// snapshot rather than from live state, so everything inside one frame agrees.
bool space_pose(const SimSpace& space, const FrameSnapshot& snap, Pose& out, bool& tracked) {
    tracked = true;
    if (space.isAction) {
        const int h = (space.hand == 1) ? 1 : 0;
        SimAction* act = nullptr;
        if (space.actionIndex < kMaxActions) act = action_get_by_index(space.actionIndex);
        const bool isAim = act && (act->control == VC_POSE_AIM_L || act->control == VC_POSE_AIM_R);
        if (!snap.rig.handValid[h]) {
            tracked = false;
            out = pose_identity();
            return false;
        }
        out = pose_mul(isAim ? snap.aimWorld[h] : snap.gripWorld[h], space.offset);
        return true;
    }

    switch (space.refType) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        if (!snap.rig.headValid) { tracked = false; out = pose_identity(); return false; }
        out = pose_mul(snap.headWorld, space.offset);
        return true;
    case XR_REFERENCE_SPACE_TYPE_STAGE: {
        // STAGE is LOCAL dropped to the floor: same orientation, origin at y=0.
        Pose stage = snap.localOrigin;
        stage.p.y = 0.0f;
        out = pose_mul(stage, space.offset);
        return true;
    }
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
    default:
        out = pose_mul(snap.localOrigin, space.offset);
        return true;
    }
}

static XrResult impl_LocateSpace(XrSpace space, XrSpace baseSpace, XrTime,
                                 XrSpaceLocation* location) noexcept {
    if (!location) return XR_ERROR_VALIDATION_FAILURE;
    SimSpace* a = space_get(space);
    SimSpace* b = space_get(baseSpace);
    if (!a || !b) return XR_ERROR_HANDLE_INVALID;

    FrameSnapshot snap;
    snapshot_copy(snap);

    Pose pa, pb;
    bool trackedA = true, trackedB = true;
    space_pose(*a, snap, pa, trackedA);
    space_pose(*b, snap, pb, trackedB);

    const Pose rel = pose_mul(pose_inverse(pb), pa);
    location->pose = to_xr(rel);
    location->locationFlags = 0;
    if (trackedA && trackedB) {
        location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                  XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                  XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                  XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    // An action space with no focus reports untracked, because a real runtime
    // stops delivering controller poses when the app is not FOCUSED. The mod's
    // fallback-to-view-ray path depends on seeing exactly that.
    if ((a->isAction || b->isAction) && current_session_state() != XR_SESSION_STATE_FOCUSED)
        location->locationFlags = 0;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------

SimSwapchain* swapchain_get(XrSwapchain h) {
    if (h == XR_NULL_HANDLE || handle_type(h) != HT_SWAPCHAIN) return nullptr;
    const uint32_t i = handle_index(h);
    if (i >= kMaxSwapchains) return nullptr;
    SimSwapchain& s = g_swapchains[i];
    if (!s.used || s.gen != handle_gen(h)) return nullptr;
    return &s;
}

static XrResult impl_EnumerateSwapchainFormats(XrSession session, uint32_t capacity,
                                               uint32_t* countOutput, int64_t* formats) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    // SRGB first, then UNORM: the mod's create_swapchains prefers exactly that
    // order, so leading with anything else would silently change what it picks.
    static const int64_t kFormats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_D32_FLOAT,           DXGI_FORMAT_D24_UNORM_S8_UINT};
    const uint32_t n = static_cast<uint32_t>(sizeof(kFormats) / sizeof(kFormats[0]));
    *countOutput = n;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < n || !formats) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(formats, kFormats, sizeof(kFormats));
    return XR_SUCCESS;
}

static XrResult impl_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info,
                                     XrSwapchain* out) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (g.hazards.swapchainFail) return XR_ERROR_RUNTIME_FAILURE;
    if (info->width == 0 || info->height == 0) return XR_ERROR_VALIDATION_FAILURE;
    if (info->faceCount != 1 || info->arraySize != 1) return XR_ERROR_FEATURE_UNSUPPORTED;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_device) return XR_ERROR_SESSION_LOST;

    uint32_t slot = kMaxSwapchains;
    for (uint32_t i = 0; i < kMaxSwapchains; ++i)
        if (!g_swapchains[i].used) { slot = i; break; }
    if (slot == kMaxSwapchains) return XR_ERROR_LIMIT_REACHED;

    SimSwapchain& sc = g_swapchains[slot];
    const uint32_t gen = sc.gen + 1;
    sc = SimSwapchain{};
    sc.gen = gen;
    sc.width = info->width;
    sc.height = info->height;
    sc.format = info->format;
    sc.imageCount = 3; // triple buffered, the shape a streaming runtime uses

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info->width;
    desc.Height = info->height;
    desc.MipLevels = info->mipCount ? info->mipCount : 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(info->format);
    desc.SampleDesc.Count = info->sampleCount ? info->sampleCount : 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    for (uint32_t i = 0; i < sc.imageCount; ++i) {
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &sc.images[i]))) {
            for (uint32_t k = 0; k < i; ++k) sc.images[k]->Release();
            sc = SimSwapchain{};
            sc.gen = gen;
            XRSIM_LOG("xrsim: CreateTexture2D failed for a %ux%u fmt %lld swapchain",
                      info->width, info->height, static_cast<long long>(info->format));
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    sc.used = true;
    *out = static_cast<XrSwapchain>(make_handle(HT_SWAPCHAIN, slot, gen));
    XRSIM_LOG("xrsim: swapchain %u created %ux%u fmt %lld (%u images)", slot, sc.width, sc.height,
              static_cast<long long>(sc.format), sc.imageCount);
    return XR_SUCCESS;
}

static XrResult impl_DestroySwapchain(XrSwapchain handle) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    for (uint32_t i = 0; i < sc->imageCount; ++i)
        if (sc->images[i]) sc->images[i]->Release();
    const uint32_t gen = sc->gen;
    *sc = SimSwapchain{};
    sc->gen = gen;
    return XR_SUCCESS;
}

static XrResult impl_EnumerateSwapchainImages(XrSwapchain handle, uint32_t capacity,
                                              uint32_t* countOutput,
                                              XrSwapchainImageBaseHeader* images) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = sc->imageCount;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < sc->imageCount || !images) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* d3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < sc->imageCount; ++i) {
        if (d3d[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
        d3d[i].texture = sc->images[i];
    }
    return XR_SUCCESS;
}

static XrResult impl_AcquireSwapchainImage(XrSwapchain handle,
                                           const XrSwapchainImageAcquireInfo*,
                                           uint32_t* index) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!index) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    sc->acquired = true;
    sc->acquiredIndex = sc->nextIndex;
    sc->nextIndex = (sc->nextIndex + 1) % sc->imageCount;
    ++sc->acquiresThisFrame;
    *index = sc->acquiredIndex;
    return XR_SUCCESS;
}

static XrResult impl_WaitSwapchainImage(XrSwapchain handle,
                                        const XrSwapchainImageWaitInfo*) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!sc->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    // The mod passes XR_INFINITE_DURATION here. The sim never actually makes an
    // image wait, so this returns immediately - and that is deliberate: an
    // unbounded wait is the one thing this runtime must never do.
    return XR_SUCCESS;
}

static XrResult impl_ReleaseSwapchainImage(XrSwapchain handle,
                                           const XrSwapchainImageReleaseInfo*) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!sc->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    sc->acquired = false;
    sc->lastReleased = sc->acquiredIndex;
    sc->everReleased = true;
    sc->releasedOnFrame = static_cast<uint32_t>(g_gate.ended.load() + 1);
    return XR_SUCCESS;
}

// Reset the per-frame acquire census. Called from xrEndFrame so the count in a
// capture JSON covers exactly one submitted frame; session 29 relies on the aim
// dot and the laser sharing ONE acquire, and this is what makes that checkable.
void swapchains_begin_frame_census() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& sc : g_swapchains)
        if (sc.used) sc.acquiresThisFrame = 0;
}

void swapchains_for_each(void (*fn)(uint32_t, const SimSwapchain&, void*), void* user) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < kMaxSwapchains; ++i)
        if (g_swapchains[i].used) fn(i, g_swapchains[i], user);
}

ID3D11Texture2D* swapchain_last_image(XrSwapchain handle, uint32_t* outW, uint32_t* outH) {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimSwapchain* sc = swapchain_get(handle);
    if (!sc || !sc->everReleased) return nullptr;
    if (outW) *outW = sc->width;
    if (outH) *outH = sc->height;
    return sc->images[sc->lastReleased];
}

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateSession(XrInstance i, const XrSessionCreateInfo* ci,
                                                   XrSession* s) {
    XRSIM_ENTRY(impl_CreateSession(i, ci, s), "CreateSession")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySession(XrSession s) {
    XRSIM_ENTRY(impl_DestroySession(s), "DestroySession")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_BeginSession(XrSession s, const XrSessionBeginInfo* i) {
    XRSIM_ENTRY(impl_BeginSession(s, i), "BeginSession")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EndSession(XrSession s) {
    XRSIM_ENTRY(impl_EndSession(s), "EndSession")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateReferenceSpaces(XrSession s, uint32_t c, uint32_t* o,
                                                              XrReferenceSpaceType* t) {
    XRSIM_ENTRY(impl_EnumerateReferenceSpaces(s, c, o, t), "EnumerateReferenceSpaces")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateReferenceSpace(XrSession s,
                                                          const XrReferenceSpaceCreateInfo* i,
                                                          XrSpace* o) {
    XRSIM_ENTRY(impl_CreateReferenceSpace(s, i, o), "CreateReferenceSpace")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateActionSpace(XrSession s,
                                                       const XrActionSpaceCreateInfo* i,
                                                       XrSpace* o) {
    XRSIM_ENTRY(impl_CreateActionSpace(s, i, o), "CreateActionSpace")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateSpace(XrSpace a, XrSpace b, XrTime t,
                                                 XrSpaceLocation* l) {
    XRSIM_ENTRY(impl_LocateSpace(a, b, t, l), "LocateSpace")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySpace(XrSpace s) {
    XRSIM_ENTRY(impl_DestroySpace(s), "DestroySpace")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateSwapchainFormats(XrSession s, uint32_t c, uint32_t* o,
                                                               int64_t* f) {
    XRSIM_ENTRY(impl_EnumerateSwapchainFormats(s, c, o, f), "EnumerateSwapchainFormats")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateSwapchain(XrSession s, const XrSwapchainCreateInfo* i,
                                                     XrSwapchain* o) {
    XRSIM_ENTRY(impl_CreateSwapchain(s, i, o), "CreateSwapchain")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySwapchain(XrSwapchain s) {
    XRSIM_ENTRY(impl_DestroySwapchain(s), "DestroySwapchain")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateSwapchainImages(XrSwapchain s, uint32_t c,
                                                              uint32_t* o,
                                                              XrSwapchainImageBaseHeader* im) {
    XRSIM_ENTRY(impl_EnumerateSwapchainImages(s, c, o, im), "EnumerateSwapchainImages")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_AcquireSwapchainImage(XrSwapchain s,
                                                           const XrSwapchainImageAcquireInfo* i,
                                                           uint32_t* idx) {
    XRSIM_ENTRY(impl_AcquireSwapchainImage(s, i, idx), "AcquireSwapchainImage")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_WaitSwapchainImage(XrSwapchain s,
                                                        const XrSwapchainImageWaitInfo* i) {
    XRSIM_ENTRY(impl_WaitSwapchainImage(s, i), "WaitSwapchainImage")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_ReleaseSwapchainImage(XrSwapchain s,
                                                           const XrSwapchainImageReleaseInfo* i) {
    XRSIM_ENTRY(impl_ReleaseSwapchainImage(s, i), "ReleaseSwapchainImage")
}

} // namespace xrsim
