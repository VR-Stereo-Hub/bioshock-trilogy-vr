#include "overlay.h"

#include "core/framework/framework.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/igame_adapter.h"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace bvr::overlay {
namespace {

bool g_initialized = false;
bool g_visible = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
        return TRUE;
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
}

bool CreateRenderTarget(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
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
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    BVR_LOG("overlay initialized (hwnd=%p) - F10 toggles it", g_window);
    return true;
}

void DrawUi() {
    ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("BioShock VR " BVR_VERSION);
    ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    ImGui::Separator();
    vr::draw_debug_ui();
    if (auto* adapter = game::adapter()) {
        ImGui::Separator();
        adapter->drawDebugUi();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Log: %%LOCALAPPDATA%%\\BioshockVR\\bioshockvr.log");
    ImGui::End();
}

} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_initialized) {
        g_initialized = Init(swapchain);
        if (!g_initialized) return;
    }
    if (!g_rtv && !CreateRenderTarget(swapchain)) return;

    // F10, edge-triggered (Insert was the original choice, but not every
    // keyboard has it)
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (isDown && !wasDown) {
        g_visible = !g_visible;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }
    wasDown = isDown;

    if (!g_visible) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
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
}

} // namespace bvr::overlay
