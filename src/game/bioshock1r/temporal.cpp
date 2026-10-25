// See temporal.h. The design and its measurements: docs/bioshock1/TEMPORAL.md.
#include "game/bioshock1r/temporal.h"
#include "core/gfx/frame_pacing.h"
#include "game/bioshock1r/native_repair.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/gun_flip.h"

#include "core/gfx/blit.h"
#include "core/gfx/dlss_client.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/afw_gpu.h"
#include "game/bioshock1r/afw_motion.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/handback.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/laser_draw.h"
#include "game/bioshock1r/resize.h"
#include "game/bioshock1r/scenedraw.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace bvr::b1r::temporal {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- settings (any thread) --------------------------------------------------------------
enum Mode { kOff = 0, kDlaa = 1, kSr = 2, kTaa = 3 };
std::atomic<float> g_taaWeight{0.12f};   // TAA's weight of the current image (lower = smoother, more lag)
std::atomic<int> g_mode{kOff};
std::atomic<uint64_t> g_resolutionSerial{0};
uint64_t g_resolutionApplied = 0; // game thread
std::atomic<int> g_quality{1};   // index into kRatios
const float kRatios[5] = {1.3f, 1.5f, 1.72f, 2.0f, 3.0f};
const char* const kQualityNames[5] = {"Ultra Quality", "Quality", "Balanced", "Performance", "Ultra Performance"};
std::atomic<int> g_model{0};
// NVSDK_NGX_DLSS_Hint_Render_Preset values: K, J, M, E.
const int kModelPresets[4] = {11, 10, 13, 5};
const char* const kModelNames[4] = {"Transformer K", "Transformer J", "Transformer 2 M", "Fast CNN (E)"};
std::atomic<bool> g_afw{false};
std::atomic<bool> g_afwTint{false};
std::atomic<bool> g_afwStale{true};
std::atomic<bool> g_afwRefine{true};
std::atomic<int> g_afwGrid{2};
// Crash isolation (2026-10-10: the driver crashed 5-11 s into AFW twice; a 30 s offline soak
// of the rebuild did not): off = AlternateEye with the held eye left to the compositor.
std::atomic<bool> g_afwRebuild{true};
// Hand depth gain is a diagnostic override, not a calibration inferred from one
// disparity ratio. Moving geometry and eye-dependent IK can change the apparent
// ratio too (run eleven: eye-centred shoulders, fixed controller wrists).
// Keep unity unless independent projection evidence establishes a depth error.
std::atomic<float> g_afwFgGain{1.0f};
std::atomic<float> g_afwFgBand{80.0f};
// The hands from the fresh eye only (afw::Params::handsFromFresh), by the hands mask - the
// viewmodel's squeezed depth range, measured [0, 0.010) on 2026-10-10. Off = the fifth-run
// control: the held eye's old hands carried as world geometry (a frame behind while moving).
std::atomic<bool> g_afwMask{true};
// The hands' lens over the world's for the rebuild (afw::Params::fgTanGain): 0 = the lens
// match's own k (camera::foreground_lens_gain); `afw lens <x>` overrides for an A/B.
std::atomic<float> g_afwLensGain{0.0f};
// The hand-depth probe: needs native pairs (Stereo method: both eyes every frame).
std::atomic<bool> g_probeOn{false};
// The vector audit answered its question on the first headset run (2026-10-10: vectors
// 0.0071 vs no motion 0.0171, depth scale 1 lowest): off by default now - it copies every
// eye image each frame. `dlss audit on` / F10 Debug brings it back.
std::atomic<bool> g_audit{false};
std::atomic<float> g_nearUu{10.0f};
std::atomic<float> g_farUu{65536.0f};
// The Dishonored mod's body band (rotation-only vectors nearer than this). OFF for BS1: its
// world depth does not contain the arms (they are the separate foreground pass, which the
// hands mask covers), so the band would only ever catch near WORLD geometry, where it is
// wrong - the integration test measured exactly that against a crate 0.3 m away.
std::atomic<float> g_bodyM{0.0f};
std::atomic<bool> g_fgBias{true};   // the hands mask as DLSS's bias-current-colour mask
// Tracked hands carry their controller's own motion in the vectors (temporal_gpu.h
// Params::hand): full within the first radius of the grip, none beyond the second.
std::atomic<bool> g_handVectors{true};
std::atomic<uint64_t> g_laserDrawMs{0}; // the in-image laser's last draw (laser_drawn_recently)
std::atomic<float> g_handFullM{0.35f}, g_handZeroM{0.60f};
std::atomic<bool> g_retry{false};
std::atomic<uint32_t> g_resetEpoch{1};

bool active() {
    return g_mode.load(std::memory_order_relaxed) != kOff || g_afw.load(std::memory_order_relaxed) ||
           g_probeOn.load(std::memory_order_relaxed);
}

// ---- cameras: game thread writes, render thread reads -------------------------------------
struct CamRec {
    uint64_t build = 0;
    int sign = 0;
    FVector loc{};
    FRotator rot{};
    afw::TrackingFrame tracking;
    tgpu::GripPose grip[2];       // each controller's grip, world, as this build placed it
    float gripWeight[2] = {};     // 1 = the controller drives that hand, 0 = the game does
    laser_draw::Segment laser;    // this build's laser and reticle on the fire ray (aim.cpp)
    uint64_t stampMs = 0;
};
const int kCamRing = 16;
// The last render view noted with its frame context. SequentialReentry's second pass
// replays the same game frame for the right eye, so it takes the same tracking and grips.
CamRec g_lastFramed;
std::mutex g_camMutex;
CamRec g_cams[kCamRing];
uint64_t g_buildCounter = 0;
// AlternateEye: the render views in presentation order (the eye source pops one per
// present). One entry per FRAME - note_camera is only called for the render view.
const int kFifo = 8;
uint64_t g_fifo[kFifo] = {};
int g_fifoCount = 0;
// BRVR's eye-queue health (CameraHook_NextEye): depth is exactly 1 at each pop in steady
// state. After an underrun, or after 8 pops in a row that found more than one queued, the
// queue has slipped and every image would be named one frame late - which SWAPS the eyes
// persistently - so the pop jumps to the newest entry.
bool g_fifoResync = false;
int g_fifoDeepPops = 0;
uint64_t g_fifoUnderruns = 0, g_fifoResyncs = 0;
// The build of the AlternateEye image being presented: set by the eye source, read by the
// processor in the same present (render thread).
uint64_t g_aerBuild = 0;

bool camera_for_build(uint64_t build, CamRec* out) {
    std::lock_guard<std::mutex> lk(g_camMutex);
    const CamRec& c = g_cams[build % kCamRing];
    if (c.build != build || !build) return false;
    *out = c;
    return true;
}
bool latest_camera(int sign, CamRec* out) {
    std::lock_guard<std::mutex> lk(g_camMutex);
    const CamRec* best = nullptr;
    for (const CamRec& c : g_cams)
        if (c.build && c.sign == sign && (!best || c.build > best->build)) best = &c;
    if (!best) return false;
    *out = *best;
    return true;
}

tgpu::Camera to_tcam(const CamRec& c, float tanH, float tanV) {
    tgpu::Camera t;
    t.loc[0] = c.loc.x; t.loc[1] = c.loc.y; t.loc[2] = c.loc.z;
    ue_rot_basis(c.rot, t.fwd, t.right, t.up);
    t.tanH = tanH; t.tanV = tanV;
    return t;
}

// ---- render thread state ------------------------------------------------------------------
ID3D11Device* g_dev = nullptr;

// Depth capture. A "segment" is one stretch of draws against one depth surface, ended by a
// rebind, a depth clear or the eye's present; the segment with the most DrawIndexed calls
// in the present interval is the world pass and is the one kept.
const uint32_t kMinWorldDraws = 8;
struct Segment { ID3D11Texture2D* tex = nullptr; uint32_t draws = 0; };
Segment g_seg;
uint32_t g_bestDraws = 0;
bool g_haveDepth = false;
uint64_t g_intervalPresent = 0;
struct Capture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;
};
Capture g_world, g_fg;   // the world pass, and the foreground pass that followed it
ID3D11Texture2D* g_worldTex = nullptr;   // identity of the surface the world pass used
// The world surface's depth was cleared after the world copy: only then is a later pass on
// it a separate (foreground) pass. Without a clear it is more of the world - a shadow or
// reflection pass on another surface split it - and is folded into the world copy.
bool g_worldCleared = false;
bool g_haveFg = false;
uint32_t g_fgDraws = 0;
// The passes of the current interval (diagnostics: logged with the 5 s line).
// 'W' world (copied), 'w' more of the world (copy refreshed), 'F' foreground (copied), '-' other,
// '|' the world surface's depth was cleared.
struct PassNote { uint32_t draws; char kind; };
PassNote g_passes[12] = {};
int g_passCount = 0;
char g_lastPasses[96] = "";
uint32_t g_expectW = 0, g_expectH = 0;   // the backbuffer of the last eye frame
struct Verdict { ID3D11Texture2D* tex; bool ok; };
Verdict g_verdicts[8] = {};
int g_verdictNext = 0;
bool g_loggedDepthShape = false;
uint64_t g_depthCopies = 0, g_depthMisses = 0;

// Per-eye history and what AFW reads.
struct EyeStore {
    ID3D11Texture2D* out = nullptr;              // the image this eye showed (R8G8B8A8_UNORM)
    ID3D11ShaderResourceView* outSrv = nullptr;
    uint32_t ow = 0, oh = 0;
    ID3D11Texture2D* lz = nullptr;               // its linear depth (render size)
    ID3D11ShaderResourceView* lzSrv = nullptr;
    uint32_t lw = 0, lh = 0;
    ID3D11Texture2D* hm = nullptr;               // its hands mask (render size), when hmValid
    ID3D11ShaderResourceView* hmSrv = nullptr;
    bool hmValid = false;
    ID3D11Texture2D* in[2] = {};                 // input colour ping-pong (the audit)
    ID3D11ShaderResourceView* inSrv[2] = {};
    int inCur = 0;
    bool inPrevValid = false;
    DXGI_FORMAT inFmt = DXGI_FORMAT_UNKNOWN;
    tgpu::Camera cam;
    afw::TrackingFrame tracking;
    tgpu::GripPose grip[2];
    float gripWeight[2] = {};
    laser_draw::Segment laser;                   // the last image's build's (the held rebuild draws it)
    uint64_t build = 0;
    uint64_t stampMs = 0;
    uint32_t w = 0, h = 0;
    uint32_t epoch = 0;
    bool histValid = false;                      // cam/size describe the previous image
    bool afwValid = false;                       // out/lz/cam: a rebuild source (depth and camera)
    bool outShown = false;                       // out holds the last image this eye showed (any kind)
};
EyeStore g_store[2];
tgpu::TemporalGpu g_guides;
tgpu::TaaGpu g_taa;
afw::WarpGpu g_warp;
bool g_freshValid = false;   // this present's image is a rebuild source
bool g_freshShown = false;   // this present showed an image (the held eye can be written)
int g_freshEye = 0;
// The hand-depth probe: the left image and depth of the pair being probed, kept until the
// right image of the same tick arrives.
tgpu::DepthProbe g_probe;
ID3D11Texture2D* g_probeCol = nullptr;
ID3D11ShaderResourceView* g_probeColSrv = nullptr;
ID3D11Texture2D* g_probeLz = nullptr;
ID3D11ShaderResourceView* g_probeLzSrv = nullptr;
tgpu::Camera g_probeCam;
uint64_t g_probeBuild = 0;
bool g_probeArmed = false;
uint32_t g_probeTick = 0;

