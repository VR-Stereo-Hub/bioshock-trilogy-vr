// bvr_xrsim32 - a simulated 32-bit OpenXR runtime for agent-driven testing.
//
// This is NOT part of the mod. It links nothing from bioshockvr and ships in no
// release; it exists so an agent can run the game against a Quest-3-shaped
// runtime with no headset present, drive the head/hands/controls from a file,
// and capture what the compositor would have shown.
//
// Selected per-process via XR_RUNTIME_JSON (the loader checks that env var
// before the registry - see third_party/OpenXR-SDK/src/loader/manifest_file.cpp).
// The machine's real ActiveRuntime is never touched, so VDXR stays live for
// headset sessions.
//
// Shared declarations: handles, the global state singleton, logging, SEH.
// Keep this file pure ASCII (PowerShell 5.1 and MSVC both misread BOM-less UTF-8).

#pragma once

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "xrsim_math.h"

namespace xrsim {

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// Mirrors core/util/log.cpp on purpose: same [HH:MM:SS.mmm] prefix, same
// _SH_DENYNO share mode, same .prev.log rotation, so tools/tail-log.ps1 follows
// xrsim.log exactly the way it follows bioshockvr.log.
namespace log {
void init();
const wchar_t* dir();          // %LOCALAPPDATA%\BioshockVR\xrsim, or $BVR_XRSIM_DIR
void write(const char* fmt, ...);
} // namespace log

#define XRSIM_LOG(...) ::xrsim::log::write(__VA_ARGS__)

// Log a line at most once per call site. Used for the "this is wrong but the
// run should continue" diagnostics that would otherwise flood at 90 Hz.
#define XRSIM_LOG_ONCE(...)                     \
    do {                                        \
        static bool s_logged_once = false;      \
        if (!s_logged_once) {                   \
            s_logged_once = true;               \
            ::xrsim::log::write(__VA_ARGS__);   \
        }                                       \
    } while (0)

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
// On Win32 every XR handle is a uint64_t (openxr.h picks the integer form when
// XR_PTR_SIZE != 8), so handles can carry a generation counter instead of being
// raw pointers. A stale handle from a double-destroy then returns
// XR_ERROR_HANDLE_INVALID and a log line rather than dereferencing freed memory.
// That matters here: the session-23 crash in this project was a real runtime
// dereferencing poisoned pointers, and a test tool must never be able to do that
// to a debugging session.

enum HandleType : uint8_t {
    HT_NONE = 0,
    HT_INSTANCE = 1,
    HT_SESSION = 2,
    HT_SPACE = 3,
    HT_SWAPCHAIN = 4,
    HT_ACTIONSET = 5,
    HT_ACTION = 6,
};

constexpr uint64_t make_handle(HandleType type, uint32_t index, uint32_t gen) {
    return (static_cast<uint64_t>(gen) << 32) | (static_cast<uint64_t>(type) << 24) |
           static_cast<uint64_t>(index & 0x00FFFFFFu);
}
constexpr HandleType handle_type(uint64_t h) {
    return static_cast<HandleType>((h >> 24) & 0xFFu);
}
constexpr uint32_t handle_index(uint64_t h) { return static_cast<uint32_t>(h & 0x00FFFFFFu); }
constexpr uint32_t handle_gen(uint64_t h) { return static_cast<uint32_t>(h >> 32); }

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
// The mod submits at most 1 projection + 8 laser dots + aim dot + HUD quad = 11
// layers (openxr_runtime.cpp: layers[1 + kMaxLaserDots + 2]). 16 gives headroom
// and matches the maxLayerCount the real Quest 3 reports.
constexpr uint32_t kMaxLayers = 16;
constexpr uint32_t kMaxSpaces = 32;
constexpr uint32_t kMaxSwapchains = 16;
constexpr uint32_t kMaxSwapchainImages = 4;
constexpr uint32_t kMaxActionSets = 8;
constexpr uint32_t kMaxActions = 64;
constexpr uint32_t kMaxPaths = 256;
constexpr uint32_t kMaxEvents = 64;
constexpr uint32_t kMaxBindings = 128;

// ---------------------------------------------------------------------------
// Virtual controls
// ---------------------------------------------------------------------------
// One enum for every physical control the mod binds, so a suggested binding path
// resolves to a slot once at attach time and xrGetActionState* is a table read.
enum VirtualControl : uint8_t {
    VC_NONE = 0,
    VC_STICK_L,   VC_STICK_R,      // vector2f
    VC_TRIGGER_L, VC_TRIGGER_R,    // float
    VC_SQUEEZE_L, VC_SQUEEZE_R,    // float
    VC_BTN_A,     VC_BTN_B,        // right hand
    VC_BTN_X,     VC_BTN_Y,        // left hand
    VC_MENU,
    VC_CLICK_L,   VC_CLICK_R,      // thumbstick click
    VC_REST_L,    VC_REST_R,       // thumbrest touch
    VC_POSE_GRIP_L, VC_POSE_GRIP_R,
    VC_POSE_AIM_L,  VC_POSE_AIM_R,
    VC_COUNT
};

// ---------------------------------------------------------------------------
// The virtual rig - what the agent drives
// ---------------------------------------------------------------------------
// One flat, trivially copyable struct. The control thread stages a copy, and the
// frame path swaps it in at a single point inside xrWaitFrame, so a multi-line
// command file lands as one instantaneous change and never tears across a frame.
struct Rig {
    Pose head;
    bool headValid;

