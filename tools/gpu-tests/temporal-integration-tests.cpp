// gpu-test-sources: src/game/bioshock1r/temporal.cpp src/game/bioshock1r/temporal_gpu.cpp src/game/bioshock1r/afw_gpu.cpp src/game/bioshock1r/alt_eye.cpp src/core/gfx/dlss_client.cpp src/core/gfx/blit.cpp src/core/gfx/frame_pacing.cpp
// gpu-test-needs: dlss-host
// The PRODUCTION glue (game/bioshock1r/temporal.cpp) driven the way the game and core drive
// it, with the engine and core seams replaced by stubs that record what they are given:
//   - the draw tap sees a world pass, a depth clear and a foreground pass on one D24 surface,
//     exactly the order ENGINE_NOTES' frame map describes - the world pass must be the
//     depth that is used;
//   - SequentialReentry presents (L, R per tick) and AlternateEye presents call the eye
//     processor; the real DLSS helper is started on its worker and must take over;
//   - Super Resolution must request its output size and reconstruct to it;
//   - the stereo methods (alt_eye.cpp, also production) are driven with the engine's real
//     CalcView pattern - three consumer calls and then the render view inside the scene
//     build, every frame (BRVR measured 4.2 per frame). The first AFW build flipped the eye
//     per CALL and crossed the eyes in the headset (2026-10-10); here every call of a frame
//     must carry that frame's eye;
//   - the alternate frame warp must name each image's eye from the build it was rendered
//     with, rebuild the held eye, and match a true render of it;
//   - alternate eye (AER, BRVR's method) must tag exactly one eye per frame, L R L R, start
//     every pair on the left, and render the right frame from the left frame's cameras;
//   - switching everything off must hand back the plain path and stop the helper.
// No game, no headset. Needs an NVIDIA RTX GPU for the DLSS parts.
#include "gpu_test_util.h"

#include "core/gfx/blit.h"
#include "core/gfx/frame_inspector.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/alt_eye.h"
#include "game/bioshock1r/fg_sync.h"
#include "game/bioshock1r/frame_context.h"
#include "game/bioshock1r/temporal.h"
#include "game/bioshock1r/native_repair.h"

#include <MinHook.h>
#include <imgui.h>

#include <string>
#include <vector>

// ---- stubs: everything temporal.cpp reaches outside itself --------------------------------
namespace stub {
const bvr::frame_inspector::DrawTap* tap = nullptr;
bvr::vr::EyeProcessorFn processor = nullptr;
bvr::vr::AerEyeSourceFn eyeSource = nullptr;
bvr::vr::HeldReadyFn heldReady = nullptr;
bvr::vr::HeldRebuildFn heldRebuild = nullptr;
uint32_t outW = 0, outH = 0;
bool alternateEye = false;
bool doubling = true;
uint64_t presents = 1;
bool overlayVisible = false;
bool resizeBusy = false;
uint32_t renderW = 384, renderH = 360, resizeW = 0, resizeH = 0;
int resizeRequests = 0;
std::wstring dataDir;
bool inBuild = false;          // the current CalcView is the render view (inside the scene build)
bool fgSync = false, fgHook = false;
uint32_t fgSubs = 0, fgMisses = 0;
bool buildHook = false;
std::vector<int> srTags;       // vr::sr_push_eye, in order
bool driftResync = false;      // vr::set_sr_drift_resync
} // namespace stub

// The native foreground module has its own complete draw/resource-hook fixture
// (native-repair-tests). Keep it disabled while checking accepted temporal paths.
namespace bvr::b1r::native_repair {
void set_active(bool) {}
void note_camera(uint64_t,int,const tgpu::Camera&,float,const tgpu::Params&,const afw::Params&) {}
void finish_eye(ID3D11Texture2D*,uint64_t,int,float,float,bool) {}
bool copy_output(ID3D11DeviceContext*,ID3D11Texture2D*,uint64_t,int,float,float) {return false;}
}
// The hand motion vectors' grips: no controllers here, so the guides keep the room-fixed
// foreground vectors.
namespace bvr::b1r::hands {
bool active() { return false; }
}
namespace bvr::b1r::handback {
float weight(int, uint64_t) { return 1.0f; }
}

// alt_eye.cpp's MinHook use: the frame-delta hook needs the real exe, which a test does not
// have (the image base is null, so the byte check refuses before MinHook is reached).
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "stub"; }
}
namespace bvr::pattern_scan {
bool is_memory_valid(const void*, size_t) { return false; }
} // namespace bvr::pattern_scan

namespace bvr::log {
void init(const wchar_t*) {}
void write(const char* fmt, ...) {
    char line[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    printf("      | %s\n", line);
}
const wchar_t* data_dir() { return stub::dataDir.c_str(); }
} // namespace bvr::log
namespace bvr::d3d11_hook {
uint64_t present_count() { return stub::presents; }
} // namespace bvr::d3d11_hook
namespace bvr::hud {
bool backbuffer_dims(unsigned* w, unsigned* h) { *w=stub::renderW;*h=stub::renderH;return true; }
}
namespace bvr::overlay {
bool visible() { return stub::overlayVisible; }
} // namespace bvr::overlay
namespace bvr::frame_inspector {
void set_draw_tap(const DrawTap* t) { stub::tap = t; }
ScopedSuppress::ScopedSuppress() {}
ScopedSuppress::~ScopedSuppress() {}
} // namespace bvr::frame_inspector
namespace bvr::vr {
void set_eye_processor(EyeProcessorFn fn) { stub::processor = fn; }
void set_eye_output_size(uint32_t w, uint32_t h) { stub::outW = w; stub::outH = h; }
void eye_output_size(uint32_t* w, uint32_t* h) { *w = stub::outW; *h = stub::outH; }
// The stub headset's eye size: 1.5x the 384x360 render, so SR runs at the Quality ratio the
// real headset gets (a 3x cap of the render scores level with bilinear on this scene).
bool recommended_eye_size(uint32_t* w, uint32_t* h) { *w = 576; *h = 540; return true; }
void set_aer_eye_source(AerEyeSourceFn fn) { stub::eyeSource = fn; }
void set_held_rebuild(HeldReadyFn r, HeldRebuildFn fn) { stub::heldReady = r; stub::heldRebuild = fn; }
void set_alternate_eye(bool on) { stub::alternateEye = on; }
int current_eye_sign() { return 0; }
bool cinematic_active() { return false; }
bool get_hand_pose(int, bool, HeadPose&) { return false; }
void sr_push_eye(int sign) { stub::srTags.push_back(sign); }
void set_sr_drift_resync(bool on) { stub::driftResync = on; }
} // namespace bvr::vr
namespace bvr::b1r::camera {
void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yaw, float* ws) { *pose = {}; *yaw = 0; *ws = 100.0f; }
float eye_half_separation_uu() { return 3.2f; }
float foreground_lens_gain() { return 1.0f; }   // the pretend game draws its hand with the world's lens
void request_game_resolution(uint32_t, uint32_t) {}
} // namespace bvr::b1r::camera
namespace bvr::b1r::resize {   // the live resize (resize.h): the F10 status only, never asked here
void request(uint32_t w, uint32_t h, const char*) {
    ++stub::resizeRequests;stub::resizeW=w;stub::resizeH=h;stub::resizeBusy=true;
}
bool busy() { return stub::resizeBusy; }
const char* status() { return "stub"; }
} // namespace bvr::b1r::resize
namespace bvr::b1r::scenedraw {
bool set_fg_sync(bool on) { if (on) stub::fgHook = true; stub::fgSync = on; return true; }
bool fg_sync_on() { return stub::fgSync; }
void fg_sync_counts(uint32_t* s, uint32_t* m) { *s = stub::fgSubs; *m = stub::fgMisses; }
bool doubling_on() { return stub::doubling; }
void request_stereo_only(bool on) { stub::doubling = on; }
bool in_gameplay_build() { return stub::inBuild; }
bool in_hooked_call() { return stub::inBuild; }
bool build_hook_live() { return stub::buildHook; }
void handle_command(const char* args) { if (strncmp(args, "hook", 4) == 0) stub::buildHook = true; }
} // namespace bvr::b1r::scenedraw
namespace ImGui {
void Text(const char*, ...) {}
bool Button(const char*, const ImVec2&) { return false; }
void SameLine(float, float) {}
} // namespace ImGui