// DLSS client and its lifecycle.
enum ClientState { kIdle = 0, kStarting, kUp, kFailed, kStopping };
// Never destroyed: a static destructor would run inside DLL_PROCESS_DETACH, under the loader
// lock, and Client::stop() talks to the helper and waits for it. At process exit the helper
// dies with its kill-on-close job instead.
bvr::dlss::Client& g_client = *new bvr::dlss::Client();
std::atomic<int> g_cstate{kIdle};
// The worker (start / stop) runs DETACHED: a joinable std::thread left in a global at exit
// is std::terminate (the integration test caught exactly that). Only the present thread
// launches one, and only while the previous one has finished.
std::atomic<bool> g_cthreadDone{true};
std::mutex g_whyMutex;
std::string g_why = "off";
struct BuildKey { uint32_t w = 0, h = 0, ow = 0, oh = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; int preset = -1; };
BuildKey g_built[2];
bool g_srOutputSet = false;
uint32_t g_srAskedW = 0, g_srAskedH = 0;   // the output size last asked of core

// Telemetry (render thread), logged every 5 s while active.
struct Tally {
    uint64_t frames = 0, dlss = 0, taa = 0, plain = 0, noDepth = 0, noCamera = 0, resets = 0, hands = 0;
    uint64_t afwRebuilds = 0, afwCopies = 0, afwHeldSrc = 0;
} g_tally;
Telemetry g_life;   // lifetime (telemetry())
uint64_t g_lastLogMs = 0;
uint64_t g_eyeCount[2] = {};

void set_why(const char* fmt, ...) {
    char line[256];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> lk(g_whyMutex);
    g_why = line;
}
std::string why_now() {
    std::lock_guard<std::mutex> lk(g_whyMutex);
    return g_why;
}

void client_log(int level, const char* line) {
    BVR_LOG("[dlss]%s %s", level == 2 ? " ERROR" : level == 1 ? " warn" : "", line);
}

bool host_paths(std::wstring* exe, std::wstring* data) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&host_paths), &self))
        return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
    if (wchar_t* s = wcsrchr(path, L'\\')) *(s + 1) = 0;
    *exe = std::wstring(path) + L"bvr_dlss\\bvr_dlss_host64.exe";
    *data = std::wstring(bvr::log::data_dir()) + L"\\dlss";
    return true;
}

void start_client() {
    if (!g_cthreadDone.load(std::memory_order_acquire)) return;
    std::wstring exe, data;
    if (!host_paths(&exe, &data)) {
        g_cstate.store(kFailed);
        set_why("cannot resolve the mod's folder");
        return;
    }
    g_cstate.store(kStarting);
    set_why("starting the DLSS helper");
    g_cthreadDone.store(false, std::memory_order_release);
    ID3D11Device* dev = g_dev;
    dev->AddRef();
    std::thread([exe, data, dev]() {
        bvr::dlss::StartParams sp;
        sp.hostExe = exe.c_str();
        sp.dataDir = data.c_str();
        sp.log = client_log;
        char why[256] = "";
        const bool ok = g_client.start(dev, sp, why, sizeof(why));
        dev->Release();
        if (ok) {
            set_why("ready on %s", g_client.adapter());
            g_cstate.store(kUp);
        } else {
            BVR_LOG("[dlss] unavailable: %s - eyes keep the normal copy", why);
            set_why("unavailable: %s", why);
            g_cstate.store(kFailed);
        }
        g_cthreadDone.store(true, std::memory_order_release);
    }).detach();
}

void stop_client() {
    if (!g_cthreadDone.load(std::memory_order_acquire)) return;
    const int st = g_cstate.load();
    if (st != kUp && st != kFailed) return;
    g_cstate.store(kStopping);
    g_cthreadDone.store(false, std::memory_order_release);
    g_built[0] = g_built[1] = BuildKey{};
    std::thread([]() {
        g_client.stop();
        set_why("off");
        g_cstate.store(kIdle);
        g_cthreadDone.store(true, std::memory_order_release);
    }).detach();
}

DXGI_FORMAT typed_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}

// Output intent stays independent of the current render while SetRes is in flight.
std::atomic<uint32_t> g_outW{0}, g_outH{0}; // 0 = headset recommendation
bool output_target(uint32_t* w, uint32_t* h) {
    *w=g_outW.load();*h=g_outH.load();
    if(*w && *h) return true;
    return bvr::vr::recommended_eye_size(w,h) && *w && *h;
}
void sr_output_for(uint32_t w, uint32_t h, uint32_t* ow, uint32_t* oh) {
    uint32_t tw=0,th=0;
    if(!output_target(&tw,&th)) {tw=w;th=h;}
    // The transition frame must still satisfy NGX's render <= output <= 3 * render.
    // Once the requested render arrives, both axes keep the exact chosen output.
    *ow=(std::max)(w,(std::min)(tw,(std::min)(8192u,3*w)));
    *oh=(std::max)(h,(std::min)(th,(std::min)(8192u,3*h)));
}
void sr_render_for(uint32_t ow, uint32_t oh, uint32_t* rw, uint32_t* rh) {
    const float r=kRatios[g_quality.load()];
    *rw=static_cast<uint32_t>(ow/r+0.5f)&~1u;
    *rh=static_cast<uint32_t>(oh/r+0.5f)&~1u;
    // Even rounding must not put Ultra Performance beyond NGX's 3x limit.
    if(3*(*rw)<ow) *rw+=2;
    if(3*(*rh)<oh) *rh+=2;
}

// ---- depth capture (render thread, the draw tap) ----------------------------------------
ID3D11Texture2D* texture_of(ID3D11DepthStencilView* dsv) {
    if (!dsv) return nullptr;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (!r) return nullptr;
    ID3D11Texture2D* t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
    r->Release();
    return t;   // AddRef'd
}

bool qualifies(ID3D11DepthStencilView* dsv, ID3D11Texture2D* tex) {
    for (const Verdict& v : g_verdicts)
        if (v.tex == tex) return v.ok;
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
    dsv->GetDesc(&vd);
    const bool fmt = td.Format == DXGI_FORMAT_R24G8_TYPELESS || td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    const bool shape = td.SampleDesc.Count == 1 && td.ArraySize == 1 &&
                       vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && vd.Texture2D.MipSlice == 0;
    const bool size = g_expectW ? (td.Width == g_expectW && td.Height == g_expectH) : td.Width >= 640;
    const bool ok = fmt && shape && size;
    if (!ok && fmt && td.Width >= 640 && !g_loggedDepthShape) {
        g_loggedDepthShape = true;
        BVR_LOG("[temporal] a depth surface was refused: %ux%u fmt %d samples %u array %u view %d (eye image %ux%u)",
                td.Width, td.Height, (int)td.Format, td.SampleDesc.Count, td.ArraySize, (int)vd.ViewDimension,
                g_expectW, g_expectH);
    }
    g_verdicts[g_verdictNext] = {tex, ok};
    g_verdictNext = (g_verdictNext + 1) % 8;
    return ok;
}

bool ensure_capture(Capture& c, ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    if (c.tex && c.w == sd.Width && c.h == sd.Height) return true;
    rel(c.srv); rel(c.tex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    // DEPTH_STENCIL keeps the copy legal (a depth resource's copy partner must be
    // depth-capable); TYPELESS lets the R24 view read it.
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &c.tex)) || FAILED(g_dev->CreateShaderResourceView(c.tex, &v, &c.srv))) {
        rel(c.srv); rel(c.tex);
        BVR_LOG("[temporal] depth capture %ux%u could not be created", sd.Width, sd.Height);
        return false;
    }
    c.w = sd.Width; c.h = sd.Height;
    BVR_LOG("[temporal] depth capture %ux%u (game depth format %d)", sd.Width, sd.Height, (int)sd.Format);
    return true;
}

// A tap event in a present interval the eye processor did not close (a present without an
// eye image): start over, so a large pass from an earlier interval cannot outvote this one.
void new_interval_if_presented() {
    const uint64_t pc = bvr::d3d11_hook::present_count();
    if (pc == g_intervalPresent) return;
    g_intervalPresent = pc;
    g_bestDraws = 0;
    g_haveDepth = false;
    g_haveFg = false;
    g_fgDraws = 0;
    g_worldCleared = false;
    g_passCount = 0;
}

// Copies a pass's depth surface, detaching it around the copy when it may still be bound.
void copy_depth(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* src, bool bound) {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    bool detached = false;
    UINT n = 0;
    if (bound) {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (rtvs[i]) n = i + 1;
        if (dsv) {
            ID3D11Texture2D* t = texture_of(dsv);
            if (t == src) {
                ctx->OMSetRenderTargets(n, rtvs, nullptr);
                detached = true;
            }
            rel(t);
        }
    }
    ctx->CopyResource(dst, src);
    if (detached) ctx->OMSetRenderTargets(n, rtvs, dsv);
    for (auto*& r : rtvs) rel(r);
    rel(dsv);
}

// Ends the current segment. The pass with the most draws so far is the world and is kept.
// A later segment on the SAME surface is either more of the world (no depth clear between:
// the copy is refreshed, so it holds everything drawn so far) or, after the world's depth
// was cleared, the foreground - the arms and the weapon - kept beside it.
void end_segment(ID3D11DeviceContext* ctx, bool bound) {
    char kind = '-';
    const bool onWorld = g_seg.tex && g_haveDepth && g_seg.tex == g_worldTex;
    if (onWorld && !g_worldCleared && g_seg.draws > 0) {
        copy_depth(ctx, g_world.tex, g_seg.tex, bound);
        g_bestDraws += g_seg.draws;
        ++g_depthCopies;
        ++g_life.depthCopies;
        kind = 'w';
    } else if (onWorld && g_worldCleared && g_seg.draws > 0 && ensure_capture(g_fg, g_seg.tex)) {
        copy_depth(ctx, g_fg.tex, g_seg.tex, bound);
        g_haveFg = true;
        g_fgDraws += g_seg.draws;
        kind = 'F';
    } else if (g_seg.tex && g_seg.draws >= kMinWorldDraws && g_seg.draws >= g_bestDraws &&
               ensure_capture(g_world, g_seg.tex)) {
        copy_depth(ctx, g_world.tex, g_seg.tex, bound);
        g_bestDraws = g_seg.draws;
        g_haveDepth = true;
        g_haveFg = false;   // a foreground from before this world pass is not its foreground
        g_fgDraws = 0;
        g_worldCleared = false;
        g_worldTex = g_seg.tex;
        ++g_depthCopies;
        ++g_life.depthCopies;
        kind = 'W';
    }
    if (g_seg.tex && g_seg.draws && g_passCount < 12) g_passes[g_passCount++] = {g_seg.draws, kind};
    g_seg.draws = 0;
}

