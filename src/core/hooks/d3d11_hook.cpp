#include "d3d11_hook.h"

#include "core/framework/command.h"
#include "core/hooks/d3d11_debug.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/frame_pacing.h"
#include "core/gfx/desktop_present.h"
#include "core/gfx/shader_bytecode.h"
#include "core/gfx/gfx_hud.h"
#include "core/gfx/hud_capture.h"
#include "core/ui/overlay.h"
#include "core/util/crash.h"
#include "core/util/diag.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <atomic>

namespace bvr::d3d11_hook {
namespace {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
std::atomic<bool> g_loggedFirstPresent{false};
std::atomic<bool> g_loggedNonblockingPresent{false};
std::atomic<uint64_t> g_presentCount{0};
std::atomic<uint32_t> g_lastPresentTid{0};

std::atomic<void*> g_gameWindow{nullptr};

void LogSwapchainInfo(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g_gameWindow.store(desc.OutputWindow, std::memory_order_relaxed);
    BVR_LOG("first Present: backbuffer %ux%u format %u, windowed=%d, hwnd=%p, effect=%u buffers=%u flags=0x%X",
            desc.BufferDesc.Width, desc.BufferDesc.Height,
            desc.BufferDesc.Format, desc.Windowed, desc.OutputWindow,
            desc.SwapEffect, desc.BufferCount, desc.Flags);

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        BVR_LOG("device feature level: 0x%X", device->GetFeatureLevel());

        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc{};
                adapter->GetDesc(&adapterDesc);
                BVR_LOG("adapter: %ls (%zu MB dedicated)",
                        adapterDesc.Description,
                        adapterDesc.DedicatedVideoMemory / (1024 * 1024));
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        device->Release();
    }
}

HRESULT WINAPI PresentDetour(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    const uint64_t presents = g_presentCount.fetch_add(1, std::memory_order_relaxed);
    frame_pacing::Present timing(presents + 1, syncInterval, flags);
    desktop_present::Frame desktopFrame;
    g_lastPresentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // Cheap re-arm of our unhandled-exception filter (~every 10 s at 60 fps).
    // The game, the Steam overlay and the 2K SDK all install theirs after our
    // DLL attach, and last writer wins - session 23: a tester's crash produced
    // no dump AND no log line, the exact signature of a displaced filter.
    if ((presents % 600) == 0) crash::rearm();
    if (!g_loggedFirstPresent.exchange(true)) {
        LogSwapchainInfo(swapchain); // DR-2: confirms the D3D11 path is live
        // Frame inspector hooks onto the GAME's immediate context (fail-soft;
        // one attempt). Its vtable is the one real draws dispatch through -
        // the throwaway device's context vtable is neither shared nor
        // outlives its device (see FindVTable note).
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device))) && device) {
            shader_bytecode::bind_device(device); // opt-in, exact device after first Present
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                if (diag::skip("inspector"))
                    BVR_LOG("BVR_SKIP: frame inspector NOT installed");
                else
                    frame_inspector::install(*reinterpret_cast<void***>(context), context);
                context->Release();
            }
            device->Release();
        }
    }
    // Command seam, for adapters with no engine hook to poll from (session 35).
    // No-op unless an adapter armed the Present pump, so BS1 and BS2 - which
    // poll from their own camera hooks - see one atomic load and nothing else.
    // Ahead of the frame boundary so a `dumpframe` arms for the NEXT frame
    // rather than half of this one.
    command::poll_from_present(GetTickCount64());
    // Frame boundary first: finalize any armed dump, then suppress our own
    // overlay/VR draws for the rest of the detour so they never enter a dump.
    // Session 34: stamp every segment of this detour. The BS2 stereo hang wedges
    // the present thread OUTSIDE the VR module's own spans, so the pace trace
    // could see only "-" and said nothing at all. These names are what turn a
    // silent freeze into a named one.
    vr::set_present_stage("inspector");
    { frame_pacing::Scope span(frame_pacing::Inspector); frame_inspector::on_present(swapchain); }
    frame_inspector::ScopedSuppress suppress;
    // Letterbox sampling BEFORE any of our additions touch the backbuffer
    // (overlay draw, mirror re-blit, window HUD composite) - the frame here
    // is pure game pixels (session 22 round 3).
    vr::set_present_stage("letterbox");
    {
        frame_pacing::Scope span(frame_pacing::Letterbox);
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device))) && device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                if (!diag::skip("letterbox")) hud::letterbox_sample(context, swapchain);
                context->Release();
            }
            device->Release();
        }
    }
    vr::set_present_stage("vrBegin");
    vr::on_present_begin(swapchain); // xrWaitFrame paces the game while a session runs
    vr::set_present_stage("overlay");
    { frame_pacing::Scope span(frame_pacing::Overlay);
      if (!diag::skip("overlay")) overlay::on_present(swapchain); }
    vr::set_present_stage("vrEnd");
    vr::on_present_end(swapchain);   // processes/captures eyes, then submits separate UI layers
    { frame_pacing::Scope span(frame_pacing::Mirror);
      if (!diag::skip("overlay")) overlay::draw_mirror(); } // desktop only, after every eye consumer
    vr::set_present_stage("hudPresent");
    // HUD capture rolls LAST: every consumer of the redirected HUD RT (eye
    // capture, window composite, quad copy - all inside on_present_end) has
    // run by now; this clears the RT and resets the interval classifier.
    {
        frame_pacing::Scope span(frame_pacing::Hud);
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device))) && device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                hud::on_present(context, swapchain);
                gfx_hud::on_present(context, swapchain); // s52: the GFx HUD lane
                context->Release();
            }
            device->Release();
        }
    }
    // The debug layer's messages for everything above (ours and the game's), when armed.
    { frame_pacing::Scope span(frame_pacing::Debug); d3d11_debug::on_present(swapchain); }
    vr::set_present_stage("gamePresent"); // the game's own Present - not our code
    desktop_present::Result desktop;
    { frame_pacing::Scope span(frame_pacing::DesktopPresent);
      desktop = desktop_present::present(g_origPresent, swapchain, syncInterval, flags); }
    frame_pacing::desktop(desktop.flags, desktop.first, desktop.value,
                          desktop.attempted, desktop.skipped, desktop.rejected);
    if (desktop.attempted && !g_loggedNonblockingPresent.exchange(true))
        BVR_LOG("[pacing] nonblocking desktop Present active after a focused XR projection submission");
    if (desktop.rejected)
        BVR_LOG("[pacing] desktop DO_NOT_WAIT rejected (0x%08X); original Present restored (0x%08X), opt-in disabled",
                desktop.first, desktop.value);
    vr::set_present_stage(nullptr);
    return desktop.value;
}

