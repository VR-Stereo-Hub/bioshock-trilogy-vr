// gpu-test-sources: src/core/vr/openxr_runtime.cpp src/core/gfx/blit.cpp
// gpu-test-defines: BVR_WITH_OPENXR
// gpu-test-libs: build/third_party/OpenXR-SDK/src/loader/RelWithDebInfo/openxr_loader.lib build/third_party/RelWithDebInfo/imgui.lib advapi32.lib shell32.lib ole32.lib windowscodecs.lib
// gpu-test-needs: xrsim
// The core OpenXR runtime's temporal seams against a REAL OpenXR runtime - the simulated
// Quest 3 (bvr_xrsim32), with no game anywhere. The production openxr_runtime.cpp brings
// up a session on this test's own D3D11 swapchain and runs its real frame loop; the HUD,
// input and overlay modules it talks to are stubbed. The oracle is the simulator's own
// per-eye compositor capture - what a headset would show:
//   - SequentialReentry eye images go through the eye processor, eye by eye;
//   - an output size rebuilds the eye swapchains, the processor sees it, and a declined
//     image is stretched rather than lost;
//   - a size the runtime refuses falls back to the backbuffer size, with the session alive;
//   - AlternateEye with an adapter eye source and a held-eye rebuild: every present
//     carries the fresh eye from the processor and the other eye from the rebuild, and
//     the HUD gate opens for those frames.
#include "gpu_test_util.h"

#include "core/gfx/hud_capture.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <openxr/openxr.h>
#include "core/vr/openxr_input.h"

#include <wincodec.h>

#include <string>