void tap_setrt(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) {
    gun_flip::on_boundary(ctx);
    if (!active() || !g_dev) return;
    new_interval_if_presented();
    ID3D11Texture2D* t = texture_of(dsv);
    if (t && t == g_seg.tex) { rel(t); return; }   // an RTV-only rebind: one continuous pass
    end_segment(ctx, false);                         // the old surface is no longer bound
    rel(g_seg.tex);
    g_seg = Segment{};
    if (t && qualifies(dsv, t)) g_seg.tex = t; else rel(t);
}

// The viewport depth ranges the scene's draws use. A first-person viewmodel is often kept in
// front of the world by drawing it into a small slice of depth (MinDepth 0, MaxDepth 0.1,
// say); its stored depth then reads as nearly at the near plane and every reprojection of the
// hands is wrong by a large factor - which no draw's matrix shows (the 2026-10-09 dumps record
// matrices, not viewports). Sampled every 8th frame; counted per 5 s window.
struct RangeCount { float lo = 0, hi = 0; uint32_t draws = 0; };
RangeCount g_ranges[4];
uint32_t g_rangeOther = 0;
std::atomic<bool> g_squeezeAuto{true};
float g_squeezeLo = 0.0f, g_squeezeHi = 0.0f;   // the squeezed range in use (render thread)

void tap_draw(ID3D11DeviceContext* ctx) {
    if (!g_seg.tex) return;
    ++g_seg.draws;
    if ((bvr::d3d11_hook::present_count() & 7) != 0) return;
    D3D11_VIEWPORT vp = {};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    if (!n) return;
    for (RangeCount& r : g_ranges) {
        if (r.draws && r.lo == vp.MinDepth && r.hi == vp.MaxDepth) { ++r.draws; return; }
        if (!r.draws) { r.lo = vp.MinDepth; r.hi = vp.MaxDepth; r.draws = 1; return; }
    }
    ++g_rangeOther;
}

// Once per 5 s window: log the census and choose the squeezed range (a [0, hi) slice beside
// the full [0, 1]) the guides re-expand.
void squeeze_window() {
    char line[256] = "";
    int at = 0;
    float lo = 0.0f, hi = 0.0f;
    uint32_t squeezed = 0, full = 0, total = 0;
    for (const RangeCount& r : g_ranges) {
        if (!r.draws) continue;
        total += r.draws;
        at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, "%s[%.3f, %.3f] %u", at ? ", " : "", r.lo, r.hi, r.draws);
        if (r.lo <= 0.0f && r.hi >= 1.0f) full += r.draws;
        else if (r.lo < 0.001f && r.hi > 0.0001f && r.hi <= 0.5f && r.draws > squeezed) { squeezed = r.draws; lo = r.lo; hi = r.hi; }
    }
    if (!total) return;
    BVR_LOG("[temporal] viewport depth ranges of the scene's draws (sampled): %s%s", line,
            g_rangeOther ? " + more ranges" : "");
    const bool use = g_squeezeAuto.load() && squeezed && full;
    if (use && (lo != g_squeezeLo || hi != g_squeezeHi)) {
        BVR_LOG("[temporal] draws squeezed into depth [%.3f, %.3f) next to full-range draws: that depth is re-expanded "
                "before it is linearised (the hands' true distance for the guides and the warp)", lo, hi);
        g_squeezeLo = lo; g_squeezeHi = hi;
    } else if (!use && total > 100 && g_squeezeHi > 0.0f) {
        BVR_LOG("[temporal] no squeezed depth range any more - re-expansion off");
        g_squeezeLo = g_squeezeHi = 0.0f;
    }
    for (RangeCount& r : g_ranges) r = RangeCount{};
    g_rangeOther = 0;
}

void tap_clear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, unsigned flags) {
    gun_flip::on_boundary(ctx);
    if (!active() || !g_dev || !(flags & D3D11_CLEAR_DEPTH)) return;
    new_interval_if_presented();
    ID3D11Texture2D* t = texture_of(dsv);
    if (!t) return;
    if (t == g_seg.tex) end_segment(ctx, true);   // keep the pass this clear is about to erase
    if (g_haveDepth && t == g_worldTex) {
        g_worldCleared = true;
        if (g_passCount < 12) g_passes[g_passCount++] = {0, '|'};
    }
    if (t == g_seg.tex) {
        // (the segment continues on the cleared surface)
    } else if (!g_seg.tex) {
        // The surface may have stayed bound across the present with no rebind: the clear
        // that starts the next eye is then the only event that names it.
        ID3D11DepthStencilView* bound = nullptr;
        ctx->OMGetRenderTargets(0, nullptr, &bound);
        if (bound == dsv && qualifies(dsv, t)) { g_seg.tex = t; t = nullptr; }
        rel(bound);
    }
    rel(t);
}

// 2026-10-25: a reflected gun's draws cull the other face (gun_flip.cpp needs each draw's index count).
void tap_draw_args(ID3D11DeviceContext* ctx, unsigned indexCount, unsigned /*startIndex*/, int /*baseVertex*/) {
    gun_flip::on_draw(ctx, indexCount);
}

const bvr::frame_inspector::DrawTap kTap = {tap_setrt, tap_draw, tap_clear, tap_draw_args};

// ---- the eye store ------------------------------------------------------------------------
bool ensure_tex(ID3D11Texture2D** t, ID3D11ShaderResourceView** s, uint32_t w, uint32_t h, DXGI_FORMAT f) {
    if (*t) {
        D3D11_TEXTURE2D_DESC d = {};
        (*t)->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.Format == f) return true;
        rel(*s); rel(*t);
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = typed_of(f); v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t)) || FAILED(g_dev->CreateShaderResourceView(*t, &v, s))) {
        rel(*s); rel(*t);
        return false;
    }
    return true;
}

void release_store() {
    for (EyeStore& e : g_store) {
        rel(e.outSrv); rel(e.out); rel(e.lzSrv); rel(e.lz); rel(e.hmSrv); rel(e.hm);
        for (int i = 0; i < 2; ++i) { rel(e.inSrv[i]); rel(e.in[i]); }
        e = EyeStore{};
    }
}

void log_maybe(uint64_t now) {
    if (now - g_lastLogMs < 5000) return;
    g_lastLogMs = now;
    squeeze_window();
    const Tally& t = g_tally;
    uint32_t fgSubs = 0, fgMisses = 0;
    static uint32_t fgSubsMark = 0, fgMissesMark = 0;
    scenedraw::fg_sync_counts(&fgSubs, &fgMisses);
    char dl[160] = "";
    if (g_mode.load() == kDlaa || g_mode.load() == kSr) {
        const auto& st = g_client.stats;
        _snprintf_s(dl, _TRUNCATE, " | DLSS %s: evaluate GPU %.2f/%.2f ms, round trip %.2f/%.2f ms (L/R)",
                    why_now().c_str(),
                    st.gpuN[0] ? st.gpuMsSum[0] / st.gpuN[0] : -1.0, st.gpuN[1] ? st.gpuMsSum[1] / st.gpuN[1] : -1.0,
                    st.frames[0] ? st.cpuMsSum[0] / st.frames[0] : -1.0, st.frames[1] ? st.cpuMsSum[1] / st.frames[1] : -1.0);
    }
    BVR_LOG("[temporal] 5 s: eye frames %llu (L %llu R %llu): dlss %llu, taa %llu, plain %llu, no depth %llu, no camera %llu, "
            "history resets %llu, with a hands pass %llu | depth passes of the last image (draws, "
            "W world F foreground - other): %s | afw rebuilds %llu (with the held eye %llu), fallbacks %llu | fg sync subs/misses %u/%u%s",
            (unsigned long long)t.frames, (unsigned long long)g_eyeCount[0], (unsigned long long)g_eyeCount[1],
            (unsigned long long)t.dlss, (unsigned long long)t.taa, (unsigned long long)t.plain, (unsigned long long)t.noDepth,
            (unsigned long long)t.noCamera, (unsigned long long)t.resets,
            (unsigned long long)t.hands, g_lastPasses[0] ? g_lastPasses : "none", (unsigned long long)t.afwRebuilds,
            (unsigned long long)t.afwHeldSrc, (unsigned long long)t.afwCopies,
            fgSubs - fgSubsMark, fgMisses - fgMissesMark, dl);
    fgSubsMark = fgSubs; fgMissesMark = fgMisses;
    if (g_probe.result.pairs) {
        uint64_t nh = 0, nw = 0;
        const double hm = g_probe.result.median(0, &nh), wm = g_probe.result.median(1, &nw);
        BVR_LOG("[temporal] hand-depth probe (%llu native pairs; stereo disparity measured / predicted by depth): "
                "hands (< %.0f uu) %.2f over %llu samples | world (150-1500 uu, the control) %.2f over %llu | "
                "1.00 = agreement; motion or eye-dependent geometry can also change the ratio",
                (unsigned long long)g_probe.result.pairs, g_afwFgBand.load(), hm, (unsigned long long)nh, wm,
                (unsigned long long)nw);
        g_probe.result.clear();
    }
    const tgpu::Audit& a = g_guides.audit;
    if (a.frames) {
        const double n = (double)a.samples;
        BVR_LOG("[temporal] vector audit (%llu frames, %llu samples, mean luminance error): vectors %.4f, "
                "rotation only %.4f, no motion %.4f | depth scale 0.25:%.4f 0.5:%.4f 1:%.4f 2:%.4f 4:%.4f "
                "(1 lowest = near plane right; vectors well under no motion = vectors right)",
                (unsigned long long)a.frames, (unsigned long long)a.samples, a.vecErr / n, a.rotErr / n,
                a.zeroErr / n, a.scaleErr[0] / n, a.scaleErr[1] / n, a.scaleErr[2] / n, a.scaleErr[3] / n,
                a.scaleErr[4] / n);
        g_guides.audit.clear();
    }
    g_tally = Tally{};
    g_eyeCount[0] = g_eyeCount[1] = 0;
    g_depthCopies = 0;
}

