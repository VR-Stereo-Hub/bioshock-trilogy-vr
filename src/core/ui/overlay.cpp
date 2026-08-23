#include "overlay.h"

#include "core/framework/framework.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/igame_adapter.h"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_internal.h> // SetActiveID: the slider tweak path, see kTweak below
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace bvr::overlay {
namespace {

bool g_initialized = false;
// Text size, independent of the window size (the two were reported wrong in
// opposite directions - window right, text too big). <= 0 means "not chosen
// yet"; DrawUi seeds it from the backbuffer once and the slider owns it after.
float g_uiScale = 0.0f;
// What the Win32 backend reported as the display size (the WINDOW CLIENT RECT)
// before we overrode it with the backbuffer. Kept purely as evidence: if this
// is shorter than the backbuffer, that ratio IS where the panel used to be
// guillotined, and the probe line below says so in numbers.
ImVec2 g_clientRect{0.0f, 0.0f};
// Was anything under the cursor last frame? Sampled at the end of DrawUi,
// where IsAnyItemHovered() is meaningful, and read by the stick-adjust lane,
// which runs before NewFrame and so cannot ask directly.
bool g_anyItemHovered = false;
// Is a tracked controller driving the cursor? While it is, the real mouse is
// not queued at all - two position events per present is pointless traffic in
// a queue whose draining order decides whether the wheel gets applied.
bool g_controllerPointing = false;

// ---- stick-driven slider tweak (2026-08-22) --------------------------------
// FIRST ATTEMPT, AND WHY IT WAS WRONG: it synthesised a mouse click-and-drag.
// ImGui sliders position ABSOLUTELY on click - imgui_widgets.cpp computes
// `clicked_around_grab` and only preserves the value when the click lands ON
// the grab handle, otherwise the value jumps to wherever the cursor is. Aiming
// at the track therefore reset the slider to the aim point every time the stick
// re-engaged, which is exactly what was reported.
//
// The right path is the one the widget already implements for keyboard and
// gamepad (imgui_widgets.cpp, `ActiveIdSource == Keyboard || Gamepad`): it
// starts from the CURRENT value and applies a relative delta, one arrow-key
// press being 1% of the slider's range. Nothing about it involves the cursor.
//
// Reaching it needs two things ImGui does not expose publicly: the item must be
// made active (SetActiveID) and its source marked as keyboard. Both live in
// imgui_internal.h. The arrow keys themselves go through the public event API.
//
// Step RATE is ours rather than ImGui's repeat timer, so stick pressure still
// means something: the key is PULSED once per due step, and GetKeyPressedAmount
// counts one press per down-edge.
bool g_tweakWant = false;      // the stick is asking to tweak (set pre-NewFrame)
ImGuiID g_tweakId = 0;         // the item we activated, 0 = none held
constexpr float kTweakMinPerSec = 2.0f;
constexpr float kTweakMaxPerSec = 30.0f;
bool g_visible = false;
// Cross-thread mirror of g_visible (which is present-thread-only). Written
// wherever g_visible is, read by visible() from the XR and game threads.
std::atomic<bool> g_visibleApplied{false};
std::atomic<int> g_visibleRequest{-1}; // session 22: seam toggle (-1 = none)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_rtvBackbuffer = nullptr; // identity only, never deref'd
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Session 38: the subclass is on the GAME's main window, so it is the
    // earliest game-agnostic sight of a close. BS2's engine faults on its own
    // exit path (hook-free-proven); noting teardown here turns that into a
    // quiet fast exit instead of a dump per close. WM_ENDSESSION covers
    // logoff/shutdown. Always forwarded - observation only.
    if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wparam))
        crash::note_teardown(msg == WM_CLOSE     ? "WM_CLOSE"
                             : msg == WM_DESTROY ? "WM_DESTROY"
                                                 : "WM_ENDSESSION");
    if (g_visible) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        // Session 22 (user report: overlay unusable while scrolling): while
        // ImGui owns the mouse/keyboard, CONSUME those messages instead of
        // letting the game fight the overlay for them (the wheel doubled as
        // weapon-cycle, clicks re-captured the cursor mid-drag).
        ImGuiIO& io = ImGui::GetIO();
        bool mouseMsg = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool keyMsg = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        if ((mouseMsg && io.WantCaptureMouse) || (keyMsg && io.WantCaptureKeyboard))
            return TRUE;
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
}

