#include "d3d11_hook.h"

#include "core/ui/overlay.h"
#include "core/util/log.h"

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

void LogSwapchainInfo(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    BVR_LOG("first Present: backbuffer %ux%u format %u, windowed=%d, hwnd=%p",
            desc.BufferDesc.Width, desc.BufferDesc.Height,
            desc.BufferDesc.Format, desc.Windowed, desc.OutputWindow);

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
    if (!g_loggedFirstPresent.exchange(true)) {
        LogSwapchainInfo(swapchain); // DR-2: confirms the D3D11 path is live
    }
    overlay::on_present(swapchain);
    return g_origPresent(swapchain, syncInterval, flags);
}

HRESULT WINAPI ResizeBuffersDetour(IDXGISwapChain* swapchain, UINT bufferCount,
                                   UINT width, UINT height, DXGI_FORMAT format,
                                   UINT swapchainFlags) {
    overlay::on_resize();
    HRESULT hr = g_origResizeBuffers(swapchain, bufferCount, width, height, format, swapchainFlags);
    BVR_LOG("ResizeBuffers: %ux%u format %u -> hr=0x%08X", width, height, format, hr);
    return hr;
}

// kiero technique: make a throwaway device + swapchain purely to read the
// shared vtable, then hook its Present/ResizeBuffers slots.
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

} // namespace bvr::d3d11_hook