// ---- the eye processor (render thread, core's seam) ---------------------------------------
bool process_eye(ID3D11DeviceContext* ctx, const bvr::vr::EyeFrame& f) {
    bvr::frame_pacing::Scope timing(bvr::frame_pacing::Temporal);
    bvr::frame_pacing::eye(f.sequential ? 1 : (g_afw.load() ? 2 : 0), g_mode.load(), f.eye);
    if (!active()) {
        // Switched off: return the helper's memory, from this thread - the only one that
        // ever calls into the client while it runs.
        const int cs = g_cstate.load();
        if (cs == kUp || cs == kFailed) stop_client();
        if (g_srOutputSet) { bvr::vr::set_eye_output_size(0, 0); g_srOutputSet = false; }
        return false;
    }
    bvr::frame_inspector::ScopedSuppress suppress;   // our own state changes stay out of the tap
    if (!g_dev) ctx->GetDevice(&g_dev);
    const uint64_t now = GetTickCount64();
    const int eye = f.eye & 1;
    ++g_tally.frames;
    ++g_eyeCount[eye];
    if (g_expectW != f.srcW || g_expectH != f.srcH) {
        // A new eye size: every depth-surface verdict was taken against the old one.
        for (Verdict& v : g_verdicts) v = Verdict{};
        g_expectW = f.srcW; g_expectH = f.srcH;
    }
    g_freshValid = false;
    g_freshShown = false;

    // 1. Depth: the interval's world pass. The present counter has already advanced for
    // THIS present (d3d11_hook counts at the detour's head), so the interval being closed
    // is the one that began at the previous count; the next one begins now.
    const uint64_t pc = bvr::d3d11_hook::present_count();
    if (g_seg.tex && g_seg.draws) end_segment(ctx, true);
    const bool depthOk = g_intervalPresent + 1 == pc && g_haveDepth && g_world.tex && g_world.w == f.srcW && g_world.h == f.srcH;
    const bool fgOk = depthOk && g_haveFg && g_fg.tex && g_fg.w == f.srcW && g_fg.h == f.srcH;
    if (depthOk) { g_life.lastWorldDraws = g_bestDraws; g_life.lastFgDraws = fgOk ? g_fgDraws : 0; }
    {
        // The pass structure of this image, for the log (the first headset run tells
        // whether BS1's foreground pass clears depth or shares the world's).
        int at = 0;
        for (int i = 0; i < g_passCount && at < (int)sizeof(g_lastPasses) - 12; ++i)
            at += g_passes[i].kind == '|'
                      ? _snprintf_s(g_lastPasses + at, sizeof(g_lastPasses) - at, _TRUNCATE, "%s|", i ? " " : "")
                      : _snprintf_s(g_lastPasses + at, sizeof(g_lastPasses) - at, _TRUNCATE, "%s%u%c", i ? " " : "",
                                    g_passes[i].draws, g_passes[i].kind);
        if (!g_passCount) g_lastPasses[0] = 0;
    }
    g_haveDepth = false;
    g_haveFg = false;
    g_fgDraws = 0;
    g_worldCleared = false;
    g_passCount = 0;
    g_bestDraws = 0;
    g_intervalPresent = pc;
    ++g_life.eyeFrames;
    if (!depthOk) { ++g_tally.noDepth; ++g_depthMisses; ++g_life.noDepth; }
    if (fgOk) ++g_tally.hands;

    // 2. The camera this image was rendered with.
    CamRec cam{};
    bool camOk = false;
    if (f.sequential) {
        camOk = latest_camera(f.imageSign, &cam);
    } else {
        // AlternateEye: the eye source popped this image's build already (aer_source).
        camOk = g_aerBuild && camera_for_build(g_aerBuild, &cam) && cam.sign == f.imageSign;
    }
    if (camOk && now - cam.stampMs > 250) camOk = false;
    if (!camOk) { ++g_tally.noCamera; ++g_life.noCamera; }

    EyeStore& st = g_store[eye];
    const uint32_t epoch = g_resetEpoch.load(std::memory_order_relaxed);
    const tgpu::Camera tc = to_tcam(cam, f.tanH, f.tanV);
    bool hist = camOk && depthOk && st.histValid && st.epoch == epoch && st.w == f.srcW && st.h == f.srcH &&
                std::fabs(st.cam.tanH - tc.tanH) < 1e-4f && std::fabs(st.cam.tanV - tc.tanV) < 1e-4f &&
                cam.build > st.build && now - st.stampMs < 250;
    if (hist) {
        const float dx = tc.loc[0] - st.cam.loc[0], dy = tc.loc[1] - st.cam.loc[1], dz = tc.loc[2] - st.cam.loc[2];
        if (dx * dx + dy * dy + dz * dz > 300.0f * 300.0f) hist = false;   // a teleport, a load
    }
    if (!hist) ++g_tally.resets;

    // 3. Guides.
    tgpu::Params gp;
    gp.w = f.srcW; gp.h = f.srcH;
    gp.nearUu = g_nearUu.load(); gp.farUu = g_farUu.load();
    gp.historyValid = hist;
    float ws = 100.0f;
    {
        bvr::vr::HeadPose rp{}; int32_t ry = 0;
        camera::get_recenter_state(&rp, &ry, &ws);
        if (!(ws > 1.0f)) ws = 100.0f;
    }
    gp.bodyUu = g_bodyM.load() * ws;
    gp.haveForeground = fgOk;
    gp.squeezeLo = g_squeezeLo;
    gp.squeezeHi = g_squeezeHi;
    gp.cur = tc;
    gp.prev = st.cam;
    // A stick turn or walk moves tracked hands with the player. Their previous
    // SAME-eye view must move with that tracking frame, while the world stays put.
    // Head/controller motion is deliberately not included in this transport.
    gp.foregroundCameraValid = hist && afw::transport_foreground_camera(
        st.cam, st.tracking, cam.tracking, gp.prevForeground);
    if (gp.foregroundCameraValid) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            BVR_LOG("[temporal] foreground motion vectors include player locomotion (same-eye history)");
        }
    }
    // And each hand's own controller motion on top: a hand swung through the air moves its
    // texture, and a vector that says it stood still in the room makes DLSS smear it.
    if (hist && g_handVectors.load(std::memory_order_relaxed)) {
        gp.handFullUu = g_handFullM.load() * ws;
        gp.handZeroUu = (std::max)(g_handZeroM.load() * ws, gp.handFullUu + 1.0f);
        for (int h = 0; h < 2; ++h) {
            const tgpu::GripPose &a = st.grip[h], &b = cam.grip[h];
            if (!a.valid || !b.valid) continue;
            const float dx = b.loc[0] - a.loc[0], dy = b.loc[1] - a.loc[1], dz = b.loc[2] - a.loc[2];
            if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) continue;   // a recenter or a load
            gp.hand[h] = tgpu::hand_motion(a, b, (std::min)(st.gripWeight[h], cam.gripWeight[h]));
        }
        if (gp.hand[0].valid || gp.hand[1].valid) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                BVR_LOG("[temporal] hand motion vectors on: controller motion within %.0f-%.0f uu of each grip",
                        gp.handFullUu, gp.handZeroUu);
            }
        }
    }
    bool guidesOk = false;
    char why[256] = "";
    if (depthOk && camOk) {
        guidesOk = g_guides.run(g_dev, ctx, g_world.srv, fgOk ? g_fg.srv : nullptr, gp, why, sizeof(why));
        if (!guidesOk) {
            static bool logged = false;
            if (!logged) { logged = true; BVR_LOG("[temporal] guides failed: %s", why); }
        }
    }
    // 2026-10-23: this image's linear depth to the room snapshot, so the room keeps its distances
    // and the loading screen's backdrop is stereo (openxr_runtime.h provide_room_depth).
    if (guidesOk) {
        float ws = 0.0f;
        camera::get_recenter_state(nullptr, nullptr, &ws);
        bvr::vr::provide_room_depth(g_guides.linear_z_srv(), ws > 1.0f ? ws : 100.0f);
    }

    // The hands mask is worth binding when something is in it: a foreground pass, or the
    // viewmodel's squeezed depth range (BS1's case - the guides mark it from the raw depth).
    const bool maskOk = guidesOk && (fgOk || g_squeezeHi > g_squeezeLo);

    // This eye's input as a texture of our own: TAA reads it, the audit and the probe keep it.
    const int mode = g_mode.load();
    const bool inCopied = guidesOk && (mode == kTaa || g_audit.load() || g_probeOn.load()) && [&] {
        D3D11_TEXTURE2D_DESC sd = {};
        f.src->GetDesc(&sd);
        const int cur = st.inCur;
        st.inFmt = sd.Format;
        if (!ensure_tex(&st.in[cur], &st.inSrv[cur], f.srcW, f.srcH, sd.Format)) return false;
        ctx->CopyResource(st.in[cur], f.src);
        return true;
    }();

    // 4. DLSS, or TAA.
    bool written = false;
    if (mode == kOff || mode == kTaa) {
        if (g_srOutputSet) { bvr::vr::set_eye_output_size(0, 0); g_srOutputSet = false; }
        stop_client();
        if (mode == kTaa && inCopied && f.dstW == f.srcW && f.dstH == f.srcH) {
            char twhy[256] = "";
            if (g_taa.run(g_dev, ctx, eye, st.inSrv[st.inCur], g_guides.motion_srv(), maskOk ? g_guides.hands_srv() : nullptr,
                          f.srcW, f.srcH, hist, g_taaWeight.load(), twhy, sizeof(twhy))) {
                ctx->CopyResource(f.dst, g_taa.out(eye));
                written = true;
                ++g_tally.taa;
                ++g_life.taaFrames;
            } else {
                static bool logged = false;
                if (!logged) { logged = true; BVR_LOG("[temporal] TAA failed: %s", twhy); }
            }
        } else if (mode == kTaa) {
            g_taa.forget(eye);
        }
    } else {
        if (g_retry.exchange(false) && g_cstate.load() == kFailed) stop_client();
        const int cs = g_cstate.load();
        if (cs == kIdle) start_client();
        if (mode == kSr && cs != kFailed) {
            // Asked for once per size: if the runtime refuses it, core drops the request
            // and the eyes stay at the render size - asking again every frame would
            // rebuild the swapchains every frame.
            uint32_t ow, oh;
            sr_output_for(f.srcW, f.srcH, &ow, &oh);
            if (!g_srOutputSet || g_srAskedW != ow || g_srAskedH != oh) {
                // An output equal to the render is the backbuffer size: asked of core as 0 x 0,
                // so the eye swapchains are NOT rebuilt for nothing (the fifth run rebuilt them
                // at 2750x2850 -> 2750x2850 six seconds before the driver crash).
                const bool same = ow == f.srcW && oh == f.srcH;
                bvr::vr::set_eye_output_size(same ? 0 : ow, same ? 0 : oh);
                g_srOutputSet = true;
                g_srAskedW = ow; g_srAskedH = oh;
                uint32_t rw = 0, rh = 0, gw = 0, gh = 0;
                bvr::vr::recommended_eye_size(&rw, &rh);
                sr_render_for(ow, oh, &gw, &gh);
                const float ratio = (float)ow / (float)f.srcW;
                BVR_LOG("[dlss] super resolution: eye output %ux%u (the headset's recommended size is %ux%u%s), the game "
                        "renders %ux%u = %.2fx per axis; %s live render target %ux%u",
                        ow, oh, rw, rh, g_outW.load() ? ", overridden by `dlss output`" : "", f.srcW, f.srcH, ratio,
                        kQualityNames[g_quality.load() % 5], gw, gh);
            }
        } else if (g_srOutputSet) {
            bvr::vr::set_eye_output_size(0, 0);
            g_srOutputSet = false;
            g_srAskedW = g_srAskedH = 0;
        }
        // Super Resolution with an output equal to the render (the game at the headset's size)
        // runs as DLAA rather than doing nothing (the seventh run: "DLSS didn't seem to work").
        const bool sizeOk = mode == kSr ? (f.dstW >= f.srcW && f.dstH >= f.srcH) : (f.dstW == f.srcW && f.dstH == f.srcH);
        if (g_cstate.load() == kUp && guidesOk && sizeOk) {
            const DXGI_FORMAT cf = typed_of(([&] { D3D11_TEXTURE2D_DESC d = {}; f.src->GetDesc(&d); return d.Format; })());
            const int preset = kModelPresets[g_model.load() & 3];
            BuildKey want{f.srcW, f.srcH, f.dstW, f.dstH, cf, preset};
            BuildKey& have = g_built[eye];
            bool built = have.w == want.w && have.h == want.h && have.ow == want.ow && have.oh == want.oh &&
                         have.fmt == want.fmt && have.preset == want.preset;
            if (!built) {
                char bwhy[256] = "";
                if (g_client.build(eye, f.srcW, f.srcH, f.dstW, f.dstH, cf, preset, bwhy, sizeof(bwhy))) {
                    have = want;
                    built = true;
                    hist = false;   // a new feature has no history
                } else {
                    BVR_LOG("[dlss] eye %d build refused: %s - normal copy until `dlss retry`", eye, bwhy);
                    set_why("build refused: %s", bwhy);
                    g_cstate.store(kFailed);
                }
            }
            if (built) {
                bvr::dlss::EyeInputs in;
                in.color = f.src;
                in.depth = g_guides.dlss_depth();
                in.motion = g_guides.motion();
                // The arms and weapon move with the controllers, which no camera vector
                // describes: DLSS takes them from the current image (the Dishonored mod's
                // fgbias, default on there too).
                in.bias = maskOk && g_fgBias.load() ? g_guides.hands() : nullptr;
                in.reset = !hist;
                char ewhy[256] = "";
                const bool evaluated = g_client.evaluate(ctx, eye, in, ewhy, sizeof(ewhy));
                bvr::frame_pacing::dlss(g_client.stats.uploadMsLast[eye], g_client.stats.replyMsLast[eye],
                    g_client.stats.waitMsLast[eye], g_client.stats.gpuMsLast[eye]);
                if (evaluated) {
                    ctx->CopyResource(f.dst, g_client.output_texture(eye));
                    written = true;
                    ++g_tally.dlss;
                    ++g_life.dlssFrames;
                } else if (!g_client.running()) {
                    set_why("stopped: %s", ewhy);
                    g_cstate.store(kFailed);
                    g_built[0] = g_built[1] = BuildKey{};
                }
            }
        }
    }

    // 5. The plain path, when AFW needs this eye's image kept anyway.
    if (!written && g_afw.load()) {
        bvr::blit::stretch_copy(ctx, f.dst, f.src);
        written = true;
    }
    if (!written) ++g_tally.plain;

    // 6. History for the next image of this eye, and the AFW sources.
    if (camOk && depthOk && guidesOk) {
        st.cam = tc;
        st.tracking = cam.tracking;
        for (int h = 0; h < 2; ++h) { st.grip[h] = cam.grip[h]; st.gripWeight[h] = cam.gripWeight[h]; }
        st.build = cam.build;
        st.stampMs = now;
        st.w = f.srcW; st.h = f.srcH;
        st.epoch = epoch;
        st.histValid = true;
        if (inCopied) {
            // Keep this input; with history, score the vectors against the previous one.
            const int cur = st.inCur;
            if (g_audit.load() && hist && st.inPrevValid && st.in[cur ^ 1])
                g_guides.audit_frame(g_dev, ctx, st.inSrv[cur], st.inSrv[cur ^ 1], gp);
            // The hand-depth probe: a SequentialReentry pair is two native eyes of one tick.
            // Every 15th pair: keep the left image and depth, probe when its right arrives.
            if (g_probeOn.load() && f.sequential) {
                if (eye == 0) {
                    g_probeArmed = (++g_probeTick % 15) == 0 &&
                                   ensure_tex(&g_probeCol, &g_probeColSrv, f.srcW, f.srcH, st.inFmt) &&
                                   ensure_tex(&g_probeLz, &g_probeLzSrv, f.srcW, f.srcH, DXGI_FORMAT_R32_FLOAT);
                    if (g_probeArmed) {
                        ctx->CopyResource(g_probeCol, st.in[cur]);
                        ctx->CopyResource(g_probeLz, g_guides.linear_z());
                        g_probeCam = tc;
                        g_probeBuild = cam.build;
                    }
                } else if (g_probeArmed) {
                    g_probeArmed = false;
                    if (cam.build == g_probeBuild + 1) {
                        char pwhy[256] = "";
                        if (!g_probe.run(g_dev, ctx, g_probeColSrv, g_probeLzSrv, st.inSrv[cur], g_probeCam, tc, f.srcW,
                                         f.srcH, g_afwFgBand.load(), pwhy, sizeof(pwhy))) {
                            static bool logged = false;
                            if (!logged) { logged = true; BVR_LOG("[temporal] hand-depth probe failed: %s", pwhy); }
                        }
                    }
                }
            }
            st.inCur ^= 1;
            st.inPrevValid = true;
        }
    } else {
        st.histValid = false;
        st.inPrevValid = false;
    }

    // 7. The AFW store. EVERY image this eye shows is kept, so the held eye can be written on
    //    every present, including a frame without depth. A complete depth/camera
    //    sample may feed the next rebuild; F10 lives in a separate quad and never
    //    changes this history or its validity.
    if (g_afw.load() && written && !f.sequential) {
        const bool okOut = ensure_tex(&st.out, &st.outSrv, f.dstW, f.dstH, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (okOut) {
            ctx->CopyResource(st.out, f.dst);
            st.ow = f.dstW; st.oh = f.dstH;
            st.outShown = true;
            g_freshShown = true;
            g_freshEye = eye;
        }
        const bool source = okOut && camOk && depthOk && guidesOk &&
                            ensure_tex(&st.lz, &st.lzSrv, f.srcW, f.srcH, DXGI_FORMAT_R32_FLOAT);
        if (source) {
            ctx->CopyResource(st.lz, g_guides.linear_z());
            st.lw = f.srcW; st.lh = f.srcH;
            st.afwValid = true;
            g_freshValid = true;
            // The hands mask rides with the depth: the rebuild takes the hands from the fresh
            // eye and drops the held eye's (afw_gpu.h).
            st.hmValid = maskOk && ensure_tex(&st.hm, &st.hmSrv, f.srcW, f.srcH, DXGI_FORMAT_R8_UNORM);
            if (st.hmValid) ctx->CopyResource(st.hm, g_guides.hands());
        } else {
            st.afwValid = false;
            st.hmValid = false;
        }
    } else {
        st.afwValid = false;
    }
    native_repair::finish_eye(f.src, camOk ? cam.build : 0, f.imageSign, f.tanH, f.tanV,
        mode == kOff && g_afw.load() && !f.sequential && f.srcW == f.dstW && f.srcH == f.dstH);

    // 8. The laser, last: after the AFW store copied this image, so the beam never becomes
    //    scenery the rebuild carries into the other eye. Drawn through this image's camera.
    st.laser = camOk ? cam.laser : laser_draw::Segment{};
    if (written && camOk && laser_draw::draw(ctx, f.dst, f.dstW, f.dstH, tc, cam.laser))
        g_laserDrawMs.store(now, std::memory_order_relaxed);
    log_maybe(now);
    return written;
}

