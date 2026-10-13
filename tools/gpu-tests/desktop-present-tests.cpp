// gpu-test-sources: src/core/gfx/desktop_present.cpp
// Production desktop policy: error contracts, current-frame proof and a hidden
// real D3D11 swapchain. No game, OpenXR session, headset or simulator is launched.
#include "core/gfx/desktop_present.h"
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <vector>
namespace dp = bvr::desktop_present;
unsigned checks = 0;
void check(bool ok, const char* what) {
    ++checks; if (!ok) { std::printf("FAIL: %s\n", what); std::exit(1); }
}
struct Call { IDXGISwapChain* chain; UINT sync, flags; };
thread_local std::vector<Call> calls;
thread_local std::vector<HRESULT> replies;
HRESULT WINAPI original(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if (calls.size() >= replies.size()) { std::puts("FAIL: unexpected desktop retry"); std::abort(); }
    const HRESULT hr = replies[calls.size()]; calls.push_back({chain,sync,flags}); return hr;
}
IDXGISwapChain* const dummy = reinterpret_cast<IDXGISwapChain*>(uintptr_t{0x1234});
void arm(std::initializer_list<HRESULT> result) { calls.clear(); replies = result; }
dp::Result attempt(bool xr, UINT sync = 0, UINT flags = 0) {
    dp::Frame frame; dp::mark_xr_submission(xr); return dp::present(original,dummy,sync,flags);
}
HRESULT WINAPI real_present(IDXGISwapChain* sc, UINT sync, UINT flags) { return sc->Present(sync,flags); }
void smoke() {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BvrDesktopPolicyTest"; RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    check(hwnd != nullptr, "hidden test window created");
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferDesc.Width = desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 1;
    desc.OutputWindow = hwnd; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* device = nullptr; ID3D11DeviceContext* ctx = nullptr;
    const HRESULT init = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&desc,&sc,&device,nullptr,&ctx);
    check(SUCCEEDED(init) && sc && device && ctx, "hardware D3D11 discard swapchain created");
    ID3D11Texture2D* texture = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    check(SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&texture))), "real backbuffer acquired");
    check(SUCCEEDED(device->CreateRenderTargetView(texture,nullptr,&rtv)), "real render target created");
    dp::configure(true);
    for (unsigned i = 0; i < 8; ++i) {
        const float clear[4] = {0.01f * i,0,0,1}; ctx->ClearRenderTargetView(rtv,clear);
        dp::Frame frame; dp::mark_xr_submission(true);
        const auto r = dp::present(real_present,sc,0,0);
        check(r.attempted && r.flags == DXGI_PRESENT_DO_NOT_WAIT, "real swapchain uses guarded nonblocking flag");
        check(!r.rejected && (SUCCEEDED(r.first) || r.first == DXGI_ERROR_WAS_STILL_DRAWING),
              "installed driver accepts nonblocking desktop flag");
        check(r.value == (r.skipped ? S_OK : r.first), "real API status/error semantics preserved");
    }
    ctx->ClearState(); rtv->Release(); texture->Release(); sc->Release(); ctx->Release(); device->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::puts("Hidden D3D11 hardware smoke: 8 presentations accepted; no game/headset performance claim.");
}
int main() {
    arm({DXGI_ERROR_WAS_STILL_DRAWING}); auto r = attempt(true);
    check(!r.attempted && !r.skipped && r.value == DXGI_ERROR_WAS_STILL_DRAWING,
          "default off preserves the original busy result");
    check(calls.size() == 1 && calls[0].chain == dummy && calls[0].sync == 0 && calls[0].flags == 0,
          "default off forwards original arguments exactly once");
    dp::configure(true);
    arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = attempt(true);
    check(r.attempted && r.skipped && !r.rejected && r.value == S_OK && r.first == DXGI_ERROR_WAS_STILL_DRAWING,
          "current XR frame allows a busy desktop update to be skipped");
    check(calls.size() == 1 && calls[0].flags == DXGI_PRESENT_DO_NOT_WAIT,
          "busy desktop is never retried or waited for");
    arm({S_OK}); r = attempt(true); check(r.attempted && !r.skipped && r.value == S_OK, "successful desktop update preserved");
    arm({DXGI_STATUS_OCCLUDED}); r = attempt(true);
    check(r.value == DXGI_STATUS_OCCLUDED && !r.skipped, "occlusion status is not hidden from the application");
    arm({DXGI_ERROR_DEVICE_REMOVED}); r = attempt(true);
    check(r.value == DXGI_ERROR_DEVICE_REMOVED && !r.skipped && !r.rejected && calls.size() == 1,
          "real device removal propagates without a retry");
    arm({DXGI_ERROR_DEVICE_RESET}); r = attempt(true);
    check(r.value == DXGI_ERROR_DEVICE_RESET && calls.size() == 1, "device reset propagates");
    arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = attempt(false);
    check(!r.attempted && r.value == DXGI_ERROR_WAS_STILL_DRAWING && calls[0].flags == 0,
          "empty/failed/quad/no-XR frame cannot borrow earlier submission proof");
    arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = attempt(true,1,0);
    check(!r.attempted && calls[0].sync == 1 && calls[0].flags == 0 && r.value == DXGI_ERROR_WAS_STILL_DRAWING,
          "application VSync request is preserved");
    for (UINT flag : {DXGI_PRESENT_TEST,DXGI_PRESENT_DO_NOT_WAIT,DXGI_PRESENT_DO_NOT_SEQUENCE,DXGI_PRESENT_ALLOW_TEARING}) {
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = attempt(true,0,flag);
        check(!r.attempted && !r.skipped && calls[0].flags == flag && r.value == DXGI_ERROR_WAS_STILL_DRAWING,
              "existing presentation flags keep their original contract");
    }
    arm({DXGI_ERROR_WAS_STILL_DRAWING}); dp::mark_xr_submission(true); r = dp::present(original,dummy,0,0);
    check(!r.attempted && r.value == DXGI_ERROR_WAS_STILL_DRAWING, "submission outside a root Present is ignored");
    {
        dp::Frame outer; dp::mark_xr_submission(true);
        { dp::Frame inner; dp::mark_xr_submission(false);
          arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = dp::present(original,dummy,0,0);
          check(!r.attempted && r.value == DXGI_ERROR_WAS_STILL_DRAWING, "nested desktop call cannot consume parent proof"); }
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = dp::present(original,dummy,0,0);
        check(r.skipped, "root proof survives an unrelated nested call");
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = dp::present(original,dummy,0,0);
        check(!r.attempted, "submission proof authorizes only one desktop attempt");
    }
    {
        dp::Frame outer;
        { dp::Frame inner; dp::mark_xr_submission(true); }
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = dp::present(original,dummy,0,0);
        check(!r.attempted, "nested submission cannot authorize its parent");
    }
    {
        dp::Frame outer; dp::mark_xr_submission(true); bool separate = false;
        std::thread other([&] {
            arm({DXGI_ERROR_WAS_STILL_DRAWING}); auto x = attempt(false);
            separate = !x.attempted && x.value == DXGI_ERROR_WAS_STILL_DRAWING;
        }); other.join(); check(separate, "another render thread cannot borrow current-frame proof");
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = dp::present(original,dummy,0,0);
        check(r.skipped, "other-thread Present does not consume root proof");
    }
    for (HRESULT unsupported : {DXGI_ERROR_INVALID_CALL,DXGI_ERROR_UNSUPPORTED,E_INVALIDARG}) {
        dp::configure(true); arm({unsupported,S_OK}); r = attempt(true);
        check(r.attempted && r.rejected && !r.skipped && r.first == unsupported && r.value == S_OK,
              "unsupported flag restores original presentation");
        check(calls.size() == 2 && calls[0].flags == DXGI_PRESENT_DO_NOT_WAIT && calls[1].flags == 0 && calls[1].sync == 0,
              "unsupported flag gets exactly one fallback with original arguments");
        arm({DXGI_ERROR_WAS_STILL_DRAWING}); r = attempt(true);
        check(!r.attempted && r.value == DXGI_ERROR_WAS_STILL_DRAWING && calls.size() == 1,
              "unsupported driver cannot repeat failing attempts each frame");
    }
    dp::configure(true); arm({DXGI_ERROR_INVALID_CALL,DXGI_ERROR_DEVICE_REMOVED}); r = attempt(true);
    check(r.rejected && r.value == DXGI_ERROR_DEVICE_REMOVED, "fallback failure is preserved");
    dp::configure(false); arm({S_OK}); r = attempt(true);
    check(!r.attempted && calls[0].flags == 0, "explicit opt-out restores original call");
    smoke();
    std::printf("desktop present: %u checks passed\n",checks);
}