HRESULT WINAPI ResizeBuffersDetour(IDXGISwapChain* swapchain, UINT bufferCount,
                                   UINT width, UINT height, DXGI_FORMAT format,
                                   UINT swapchainFlags) {
    // An opt-in replay owner may retain views of the real backbuffer. Release
    // them before DXGI requires every outstanding backbuffer reference gone.
    ID3D11Device* resizeDevice = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&resizeDevice))) && resizeDevice) {
        ID3D11DeviceContext* resizeContext = nullptr;
        resizeDevice->GetImmediateContext(&resizeContext);
        if (resizeContext) {
            frame_inspector::on_resource_reset(resizeContext);
            resizeContext->Release();
        }
        resizeDevice->Release();
    }
    vr::on_resize(width, height, static_cast<unsigned>(format));
    overlay::on_resize();
    hud::release_resources(); // recreated lazily at the new size
    HRESULT hr = g_origResizeBuffers(swapchain, bufferCount, width, height, format, swapchainFlags);
    BVR_LOG("ResizeBuffers: %ux%u format %u -> hr=0x%08X", width, height, format, hr);
    return hr;
}

// kiero technique: make a throwaway device + swapchain purely to read the
// shared vtable, then hook its Present/ResizeBuffers slots.
// NOTE: the frame inspector does NOT take the throwaway context's vtable -
// context vtables are not shared across devices the way the swapchain's is
// (releasing the dummy freed the vtable we captured -> install-time AV at
// bioshockvr.dll+0x30BE5, and its Draw slots never matched the game's
// context, which is why dumps had SetRT events but zero draws). The game's
// REAL immediate context is grabbed at first Present instead.

bool FindVTable(void** outPresent, void** outResizeBuffers) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BvrDummyWindow";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                                  0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context);

    bool ok = false;
    if (SUCCEEDED(hr) && swapchain) {
        void** vtable = *reinterpret_cast<void***>(swapchain);
        *outPresent = vtable[8];         // IDXGISwapChain::Present
        *outResizeBuffers = vtable[13];  // IDXGISwapChain::ResizeBuffers
        shader_bytecode::install(device); // no hooks unless an adapter requested metadata
        ok = true;
    } else {
        BVR_LOG("dummy swapchain creation failed: hr=0x%08X", hr);
    }

    if (context) context->Release();
    if (device) device->Release();
    if (swapchain) swapchain->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool install() {
    void* present = nullptr;
    void* resizeBuffers = nullptr;
    if (!FindVTable(&present, &resizeBuffers)) return false;

    if (MH_CreateHook(present, reinterpret_cast<void*>(&PresentDetour),
                      reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
        MH_CreateHook(resizeBuffers, reinterpret_cast<void*>(&ResizeBuffersDetour),
                      reinterpret_cast<void**>(&g_origResizeBuffers)) != MH_OK) {
        BVR_LOG("MH_CreateHook failed for swapchain hooks");
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        BVR_LOG("MH_EnableHook failed");
        return false;
    }
    BVR_LOG("D3D11 swapchain hooks installed (Present @ %p, ResizeBuffers @ %p)",
            present, resizeBuffers);
    return true;
}

uint32_t last_present_tid() {
    return g_lastPresentTid.load(std::memory_order_relaxed);
}

void* game_window() { return g_gameWindow.load(std::memory_order_relaxed); }

uint64_t present_count() {
    return g_presentCount.load(std::memory_order_relaxed);
}

} // namespace bvr::d3d11_hook