int aer_source() {
    std::lock_guard<std::mutex> lk(g_camMutex);
    g_aerBuild = 0;
    if (!g_fifoCount) {
        // No render view waiting (a menu, a load, a scripted camera): unknown, so this
        // present is not an eye image.
        ++g_fifoUnderruns;
        g_fifoResync = true;
        g_fifoDeepPops = 0;
        return 0;
    }
    g_fifoDeepPops = g_fifoCount > 1 ? g_fifoDeepPops + 1 : 0;
    if ((g_fifoResync || g_fifoDeepPops >= 8) && g_fifoCount > 1) {
        ++g_fifoResyncs;
        BVR_LOG("[afw] eye queue resync (%s): dropped %d stale entr%s", g_fifoResync ? "after an underrun" : "drift",
                g_fifoCount - 1, g_fifoCount == 2 ? "y" : "ies");
        g_fifo[0] = g_fifo[g_fifoCount - 1];
        g_fifoCount = 1;
    }
    g_fifoResync = false;
    if (g_fifoCount <= 1) g_fifoDeepPops = 0;
    const uint64_t b = g_fifo[0];
    for (int i = 1; i < g_fifoCount; ++i) g_fifo[i - 1] = g_fifo[i];
    --g_fifoCount;
    const CamRec& c = g_cams[b % kCamRing];
    if (c.build != b) return 0;
    g_aerBuild = b;
    return c.sign;
}

// ---- the AFW capture (temporal.h request_dump) ------------------------------------------
std::atomic<int> g_dumpLeft{0};
std::atomic<uint64_t> g_dumpAtMs{0};
wchar_t g_dumpDir[MAX_PATH] = L"";
uint32_t g_dumpIndex = 0;
char g_dumpStatus[200] = "no capture yet";

