#pragma once
// In-process OpenXR runtime (M2): session on the game's own D3D11 device,
// frame pacing grafted onto the Present hook, and the game frame shown on a
// quad layer ("cinema screen") in the headset. Everything is fail-soft: no
// runtime, no headset, or any XR error just leaves the game running flat.
//
// Threading: init_instance() runs on the framework init thread before the
// D3D11 hooks install; everything else runs on the game's render thread
// inside the Present/ResizeBuffers detours.

struct IDXGISwapChain;

namespace bvr::vr {

// Create the XrInstance (loads the active 32-bit runtime). Fail-soft.
void init_instance();

// Present-hook head: bring up / pump the session, xrWaitFrame + xrBeginFrame.
// xrWaitFrame blocks, which paces the game to the headset refresh while a
// session is running.
void on_present_begin(IDXGISwapChain* swapchain);

// Present-hook tail: copy the backbuffer (incl. overlay) into the quad-layer
// swapchain and xrEndFrame.
void on_present_end(IDXGISwapChain* swapchain);

// ResizeBuffers: drop backbuffer-size-dependent resources (quad swapchain).
void on_resize();

// Status + controls section for the overlay.
void draw_debug_ui();

// --- M3: head pose for the camera drive ------------------------------------
// Core speaks meters + quaternions (XR convention); the game adapter owns the
// conversion to engine units.

struct HeadPose {
    float px, py, pz;     // meters, XR LOCAL space (right +X, up +Y, fwd -Z)
    float qx, qy, qz, qw; // orientation quaternion
};

// Latest predicted head pose (located at Present-head for the upcoming
// display time). False while not tracking.
bool get_head_pose(HeadPose& out);

// True when the user enabled VR camera mode AND a session is running; the
// adapter drives the game camera from the HMD only while this holds. Frame
// submission switches from the quad to a projection layer at the same time.
bool vr_camera_mode();

// Symmetric horizontal FOV (degrees) circumscribing the headset's per-eye
// FOV at the backbuffer aspect - what the game should render with in camera
// mode. 0 until the first views are located.
float suggested_hfov_deg();

} // namespace bvr::vr