// ---- controller pointer (2026-08-22) --------------------------------------
// The panel is drawn flat into the backbuffer, which IS the eye image, so a
// controller ray can be turned into a cursor with nothing more than the angle
// between the ray and the head's forward. No world position is involved:
// rotate the aim direction into the HEAD's frame, divide out the forward
// component, and scale by the eye's tangents.
//
// KNOWN APPROXIMATION, first thing to check if the cursor sits off the ray:
// the backbuffer holds the game's render at the GAME's fov while the
// compositor presents it through the CLAIMED fov. Those agree today (the
// session-15 lens match writes the world-equivalent spec), but if they ever
// diverge the cursor picks up a constant scale error - which is a factor to
// find, not a redesign.
//
// Both eyes get the panel at the same backbuffer pixel under SequentialReentry,
// so it reads as head-locked at infinite depth. Accepted for now.

// Rotate v by the CONJUGATE of q (world -> local).
void rotate_by_conj(const float q[4], const float v[3], float out[3]) {
    // q* = (-x, -y, -z, w); out = q* * v * q
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Rotate the XR forward (0,0,-1) by q, giving the ray direction in XR space.
void forward_of(const float q[4], float out[3]) {
    const float v[3] = {0.0f, 0.0f, -1.0f};
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Feed ImGui a cursor from the right controller and a click from RT. Must run
// AFTER ImGui_ImplWin32_NewFrame (which writes io.MousePos from the real
// cursor) and BEFORE ImGui::NewFrame. Leaves the mouse ALONE whenever the hand
// is not tracked, so a real mouse keeps working.
void InjectControllerPointer() {
    ImGuiIO& io = ImGui::GetIO();

    // EVERYTHING HERE GOES THROUGH THE EVENT QUEUE, and that is not a style
    // choice. Since ImGui 1.87 the io.MousePos / io.MouseDown[] / io.MouseWheel
    // fields are DERIVED STATE, rebuilt from the queued events inside
    // NewFrame() (imgui.cpp: `io.MouseDown[button] = e->MouseButton.Down`), and
    // imgui.cpp's own obsolescence list says so: "Backend writing to
    // io.MouseDown[] -> backend should call io.AddMouseButtonEvent()".
    //
    // Writing the fields directly, as the first cut did, produced exactly the
    // symptoms reported: the CURSOR moved (nothing else queues a position
    // event while the physical mouse is still, so the direct write survived by
    // luck) while the TRIGGER never clicked and the stick-drag never grabbed
    // anything (the queue always carries the real button state, so those writes
    // were overwritten every frame without fail).
    bvr::vr::HeadPose head{}, aim{};
    bool haveRay = false;
    float px = 0.0f, py = 0.0f;
    if (bvr::vr::peek_head_pose(head) && bvr::vr::get_hand_pose(1, true, aim)) {
        float dirWorld[3];
        forward_of(&aim.qx, dirWorld);
        float d[3];
        rotate_by_conj(&head.qx, dirWorld, d);
        // XR head frame: +X right, +Y up, -Z forward. Behind the head has no pixel.
        const float fwd = -d[2];
        float tanH = 0.0f, tanV = 0.0f;
        unsigned w = 0, h = 0;
        bvr::vr::fov_audit(&tanH, &tanV, nullptr, &w, &h);
        if (!w || !h) bvr::hud::backbuffer_dims(&w, &h);
        if (fwd > 0.05f && tanH > 0.0f && tanV > 0.0f && w && h) {
            const float ndcX = (d[0] / fwd) / tanH; // -1 left .. +1 right
            const float ndcY = (d[1] / fwd) / tanV; // -1 down .. +1 up
            px = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
            py = (0.5f - ndcY * 0.5f) * static_cast<float>(h);
            haveRay = true;
        }
    }

    // HOLD THE LAST GOOD POSITION rather than bailing out. Each eye is its own
    // Present under SequentialReentry, so a pose read that fails on alternate
    // passes leaves the cursor set on one eye and unset on the other - which is
    // precisely the "cursor only renders in the right eye" report. A cached
    // position makes the two eyes agree whatever the pose layer is doing.
    static bool s_havePtr = false;
    static ImVec2 s_ptr{0.0f, 0.0f};
    if (haveRay) {
        s_havePtr = true;
        s_ptr = ImVec2(px, py);
    }
    // Recency, not a latch: a controller that goes untracked (asleep, set
    // down) must hand the cursor back to the real mouse rather than owning it
    // for the rest of the session. The cached POSITION still persists - that is
    // the per-eye fix above - but the ownership claim expires.
    static uint64_t s_lastRayMs = 0;
    const uint64_t nowMs = GetTickCount64();
    if (haveRay) s_lastRayMs = nowMs;
    g_controllerPointing = s_lastRayMs != 0 && nowMs - s_lastRayMs < 500;

    bvr::input::Gamepad pad{};
    bool active = false;
    bvr::input::last_xr_pad(&pad, &active);

    if (!active) {
        g_tweakWant = false;
        if (s_havePtr) io.AddMousePosEvent(s_ptr.x, s_ptr.y);
        return;
    }

    // Right stick: Y scrolls, X tweaks whatever slider the pointer is over.
    // DOMINANT AXIS, so a scroll never nudges a value on the way past.
    const float rx = static_cast<float>(pad.rx) / 32767.0f;
    const float ry = static_cast<float>(pad.ry) / 32767.0f;
    constexpr float kStickDead = 0.25f;
    const bool xLane = fabsf(rx) > kStickDead && fabsf(rx) >= fabsf(ry);
    const bool yLane = fabsf(ry) > kStickDead && fabsf(ry) > fabsf(rx);

    if (s_havePtr) io.AddMousePosEvent(s_ptr.x, s_ptr.y);

    // The cursor is never moved by the tweak - the keyboard path ignores it -
    // so pointing stays honest while a value changes.
    static bool s_keyDown = false;
    static ImGuiKey s_key = ImGuiKey_RightArrow;
    static float s_stepAcc = 0.0f;
    const bool tweaking = xLane && (g_tweakId != 0 || g_anyItemHovered);

    if (tweaking) {
        const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 120.0f;
        const float mag = fabsf(rx);
        // Squared response: fine control near centre, useful travel at full push.
        const float perSec =
            kTweakMinPerSec + mag * mag * (kTweakMaxPerSec - kTweakMinPerSec);
        s_stepAcc += perSec * dt;
        const ImGuiKey want = rx > 0.0f ? ImGuiKey_RightArrow : ImGuiKey_LeftArrow;
        if (want != s_key && s_keyDown) {
            io.AddKeyEvent(s_key, false); // direction reversed mid-hold
            s_keyDown = false;
        }
        s_key = want;
        // Pulse: one down-edge per due step. GetKeyPressedAmount counts edges,
        // so this makes the rate ours instead of ImGui's repeat timer.
        if (s_keyDown) {
            io.AddKeyEvent(s_key, false);
            s_keyDown = false;
        } else if (s_stepAcc >= 1.0f) {
            s_stepAcc -= 1.0f;
            io.AddKeyEvent(s_key, true);
            s_keyDown = true;
        }
        if (s_stepAcc > 4.0f) s_stepAcc = 4.0f; // never bank a burst
        g_tweakWant = true;
        io.AddMouseButtonEvent(0, false); // never let RT steal the item mid-tweak
        return;
    }

    if (s_keyDown) {
        io.AddKeyEvent(s_key, false);
        s_keyDown = false;
    }
    s_stepAcc = 0.0f;
    g_tweakWant = false;

    if (yLane) {
        // Per second - the present rate swings with the scene, and a per-frame
        // step made scroll speed a function of framerate.
        const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 120.0f;
        io.AddMouseWheelEvent(0.0f, ry * 2.5f * dt);
    }
    // ~50% of travel, well past any resting noise. The published XR pad still
    // carries the real trigger - the game-facing suppression happens downstream
    // in compose_synthetic, precisely so this read still works.
    io.AddMouseButtonEvent(0, pad.rt > 128);
}

// ---- window-geometry probe (2026-08-22) -----------------------------------
// The panel opens small and off-lens because io.IniFilename is null, so ImGui
// never persisted anything and there is no record of where it was left. Rather
// than guess a default, log where it actually gets dragged to - as FRACTIONS
// of the backbuffer, so the number that eventually gets baked in is not tied
// to one resolution. On change, debounced, so dragging does not flood the log.
void ProbeWindowGeometry() {
    static float s_lx = -1, s_ly = -1, s_lw = -1, s_lh = -1;
    static uint64_t s_nextMs = 0;
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 s = ImGui::GetWindowSize();
    const bool moved = fabsf(p.x - s_lx) > 1.0f || fabsf(p.y - s_ly) > 1.0f ||
                       fabsf(s.x - s_lw) > 1.0f || fabsf(s.y - s_lh) > 1.0f;
    if (!moved) return;
    const uint64_t now = GetTickCount64();
    if (now < s_nextMs) return;
    s_nextMs = now + 1000;
    s_lx = p.x; s_ly = p.y; s_lw = s.x; s_lh = s.y;
    unsigned bw = 0, bh = 0;
    if (!bvr::hud::backbuffer_dims(&bw, &bh) || !bw || !bh) return;
    BVR_LOG("overlay: window pos %.0f,%.0f size %.0fx%.0f | backbuffer %ux%u | "
            "fractions pos %.4f,%.4f size %.4f,%.4f | fontScale %.2f",
            p.x, p.y, s.x, s.y, bw, bh, p.x / bw, p.y / bh, s.x / bw, s.y / bh,
            ImGui::GetIO().FontGlobalScale);
    // THE CLIP EVIDENCE, logged once. Windows clamps a window to the monitor,
    // so a game rendering a ~2750x2780 backbuffer on a 1440p display gets a
    // client rect roughly half that height - and ImGui's viewport was that
    // client rect, which is why the panel was cut "halfway down the view area"
    // no matter how tall it was asked to be. If clientH >= backbuffer H here,
    // this was NOT the cause and the cutoff needs looking at again.
    static bool s_toldClip = false;
    if (!s_toldClip && g_clientRect.x > 0.0f && g_clientRect.y > 0.0f) {
        s_toldClip = true;
        BVR_LOG("overlay: window client rect %.0fx%.0f vs backbuffer %ux%u - ImGui's "
                "viewport was the CLIENT rect, so the drawable area ended at %.0f%% of "
                "the eye image%s",
                g_clientRect.x, g_clientRect.y, bw, bh,
                100.0f * g_clientRect.y / static_cast<float>(bh),
                g_clientRect.y < static_cast<float>(bh) * 0.98f
                    ? " (THIS was the cutoff; now overridden to the backbuffer)"
                    : " (matches - the cutoff was something else)");
    }
}

bool CreateRenderTarget(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    g_rtvBackbuffer = SUCCEEDED(hr) ? backbuffer : nullptr;
    backbuffer->Release();
    return SUCCEEDED(hr);
}

bool Init(IDXGISwapChain* swapchain) {
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    if (!CreateRenderTarget(swapchain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't scatter imgui.ini into the game folder
    // NO EVENT TRICKLING. Trickling exists for real hardware, where a burst of
    // events inside one frame has to be spread over several so a fast click
    // is not merged into a move. Our input is SYNTHETIC and already
    // frame-paced: we queue exactly one consistent pointer state per present.
    //
    // With trickling ON it actively breaks: UpdateInputEvents' wheel branch is
    // `if (trickle_fast_inputs && (mouse_moved || mouse_button_changed)) break;`
    // - and the ray cursor moves every present (hand jitter guarantees it), so
    // every wheel event was DEFERRED rather than applied. They queued up and
    // drained about one per frame, which is why scrolling kept running after
    // the stick was released, like a flywheel.
    io.ConfigInputTrickleEventQueue = false;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    BVR_LOG("overlay initialized (hwnd=%p) - F10 toggles it", g_window);
    return true;
}

// Hold or release the tweak target. MUST run after NewFrame (HoveredWindow is
// updated there, and an ActiveId set before it would not survive) and before
// the sliders are submitted, so the widget sees ActiveId already set this frame.
void UpdateSliderTweak() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return;
    ImGuiContext& g = *ctx;
    if (g_tweakWant && g_tweakId == 0) {
        // HoveredIdPreviousFrame, because NewFrame has already moved this
        // frame's HoveredId out of the way and no item has been submitted yet.
        const ImGuiID id = g.HoveredIdPreviousFrame;
        if (id != 0 && g.HoveredWindow != nullptr) {
            ImGui::SetActiveID(id, g.HoveredWindow);
            g.ActiveIdSource = ImGuiInputSource_Keyboard;
            g.NavInputSource = ImGuiInputSource_Keyboard; // picks the arrow keys
            g_tweakId = id;
        }
    } else if (!g_tweakWant && g_tweakId != 0) {
        // Only ever clear OUR item - something else may have taken it since.
        if (g.ActiveId == g_tweakId) ImGui::ClearActiveID();
        g_tweakId = 0;
    } else if (g_tweakId != 0 && g.ActiveId != g_tweakId) {
        g_tweakId = 0; // lost it (window closed, item gone); stop tracking
    }
}

void DrawUi() {
    UpdateSliderTweak();
    // INTERIM DEFAULT, to be replaced by whatever ProbeWindowGeometry logs.
    // The old default was a fixed 420x420 at ImGui's own top-left corner,
    // which is a postage stamp in the far corner of a ~2560x2560 backbuffer -
    // outside the lenses entirely, and unreadable even when found. Centre it
    // and size it off the backbuffer so it lands in the lens-visible area at
    // any resolution.
    {
        unsigned bw = 0, bh = 0;
        if (bvr::hud::backbuffer_dims(&bw, &bh) && bw && bh) {
            const float w = static_cast<float>(bw), h = static_cast<float>(bh);
            // 45% of the eye image, centred - so trimming it pulls in equally
            // from the top and the bottom, which is what was asked for. (Before
            // the viewport fix above, height was moot: anything past the
            // client-rect height was invisible whatever the window asked for.)
            const float sw = w * 0.42f, sh = h * 0.45f;
            ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2((w - sw) * 0.5f, (h - sh) * 0.5f),
                                    ImGuiCond_FirstUseEver);
            // The font is authored for a 1080p desktop, so it needs SOME lift
            // at 2560 square - but h/1080 (2.37x) was reported as far too big
            // with the window itself sized right. Half that lift reads much
            // closer; the slider below is there because this is a perceptual
            // number and guessing it from outside a headset is how the first
            // attempt got it wrong.
            float fs = 1.0f + (h / 1080.0f - 1.0f) * 0.5f;
            if (fs < 1.0f) fs = 1.0f;
            if (fs > 2.0f) fs = 2.0f;
            if (g_uiScale <= 0.0f) g_uiScale = fs;
            ImGui::GetIO().FontGlobalScale = g_uiScale;
        } else {
            // No backbuffer yet (flat testing, or the very first present).
            ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
            if (g_uiScale <= 0.0f) g_uiScale = 1.0f; // the slider must never read 0
            ImGui::GetIO().FontGlobalScale = g_uiScale;
        }
    }
    // Build id in the title so an in-headset screenshot identifies the build.
    ImGui::Begin("BioShock VR " BVR_VERSION " [" BVR_BUILD_ID "]");
    ProbeWindowGeometry();
    ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    ImGui::Separator();
    vr::draw_debug_ui();
    ImGui::Separator();
    input::draw_debug_ui();
    if (auto* adapter = game::adapter()) {
        ImGui::Separator();
        adapter->drawDebugUi();
    }
    ImGui::Separator();
    frame_inspector::draw_debug_ui();
    ImGui::Separator();
    ImGui::Separator();
    if (ImGui::SliderFloat("UI text scale", &g_uiScale, 0.8f, 2.5f, "%.2f"))
        ImGui::GetIO().FontGlobalScale = g_uiScale;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Size of the TEXT only - drag the window's edge to "
                          "resize the panel itself.\nBoth are reported by the "
                          "geometry probe in the log, so whatever\nlooks right in "
                          "the headset can be read back out and made the default.");
    ImGui::TextWrapped("Log: %%LOCALAPPDATA%%\\BioshockVR\\bioshockvr.log");
    g_anyItemHovered = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
    ImGui::End();
}

} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_initialized) {
        g_initialized = Init(swapchain);
        if (!g_initialized) return;
    }
    // Session 22 (user report: overlay gone until an alt-tab): if the game
    // swaps its backbuffer object WITHOUT a ResizeBuffers (fullscreen-state
    // churn), a held RTV keeps drawing into the dead buffer - F10 toggles an
    // overlay nobody can see. Track the buffer identity and re-create.
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            if (g_rtv && bb != g_rtvBackbuffer) {
                g_rtv->Release();
                g_rtv = nullptr;
                BVR_LOG("overlay: backbuffer identity changed - RTV re-created");
            }
            bb->Release();
        }
    }
    if (!g_rtv && !CreateRenderTarget(swapchain)) return;

    // F10, edge-triggered (Insert was the original choice, but not every
    // keyboard has it); the seam request lane covers harness toggling.
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (isDown && !wasDown) {
        g_visible = !g_visible;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }
    wasDown = isDown;
    int req = g_visibleRequest.exchange(-1, std::memory_order_relaxed);
    if (req >= 0) {
        g_visible = req != 0;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }
    g_visibleApplied.store(g_visible, std::memory_order_relaxed);

    if (!g_visible) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // THE HALF-SCREEN CUTOFF (headset screenshot, 2026-08-22). The Win32
    // backend sets io.DisplaySize from the WINDOW CLIENT RECT
    // (imgui_impl_win32.cpp: GetClientRect), and the DX11 backend then sets the
    // D3D VIEWPORT to exactly that (imgui_impl_dx11.cpp: vp.Width/Height =
    // DisplaySize). But we draw into the BACKBUFFER, and in VR the backbuffer
    // is square (~2560x2560) while the game's window client area is far
    // shorter - so everything below the client height fell outside the viewport
    // and was never rasterised. The panel was not too tall; it was being
    // guillotined a little over halfway down.
    //
    // Point ImGui at the surface we actually render to. This also makes the
    // whole eye image addressable, which is what the centring maths and the
    // controller pointer below both already assumed.
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned bw = 0, bh = 0;
        if (bvr::hud::backbuffer_dims(&bw, &bh) && bw && bh) {
            const ImVec2 client = io.DisplaySize; // what the Win32 backend set
            g_clientRect = client;
            // Rescale the REAL cursor into backbuffer space, or the desktop
            // mouse lands somewhere else entirely once the coordinate space
            // changes underneath it. Read from the OS and queued as an event -
            // the backend's own position event is in client pixels, and a
            // direct io.MousePos write would be overwritten by NewFrame (see
            // InjectControllerPointer). The controller pointer queues after
            // this and wins whenever a hand is tracked.
            POINT cur{};
            if (!g_controllerPointing && client.x > 0.0f && client.y > 0.0f && g_window &&
                GetCursorPos(&cur) && ScreenToClient(g_window, &cur)) {
                io.AddMousePosEvent(static_cast<float>(cur.x) * static_cast<float>(bw) /
                                        client.x,
                                    static_cast<float>(cur.y) * static_cast<float>(bh) /
                                        client.y);
            }
            io.DisplaySize = ImVec2(static_cast<float>(bw), static_cast<float>(bh));
        }
    }
    // AFTER the Win32 backend (it writes MousePos from the real cursor) and
    // BEFORE NewFrame (which latches it) - the only window where an injected
    // cursor survives.
    InjectControllerPointer();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void on_resize() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_rtvBackbuffer = nullptr;
}

void set_visible(bool on) {
    g_visibleRequest.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool visible() {
    return g_visibleApplied.load(std::memory_order_relaxed);
}

} // namespace bvr::overlay