uint32_t bytes_per_pixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return 4;
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R32G32_FLOAT: return 8;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        default: return 0;
    }
}
// Raw rows, tightly packed; the header line says the size, format and bytes per pixel.
bool dump_texture(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const wchar_t* path, FILE* meta, const char* key) {
    if (!t) { fprintf(meta, "%s=absent\n", key); return false; }
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    const uint32_t bpp = bytes_per_pixel(d.Format);
    if (!bpp || d.SampleDesc.Count != 1) { fprintf(meta, "%s=unsupported fmt %u\n", key, (unsigned)d.Format); return false; }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) { fprintf(meta, "%s=staging refused\n", key); return false; }
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m = {};
    bool ok = false;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path, L"wb") && f) {
            for (uint32_t y = 0; y < d.Height; ++y) fwrite((const uint8_t*)m.pData + (size_t)y * m.RowPitch, bpp, d.Width, f);
            fclose(f);
            ok = true;
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    fprintf(meta, "%s=%s %ux%u fmt %u bpp %u\n", key, ok ? "ok" : "write failed", d.Width, d.Height, (unsigned)d.Format, bpp);
    return ok;
}
void meta_camera(FILE* f, const char* key, const tgpu::Camera& c) {
    fprintf(f, "%s=%.4f %.4f %.4f | %.7f %.7f %.7f | %.7f %.7f %.7f | %.7f %.7f %.7f | %.7f %.7f\n", key, c.loc[0], c.loc[1],
            c.loc[2], c.fwd[0], c.fwd[1], c.fwd[2], c.right[0], c.right[1], c.right[2], c.up[0], c.up[1], c.up[2], c.tanH, c.tanV);
}
void dump_tick(ID3D11DeviceContext* ctx, const bvr::vr::HeldFrame& hf, const EyeStore& F, const EyeStore& Hs,
               const afw::Source& A, const afw::Source& B, const afw::Params& p, ID3D11Texture2D* rebuilt, int how) {
    if (g_dumpLeft.load() <= 0 || GetTickCount64() < g_dumpAtMs.load() || !g_dumpDir[0]) return;
    const uint32_t n = g_dumpIndex++;
    wchar_t base[MAX_PATH], path[MAX_PATH];
    _snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%s\\p%02u", g_dumpDir, n);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s.txt", base);
    FILE* meta = nullptr;
    if (_wfopen_s(&meta, path, L"w") || !meta) {
        g_dumpLeft.store(0);
        BVR_LOG("[afw] capture: cannot write %ls - stopped", path);
        return;
    }
    fprintf(meta, "present=%u\nhow=%d\nfresh=%d\nheld=%d\noutput=%ux%u\ntanH=%.7f\ntanV=%.7f\n", n, how, hf.fresh, hf.held, hf.w,
            hf.h, hf.tanH, hf.tanV);
    fprintf(meta, "renderer=%s\n", how == 3 ? "native-foreground-hdr" : "legacy-afw");
    fprintf(meta, "nearUu=%.3f\nfarUu=%.1f\nsqueeze=%.6f %.6f\neyeHalfSepUu=%.4f\n", g_nearUu.load(), g_farUu.load(), g_squeezeLo,
            g_squeezeHi, camera::eye_half_separation_uu());
    fprintf(meta, "grid=%d\nrefine=%d\nstaleTest=%d\nedgeRel=%.4f\nedgeAbsUu=%.3f\nstaleRel=%.4f\nstaleAbsUu=%.3f\nnearerRel=%.4f\nskyUu=%.1f\n"
            "fgGain=%.4f\nfgBandUu=%.2f\nhandsFromFresh=%d\ntint=%d\nfgTanGain=%.5f\n", p.grid, (int)p.refine, (int)p.staleTest, p.edgeRel,
            p.edgeAbsUu, p.staleRel, p.staleAbsUu, p.nearerRel, p.skyUu, p.fgGain, p.fgBandUu, (int)p.handsFromFresh, (int)p.tint,
            p.fgTanGain);
    fprintf(meta, "fresh.valid=%d\nfresh.build=%llu\nfresh.stampMs=%llu\n", (int)B.valid, (unsigned long long)F.build,
            (unsigned long long)F.stampMs);
    meta_camera(meta, "fresh.cam", B.cam);
    // The fire ray of the fresh build - the in-image laser's line - and the engine's muzzle
    // socket and barrel axis behind it, so a capture can be checked against the drawn gun.
    {
        const laser_draw::Segment& L = F.laser;
        fprintf(meta, "fresh.ray=%d %.4f %.4f %.4f | %.7f %.7f %.7f\n", (int)L.valid, L.origin[0], L.origin[1],
                L.origin[2], L.dir[0], L.dir[1], L.dir[2]);
        fprintf(meta, "fresh.muzzle=%d %.4f %.4f %.4f | %.7f %.7f %.7f\n", (int)L.muzzleValid, L.muzzle[0],
                L.muzzle[1], L.muzzle[2], L.barrelAxis[0], L.barrelAxis[1], L.barrelAxis[2]);
    }
    fprintf(meta, "held.valid=%d\nheld.build=%llu\nheld.stampMs=%llu\n", (int)A.valid, (unsigned long long)Hs.build,
            (unsigned long long)Hs.stampMs);
    meta_camera(meta, "held.cam", A.cam);
    fprintf(meta, "held.foregroundCameraValid=%d\n", (int)A.foregroundCameraValid);
    if (A.foregroundCameraValid) meta_camera(meta, "held.foregroundCam", A.foregroundCam);
    meta_camera(meta, "target.cam", p.target);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_fresh.raw", base);       dump_texture(ctx, F.out, path, meta, "fresh.color");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_fresh_depth.raw", base); dump_texture(ctx, B.valid ? F.lz : nullptr, path, meta, "fresh.depth");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_fresh_mask.raw", base);  dump_texture(ctx, F.hmValid ? F.hm : nullptr, path, meta, "fresh.mask");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_held.raw", base);        dump_texture(ctx, Hs.outShown ? Hs.out : nullptr, path, meta, "held.color");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_held_depth.raw", base);  dump_texture(ctx, A.valid ? Hs.lz : nullptr, path, meta, "held.depth");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_held_mask.raw", base);   dump_texture(ctx, A.valid && Hs.hmValid ? Hs.hm : nullptr, path, meta, "held.mask");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s_rebuilt.raw", base);     dump_texture(ctx, rebuilt, path, meta, "rebuilt");
    fclose(meta);
    const int left = g_dumpLeft.fetch_sub(1) - 1;
    if (left > 0) _snprintf_s(g_dumpStatus, sizeof(g_dumpStatus), _TRUNCATE, "capturing: %u of %u presents", n + 1, n + 1 + (uint32_t)left);
    else _snprintf_s(g_dumpStatus, sizeof(g_dumpStatus), _TRUNCATE, "done: %u presents in %ls", n + 1, g_dumpDir);
    if (left <= 0)
        BVR_LOG("[afw] capture: %u consecutive rebuilt presents written to %ls (both eyes' colour, depth and hands mask, the "
                "rebuilt image and a header per present) - tools\\afw-replay.ps1 -Capture <that folder>", n + 1, g_dumpDir);
}

// True on every AFW present that showed an image: the held eye is then written every present.
bool held_ready(int held) {
    return g_afw.load() && g_afwRebuild.load() && g_freshShown && g_freshEye == (held ^ 1);
}

int held_rebuild(ID3D11DeviceContext* ctx, const bvr::vr::HeldFrame& hf) {
    bvr::frame_inspector::ScopedSuppress suppress;
    const bool freshSource = g_freshValid;
    g_freshValid = false;
    g_freshShown = false;
    const EyeStore& F = g_store[hf.fresh & 1];
    const EyeStore& Hs = g_store[hf.held & 1];
    const uint64_t now = GetTickCount64();
    afw::Source B, A;
    B.valid = F.afwValid;
    B.color = F.outSrv; B.depth = F.lzSrv; B.depthW = F.lw; B.depthH = F.lh; B.cam = F.cam;
    B.mask = F.hmValid ? F.hmSrv : nullptr;
    // A capture stalls each present on its readbacks (~120 MB), so while one runs the held
    // image is allowed to be older: otherwise every other captured present lost its held
    // source (the seventh run's capture: held.valid=0 on the odd presents).
    const uint64_t maxHeldAgeMs = g_dumpLeft.load() > 0 ? 2000 : 150;
    A.valid = Hs.afwValid && now - Hs.stampMs < maxHeldAgeMs && Hs.lw == F.lw && Hs.lh == F.lh;
    A.color = Hs.outSrv; A.depth = Hs.lzSrv; A.depthW = Hs.lw; A.depthH = Hs.lh; A.cam = Hs.cam;
    A.mask = Hs.hmValid ? Hs.hmSrv : nullptr;
    A.foregroundCameraValid = afw::transport_foreground_camera(
        Hs.cam, Hs.tracking, F.tracking, A.foregroundCam);
    afw::Params p;
    p.w = hf.w; p.h = hf.h;
    p.target = F.cam;
    // The held eye at this instant: the fresh camera moved across by the eye separation.
    const float half = camera::eye_half_separation_uu();
    const float s = (hf.held == 0 ? -1.0f : 1.0f) * 2.0f * half;
    for (int i = 0; i < 3; ++i) p.target.loc[i] += F.cam.right[i] * s;
    p.target.tanH = hf.tanH; p.target.tanV = hf.tanV;
    p.grid = g_afwGrid.load();
    p.refine = g_afwRefine.load();
    p.staleTest = g_afwStale.load();
    p.tint = g_afwTint.load();
    p.fgGain = g_afwFgGain.load();
    p.fgBandUu = g_afwFgBand.load();
    p.handsFromFresh = g_afwMask.load();
    // The hands' lens: 1 by default. The lens match writes k = 1.38 into the engine's foreground
    // FOV field, but the capture's disparity probe reads the hands' geometry as the world's lens
    // with a depth factor (`afw lens 1.38` scored worse than the depth gain alone).
    p.fgTanGain = g_afwLensGain.load() > 0.0f ? g_afwLensGain.load() : 1.0f;
    if (freshSource && native_repair::copy_output(ctx, hf.dst, F.build, hf.fresh, hf.tanH, hf.tanV)) {
        ++g_tally.afwRebuilds;
        ++g_life.afwRebuilds;
        dump_tick(ctx, hf, F, Hs, A, B, p, hf.dst, 3);
        if (laser_draw::draw(ctx, hf.dst, hf.w, hf.h, p.target, F.laser)) g_laserDrawMs.store(now, std::memory_order_relaxed);
        return 1;
    }
    char why[256] = "";
    if (freshSource && B.valid && F.ow == hf.w && F.oh == hf.h && g_warp.run(g_dev, ctx, A, B, p, why, sizeof(why))) {
        ctx->CopyResource(hf.dst, g_warp.out());
        ++g_tally.afwRebuilds;
        ++g_life.afwRebuilds;
        if (A.valid && A.foregroundCameraValid) {
            if (!g_life.afwForegroundMotion)
                BVR_LOG("[afw] foreground locomotion transport active (per-build tracking frame)");
            ++g_life.afwForegroundMotion;
        }
        if (A.valid) { ++g_tally.afwHeldSrc; ++g_life.afwWithHeld; }
        dump_tick(ctx, hf, F, Hs, A, B, p, g_warp.out(), 1);
        // The rebuilt eye is this instant's held view (p.target), so the beam is drawn as that
        // eye sees it - the fresh build's ray, through the held eye's own camera.
        if (laser_draw::draw(ctx, hf.dst, hf.w, hf.h, p.target, F.laser)) g_laserDrawMs.store(now, std::memory_order_relaxed);
        return 1;
    }
    static bool logged = false;
    if (!logged && why[0]) { logged = true; BVR_LOG("[afw] rebuild failed: %s (the held eye keeps its last image)", why); }
    ++g_tally.afwCopies;
    ++g_life.afwFallbacks;
    // Missing depth/camera or a GPU failure: re-show the held eye's own last
    // image with its pose kept. Keep writing the swapchain every present.
    if (Hs.outShown && Hs.out) bvr::blit::stretch_copy(ctx, hf.dst, Hs.out);
    else if (F.out) bvr::blit::stretch_copy(ctx, hf.dst, F.out);
    return 2;
}