// ---- stubs for what the runtime reaches outside itself -----------------------------------
namespace stubs {
bool hudGate = false;
std::wstring dataDir;
}
namespace bvr::log {
void write(const char* fmt, ...) {
    char line[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    char verbose[8] = "";
    GetEnvironmentVariableA("BVR_XR_VERBOSE", verbose, sizeof(verbose));
    if (verbose[0] || strstr(line, "fail") || strstr(line, "FAIL") || strstr(line, "eye output") ||
        strstr(line, "swapchain pair") || strstr(line, "requested output"))
        printf("      | %s\n", line);
}
const wchar_t* data_dir() { return stubs::dataDir.c_str(); }
} // namespace bvr::log
namespace bvr::overlay {
bool visible() { return false; }
} // namespace bvr::overlay
namespace bvr::hud {
bool bar_draw_active() { return false; }
bool bars_hidden() { return false; }
bool cine_subs_in_frame() { return false; }
bool effects_in_frame() { return false; }
bool enabled() { return false; }
bool fov_mismatch() { return false; }
bool fov_watch(float*, float*, unsigned long long*, unsigned long long) { return false; }
bool letterbox(unsigned*, unsigned*) { return false; }
bool postfx_cine_size() { return false; }
bool postfx_rt_only() { return false; }
bool redirected_this_interval() { return false; }
bool restore_rt() { return false; }
bool screen_only() { return false; }
ID3D11ShaderResourceView* srv(ID3D11DeviceContext*) { return nullptr; }
ID3D11Texture2D* texture(ID3D11DeviceContext*) { return nullptr; }
unsigned bar_verts() { return 0; }
unsigned effect_max_verts() { return 0; }
void get_bar_stats(unsigned* a, unsigned* b, unsigned* c) { *a = *b = *c = 0; }
void get_dump_on_edge(int* a, int* b) { *a = *b = 0; }
void get_route_stats(RouteStats* out) { *out = RouteStats{}; }
void set_bar_verts(unsigned) {}
void set_bars_hidden(bool) {}
void set_cine_subs_in_frame(bool) {}
void set_dump_on_edge(int, int) {}
void set_effect_max_verts(unsigned) {}
void set_effects_in_frame(bool) {}
void set_enabled(bool) {}
void set_gate(bool on) { stubs::hudGate = on; }
void set_postfx_cine_size(bool) {}
void set_postfx_rt_only(bool) {}
void set_restore_rt(bool) {}
} // namespace bvr::hud
namespace bvr::vr {
void input_create(XrInstance) {}
void input_on_session_created(XrSession, XrSpace) {}
void input_on_session_teardown() {}
void input_sync(XrSession, XrTime) {}
void input_draw_debug_ui() {}
bool input_get_hand_pose(int, bool, float*, float*) { return false; }
void input_set_sim_hand(int, bool, bool, const float[3], const float[4]) {}
void input_clear_sim_hands() {}
} // namespace bvr::vr

using namespace gt;
namespace vr = bvr::vr;

namespace {

const UINT BW = 640, BH = 640;
HWND g_wnd = nullptr;
IDXGISwapChain* g_sc = nullptr;
ID3D11RenderTargetView* g_bbRtv = nullptr;
std::wstring g_simDir;

// What the seams were handed.
struct Seen {
    int calls = 0, seqCalls = 0, aerCalls = 0;
    int lastEye = -1, lastSign = 0;
    uint32_t srcW = 0, srcH = 0, dstW = 0, dstH = 0;
    float tanH = 0, tanV = 0;
    int eyeCount[2] = {};
    bool returnTrue = true;
    int heldCalls = 0, heldWrong = 0;
};
Seen g_seen;
int g_nextSign = -1;

// Solid colours per role, written into whatever image a seam is given.
const uint32_t kProcL = 0xFF0000FFu, kProcR = 0xFF00FF00u;    // red, green (RGBA8 little endian: R in the low byte)
const uint32_t kHeldL = 0xFFFF0000u, kHeldR = 0xFF00FFFFu;    // blue, yellow
ID3D11Texture2D* solid_like(ID3D11Texture2D* dst, uint32_t rgba) {
    D3D11_TEXTURE2D_DESC d = {};
    dst->GetDesc(&d);
    std::vector<uint32_t> px((size_t)d.Width * d.Height, rgba);
    return make_tex(d.Width, d.Height, DXGI_FORMAT_R8G8B8A8_UNORM, 0, px.data(), d.Width * 4);
}

bool processor(ID3D11DeviceContext* c, const vr::EyeFrame& f) {
    ++g_seen.calls;
    if (f.sequential) ++g_seen.seqCalls; else ++g_seen.aerCalls;
    g_seen.lastEye = f.eye; g_seen.lastSign = f.imageSign;
    g_seen.srcW = f.srcW; g_seen.srcH = f.srcH; g_seen.dstW = f.dstW; g_seen.dstH = f.dstH;
    g_seen.tanH = f.tanH; g_seen.tanV = f.tanV;
    ++g_seen.eyeCount[f.eye & 1];
    if (!g_seen.returnTrue) return false;
    ID3D11Texture2D* s = solid_like(f.dst, f.eye == 0 ? kProcL : kProcR);
    c->CopyResource(f.dst, s);
    s->Release();
    return true;
}
int eye_source() {
    const int s = g_nextSign;
    g_nextSign = -g_nextSign;
    return s;
}
bool held_ready(int) { return true; }
int held_rebuild(ID3D11DeviceContext* c, const vr::HeldFrame& h) {
    ++g_seen.heldCalls;
    if (h.held != (h.fresh ^ 1)) ++g_seen.heldWrong;
    ID3D11Texture2D* s = solid_like(h.dst, h.held == 0 ? kHeldL : kHeldR);
    c->CopyResource(h.dst, s);
    s->Release();
    return 1;
}

// One present the way the hooked game's would run: the runtime's head, the game's image,
// the runtime's tail.
void present(float grey = 0.5f) {
    vr::publish_gameplay_view(true);   // what the adapter's CalcView publishes in gameplay
    vr::on_present_begin(g_sc);
    const float col[4] = {grey, grey, grey, 1};
    ctx->ClearRenderTargetView(g_bbRtv, col);
    vr::on_present_end(g_sc);
    g_sc->Present(0, 0);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// ---- the simulator's control files ---------------------------------------------------------
std::string read_file(const std::wstring& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return "";
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
long json_int(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t at = s.find(k);
    if (at == std::string::npos) return -1;
    const size_t colon = s.find(':', at);
    return colon == std::string::npos ? -1 : strtol(s.c_str() + colon + 1, nullptr, 10);
}
// Asks the simulator for a capture of the next submitted frame, keeps presenting until it
// lands, and returns the centre pixel of each eye (RGB, 0..255) from its PNGs.
bool read_png_center(const std::wstring& path, uint8_t rgb[3]) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
              SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    if (ok) {
        UINT w = 0, h = 0; cv->GetSize(&w, &h);
        WICRect r = {(INT)w / 2, (INT)h / 2, 1, 1};
        ok = SUCCEEDED(cv->CopyPixels(&r, 3, 3, rgb));
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (d) d->Release(); f->Release();
    return ok;
}
// `pairs`: keep presenting SequentialReentry pairs (tagged L then R) while the capture is
// pending, as the game would; otherwise single presents (AlternateEye / mono).
bool g_shotPairs = false;
float g_shotGrey = 0.5f;
float g_shotGreyR = -1.0f;   // >= 0: the RIGHT image of each pair is this grey (the left g_shotGrey)
bool shot(const char* tag, uint8_t left[3], uint8_t right[3]) {
    const long before = json_int(read_file(g_simDir + L"\\state.json"), "captureSeq");
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (g_simDir + L"\\command.txt").c_str(), L"wb");
        if (!f) return false;
        fprintf(f, "shot %s\n", tag);
        fclose(f);
    }
    for (int i = 0; i < 400; ++i) {
        if (g_shotPairs) {
            vr::sr_push_eye(-1); present(g_shotGrey);
            vr::sr_push_eye(+1); present(g_shotGreyR >= 0.0f ? g_shotGreyR : g_shotGrey);
        }
        else present(g_shotGrey);
        if (i % 4 == 0 && json_int(read_file(g_simDir + L"\\state.json"), "captureSeq") > before) break;
        Sleep(5);
    }
    const std::wstring wt(tag, tag + strlen(tag));
    return read_png_center(g_simDir + L"\\capture\\" + wt + L"_left.png", left) &&
           read_png_center(g_simDir + L"\\capture\\" + wt + L"_right.png", right);
}
bool is(const uint8_t c[3], uint32_t rgba) {
    const int r = rgba & 255, g = (rgba >> 8) & 255, b = (rgba >> 16) & 255;
    return std::abs(c[0] - r) < 40 && std::abs(c[1] - g) < 40 && std::abs(c[2] - b) < 40;
}
const char* name_of(const uint8_t c[3]) {
    static char s[4][32];   // several per printf
    static int k = 0;
    k = (k + 1) & 3;
    _snprintf_s(s[k], _TRUNCATE, "(%u,%u,%u)", c[0], c[1], c[2]);
    return s[k];
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
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
        ExitProcess(3);
    });
    printf("xr-seams: the core temporal seams on the simulated OpenXR runtime\n");
    (void)argc; (void)argv;
    wchar_t dir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"BVR_XRSIM_DIR", dir, MAX_PATH);
    g_simDir = dir;
    if (g_simDir.empty()) { printf("BVR_XRSIM_DIR not set - run through tools\\gpu-test.ps1\n"); return 1; }
    stubs::dataDir = g_simDir;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // A small window and a swapchain on our own device: the "game".
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"bvrxrseams";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"xr-seams", WS_OVERLAPPEDWINDOW, 0, 0, BW, BH, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = BW; sd.BufferDesc.Height = BH; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_wnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                             D3D11_SDK_VERSION, &sd, &g_sc, &dev, nullptr, &ctx))) {
        printf("no device/swapchain\n"); return 1;
    }
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev->CreateRenderTargetView(bb, nullptr, &g_bbRtv);
    bb->Release();

    vr::init_instance();
    vr::set_enabled(true);
    vr::set_camera_mode(true);
    vr::set_rendered_hfov(100.0f);
    bool live = false;
    for (int i = 0; i < 2000 && !live; ++i) {
        present();
        live = vr::vr_camera_mode() && strcmp(vr::session_state_name(), "FOCUSED") == 0;
        Sleep(2);
    }
    check(live, "the simulated session comes up FOCUSED with camera mode", "state %s", vr::session_state_name());
    if (!live) return finish("xr-seams");
    for (int i = 0; i < 30; ++i) present();   // views located, projection ready

    // 1. SequentialReentry pairs through the eye processor.
    vr::set_eye_processor(&processor);
    for (int i = 0; i < 40; ++i) { vr::sr_push_eye(-1); present(); vr::sr_push_eye(+1); present(); }
    check(g_seen.seqCalls >= 60 && g_seen.eyeCount[0] > 25 && g_seen.eyeCount[1] > 25,
          "SR eye images reach the processor, both eyes", "%d calls (L %d, R %d)", g_seen.seqCalls, g_seen.eyeCount[0], g_seen.eyeCount[1]);
    check(g_seen.srcW == BW && g_seen.dstW == BW && g_seen.dstH == BH, "processor sizes: backbuffer = eye image",
          "src %ux%u dst %ux%u", g_seen.srcW, g_seen.srcH, g_seen.dstW, g_seen.dstH);
    check(std::fabs(g_seen.tanH - std::tan(50.0f * 3.14159265f / 180.0f)) < 1e-3f && std::fabs(g_seen.tanV - g_seen.tanH) < 1e-3f,
          "processor tangents = the claimed FOV", "tanH %.4f tanV %.4f", g_seen.tanH, g_seen.tanV);
    g_shotPairs = true;
    {
        uint8_t l[3], r[3];
        const bool ok = shot("sr", l, r);
        check(ok && is(l, kProcL) && is(r, kProcR), "the compositor shows each eye's processed image",
              "left %s right %s", ok ? name_of(l) : "?", ok ? name_of(r) : "?");
    }

    // 2. Super-resolution output size: the eye swapchains rebuild larger; the processor sees
    //    it; an image it declines is stretched (the game's grey), not lost.
    vr::set_eye_output_size(960, 960);
    for (int i = 0; i < 60; ++i) { vr::sr_push_eye(-1); present(); vr::sr_push_eye(+1); present(); }
    uint32_t ow = 0, oh = 0;
    vr::eye_output_size(&ow, &oh);
    check(ow == 960 && oh == 960 && g_seen.dstW == 960 && g_seen.srcW == BW, "an output size rebuilds the eye swapchains",
          "live %ux%u, processor dst %ux%u src %ux%u", ow, oh, g_seen.dstW, g_seen.dstH, g_seen.srcW, g_seen.srcH);
    {
        uint8_t l[3], r[3];
        const bool ok = shot("srout", l, r);
        check(ok && is(l, kProcL) && is(r, kProcR), "the larger eyes show the processor's images", "left %s right %s",
              ok ? name_of(l) : "?", ok ? name_of(r) : "?");
        g_seen.returnTrue = false;
        for (int i = 0; i < 6; ++i) { vr::sr_push_eye(-1); present(0.5f); vr::sr_push_eye(+1); present(0.5f); }
        const bool ok2 = shot("srstretch", l, r);
        const uint8_t grey[3] = {188, 188, 188};   // 0.5 written raw into an sRGB image decodes brighter; accept a band
        (void)grey;
        const bool greyish = ok2 && std::abs(l[0] - l[1]) < 8 && std::abs(l[1] - l[2]) < 8 && l[0] > 60 && l[0] < 230;
        check(greyish, "a declined image at the larger size is the game's image, stretched", "left %s", ok2 ? name_of(l) : "?");
        g_seen.returnTrue = true;
    }
    // A size the runtime cannot make: the request is dropped and the session lives on.
    vr::set_eye_output_size(20000, 20000);
    for (int i = 0; i < 60; ++i) { vr::sr_push_eye(-1); present(); vr::sr_push_eye(+1); present(); }
    vr::eye_output_size(&ow, &oh);
    check(ow == BW && oh == BH && strcmp(vr::session_state_name(), "FOCUSED") == 0,
          "a refused output size falls back to the backbuffer size, session alive", "live %ux%u, %s", ow, oh, vr::session_state_name());
    vr::set_eye_output_size(0, 0);
    for (int i = 0; i < 20; ++i) { vr::sr_push_eye(-1); present(); vr::sr_push_eye(+1); present(); }

    // 3. AlternateEye with the adapter naming each image's eye and rebuilding the held eye.
    g_shotPairs = false;
    g_seen = Seen{};
    vr::set_aer_eye_source(&eye_source);
    vr::set_held_rebuild(&held_ready, &held_rebuild);
    vr::set_alternate_eye(true);
    stubs::hudGate = false;
    for (int i = 0; i < 80; ++i) present();
    check(g_seen.aerCalls > 60 && g_seen.eyeCount[0] > 25 && g_seen.eyeCount[1] > 25,
          "AlternateEye images reach the processor with the named eye", "%d calls (L %d, R %d)", g_seen.aerCalls,
          g_seen.eyeCount[0], g_seen.eyeCount[1]);
    check(g_seen.heldCalls > 60 && g_seen.heldWrong == 0, "every present rebuilds the OTHER eye", "%d rebuilds, %d wrong eye",
          g_seen.heldCalls, g_seen.heldWrong);
    check(stubs::hudGate, "the HUD gate opens for adapter-named AlternateEye frames", "");
    {
        uint8_t l[3], r[3];
        const bool ok = shot("afw", l, r);
        const bool pairA = ok && is(l, kProcL) && is(r, kHeldR);   // left fresh, right rebuilt
        const bool pairB = ok && is(l, kHeldL) && is(r, kProcR);   // right fresh, left rebuilt
        check(pairA || pairB, "each frame shows one fresh eye and one rebuilt eye", "left %s right %s",
              ok ? name_of(l) : "?", ok ? name_of(r) : "?");
    }
    // Control: without the eye source and rebuild, AlternateEye shows the processor's
    // images only (no rebuilt colour can appear).
    vr::set_held_rebuild(nullptr, nullptr);
    vr::set_aer_eye_source(nullptr);
    for (int i = 0; i < 30; ++i) present();
    {
        uint8_t l[3], r[3];
        const bool ok = shot("aer", l, r);
        check(ok && !is(l, kHeldL) && !is(r, kHeldR), "control: no rebuild, no rebuilt colour", "left %s right %s",
              ok ? name_of(l) : "?", ok ? name_of(r) : "?");
    }
    vr::set_alternate_eye(false);
    vr::set_eye_processor(nullptr);
    for (int i = 0; i < 10; ++i) present();

    // 4. Alternate eye (AER, BRVR's method) rides the SR pair path: a left image, then a
    //    right image, one XR frame per pair. Dark left, bright right - each eye must show
    //    its own. Then a left tag whose present never came: with BRVR's drift resync off the
    //    eyes stay swapped (the control), with it they are back within 8 pops.
    {
        auto pairs = [](int n) {
            for (int i = 0; i < n; ++i) { vr::sr_push_eye(-1); present(0.15f); vr::sr_push_eye(+1); present(0.85f); }
        };
        g_shotPairs = true;
        g_shotGrey = 0.15f;
        g_shotGreyR = 0.85f;
        uint8_t l[3], r[3];
        pairs(20);
        bool ok = shot("aerpairs", l, r);
        check(ok && l[0] + 60 < r[0], "AER pairs: each eye shows its own image (dark left, bright right)",
              "left %s right %s", ok ? name_of(l) : "?", ok ? name_of(r) : "?");
        vr::sr_push_eye(-1);   // the stale tag
        pairs(12);
        ok = shot("aerdrift", l, r);
        check(ok && r[0] + 60 < l[0], "control: one stale tag swaps the eyes and they stay swapped (resync off)",
              "left %s right %s", ok ? name_of(l) : "?", ok ? name_of(r) : "?");
        vr::set_sr_drift_resync(true);
        pairs(12);
        ok = shot("aerresync", l, r);
        check(ok && l[0] + 60 < r[0], "drift resync on: the eyes come back by themselves", "left %s right %s",
              ok ? name_of(l) : "?", ok ? name_of(r) : "?");
        vr::set_sr_drift_resync(false);
        g_shotPairs = false;
        g_shotGrey = 0.5f;
        g_shotGreyR = -1.0f;
    }
    // The runtime's pace and trace threads are still running, as they are in the game, and the
    // simulator DLL keeps joinable std::thread globals (xrsim_control.cpp, xrsim_compositor.cpp)
    // that std::terminate in its DLL_PROCESS_DETACH whenever a process exits with its instance
    // alive. Neither is this suite's subject: end without running any detach code.
    const int rc = finish("xr-seams");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), (UINT)rc);
    return rc;
}
