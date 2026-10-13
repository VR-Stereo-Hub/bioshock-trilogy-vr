// gpu-test-sources: src/game/bioshock1r/afw_gpu.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_demo.cpp third_party/imgui/backends/imgui_impl_dx11.cpp
// The 2026-10-10 crash pattern, offline: in all three headset runs the alternate frame warp
// crashed the NVIDIA driver (nvwgf2um.dll+0x48153A, a driver worker thread) within 1.7-10 s of
// the held-eye rebuild running AFTER the F10 panel had been shown - and never in 45 s of
// rebuilding from boot before the panel was ever opened. The panel is ImGui's DX11 backend
// drawing on the game's context (core/ui/overlay.cpp: OMSetRenderTargets(backbuffer, no DSV)
// then ImGui_ImplDX11_RenderDrawData). This suite does the same on one context: rebuilds at
// the tester's 2750x2850, then the panel drawn for 2 s with the rebuild paused (as AFW pauses
// it while the panel is up), then rebuilds again - three times. afw-soak (no ImGui) is the
// control. Passes if the driver survives.
#include "gpu_test_util.h"

#include "game/bioshock1r/afw_gpu.h"

#include <imgui.h>
#include "backends/imgui_impl_dx11.h"

#include <cstdlib>

using namespace gt;
namespace af = bvr::b1r::afw;

namespace {

bvr::b1r::tgpu::Camera cam_at(double t, int eye, uint32_t w, uint32_t h) {
    bvr::b1r::tgpu::Camera c;
    const double yaw = 0.6 * std::sin(t * 1.3), pitch = 0.15 * std::sin(t * 0.7);
    const float f[3] = {(float)(std::cos(yaw) * std::cos(pitch)), (float)(std::sin(yaw) * std::cos(pitch)), (float)std::sin(pitch)};
    const float r[3] = {(float)-std::sin(yaw), (float)std::cos(yaw), 0.0f};
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    for (int i = 0; i < 3; ++i) { c.fwd[i] = f[i]; c.right[i] = r[i]; c.up[i] = -u[i]; }
    c.loc[0] = (float)(40.0 * t); c.loc[1] = (float)(10.0 * std::sin(t)); c.loc[2] = 60.0f;
    for (int i = 0; i < 3; ++i) c.loc[i] += c.right[i] * (eye == 0 ? -3.2f : 3.2f);
    c.tanH = 1.191754f;
    c.tanV = c.tanH * h / w;
    return c;
}

double now_s() {
    static LARGE_INTEGER f0 = {}, t0 = {};
    if (!f0.QuadPart) { QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / f0.QuadPart;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("afw-soak-imgui: rebuilds, the F10 panel's ImGui DX11 drawing, rebuilds again (x3)\n");
    if (!make_device()) return 1;
    const uint32_t W = 2750, H = 2850;

    std::vector<uint32_t> col((size_t)W * H);
    std::vector<float> dep((size_t)W * H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            col[i] = 0xFF000000u | (x & 255) | (((y * 5) & 255) << 8) | ((((x ^ y) * 3) & 255) << 16);
            float z = 2000.0f + 500.0f * std::sin(x * 0.003f) * std::cos(y * 0.002f);
            if (((x / 97) + (y / 131)) % 3 == 0) z = 300.0f + (float)((x * 13 + y * 7) % 400);
            if (x > W / 2 && y > H * 2 / 3 && x < W * 4 / 5) z = 25.0f + 0.01f * x;
            dep[i] = z;
        }
    ID3D11Texture2D* ct = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, col.data(), W * 4);
    ID3D11Texture2D* dt = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, dep.data(), W * 4);
    ID3D11ShaderResourceView *cs = srv_of(ct), *ds = srv_of(dt);
    // The "backbuffer" the panel draws into, and the swapchain image the rebuild lands in.
    ID3D11Texture2D* bb = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView* bbRtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &bbRtv);
    ID3D11Texture2D* dst = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.IniFilename = nullptr;
    ImGui_ImplDX11_Init(dev, ctx);

    af::WarpGpu warp;
    char why[256] = "";
    uint64_t runs = 0, failed = 0, panels = 0;
    auto rebuild_for = [&](double seconds) {
        const double end = now_s() + seconds;
        while (now_s() < end) {
            const double t = now_s();
            const int fresh = (int)(runs & 1);
            af::Source A, B;
            B.valid = A.valid = true;
            B.color = A.color = cs; B.depth = A.depth = ds;
            B.depthW = A.depthW = W; B.depthH = A.depthH = H;
            B.cam = cam_at(t, fresh, W, H);
            A.cam = cam_at(t - 0.0077, fresh ^ 1, W, H);
            af::Params p;
            p.w = W; p.h = H;
            p.target = cam_at(t, fresh ^ 1, W, H);
            if (warp.run(dev, ctx, A, B, p, why, sizeof(why))) ctx->CopyResource(dst, warp.out());
            else ++failed;
            ++runs;
            ctx->Flush();
            Sleep(7);
        }
    };
    auto panel_for = [&](double seconds) {
        const double end = now_s() + seconds;
        while (now_s() < end) {
            io.DeltaTime = 1.0f / 130.0f;
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            ImGui::ShowDemoWindow();
            ImGui::Begin("F10");
            for (int i = 0; i < 40; ++i) ImGui::Text("setting %d: %.3f", i, now_s() * i);
            ImGui::End();
            ImGui::Render();
            ctx->OMSetRenderTargets(1, &bbRtv, nullptr);   // exactly what overlay.cpp does
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            ++panels;
            ctx->Flush();
            Sleep(7);
        }
    };

    rebuild_for(5.0);
    printf("    %llu rebuilds before the panel\n", (unsigned long long)runs);
    for (int round = 0; round < 3; ++round) {
        panel_for(2.0);
        rebuild_for(10.0);
        printf("    round %d: %llu panel frames, %llu rebuilds so far\n", round + 1, (unsigned long long)panels,
               (unsigned long long)runs);
    }
    const auto px = readback(dst);
    check(failed == 0 && runs > 100 && panels > 50, "the rebuild after the F10 panel's ImGui drawing: the driver survives",
          "%llu rebuilds, %llu panel frames, %llu failed, first pixel %.3f", (unsigned long long)runs,
          (unsigned long long)panels, (unsigned long long)failed, px.empty() ? -1.0 : px[0]);

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    warp.shutdown();
    rel(cs); rel(ds); rel(ct); rel(dt); rel(bbRtv); rel(bb); rel(dst);
    return finish("afw-soak-imgui");
}