bool g_seamsSet = false;
std::atomic<bool> g_tapSet{false};

// Armed on first use and then kept: switched off, the tap and the processor return at
// their first line, and keeping them lets the processor itself stop the helper on the
// present thread (the only thread that calls into the client while it runs).
void update_seams() {
    if (!active() || g_seamsSet) return;
    if (!g_tapSet.exchange(true)) bvr::frame_inspector::set_draw_tap(&kTap);
    bvr::vr::set_eye_processor(&process_eye);
    g_seamsSet = true;
    BVR_LOG("[temporal] seams armed (depth tap + eye processor)");
}

const char* mode_name(int m) {
    return m == kOff ? "off" : m == kDlaa ? "DLAA" : m == kSr ? "DLSS Super Resolution" : "TAA";
}

void set_mode(int m, const char* who) {
    m = m < 0 ? 0 : m > 3 ? 3 : m;
    if (g_mode.exchange(m) == m) return;
    native_repair::set_active(g_afw.load() && m == kOff);
    g_resolutionSerial.fetch_add(1);
    g_resetEpoch.fetch_add(1);
    if (m != kSr) bvr::vr::set_eye_output_size(0, 0);
    BVR_LOG("[dlss] %s (%s)", mode_name(m), who);
    update_seams();
}

bool word(const char* s, const char* w) {
    const size_t n = strlen(w);
    return strncmp(s, w, n) == 0 && (s[n] == 0 || s[n] == ' ');
}

} // namespace

void arm_draw_tap() {
    if (!g_tapSet.exchange(true)) bvr::frame_inspector::set_draw_tap(&kTap);
}

void init() {
    BVR_LOG("[temporal] DLSS/DLAA and alternate frame warp available (both off; F10 Display, `dlss`, `afw`)");
}

void tick_resolution() {
    const uint64_t serial=g_resolutionSerial.load();
    if(serial==g_resolutionApplied || resize::busy()) return;
    uint32_t bw=0,bh=0,ow=0,oh=0;
    if(!bvr::hud::backbuffer_dims(&bw,&bh) || !bw || !bh) return;
    if(!output_target(&ow,&oh)) return; // keep intent until XR knows the output
    uint32_t rw=ow,rh=oh;
    if(g_mode.load()==kSr) sr_render_for(ow,oh,&rw,&rh);
    g_resolutionApplied=serial; // refusal is not an every-frame retry loop
    if(rw!=bw || rh!=bh) resize::request(rw,rh,"DLSS / resolution change");
}

// Each controller's grip as this build placed it: the pose funnel and the XR-to-game map the
// hands use (frame_context.h), so a rigid offset between the grip and the drawn hand (the
// grip calibration, the model trim, the aim/grip split) cancels out of the motion.
static void note_grips(const FrameContext& ctx, uint64_t now, CamRec& c) {
    const bool driven = hands::active() && ctx.vrDriving;
    for (int h = 0; h < 2; ++h) {
        c.grip[h] = tgpu::GripPose{};
        c.gripWeight[h] = 0.0f;
        bvr::vr::HeadPose hp{};
        if (!driven || !bvr::vr::get_hand_pose(h, /*aimPose=*/false, hp)) continue;
        const float pos[3] = {hp.px, hp.py, hp.pz}, quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        const GamePose g = xr_pose_to_game(ctx, pos, quat);
        tgpu::GripPose& out = c.grip[h];
        out.valid = true;
        out.loc[0] = g.loc.x; out.loc[1] = g.loc.y; out.loc[2] = g.loc.z;
        ue_rot_basis(g.rot, out.fwd, out.right, out.up);
        c.gripWeight[h] = handback::tracked(h, now);
    }
}

void note_camera(int eyeSign, const FVector& loc, const FRotator& rot, const FrameContext* tracking) {
    if (!active()) return;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_camMutex);
    const uint64_t b = ++g_buildCounter;
    CamRec& c = g_cams[b % kCamRing];
    c.build = b; c.sign = eyeSign; c.loc = loc; c.rot = rot; c.stampMs = now;
    if (!aim::laser_segment(&c.laser)) c.laser = laser_draw::Segment{};
    if (tracking) {
        c.tracking = afw::tracking_frame(*tracking);
        note_grips(*tracking, now, c);
        g_lastFramed = c;
    } else if (eyeSign > 0 && g_lastFramed.build == b - 1 && g_lastFramed.sign < 0 &&
               now - g_lastFramed.stampMs <= 100) {
        // SequentialReentry's right eye: the second pass of the left eye's frame.
        c.tracking = g_lastFramed.tracking;
        for (int h = 0; h < 2; ++h) { c.grip[h] = g_lastFramed.grip[h]; c.gripWeight[h] = g_lastFramed.gripWeight[h]; }
    } else {
        c.tracking = afw::TrackingFrame{};
        for (int h = 0; h < 2; ++h) { c.grip[h] = tgpu::GripPose{}; c.gripWeight[h] = 0.0f; }
    }
    if (g_afw.load(std::memory_order_relaxed)) {
        tgpu::Params depth;
        depth.nearUu = g_nearUu.load(); depth.farUu = g_farUu.load();
        depth.squeezeLo = g_squeezeLo; depth.squeezeHi = g_squeezeHi;
        afw::Params warp;
        warp.grid = g_afwGrid.load(); warp.staleTest = g_afwStale.load();
        warp.fgGain = g_afwFgGain.load(); warp.fgBandUu = g_afwFgBand.load();
        warp.fgTanGain = g_afwLensGain.load() > 0.0f ? g_afwLensGain.load() : 1.0f;
        native_repair::note_camera(b, eyeSign, to_tcam(c, 1, 1),
            2 * camera::eye_half_separation_uu(), depth, warp);
    }
    if (g_afw.load(std::memory_order_relaxed)) {
        if (g_fifoCount == kFifo) {
            for (int i = 1; i < kFifo; ++i) g_fifo[i - 1] = g_fifo[i];
            --g_fifoCount;
        }
        g_fifo[g_fifoCount++] = b;
    } else {
        g_fifoCount = 0;
    }
}

void set_afw(bool on) {
    if (g_afw.exchange(on) == on) return;
    native_repair::set_active(on && g_mode.load() == kOff);
    if (on) {
        bvr::vr::set_aer_eye_source(&aer_source);
        bvr::vr::set_held_rebuild(&held_ready, &held_rebuild);
        bvr::vr::set_alternate_eye(true);
    } else {
        bvr::vr::set_alternate_eye(false);
        bvr::vr::set_held_rebuild(nullptr, nullptr);
        bvr::vr::set_aer_eye_source(nullptr);
    }
    {
        std::lock_guard<std::mutex> lk(g_camMutex);
        g_fifoCount = 0;
        g_fifoResync = false;
        g_fifoDeepPops = 0;
    }
    g_resetEpoch.fetch_add(1);
    update_seams();
    BVR_LOG("[afw] %s", on ? "ON: every present is an XR frame; the eye not rendered is rebuilt from both eyes"
                           : "off");
}