using namespace gt;
namespace tp = bvr::b1r::temporal;
namespace ae = bvr::b1r::alt_eye;

namespace {

const uint32_t W = 384, H = 360;
const double kNear = 10.0, kFar = 65536.0;

// ---- a pretend game: one backbuffer, one D24 scene depth, two eye swapchains --------------
struct Game {
    ID3D11Texture2D* bb = nullptr; ID3D11RenderTargetView* bbRtv = nullptr;
    ID3D11Texture2D* depth = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* swap[2] = {};
    uint32_t swapW = 0, swapH = 0;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11DepthStencilState* dsAlways = nullptr;
    bool init() {
        bb = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        dev->CreateRenderTargetView(bb, nullptr, &bbRtv);
        depth = make_tex(W, H, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(depth, &dd, &dsv);
        const char* src = R"(
Texture2D<float> d : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Depth {
    float z = d.Load(int3(pos.xy, 0));
    if (z >= 1.0f) discard;
    return z;
})";
        ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
        D3DCompile(src, strlen(src), "game", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err); rel(err);
        D3DCompile(src, strlen(src), "game", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err); rel(err);
        dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
        rel(vb); rel(pb);
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dev->CreateDepthStencilState(&ds, &dsAlways);
        return bb && bbRtv && depth && dsv && vs && ps;
    }
    void ensure_swap(uint32_t w, uint32_t h) {
        if (swapW == w && swapH == h) return;
        rel(swap[0]); rel(swap[1]);
        // Typed sRGB like the simulator's images: the copies must stay raw.
        for (auto*& s : swap) s = make_tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        swapW = w; swapH = h;
    }
    void write_depth(const std::vector<float>& raw) {
        ID3D11Texture2D* t = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, raw.data(), W * 4);
        ID3D11ShaderResourceView* s = srv_of(t);
        D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetDepthStencilState(dsAlways, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &s);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        rel(s); rel(t);
    }
    // One eye image the way BS1 draws it: bind, clear, the world pass (many draws), then a
    // depth clear and the foreground (hands) pass on the same surface, then the tonemap
    // with the scene depth still bound.
    // splitWorld: the world pass is interrupted by a pass on another depth surface (a
    // shadow map) and resumed with NO depth clear - more of the world, not a foreground.
    bool splitWorld = false;
    ID3D11Texture2D* shadow = nullptr; ID3D11DepthStencilView* shadowDsv = nullptr;
    void draw_frame(const std::vector<float>& worldRaw, const std::vector<uint32_t>& rgba, const std::vector<float>& handRaw) {
        ctx->OMSetRenderTargets(1, &bbRtv, dsv);
        if (stub::tap) stub::tap->on_setrt(ctx, dsv);
        if (stub::tap) stub::tap->on_clear_dsv(ctx, dsv, D3D11_CLEAR_DEPTH);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        write_depth(worldRaw);
        if (splitWorld) {
            for (int i = 0; i < 60; ++i) if (stub::tap) stub::tap->on_draw_indexed(ctx);
            if (!shadow) {
                shadow = make_tex(256, 256, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
                D3D11_DEPTH_STENCIL_VIEW_DESC sd = {};
                sd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; sd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                dev->CreateDepthStencilView(shadow, &sd, &shadowDsv);
            }
            ctx->OMSetRenderTargets(0, nullptr, shadowDsv);
            if (stub::tap) stub::tap->on_setrt(ctx, shadowDsv);
            if (stub::tap) stub::tap->on_clear_dsv(ctx, shadowDsv, D3D11_CLEAR_DEPTH);
            for (int i = 0; i < 30; ++i) if (stub::tap) stub::tap->on_draw_indexed(ctx);
            ctx->OMSetRenderTargets(1, &bbRtv, dsv);
            if (stub::tap) stub::tap->on_setrt(ctx, dsv);
            for (int i = 0; i < 40; ++i) if (stub::tap) stub::tap->on_draw_indexed(ctx);
        } else {
            for (int i = 0; i < 100; ++i) if (stub::tap) stub::tap->on_draw_indexed(ctx);
        }
        ctx->UpdateSubresource(bb, 0, nullptr, rgba.data(), W * 4, 0);
        if (stub::tap) stub::tap->on_clear_dsv(ctx, dsv, D3D11_CLEAR_DEPTH);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        write_depth(handRaw);
        for (int i = 0; i < 17; ++i) if (stub::tap) stub::tap->on_draw_indexed(ctx);
        ctx->OMSetRenderTargets(1, &bbRtv, dsv);   // the tonemap: an RTV-only rebind
        if (stub::tap) stub::tap->on_setrt(ctx, dsv);
    }
};

double g_detail = 0.2;
Scene world_scene() {
    Scene s;
    s.solids.push_back({{100, -30, -100}, {160, 30, 200}, 1});
    s.solids.push_back({{-60, 40, 0}, {-20, 120, 80}, 2});
    s.solids.push_back({{-10, -140, -40}, {60, -90, 30}, 4});
    s.detail = g_detail;
    return s;
}

double g_pitchDeg = -2.0;
Cam head_at(int frame) {
    Cam c;
    c.loc[0] = -150 + 1.5 * frame; c.loc[1] = 20 + 0.3 * frame; c.loc[2] = 60;
    c.rot = {(int32_t)(g_pitchDeg * bvr::ue::kRotUnitsPerDegree), (int32_t)(0.4 * frame * bvr::ue::kRotUnitsPerDegree), 0};
    c.tanH = std::tan(50.0 * 3.14159265 / 180.0);
    c.tanV = c.tanH * H / W;
    return c;
}
Cam eye_of(const Cam& h, int sign) {
    Cam e = h;
    double f[3], r[3], u[3];
    h.basis(f, r, u);
    for (int i = 0; i < 3; ++i) e.loc[i] += r[i] * sign * 3.2;
    return e;
}

// The player's hand: a box carried by the head (35 uu ahead, low right), axis-aligned.
Box hand_box(const Cam& head) {
    double f[3], r[3], u[3];
    head.basis(f, r, u);
    double c[3];
    for (int i = 0; i < 3; ++i) c[i] = head.loc[i] + f[i] * 35.0 + r[i] * 9.0 - u[i] * 11.0;
    return Box{{c[0] - 4, c[1] - 4, c[2] - 4}, {c[0] + 4, c[1] + 4, c[2] + 4}, 3};
}

// One eye image as BS1 makes it: the colour of everything, the world pass's depth (no
// hands - they are drawn after a depth clear), and the foreground pass's depth (the hands
// alone, cleared elsewhere).
struct EyeImage { Render color, world; std::vector<float> hand; std::vector<uint8_t> handMask; };
EyeImage make_eye(const Cam& head, const Cam& eye, int ss = 1, const Cam* foreground = nullptr) {
    EyeImage e;
    Scene world = world_scene();
    Scene all = world;
    all.solids.push_back(hand_box(head));
    const Cam& handEye = foreground ? *foreground : eye;
    e.color = render(all, handEye, W, H, kNear, kFar, ss);
    e.world = render(world, eye, W, H, kNear, kFar, 1);
    e.hand.assign((size_t)W * H, 1.0f);
    e.handMask.assign((size_t)W * H, 0);
    const Box hb = hand_box(head);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            double d[3];
            pixel_dir(handEye, W, H, x + 0.5, y + 0.5, d);
            double t0 = -1e30, t1 = 1e30;
            bool miss = false;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-12) { if (handEye.loc[a] < hb.lo[a] || handEye.loc[a] > hb.hi[a]) miss = true; continue; }
                double ta = (hb.lo[a] - handEye.loc[a]) / d[a], tb = (hb.hi[a] - handEye.loc[a]) / d[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            }
            if (!miss && t0 <= t1 && t0 > 0 && t0 < e.world.viewZ[(size_t)y * W + x]) {
                e.hand[(size_t)y * W + x] = device_depth(t0, kNear, kFar);
                e.handMask[(size_t)y * W + x] = 1;
            } else {
                e.color.rgba[(size_t)y * W + x] = e.world.rgba[(size_t)y * W + x];
            }
        }
    return e;
}