    Pose grip[2];   // 0 = left, 1 = right
    Pose aim[2];
    bool handValid[2];
    bool handFollowsHead[2];
    Vec3 handOffset[2];   // head-local (right, up, forward) when following
    float aimTrimPitch[2];
    float aimTrimYaw[2];

    float stick[2][2];    // [hand][x,y]
    float trigger[2];
    float squeeze[2];
    bool  btnA, btnB, btnX, btnY, menu;
    bool  click[2];
    bool  rest[2];

    float ipdM;
    float worldScale;
    Fov   fov[2];         // per eye, radians
};

void rig_defaults(Rig& r);

// A zero-extent field of view renders nothing. Callers use this to refuse a bad
// value rather than hand back a black capture that looks like a mod bug.
bool fov_is_degenerate(const Fov& f);

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------
enum class PaceMode : uint8_t { Free = 0, Step, Turbo };

// The one invariant that keeps this tool from ever wedging the game:
//
//   NO WAIT IN THE SIM IS EVER UNBOUNDED.
//
// Every wait takes a finite timeout and no lock is held across one. So an agent
// that walks away mid-step-mode leaves the game running slowly, not hung.
constexpr uint32_t kStepStarveMsDefault = 30000;
constexpr uint32_t kIdleMaxMsDefault = 20000;

struct Pacing {
    PaceMode mode;
    double   hz;
    uint32_t credits;          // Step: frames granted but not yet consumed
    uint32_t starveMs;
    bool     starveAdvance;    // on starvation: grant one frame (default) or hold
    uint32_t idleBlockMs;      // >0: xrWaitFrame stalls this long per call
    uint32_t idleMaxMs;
};

// ---------------------------------------------------------------------------
// Hazards - deliberate fault injection, so known bugs become regression tests
// ---------------------------------------------------------------------------
struct Hazards {
    bool     noSystem;         // xrGetSystem -> XR_ERROR_FORM_FACTOR_UNAVAILABLE
    uint32_t waitFail;         // next N xrWaitFrame return XR_ERROR_SESSION_LOST
    uint32_t beginFail;
    uint32_t endFail;
    bool     swapchainFail;
    bool     attachFail;
};

// Whether regaining FOCUSED requires the app to actually submit frames.
// Session 28 measured that VDXR will not re-grant FOCUSED to an app that submits
// nothing. That is one runtime's behaviour observed once, not a law, so both
// readings are selectable: baking in only one would manufacture confidence.
// Session 54 added the third reading, measured at the raffle wedge: VDXR parked
// a session at VISIBLE for minutes while the app submitted a continuous stream
// of EMPTY (zero-layer) frames - so VdxrLayers counts only layer-carrying
// frames toward re-promotion. Vdxr keeps its original any-frame meaning so the
// existing scripts keep their semantics.
enum class FocusPolicy : uint8_t { Vdxr = 0, Permissive, VdxrLayers };

// ---------------------------------------------------------------------------
// SEH
// ---------------------------------------------------------------------------
// Every entry point is a thin SEH shim over a noexcept impl. The shim holds no
// unwindable objects (the same rule core/debug/value_scan.cpp follows), so the
// __try/__except is legal and a fault inside the sim degrades to a returned
// error instead of taking the game down.
XrResult on_seh(const char* what);

#define XRSIM_ENTRY(ret_expr, name)                     \
    __try { return (ret_expr); }                        \
    __except (EXCEPTION_EXECUTE_HANDLER) {              \
        return ::xrsim::on_seh(name);                   \
    }

// ---------------------------------------------------------------------------
// Shared accessors implemented across the translation units
// ---------------------------------------------------------------------------
XrTime now_xr_time();
uint64_t now_ms();

// Event queue (xrsim_instance.cpp).
void queue_session_state(XrSession session, XrSessionState state);
void queue_event(const XrEventDataBuffer& buf);

// Session state (xrsim_session.cpp).
XrSessionState current_session_state();
const char* session_state_name(XrSessionState s);
bool session_is_running();

// The committed rig, published once per frame (xrsim_frame.cpp).
const Rig& committed_rig();

} // namespace xrsim