void handle_command(const char* verb, const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (strcmp(verb, "dlss") == 0) {
        if (word(args, "on") || word(args, "dlaa")) set_mode(kDlaa, "seam");
        else if (word(args, "sr")) set_mode(kSr, "seam");
        else if (word(args, "taa")) set_mode(kTaa, "seam");
        else if (word(args, "off")) set_mode(kOff, "seam");
        else if (word(args, "taaweight")) {
            const float v = (float)atof(args + 9);
            if (v >= 0.02f && v <= 1.0f) g_taaWeight.store(v);
            BVR_LOG("[temporal] TAA current-image weight %.2f", g_taaWeight.load());
        }
        else if (word(args, "retry")) { g_retry.store(true); BVR_LOG("[dlss] retry requested"); }
        else if (word(args, "quality")) {
            const int q=atoi(args+7);
            if(q<0 || q>=5) {BVR_LOG("[dlss] quality must be 0..4");return;}
            g_quality.store(q);
            if(g_mode.load()==kSr) g_resolutionSerial.fetch_add(1);
            g_resetEpoch.fetch_add(1);
            BVR_LOG("[dlss] quality %s (%.2fx per axis)", kQualityNames[g_quality.load()], kRatios[g_quality.load()]);
        } else if (word(args, "output")) {
            unsigned ow = 0, oh = 0;
            if (sscanf_s(args + 6, " %ux%u", &ow, &oh) == 2 && ow >= 256 && oh >= 256 && ow <= 8192 && oh <= 8192) {
                g_outW.store(ow); g_outH.store(oh);
            } else {
                g_outW.store(0); g_outH.store(0);
            }
            g_resolutionSerial.fetch_add(1);
            BVR_LOG("[dlss] super resolution output: %s", g_outW.load() ? "overridden" : "the headset's recommended size");
            if (g_outW.load()) BVR_LOG("[dlss] super resolution output %ux%u", g_outW.load(), g_outH.load());
        } else if (word(args, "model")) {
            g_model.store(atoi(args + 5) & 3);
            BVR_LOG("[dlss] model %s (preset %d)", kModelNames[g_model.load()], kModelPresets[g_model.load()]);
        } else if (word(args, "audit")) {
            g_audit.store(strstr(args, "on") != nullptr);
            BVR_LOG("[temporal] vector audit %s", g_audit.load() ? "on" : "off");
        } else if (word(args, "near")) {
            const float v = (float)atof(args + 4);
            if (v > 0.1f) g_nearUu.store(v);
            BVR_LOG("[temporal] near plane %.2f uu", g_nearUu.load());
        } else if (word(args, "far")) {
            const float v = (float)atof(args + 3);
            if (v >= 0.0f) g_farUu.store(v);
            BVR_LOG("[temporal] far plane %.0f uu (0 = infinite)", g_farUu.load());
        } else if (word(args, "body")) {
            const float v = (float)atof(args + 4);
            if (v >= 0.0f && v < 3.0f) g_bodyM.store(v);
            BVR_LOG("[temporal] body band %.2f m (rotation-only vectors nearer than this)", g_bodyM.load());
        } else if (word(args, "fgbias")) {
            g_fgBias.store(strstr(args, "on") != nullptr);
            BVR_LOG("[dlss] hands mask as the bias mask: %s", g_fgBias.load() ? "on" : "off");
        } else if (word(args, "squeeze")) {
            g_squeezeAuto.store(strstr(args, "off") == nullptr);
            BVR_LOG("[temporal] squeezed-depth re-expansion %s (in use now: [%.3f, %.3f))",
                    g_squeezeAuto.load() ? "auto" : "OFF", g_squeezeLo, g_squeezeHi);
        } else if (word(args, "handmv")) {
            g_handVectors.store(strstr(args, "off") == nullptr);
            BVR_LOG("[dlss] hand motion vectors (controller motion on the hands) %s",
                    g_handVectors.load() ? "on" : "OFF (the room-fixed control)");
        } else if (word(args, "handr")) {
            float full = 0, zero = 0;
            if (sscanf_s(args + 5, "%f %f", &full, &zero) == 2 && full >= 0.0f && zero > full && zero < 2.0f) {
                g_handFullM.store(full);
                g_handZeroM.store(zero);
            }
            BVR_LOG("[dlss] hand motion: full within %.2f m of the grip, none beyond %.2f m",
                    g_handFullM.load(), g_handZeroM.load());
        } else if (word(args, "probe")) {
            g_probeOn.store(strstr(args, "on") != nullptr);
            update_seams();
            BVR_LOG("[temporal] hand-depth probe %s (needs Stereo method: both eyes every frame)",
                    g_probeOn.load() ? "on" : "off");
        } else {
            BVR_LOG("[dlss] mode %s, quality %s, model %s, helper: %s | audit %s near %.1f far %.0f body %.2f m "
                    "| usage: dlss on|dlaa|sr|taa|off|retry|quality <0-4>|output <w>x<h>|auto|model <0-3>|audit on|off|"
                    "near <uu>|far <uu>|body <m>|fgbias on|off|handmv on|off|handr <full m> <zero m>|"
                    "taaweight <0.02-1>",
                    mode_name(g_mode.load()),
                    kQualityNames[g_quality.load()], kModelNames[g_model.load()], why_now().c_str(),
                    g_audit.load() ? "on" : "off", g_nearUu.load(), g_farUu.load(), g_bodyM.load());
        }
    } else if (strcmp(verb, "afw") == 0) {   // `afw on|off` is alt_eye's (the stereo method)
        if (word(args, "tint")) { g_afwTint.store(strstr(args, "on") != nullptr); BVR_LOG("[afw] tint %d", (int)g_afwTint.load()); }
        else if (word(args, "mask")) {
            g_afwMask.store(strstr(args, "off") == nullptr);
            BVR_LOG("[afw] hands from the fresh eye (by the hands mask) %s", g_afwMask.load() ? "on" : "OFF (the control)");
        }
        else if (word(args, "dump")) {
            const int n = atoi(args + 4);
            request_dump(n > 0 ? n : 6, 5000);
        }
        else if (word(args, "lens")) {
            const float v = (float)atof(args + 4);
            g_afwLensGain.store(v >= 0.5f && v <= 2.0f ? v : 0.0f);
            BVR_LOG("[afw] hands' lens gain %s (%.3f in use; the lens match's own k is %.3f)",
                    g_afwLensGain.load() > 0.0f ? "overridden" : "from the lens match",
                    g_afwLensGain.load() > 0.0f ? g_afwLensGain.load() : camera::foreground_lens_gain(),
                    camera::foreground_lens_gain());
        }
        else if (word(args, "refine")) {
            if(word(args+6+((args[6]==' ')?1:0),"off")) g_afwRefine.store(false);
            else if(word(args+6+((args[6]==' ')?1:0),"on")) g_afwRefine.store(true);
            BVR_LOG("[afw] per-pixel refinement %s",g_afwRefine.load()?"ON":"OFF (mesh-only control)");
        }
        else if (word(args, "stale")) { g_afwStale.store(strstr(args, "on") != nullptr); BVR_LOG("[afw] stale test %d", (int)g_afwStale.load()); }
        else if (word(args, "grid")) {
            const int g = atoi(args + 4);
            if (g >= 1 && g <= 8) g_afwGrid.store(g);
            BVR_LOG("[afw] mesh grid %d texels", g_afwGrid.load());
        } else if (word(args, "rebuild")) {
            g_afwRebuild.store(strstr(args, "off") == nullptr);
            BVR_LOG("[afw] held-eye rebuild %s", g_afwRebuild.load() ? "on" : "OFF (the compositor reprojects it)");
        } else if (word(args, "fggain")) {
            const float v = (float)atof(args + 6);
            if (v >= 0.25f && v <= 2.0f) g_afwFgGain.store(v);
            BVR_LOG("[afw] hand depth gain %.2f (depth nearer than %.0f uu read as depth / gain)", g_afwFgGain.load(),
                    g_afwFgBand.load());
        } else if (word(args, "fgband")) {
            const float v = (float)atof(args + 6);
            if (v >= 10.0f && v <= 300.0f) g_afwFgBand.store(v);
            BVR_LOG("[afw] hand depth band %.0f uu", g_afwFgBand.load());
        } else {
            BVR_LOG("[afw] %s, rebuild %s, refine %s, grid %d, stale test %d, tint %d, hands from the fresh eye %d, hand depth gain "
                    "%.2f under %.0f uu, eye queue underruns %llu resyncs %llu | usage: afw on|off|rebuild on|off|tint on|off|"
                    "refine on|off|mask on|off|stale on|off|grid <1-8>|fggain <x>|fgband <uu>|dump [n]",
                    g_afw.load() ? "ON" : "off", g_afwRebuild.load() ? "on" : "OFF", g_afwRefine.load() ? "on" : "OFF", g_afwGrid.load(), (int)g_afwStale.load(),
                    (int)g_afwTint.load(), (int)g_afwMask.load(), g_afwFgGain.load(), g_afwFgBand.load(),
                    (unsigned long long)g_fifoUnderruns, (unsigned long long)g_fifoResyncs);
        }
    }
}

float menu_read(menu::Setting id) {
    using S = menu::Setting;
    switch (id) {
        case S::Upscaler: return (float)g_mode.load();
        case S::DlssQuality: return (float)g_quality.load();
        case S::DlssModel: return (float)g_model.load();
        case S::OutputWidth: return (float)g_outW.load();
        case S::OutputHeight: return (float)g_outH.load();
        case S::TemporalTaa: return g_mode.load()==kTaa?1.f:0.f;
        case S::AfwTint: return g_afwTint.load() ? 1.0f : 0.0f;
        case S::TemporalAudit: return g_audit.load() ? 1.0f : 0.0f;
        case S::AfwRebuild: return g_afwRebuild.load() ? 1.0f : 0.0f;
        case S::AfwHandGain: return g_afwFgGain.load();
        case S::AfwHandsMask: return g_afwMask.load() ? 1.0f : 0.0f;
        case S::HandDepthProbe: return g_probeOn.load() ? 1.0f : 0.0f;
        case S::HandMotionVectors: return g_handVectors.load() ? 1.0f : 0.0f;
        default: return std::numeric_limits<float>::quiet_NaN();
    }
}

bool menu_write(menu::Setting id, float value) {
    using S = menu::Setting;
    const int v = (int)value;
    switch (id) {
        case S::Upscaler: set_mode(v, "F10"); return true;
        case S::DlssQuality:
            if(v<0 || v>=5) return false;
            if(g_quality.exchange(v)!=v) {
                g_resetEpoch.fetch_add(1);
                if(g_mode.load()==kSr) g_resolutionSerial.fetch_add(1);
            }
            return true;
        case S::DlssModel: g_model.store(v & 3); return true;
        case S::OutputWidth: case S::OutputHeight:
            if(v<0 || v>8192) return false;
            (id==S::OutputWidth?g_outW:g_outH).store(static_cast<uint32_t>(v));
            g_resolutionSerial.fetch_add(1);return true;
        case S::TemporalTaa: set_mode(v?kTaa:kOff,"F10 Debug");return true;
        case S::AfwTint: g_afwTint.store(v != 0); return true;
        case S::TemporalAudit: g_audit.store(v != 0); return true;
        case S::AfwRebuild: g_afwRebuild.store(v != 0); return true;
        case S::AfwHandGain: if (value >= 0.25f && value <= 2.0f) g_afwFgGain.store(value); return true;
        case S::AfwHandsMask: g_afwMask.store(v != 0); return true;
        case S::HandDepthProbe:
            g_probeOn.store(v != 0);
            update_seams();
            return true;
        case S::HandMotionVectors: g_handVectors.store(v != 0); return true;
        default: return false;
    }
}

void request_dump(int presents, uint32_t delayMs) {
    if (presents <= 0 || presents > 32) {
        BVR_LOG("[afw] capture refused: %d presents (1..32)", presents);
        return;
    }
    const wchar_t* dd = bvr::log::data_dir();
    if (!dd || !dd[0]) { BVR_LOG("[afw] capture refused: no data dir"); return; }
    wchar_t root[MAX_PATH];
    _snwprintf_s(root, MAX_PATH, _TRUNCATE, L"%s\\dumps", dd);
    CreateDirectoryW(root, nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf_s(g_dumpDir, MAX_PATH, _TRUNCATE, L"%s\\afw-%04u%02u%02u-%02u%02u%02u", root, t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
    CreateDirectoryW(g_dumpDir, nullptr);
    g_dumpIndex = 0;
    // 2026-10-20: and the hands' and gun's drawn pose beside the pictures (tools\blender\pose_capture.py).
    bones::arm_pose_capture(g_dumpDir, delayMs, presents < 3 ? presents : 3);
    g_dumpAtMs.store(GetTickCount64() + delayMs);
    g_dumpLeft.store(presents);
    _snprintf_s(g_dumpStatus, sizeof(g_dumpStatus), _TRUNCATE, "armed: %d presents in %.1f s", presents, delayMs / 1000.0);
    BVR_LOG("[afw] capture armed: %d consecutive rebuilt presents from %.1f s from now into %ls; each stalls the frame on its "
            "readbacks (about 120 MB per present at 2750x2850). Needs AFW with the rebuild on.",
            presents, delayMs / 1000.0, g_dumpDir);
}

const char* dump_status() { return g_dumpStatus; }

bool laser_drawn_recently() {
    return GetTickCount64() - g_laserDrawMs.load(std::memory_order_relaxed) < 250;
}

Telemetry telemetry() {
    Telemetry t = g_life;
    t.helperState = g_cstate.load();
    t.probePairs = g_probe.lifetime.pairs;
    t.probeHands = g_probe.lifetime.median(0, nullptr);
    t.probeWorld = g_probe.lifetime.median(1, nullptr);
    return t;
}

Guides debug_guides() {
    return Guides{g_guides.motion(), g_guides.linear_z(), g_guides.dlss_depth(), g_guides.hands()};
}

void draw_status() {
    const int mode=g_mode.load();
    uint32_t w=0,h=0,rw=0,rh=0;
    bvr::hud::backbuffer_dims(&rw,&rh);
    bvr::vr::eye_output_size(&w,&h);
    if(!w || !h) {w=rw;h=rh;}
    const char* name=mode==kSr?kQualityNames[g_quality.load()]:mode_name(mode);
    ImGui::Text("render %ux%u -> output %ux%u (%s)",rw,rh,w,h,name);
    if((mode==kDlaa || mode==kSr) && g_cstate.load()!=kUp)
        ImGui::Text("DLSS: %s",why_now().c_str());
    if(g_cstate.load()==kFailed && ImGui::Button("Retry DLSS")) g_retry.store(true);
}

} // namespace bvr::b1r::temporal