bvr::vr::EyeFrame frame_for(Game& g, int eye, int sign, bool sequential, const Cam& c) {
    bvr::vr::EyeFrame f;
    f.eye = eye; f.imageSign = sign; f.sequential = sequential;
    f.src = g.bb; f.srcW = W; f.srcH = H;
    const uint32_t ow = stub::outW ? stub::outW : W, oh = stub::outH ? stub::outH : H;
    g.ensure_swap(ow, oh);
    f.dst = g.swap[eye]; f.dstW = ow; f.dstH = oh;
    f.tanH = (float)c.tanH; f.tanV = (float)c.tanV;
    return f;
}

double error_of(const std::vector<float>& got, const Render& truth, uint32_t w, uint32_t h) {
    double s = 0; size_t n = 0;
    for (uint32_t y = h / 16; y < h - h / 16; ++y)
        for (uint32_t x = w / 16; x < w - w / 16; ++x) {
            const size_t i = (size_t)y * w + x;
            for (int c = 0; c < 3; ++c) { s += std::fabs(got[i * 4 + c] - ((truth.rgba[i] >> (8 * c)) & 255) / 255.0); ++n; }
        }
    return s / n;
}

// Images are kept and scored after the run: rendering the supersampled truth takes far
// longer than a frame, and the production freshness windows (250 ms) would otherwise
// see a stalled game and drop the very history being measured.
struct Kept { Cam cam; uint32_t w = 0, h = 0; std::vector<float> got, in; Cam prev; };

double error_vs(ID3D11Texture2D* t, const Render& truth, uint32_t w, uint32_t h) {
    const auto got = readback(t);
    double s = 0; size_t n = 0;
    for (uint32_t y = h / 16; y < h - h / 16; ++y)
        for (uint32_t x = w / 16; x < w - w / 16; ++x) {
            const size_t i = (size_t)y * w + x;
            for (int c = 0; c < 3; ++c) { s += std::fabs(got[i * 4 + c] - ((truth.rgba[i] >> (8 * c)) & 255) / 255.0); ++n; }
        }
    return s / n;
}

void note(const Cam& c, int sign) {
    bvr::b1r::FrameContext tracking;
    tracking.vrDriving=true;tracking.camYaw=c.rot.yaw;
    double f[3],r[3],u[3];c.basis(f,r,u);
    tracking.baseX=(float)(c.loc[0]-r[0]*sign*3.2);
    tracking.baseY=(float)(c.loc[1]-r[1]*sign*3.2);
    tracking.baseZ=(float)(c.loc[2]-r[2]*sign*3.2);
    tp::note_camera(sign, bvr::ue::FVector{(float)c.loc[0], (float)c.loc[1], (float)c.loc[2]}, c.rot, &tracking);
}

// One game frame's CalcView pattern, driven the way camera.cpp drives alt_eye: three
// consumer calls from their own call sites before the scene build, then the render view
// inside it (the only call that notes the camera and commits the frame).
struct FrameViews {
    int sign = 0;          // the render view's eye
    Cam sceneCamera, previousScene;
    Cam render;            // the render view's eye camera (after the pair lock and the offset)
    int callSign[4] = {};
    bool allSame = true;   // every call of the frame carried the frame's eye
    float headSample[4] = {};   // the head sample each call drove with (after AER's latch)
};
char g_callSites[4];
float g_headCounter = 0;   // a fresh head sample on every call, as the pace thread gives
FrameViews calc_views(const Cam& head, bool stereoView = true) {
    FrameViews fv;
    static Cam previous;
    static Cam stashed[2];
    for (int call = 0; call < 4; ++call) {
        stub::inBuild = call == 3;
        if (stub::inBuild) ae::begin_render_build();
        const int s = ae::frame_sign(stereoView);
        const int eyeSign = ae::method() != ae::kBoth ? s : 0;
        bvr::vr::HeadPose hp{};
        hp.qw = ++g_headCounter;
        ae::latch_head(&hp, eyeSign);
        fv.headSample[call] = hp.qw;
        bvr::ue::FVector loc{(float)head.loc[0], (float)head.loc[1], (float)head.loc[2]};
        bvr::ue::FRotator rot = head.rot;
        if (eyeSign) ae::pair_lock(&g_callSites[call], &loc, &rot, eyeSign);
        Cam base = head;
        base.loc[0] = loc.x; base.loc[1] = loc.y; base.loc[2] = loc.z;
        base.rot = rot;
        const Cam c = eyeSign ? eye_of(base, eyeSign) : base;
        fv.callSign[call] = eyeSign;
        if (eyeSign) stashed[eyeSign > 0 ? 1 : 0] = c;
        if (call == 3) {
            // Verified in BS1 (ENGINE_NOTES tenth-run correction): the build's
            // CalcView advances NEXT eye before the player-scene ctor reads CURRENT eye.
            // This node supplies the whole scene, including the world and the hands.
            note(c, eyeSign);
            ae::commit_render_view(eyeSign);
            fv.sceneCamera = c;
            fv.previousScene = previous;
            float l[3] = {(float)c.loc[0], (float)c.loc[1], (float)c.loc[2]};
            int32_t r[3] = {c.rot.pitch, c.rot.yaw, c.rot.roll};
            const int result = bvr::b1r::fg_sync::substitute(stub::fgSync && stub::fgHook, false,
                ae::current_sign(), false, l, r, [&](int eye, float out[3], int32_t rotOut[3]) {
                    for (int i = 0; i < 3; ++i) out[i] = (float)stashed[eye].loc[i];
                    rotOut[0] = stashed[eye].rot.pitch; rotOut[1] = stashed[eye].rot.yaw;
                    rotOut[2] = stashed[eye].rot.roll; return true;
                });
            if (result > 0) ++stub::fgSubs;
            for (int i = 0; i < 3; ++i) fv.sceneCamera.loc[i] = l[i];
            fv.sceneCamera.rot = {r[0], r[1], r[2]};
            fv.sceneCamera.tanH = c.tanH; fv.sceneCamera.tanV = c.tanV;
            previous = c;
            fv.sign = eyeSign;
            fv.render = c;
        }
    }
    stub::inBuild = false;
    for (int call = 1; call < 4; ++call) fv.allSame = fv.allSame && fv.callSign[call] == fv.callSign[0];
    return fv;
}

double dist(const Cam& a, const Cam& b) {
    return std::sqrt((a.loc[0] - b.loc[0]) * (a.loc[0] - b.loc[0]) + (a.loc[1] - b.loc[1]) * (a.loc[1] - b.loc[1]) +
                     (a.loc[2] - b.loc[2]) * (a.loc[2] - b.loc[2]));
}

bool wait_helper(int want, int ms) {
    for (int t = 0; t < ms; t += 50) {
        if (tp::telemetry().helperState == want) return true;
        Sleep(50);
    }
    return tp::telemetry().helperState == want;
}

// One SequentialReentry eye image through the pretend game and core.
struct Step { bvr::vr::EyeFrame f; bool done = false; EyeImage img; };
Step present_eye(Game& g, const Cam& head, int sign, bool sequential) {
    Step s;
    const Cam c = eye_of(head, sign);
    note(c, sign);                                      // CalcView, inside the build
    s.img = make_eye(head, c);
    g.draw_frame(s.img.world.raw, s.img.color.rgba, s.img.hand);   // the render thread draws it
    ++stub::presents;                                    // Present's head
    s.f = frame_for(g, sign < 0 ? 0 : 1, sign, sequential, c);
    s.done = stub::processor(ctx, s.f);
    return s;
}

double rotation_only_error(const Cam& now, const Cam& before, const EyeImage& beforeImg, const Render& truth) {
    std::vector<float> rot((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            double d[3]; pixel_dir(now, W, H, x + 0.5, y + 0.5, d);
            double p[3] = {before.loc[0] + d[0] * 1e6, before.loc[1] + d[1] * 1e6, before.loc[2] + d[2] * 1e6};
            double u = 0, v = 0; project(before, p, &u, &v);
            const int sx = std::min(std::max((int)(u * W), 0), (int)W - 1), sy = std::min(std::max((int)(v * H), 0), (int)H - 1);
            for (int ch = 0; ch < 4; ++ch)
                rot[((size_t)y * W + x) * 4 + ch] = ((beforeImg.color.rgba[(size_t)sy * W + sx] >> (8 * ch)) & 255) / 255.0f;
        }
    return error_of(rot, truth, W, H);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a fault must not swallow the last lines
    _set_invalid_parameter_handler([](const wchar_t* e, const wchar_t* fn, const wchar_t* file, unsigned line, uintptr_t) {
        printf("FATAL: CRT invalid parameter %ls in %ls (%ls:%u)\n", e ? e : L"?", fn ? fn : L"?", file ? file : L"?", line);
    });
    std::set_terminate([] {
        void* frames[32] = {};
        const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
        printf("FATAL: std::terminate; stack (module+offset):\n");
        for (USHORT i = 0; i < n; ++i) {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &m);
            wchar_t name[MAX_PATH] = L"?";
            if (m) GetModuleFileNameW(m, name, MAX_PATH);
            const wchar_t* base = wcsrchr(name, L'\\');
            printf("  %ls+0x%llX\n", base ? base + 1 : name, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m));
        }
        abort();
    });
    printf("temporal-integration: the production glue, driven like the game and core drive it\n");
    if (argc < 2 || !make_device()) return 1;
    // The helper beside the test exe, where the mod looks for it beside its own DLL.
    {
        const std::string repo = argv[1];
        const std::wstring wr(repo.begin(), repo.end());
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe); dir = dir.substr(0, dir.rfind(L'\\'));
        CreateDirectoryW((dir + L"\\bvr_dlss").c_str(), nullptr);
        for (const wchar_t* f : {L"bvr_dlss_host64.exe", L"nvngx_dlss.dll"})
            CopyFileW((wr + L"\\build\\dlss_host\\" + f).c_str(), (dir + L"\\bvr_dlss\\" + f).c_str(), FALSE);
        stub::dataDir = dir + L"\\integration-data";
        CreateDirectoryW(stub::dataDir.c_str(), nullptr);
    }
    Game g;
    if (!g.init()) { printf("game setup failed\n"); return 1; }
    tp::init();

    // 0. Off: nothing is armed, nothing processes.
    check(!stub::tap && !stub::processor, "off: no seam is armed", "tap %p processor %p", (void*)stub::tap, (void*)stub::processor);

    // 1. SequentialReentry + DLAA.
    tp::handle_command("dlss", "dlaa");
    check(stub::tap && stub::processor, "dlss on arms the tap and the processor", "");
    int frame = 0, upAt = -1, attempts = 0, processed = 0;
    bool helperUp = false, guidesChecked = false;
    struct KeptDlaa { Cam head, eye; std::vector<float> got, in; };
    std::vector<KeptDlaa> kept;
    for (; frame < 300 && kept.size() < 8; ++frame) {
        const Cam h = head_at(frame);
        for (int eye = 0; eye < 2; ++eye) {
            const int sign = eye == 0 ? -1 : 1;
            Step s = present_eye(g, h, sign, true);
            ++attempts;
            if (s.done) ++processed;
            if (!s.done || upAt < 0 || frame < upAt + 20 || eye != 0) continue;
            kept.push_back({h, eye_of(h, sign), readback(s.f.dst), readback(g.bb)});
            if (guidesChecked) continue;
            guidesChecked = true;
            // The integrated guides against the truth of this very image: vectors on the
            // world (the hand moves with the head - the mask covers it), depth everywhere
            // (the hand's own depth where it is), and the hands mask.
            const tp::Guides gd = tp::debug_guides();
            const auto mv = readback((ID3D11Texture2D*)gd.motion);
            const auto lz = readback((ID3D11Texture2D*)gd.linearZ);
            const auto hm = readback((ID3D11Texture2D*)gd.hands);
            const Cam prev = eye_of(head_at(frame - 1), sign);
            double mvErr = 0, lzErr = 0; size_t n = 0, nh = 0, maskWrong = 0, nz = 0;
            for (uint32_t y = 8; y < H - 8; ++y)
                for (uint32_t x = 8; x < W - 8; ++x) {
                    const size_t i = (size_t)y * W + x;
                    const bool isHand = s.img.handMask[i] != 0;
                    if ((hm[i] > 0.5f) != isHand) ++maskWrong;
                    const double tz = isHand ? s.img.color.viewZ[i] : s.img.world.viewZ[i];
                    lzErr += std::fabs(lz[i] - tz) / tz;
                    ++nz;
                    if (isHand) { ++nh; continue; }
                    double pu, pv;
                    if (!project(prev, &s.img.world.world[i * 3], &pu, &pv)) continue;
                    const double tu = pu - (x + 0.5) / W, tv = pv - (y + 0.5) / H;
                    mvErr += std::hypot((mv[i * 2] - tu) * W, (mv[i * 2 + 1] - tv) * H);
                    ++n;
                }
            check(n && mvErr / n < 0.1, "integrated vectors match the truth (world pixels)", "mean %.3f px", n ? mvErr / n : -1.0);
            check(nz && lzErr / nz < 1e-3, "integrated depth: the world, and the hand's own depth where it is",
                  "mean relative %.2e", nz ? lzErr / nz : -1.0);
            check(nh > 100 && maskWrong == 0, "the hands mask is exactly the foreground pass", "%zu hand px, %zu wrong", nh, maskWrong);
        }
        if (!helperUp) {
            helperUp = tp::telemetry().helperState == 2;
            if (helperUp) upAt = frame; else Sleep(40);
        }
    }
    double errOut = 0, errIn = 0;
    for (const KeptDlaa& k : kept) {
        Scene all = world_scene();
        all.solids.push_back(hand_box(k.head));
        const Render truth = render(all, k.eye, W, H, kNear, kFar, 3);
        errOut += error_of(k.got, truth, W, H);
        errIn += error_of(k.in, truth, W, H);
    }
    tp::Telemetry t = tp::telemetry();
    check(helperUp, "the helper comes up on its worker", "state %d after %d ticks", t.helperState, frame);
    check(t.lastWorldDraws == 100 && t.lastFgDraws == 17, "the world pass is the depth, the hands pass is the foreground",
          "world %u draws (want 100), foreground %u (want 17)", t.lastWorldDraws, t.lastFgDraws);
    check(t.noCamera == 0, "every SR image found its camera", "%llu misses", (unsigned long long)t.noCamera);
    check(t.dlssFrames > 0 && processed > 0, "DLAA reconstructs the SR eyes", "%llu DLSS frames of %d", (unsigned long long)t.dlssFrames, attempts);
    if (!kept.empty())
        check(errOut < errIn, "DLAA output is closer to supersampled truth than the raw eye", "%.5f vs %.5f",
              errOut / kept.size(), errIn / kept.size());

    // 1b. The world pass split by a shadow-map pass, resumed with no depth clear (the
    //     2026-10-10 headset log showed 6 "foreground" segments after the world - the old
    //     tap called any later segment on the world surface the hands, and the hands mask
    //     would have covered the whole image).
    g.splitWorld = true;
    {
        Step s;
        for (int k = 0; k < 3; ++k, ++frame) s = present_eye(g, head_at(frame), -1, true);
        const tp::Telemetry ts = tp::telemetry();
        const auto hm = readback((ID3D11Texture2D*)tp::debug_guides().hands);
        size_t wrong = 0, nh = 0;
        for (uint32_t y = 8; y < H - 8; ++y)
            for (uint32_t x = 8; x < W - 8; ++x) {
                const size_t i = (size_t)y * W + x;
                const bool isHand = s.img.handMask[i] != 0;
                nh += isHand;
                if ((hm[i] > 0.5f) != isHand) ++wrong;
            }
        check(ts.lastWorldDraws == 100 && ts.lastFgDraws == 17,
              "a world pass split by a shadow pass is still the world; the hands are only after the clear",
              "world %u draws (want 60 + 40), foreground %u (want 17)", ts.lastWorldDraws, ts.lastFgDraws);
        check(nh > 100 && wrong == 0, "split world: the hands mask is exactly the foreground pass", "%zu hand px, %zu wrong",
              nh, wrong);
    }
    g.splitWorld = false;

    // 1c. The hand-depth probe through temporal.cpp: SR pairs of the pretend game, whose hand
    //     depth is true, must read 1.00 for the hands.
    tp::handle_command("dlss", "probe on");
    for (int k = 0; k < 120 && tp::telemetry().probePairs < 4; ++k, ++frame) {
        present_eye(g, head_at(frame), -1, true);
        present_eye(g, head_at(frame), +1, true);
        ctx->Flush();
        Sleep(2);
    }
    {
        const tp::Telemetry tq = tp::telemetry();
        check(tq.probePairs >= 4 && std::fabs(tq.probeHands - 1.0) <= 0.03,
              "the hand-depth probe runs on SR pairs and reads the true hand depth as 1.00",
              "%llu pairs, hands %.2f, world %.2f (0 = too little world disparity at this size)",
              (unsigned long long)tq.probePairs, tq.probeHands, tq.probeWorld);
    }
    tp::handle_command("dlss", "probe off");

    // 2. Super Resolution, Quality (1.5x per axis).
    tp::handle_command("dlss", "sr");
    tp::handle_command("dlss", "quality 1");
    const uint64_t dlssBefore = tp::telemetry().dlssFrames;
    struct KeptSr { Cam head, eye; uint32_t w, h; std::vector<float> got, bil; };
    std::vector<KeptSr> keptSr;
    for (int k = 0; k < 40; ++k, ++frame) {
        const Cam h = head_at(frame);
        for (int eye = 0; eye < 2; ++eye) {
            const int sign = eye == 0 ? -1 : 1;
            Step s = present_eye(g, h, sign, true);
            if (!s.done) bvr::blit::stretch_copy(ctx, s.f.dst, s.f.src);   // what core does then
            if (s.done && k >= 25 && eye == 1 && s.f.dstW > W) {
                // The bilinear reference from a scratch texture (the eye image stays DLSS's).
                ID3D11Texture2D* scratch = make_tex(s.f.dstW, s.f.dstH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
                bvr::blit::stretch_copy(ctx, scratch, s.f.src);
                keptSr.push_back({h, eye_of(h, sign), s.f.dstW, s.f.dstH, readback(s.f.dst), readback(scratch)});
                rel(scratch);
            }
        }
    }
    double errSr = 0, errBil = 0;
    for (const KeptSr& kp : keptSr) {
        Scene all = world_scene();
        all.solids.push_back(hand_box(kp.head));
        Cam co = kp.eye; co.tanV = co.tanH * kp.h / kp.w;
        const Render truth = render(all, co, kp.w, kp.h, kNear, kFar, 3);
        errSr += error_of(kp.got, truth, kp.w, kp.h);
        errBil += error_of(kp.bil, truth, kp.w, kp.h);
    }
    // The output is the HEADSET's size (the stub's recommended 576x540) at the render's
    // aspect - not render x ratio (the round-one rule grew the swapchains past the headset).
    check(stub::outW == 576 && stub::outH == 540, "SR asks core for the headset's size",
          "%ux%u (want 576x540)", stub::outW, stub::outH);
    check(tp::telemetry().dlssFrames > dlssBefore && !keptSr.empty(), "SR reconstructs at the output size",
          "%llu SR frames, %zu scored", (unsigned long long)(tp::telemetry().dlssFrames - dlssBefore), keptSr.size());
    if (!keptSr.empty())
        check(errSr < errBil, "SR beats a bilinear stretch of the same render", "%.5f vs %.5f", errSr / keptSr.size(), errBil / keptSr.size());

    // 2b. TAA runs with F10 visible too; only input capture uses that flag.
    stub::overlayVisible=true;
    // TAA (any GPU): no helper, the eye images anti-aliased in place.
    tp::handle_command("dlss", "taa");
    {
        const uint64_t t0 = tp::telemetry().taaFrames;
        struct KeptTaa { Cam head, eye; std::vector<float> got, in; };
        std::vector<KeptTaa> keptTaa;
        for (int k = 0; k < 30; ++k, ++frame) {
            const Cam h = head_at(frame);
            for (int eye = 0; eye < 2; ++eye) {
                const int sign = eye == 0 ? -1 : 1;
                Step s = present_eye(g, h, sign, true);
                if (!s.done) bvr::blit::stretch_copy(ctx, s.f.dst, s.f.src);
                if (s.done && k >= 22 && eye == 0) keptTaa.push_back({h, eye_of(h, sign), readback(s.f.dst), readback(g.bb)});
            }
        }
        double eT = 0, eR = 0;
        for (const KeptTaa& kp : keptTaa) {
            Scene all = world_scene();
            all.solids.push_back(hand_box(kp.head));
            const Render truth = render(all, kp.eye, W, H, kNear, kFar, 3);
            eT += error_of(kp.got, truth, W, H);
            eR += error_of(kp.in, truth, W, H);
        }
        check(tp::telemetry().taaFrames > t0 + 40 && stub::outW == 0, "TAA continues with F10 visible at the original eye size",
              "%llu TAA frames", (unsigned long long)(tp::telemetry().taaFrames - t0));
        if (!keptTaa.empty())
            check(eT < eR, "TAA output is closer to supersampled truth than the raw eye", "%.5f vs %.5f",
                  eT / keptTaa.size(), eR / keptTaa.size());
        check(wait_helper(0, 6000), "TAA needs no helper: it is stopped", "state %d", tp::telemetry().helperState);
    }

    stub::overlayVisible=false;

    // 3. The alternate frame warp (DLSS off): one eye per present, the other rebuilt. The
    //    detail stripes are off here: a rebuild is a resampling, and 2-3 px stripes would
    //    score its sub-pixel interpolation rather than the geometry.
    g_detail = 0.0;
    tp::handle_command("dlss", "off");
    stub::processor(ctx, frame_for(g, 0, -1, true, head_at(frame)));   // the processor resets the SR output
    check(stub::outW == 0 && stub::outH == 0, "leaving SR returns the eye swapchains to the backbuffer size", "%ux%u", stub::outW, stub::outH);
    stub::doubling = true;
    ae::handle_command("afw", "on");
    tp::handle_command("afw", "fggain 1.0");   // both the fixture and production use unit gain
    calc_views(head_at(frame++));   // the method applies at the next CalcView outside the build
    if (stub::eyeSource) stub::eyeSource();   // ...and that frame presents like any other
    check(ae::method() == ae::kAfw && stub::alternateEye && !stub::doubling && stub::eyeSource && stub::heldRebuild &&
              stub::buildHook,
          "afw on: AlternateEye on, SequentialReentry off, build hook in, eye source and held rebuild armed",
          "method %d aer %d doubling %d hook %d", ae::method(), (int)stub::alternateEye, (int)stub::doubling,
          (int)stub::buildHook);
    struct KeptAfw { Cam head, held, heldBefore; EyeImage before; std::vector<float> got; };
    std::vector<KeptAfw> keptAfw;
    EyeImage lastImg[2];
    Cam lastCam[2];
    bool haveLast[2] = {};
    double fgError = 0, staleFgError = 0;
    int crossed = 0, mixedFrames = 0, notAlternating = 0, lastSign = 0;
    const uint64_t rebuildsBefore = tp::telemetry().afwRebuilds;
    for (int k = 0; k < 40; ++k, ++frame) {
        const Cam h = head_at(frame);
        const FrameViews fv = calc_views(h);                     // four CalcViews, one render view
        const int sign = fv.sign;
        if (!fv.allSame) ++mixedFrames;
        if (sign == 0 || sign == lastSign) ++notAlternating;
        lastSign = sign;
        const Cam c = fv.render;
        fgError = std::max(fgError, dist(fv.sceneCamera, c));
        staleFgError = std::max(staleFgError, dist(fv.previousScene, c));
        EyeImage img = make_eye(h, fv.sceneCamera);
        g.draw_frame(img.world.raw, img.color.rgba, img.hand);
        ++stub::presents;
        const int named = stub::eyeSource();                     // core asks which eye this is
        if (named != sign) ++crossed;
        const int eye = named < 0 ? 0 : 1;
        bvr::vr::EyeFrame f = frame_for(g, eye, named, false, c);
        if (!stub::processor(ctx, f)) ctx->CopyResource(f.dst, f.src);
        const int held = eye ^ 1;
        if (stub::heldReady(held)) {
            bvr::vr::HeldFrame hf;
            hf.held = held; hf.fresh = eye; hf.dst = g.swap[held]; hf.w = f.dstW; hf.h = f.dstH;
            hf.tanH = f.tanH; hf.tanV = f.tanV;
            const int how = stub::heldRebuild(ctx, hf);
            if (how == 1 && k >= 4 && haveLast[held])
                keptAfw.push_back({h, eye_of(h, held == 0 ? -1 : 1), lastCam[held], lastImg[held], readback(g.swap[held])});
        }
        lastImg[eye] = std::move(img);
        lastCam[eye] = c;
        haveLast[eye] = true;
    }
    double eWarp = 0, eRot = 0, eHand = 0;
    size_t nHand = 0;
    for (const KeptAfw& kp : keptAfw) {
        Scene all = world_scene();
        all.solids.push_back(hand_box(kp.head));
        const Render truth = render(all, kp.held, W, H, kNear, kFar, 1);
        eWarp += error_of(kp.got, truth, W, H);
        eRot += rotation_only_error(kp.held, kp.heldBefore, kp.before, truth);
        // The hand region of the truth: the rebuild must carry the hand that moved with the head.
        const EyeImage now = make_eye(kp.head, kp.held);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            if (!now.handMask[i]) continue;
            double e = 0;
            for (int ch = 0; ch < 3; ++ch) e += std::fabs(kp.got[i * 4 + ch] - ((truth.rgba[i] >> (8 * ch)) & 255) / 255.0);
            eHand += e / 3;
            ++nHand;
        }
    }
    t = tp::telemetry();
    check(stub::fgSync && stub::fgHook && stub::fgSubs >= 40 && fgError < 0.001 && staleFgError > 5.0,
          "AFW: player-scene ctor after CalcView uses the tagged eye, not NEXT eye",
          "sync %d hook %d subs %u, error %.5f uu vs stale %.3f uu", (int)stub::fgSync,
          (int)stub::fgHook, stub::fgSubs, fgError, staleFgError);
    {
        float l[3] = {1,2,3}; int32_t r[3] = {4,5,6}; int lookups = 0;
        auto missing = [&](int, float*, int32_t*) { ++lookups; return false; };
        const int disabled = bvr::b1r::fg_sync::substitute(false, false, -1, false, l, r, missing);
        const int absent = bvr::b1r::fg_sync::substitute(true, false, 1, false, l, r, missing);
        check(disabled == 0 && absent == -1 && lookups == 1 && l[0] == 1 && r[2] == 6,
              "foreground sync off or missing camera preserves the engine's arguments",
              "results %d/%d lookups %d loc %.1f rot %d", disabled, absent, lookups, l[0], r[2]);
    }
    {
        const int committed = ae::current_sign();
        const int next = ae::frame_sign(true);
        ae::begin_render_build(); // a scripted build can omit CalcView completely
        const int before = ae::current_sign();
        float l[3] = {11,22,33}; int32_t r[3] = {4,5,6}; int reads = 0;
        const int untouched = bvr::b1r::fg_sync::substitute(true, false, before, false, l, r,
            [&](int, float*, int32_t*) { ++reads; return true; });
        check(committed != 0 && next == -committed && before == 0 && untouched == 0 && reads == 0 && l[1] == 22,
              "scene camera latch differs from next eye and clears before a build without CalcView",
              "committed %d next %d new-build %d lookups %d", committed, next, before, reads);
    }
    check(mixedFrames == 0, "AFW: every CalcView call of a frame carries that frame's eye (the headset flicker)",
          "%d of 40 frames had calls with different eyes", mixedFrames);
    check(notAlternating == 0, "AFW: one eye per frame, strictly L R L R", "%d frames broke the alternation", notAlternating);
    check(crossed == 0, "every AlternateEye image is named with the eye it was rendered for", "%d crossed", crossed);
    check(t.afwRebuilds > rebuildsBefore && t.afwWithHeld > 0, "AFW rebuilds the held eye from both eyes",
          "%llu rebuilds, %llu with the held eye's own image, %llu fallbacks", (unsigned long long)(t.afwRebuilds - rebuildsBefore),
          (unsigned long long)t.afwWithHeld, (unsigned long long)t.afwFallbacks);
    if (!keptAfw.empty()) {
        check(eWarp < 0.5 * eRot, "the rebuilt held eye matches the truth far better than the rotated old image",
              "%.5f vs %.5f over %zu rebuilds", eWarp / keptAfw.size(), eRot / keptAfw.size(), keptAfw.size());
        check(nHand && eHand / nHand < 0.05, "the hand in the rebuilt eye is where the hand is now", "mean %.4f over %zu px",
              nHand ? eHand / nHand : -1.0, nHand);
    }
    // F10 is outside the eye images: sources and rebuilds continue unchanged.
    {
        auto one = [&](bool panel, int* how, bool* ready, bool* own, bool* mono) {
            stub::overlayVisible = panel;
            const Cam h = head_at(frame++);
            const FrameViews fv = calc_views(h);
            EyeImage img = make_eye(h, fv.render);
            g.draw_frame(img.world.raw, img.color.rgba, img.hand);
            ++stub::presents;
            const int named = stub::eyeSource();
            const int eye = named < 0 ? 0 : 1;
            bvr::vr::EyeFrame f = frame_for(g, eye, named, false, fv.render);
            if (!stub::processor(ctx, f)) ctx->CopyResource(f.dst, f.src);
            const auto before = readback(g.swap[eye ^ 1]);   // what the held eye showed last
            *ready = stub::heldReady(eye ^ 1);
            *how = 0;
            if (*ready) {
                bvr::vr::HeldFrame hf;
                hf.held = eye ^ 1; hf.fresh = eye; hf.dst = g.swap[eye ^ 1]; hf.w = f.dstW; hf.h = f.dstH;
                hf.tanH = f.tanH; hf.tanV = f.tanV;
                *how = stub::heldRebuild(ctx, hf);
            }
            const auto a = readback(g.swap[eye]), b = readback(g.swap[eye ^ 1]);
            *own = b == before;
            *mono = a == b;
        };
        int how = 0, panelWrites = 0, panelOwn = 0, panelMono = 0;
        bool ready = false, own = false, mono = false;
        const uint64_t r0 = tp::telemetry().afwRebuilds;
        for (int k = 0; k < 4; ++k) {
            one(true, &how, &ready, &own, &mono);
            if (ready && how == 1) ++panelWrites;
            if (own) ++panelOwn;
            if (mono) ++panelMono;
        }
        check(panelWrites == 4 && panelMono == 0 && tp::telemetry().afwRebuilds == r0+4,
              "F10 up: every present still rebuilds the other eye",
              "%d of 4 rebuilt, %d unchanged images, %d equal to fresh, %llu rebuilds",
              panelWrites, panelOwn, panelMono, (unsigned long long)(tp::telemetry().afwRebuilds - r0));
        const uint64_t h0 = tp::telemetry().afwWithHeld;
        one(false, &how, &ready, &own, &mono);
        check(ready && how == 1 && tp::telemetry().afwWithHeld == h0+1,
              "closing F10 keeps the held-eye history valid", "how %d, with held +%llu",
              how, (unsigned long long)(tp::telemetry().afwWithHeld - h0));
        stub::overlayVisible = false;
    }

    check(tp::telemetry().afwForegroundMotion>0,
          "AFW carries each build's tracking frame into the foreground rebuild",
          "%llu transported rebuilds",(unsigned long long)tp::telemetry().afwForegroundMotion);

    // 4. AFW + DLAA together, including every frame while F10 is visible.
    stub::overlayVisible=true;
    tp::handle_command("dlss", "dlaa");
    const uint64_t d0 = tp::telemetry().dlssFrames, r0 = tp::telemetry().afwRebuilds;
    for (int k = 0; k < 30; ++k, ++frame) {
        const Cam h = head_at(frame);
        const FrameViews fv = calc_views(h);
        const Cam c = fv.render;
        EyeImage img = make_eye(h, c);
        g.draw_frame(img.world.raw, img.color.rgba, img.hand);
        ++stub::presents;
        const int named = stub::eyeSource();
        const int eye = named < 0 ? 0 : 1;
        bvr::vr::EyeFrame f = frame_for(g, eye, named, false, c);
        if (!stub::processor(ctx, f)) ctx->CopyResource(f.dst, f.src);
        if (stub::heldReady(eye ^ 1)) {
            bvr::vr::HeldFrame hf;
            hf.held = eye ^ 1; hf.fresh = eye; hf.dst = g.swap[eye ^ 1]; hf.w = f.dstW; hf.h = f.dstH; hf.tanH = f.tanH; hf.tanV = f.tanV;
            stub::heldRebuild(ctx, hf);
        }
    }
    t = tp::telemetry();
    check(t.dlssFrames > d0 && t.afwRebuilds == r0+30, "F10 up: DLAA and all 30 AFW rebuilds continue", "%llu DLSS, %llu rebuilds",
          (unsigned long long)(t.dlssFrames - d0), (unsigned long long)(t.afwRebuilds - r0));

    stub::overlayVisible=false;

    // The frame-delta clamp is AER's alone: under AFW every frame advances the world.
    static char objA, objB, objC;   // the engine's two delta receivers (identities only)
    const float kDt = 0.007f;
    {
        const float a = ae::clamp_delta(&objA, kDt), b = ae::clamp_delta(&objB, kDt);
        check(a == kDt && b == kDt, "AFW: the world advances every frame (no clamp)", "%.4f %.4f", a, b);
    }

    // 4b. Alternate eye (AER, BRVR's method), DLAA still on: one eye per frame, tagged into
    //     core's pair path, the right frame rendered from the left frame's cameras.
    ae::handle_command("aer", "on");
    stub::srTags.clear();
    calc_views(head_at(frame++));   // applies the switch (this frame is the pair's LEFT)
    check(ae::method() == ae::kAer && !stub::alternateEye && !stub::eyeSource && !stub::heldRebuild && !stub::doubling &&
              stub::driftResync,
          "aer on (from AFW): AFW's seams and core's AlternateEye off, SequentialReentry stays off, drift resync on",
          "method %d resync %d", ae::method(), (int)stub::driftResync);
    {
        stub::srTags.clear();
        int mixed = 0, lockBad = 0, leftOwn = 0, tagsBad = 0, monoTagged = 0, pairsAfterMono = 0, headBad = 0;
        int sceneBad = 0, clampBad = 0, prevCommitted = -1;   // the switch frame was a LEFT
        bool lastHeld = false;                  // the last frame's delta is still banked
        double advanced[2] = {};
        float pairHead = 0;
        double lockErr = 0;
        Cam leftHead = head_at(frame - 1);   // the switch frame was the first pair's LEFT
        int expect = +1;
        const uint64_t noCam0 = tp::telemetry().noCamera, dlss0 = tp::telemetry().dlssFrames;
        const ae::Telemetry a0 = ae::telemetry();
        // 26 frames with a non-stereo frame (a scripted view) in the middle.
        for (int k = 0; k < 26; ++k, ++frame) {
            const Cam h = head_at(frame);
            const bool stereoView = k != 13;
            // The engine advances the world before it builds the frame: held after a LEFT,
            // paid out (own delta + the bank) on the frame after that.
            const float wa = ae::clamp_delta(&objA, kDt), wb = ae::clamp_delta(&objB, kDt);
            advanced[0] += wa;
            advanced[1] += wb;
            const float want = prevCommitted == -1 ? 0.0f : prevCommitted == +1 ? 2 * kDt : kDt;
            if (std::fabs(wa - want) > 1e-6f || std::fabs(wb - want) > 1e-6f) ++clampBad;
            lastHeld = want == 0.0f;
            const size_t tags0 = stub::srTags.size();
            const FrameViews fv = calc_views(h, stereoView);
            if (dist(fv.sceneCamera, fv.render) > 1e-3) ++sceneBad;
            prevCommitted = fv.sign;
            if (!fv.allSame) ++mixed;
            const bool tagged = stub::srTags.size() == tags0 + 1;
            if (!stereoView) {
                if (stub::srTags.size() != tags0 || fv.sign != 0) ++monoTagged;
                expect = -1;   // the next pair must open on the LEFT
                continue;
            }
            if (!tagged || stub::srTags.back() != fv.sign || (expect && fv.sign != expect)) ++tagsBad;
            if (k == 14 && fv.sign == -1) ++pairsAfterMono;
            expect = -fv.sign;
            // One head sample per pair: the left frame's first call's, held by every call of both frames.
            if (fv.sign < 0) pairHead = fv.headSample[0];
            if (pairHead != 0)   // from the first LEFT in this loop on
                for (float s : fv.headSample)
                    if (s != pairHead) ++headBad;
            if (fv.sign < 0) {
                leftHead = h;
                if (dist(fv.render, eye_of(h, -1)) < 1e-3) ++leftOwn;
            } else {
                // The right frame: the LEFT frame's camera, moved across by the separation.
                const double e = dist(fv.render, eye_of(leftHead, +1));
                lockErr = std::max(lockErr, e);
                if (e > 1e-3 || fv.render.rot.yaw != leftHead.rot.yaw) ++lockBad;
            }
            // The image through core's SR pair path (sequential), DLAA on.
            EyeImage img = make_eye(fv.sign < 0 ? h : leftHead, fv.render);
            g.draw_frame(img.world.raw, img.color.rgba, img.hand);
            ++stub::presents;
            bvr::vr::EyeFrame f = frame_for(g, fv.sign < 0 ? 0 : 1, fv.sign, true, fv.render);
            if (!stub::processor(ctx, f)) ctx->CopyResource(f.dst, f.src);
        }
        const ae::Telemetry a1 = ae::telemetry();
        check(mixed == 0, "AER: every CalcView call of a frame carries that frame's eye", "%d mixed frames", mixed);
        check(sceneBad == 0, "AER: constructor camera matches the committed eye after the sequence advances",
              "%d mismatched scene cameras", sceneBad);
        check(tagsBad == 0, "AER: exactly one eye tag per frame, L R L R, into core's pair path", "%d bad, %zu tags",
              tagsBad, stub::srTags.size());
        check(monoTagged == 0 && pairsAfterMono == 1, "AER: a non-stereo frame is untagged and the next pair opens LEFT",
              "mono tagged %d, left after mono %d", monoTagged, pairsAfterMono);
        check(leftOwn >= 10, "AER: the left frame renders from its own camera", "%d left frames", leftOwn);
        check(headBad == 0, "AER: one head sample per pair, held by all eight calls (BRVR's latch)",
              "%d calls drove with another sample", headBad);
        check(lockBad == 0, "AER: the right frame renders from the left frame's camera (pair lock)",
              "%d right frames off, worst %.4f uu", lockBad, lockErr);
        check(a1.lockMisses == a0.lockMisses && a1.locked - a0.locked >= 4 * 10,
              "AER: the lock covers all four call sites of every right frame", "%llu locked, %llu misses",
              (unsigned long long)(a1.locked - a0.locked), (unsigned long long)(a1.lockMisses - a0.lockMisses));
        check(tp::telemetry().noCamera == noCam0 && tp::telemetry().dlssFrames > dlss0,
              "AER + DLAA: every pair image finds its camera and is reconstructed", "%llu misses, %llu DLSS",
              (unsigned long long)(tp::telemetry().noCamera - noCam0),
              (unsigned long long)(tp::telemetry().dlssFrames - dlss0));
        check(!a1.clampHooked, "AER without the game exe: the world clamp refuses (byte check) and AER still runs",
              "hooked %d", (int)a1.clampHooked);
        check(clampBad == 0, "AER clamp: 0 on every right frame, both receivers; the next frame pays the bank",
              "%d frames wrong", clampBad);
        // Time is conserved: 26 frames of 7 ms, less at most one banked delta.
        const double pending = lastHeld ? kDt : 0.0;
        check(std::fabs(advanced[0] + pending - 26 * kDt) < 1e-4 && std::fabs(advanced[1] + pending - 26 * kDt) < 1e-4,
              "AER clamp: no game time is lost (each receiver keeps its own bank)", "%.4f / %.4f s of %.4f",
              advanced[0] + pending, advanced[1] + pending, 26 * kDt);
        // A load replaces both receivers: the banks are dropped, not paid into the new world.
        for (int k = 0; k < 2 && prevCommitted != -1; ++k) prevCommitted = calc_views(head_at(frame++)).sign;
        // A right frame comes next: both receivers bank their delta...
        ae::clamp_delta(&objA, kDt);
        ae::clamp_delta(&objB, kDt);
        // ...and a load swaps in a new receiver mid-frame (banks reset; it banks its own).
        const float cFrozen = ae::clamp_delta(&objC, kDt);
        calc_views(head_at(frame++));   // the right frame
        const float cPaid = ae::clamp_delta(&objC, kDt);    // the new world: its own bank paid
        const float aAfter = ae::clamp_delta(&objA, kDt);   // the old receiver: its bank was dropped
        check(cFrozen == 0.0f && std::fabs(cPaid - 2 * kDt) < 1e-6f && std::fabs(aAfter - kDt) < 1e-6f,
              "AER clamp: a third receiver (a load) drops the old banks instead of paying them out",
              "new receiver %.4f then %.4f, old receiver %.4f (want 0, %.4f, %.4f)", cFrozen, cPaid, aAfter, 2 * kDt, kDt);
    }

    // 5. Everything off: the plain path, SR doubling back, the helper stopped.
    ae::handle_command("aer", "off");
    calc_views(head_at(frame++));   // applies the switch
    tp::handle_command("dlss", "off");
    check(ae::method() == ae::kBoth && !stub::alternateEye && stub::doubling && !stub::heldRebuild && !stub::eyeSource &&
              !stub::driftResync,
          "back to both eyes: SequentialReentry restored, every alternate seam disarmed", "");
    {
        Step s = present_eye(g, head_at(frame), -1, true);
        check(!s.done, "off: the processor hands every image back to the plain copy", "");
    }
    const bool stopped = wait_helper(0, 6000);
    check(stopped, "off: the helper is stopped", "state %d", tp::telemetry().helperState);
    // The production game-thread resize scheduler (no window/game in this fixture).
    stub::renderW=576;stub::renderH=540;
    tp::handle_command("dlss","quality 1");tp::handle_command("dlss","sr");
    tp::tick_resolution();
    check(stub::resizeRequests==1 && stub::resizeW==384 && stub::resizeH==360,
          "SR mode requests output / ratio live", "%d requests, %ux%u", stub::resizeRequests,stub::resizeW,stub::resizeH);
    tp::handle_command("dlss","dlaa");tp::handle_command("dlss","output 960x900");
    tp::handle_command("dlss","quality 3");tp::handle_command("dlss","sr");tp::tick_resolution();
    check(stub::resizeRequests==1,"mode/output changes wait while a resize is in flight","%d requests",stub::resizeRequests);
    stub::resizeBusy=false;stub::renderW=384;stub::renderH=360;tp::tick_resolution();
    check(stub::resizeRequests==2 && stub::resizeW==480 && stub::resizeH==450,
          "after confirmation the latest mode/output wins", "%d requests, %ux%u",stub::resizeRequests,stub::resizeW,stub::resizeH);
    stub::resizeBusy=false;tp::tick_resolution();
    check(stub::resizeRequests==2,"a refusal does not cause an unbounded resize retry","%d requests",stub::resizeRequests);
    tp::handle_command("dlss","off");tp::tick_resolution();
    check(stub::resizeW==960 && stub::resizeH==900,"turning SR off restores the output as the render","%ux%u",stub::resizeW,stub::resizeH);
    using S=bvr::b1r::menu::Setting;
    stub::resizeBusy=false;
    tp::menu_write(S::OutputWidth,2400);tp::menu_write(S::OutputHeight,2280);
    tp::menu_write(S::Upscaler,2);
    const uint32_t expected[5][2]={{1846,1754},{1600,1520},{1394,1326},{1200,1140},{800,760}};
    for(int q=0;q<5;++q) {
        stub::resizeBusy=false;tp::menu_write(S::DlssQuality,(float)q);tp::tick_resolution();
        check(stub::resizeW==expected[q][0] && stub::resizeH==expected[q][1],
              "each saved quality value requests its documented ratio", "q%d %ux%u, expected %ux%u",q,
              stub::resizeW,stub::resizeH,expected[q][0],expected[q][1]);
    }
    stub::resizeBusy=false;tp::menu_write(S::OutputWidth,2750);tp::menu_write(S::OutputHeight,2850);tp::tick_resolution();
    check(stub::resizeW==918 && stub::resizeH==950,"Ultra Performance preserves the exact 2750x2850 output within the 3x limit","%ux%u",stub::resizeW,stub::resizeH);
    tp::menu_write(S::OutputWidth,2400);tp::menu_write(S::OutputHeight,2280);
    stub::resizeBusy=false;tp::menu_write(S::Upscaler,1);tp::tick_resolution();
    check(stub::resizeW==2400 && stub::resizeH==2280,"DLAA returns to full output resolution","%ux%u",stub::resizeW,stub::resizeH);
    tp::handle_command("dlss","quality -1");
    check(tp::menu_read(S::DlssQuality)==4,"invalid quality command preserves the previous choice","quality %.0f",tp::menu_read(S::DlssQuality));
    tp::handle_command("dlss","off");
    return finish("temporal-integration");
}
