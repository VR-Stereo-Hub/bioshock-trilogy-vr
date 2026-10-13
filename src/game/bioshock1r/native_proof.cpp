#include "game/bioshock1r/native_proof.h"
#include "game/bioshock1r/native_proof_mask.h"
#include "game/bioshock1r/native_proof_eye.h"
#include "game/bioshock1r/native_audit_com.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/util/log.h"
#include "core/hooks/pattern_scan.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <vector>

namespace bvr::b1r::native_proof {
namespace {
using native_audit::ComPtr;
using frame_inspector::DrawArgs;
constexpr uint32_t kMaxDraws = 16;
// Eight 4-byte textures, reused across draws: at most 256 MiB of proof surfaces.
constexpr uint64_t kMaxPixels = 8ull * 1024 * 1024;
Stats g_stats;
bool g_auto = false, g_pending = false, g_collect = false, g_masked = false;
bool g_frameFg = false, g_frameWorld = false, g_prepared = false;
bool g_eye = false; // chosen at initialization, no live mode switch
struct EyeTag {
    uint64_t serial = 0; int sign = 0; float separation = 0;
    float worldPosition[3]{}; bool positionValid = false;
};
thread_local EyeTag t_eye;
uint64_t g_eyeBoundary = 0, g_eyeSample = 0;
float g_eyeShift = 0, g_eyeWorld[3]{};
uint64_t g_stableSince = 0, g_started = 0;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11Device> g_device;
struct Surface {
    ComPtr<ID3D11Texture2D> scratch, before, expected, result;
};
Surface g_colour, g_depth;
ComPtr<ID3D11RenderTargetView> g_scratchRtv, g_originalRtv;
ComPtr<ID3D11DepthStencilView> g_scratchDsv, g_originalDsv;
ComPtr<ID3D11Texture2D> g_originalColour, g_originalDepth;
UINT g_width = 0, g_height = 0;
DXGI_FORMAT g_depthFormat = DXGI_FORMAT_UNKNOWN;
UINT g_depthFlags = 0;

void release_draw() {
    g_prepared = false;
    g_originalRtv.Reset(); g_originalDsv.Reset();
    g_originalColour.Reset(); g_originalDepth.Reset();
}
void release_surfaces() {
    g_scratchRtv.Reset(); g_scratchDsv.Reset();
    g_colour = {}; g_depth = {}; g_width = g_height = 0;
}
void stop() {
    release_draw(); release_surfaces(); detail::masked_stop();
    g_collect = g_auto = g_pending = false;
    g_stats.finished = true;
    shader_bytecode::stop();
    frame_inspector::set_draw_replay_probe(nullptr);
    g_context.Reset(); g_device.Reset();
}
bool skip(const char* reason) {
    ++g_stats.skipped;
    BVR_LOG("[native-proof] skip candidate=%u reason=%s", g_stats.candidates, reason);
    release_draw(); return false;
}
bool shader_read_only(ID3D11DeviceChild* shader) {
    std::vector<uint8_t> bytes;
    if (!shader || !shader_bytecode::read(shader, bytes)) return false;
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes.data(), bytes.size(), __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(reflection.GetAddressOf())))) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc))) return false;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &b))) return false;
        switch (b.Type) {
        case D3D_SIT_CBUFFER: case D3D_SIT_TBUFFER: case D3D_SIT_TEXTURE:
        case D3D_SIT_SAMPLER: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS: break;
        default: return false; // every UAV/unknown kind is refused
        }
    }
    return true;
}
bool has_alias(ID3D11DeviceContext* ctx) {
    // OM rebinding must not silently unbind a read-only DSV alias, even on an
    // inactive stage. Replaying such a shader also needs the pre-draw resource.
    using GetFn = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView**);
    const GetFn getters[] = {&ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::PSGetShaderResources,
        &ID3D11DeviceContext::GSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
        &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
    bool alias = false;
    for (auto get : getters) {
        ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        (ctx->*get)(0, _countof(views), views);
        for (auto* view : views) {
            if (!view) continue;
            ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf()); view->Release();
            alias |= resource.Get() == g_originalColour.Get() || resource.Get() == g_originalDepth.Get();
        }
    }
    return alias;
}
bool eligible_state(ID3D11DeviceContext* ctx) {
    ComPtr<ID3D11Predicate> predicate; BOOL value = FALSE;
    ctx->GetPredication(predicate.GetAddressOf(), &value);
    if (predicate) return false;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {}; ctx->SOGetTargets(_countof(so), so);
    bool unsupported = false;
    for (auto* buffer : so) if (buffer) { unsupported = true; buffer->Release(); }
    // FL11_1 exposes 64 UAV slots, FL11_0 exposes 8 (Microsoft's feature table).
    ID3D11UnorderedAccessView* uavs[64] = {};
    const UINT nu = g_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, nu, uavs);
    for (auto* view : uavs) if (view) { unsupported = true; view->Release(); }
    ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(gs.GetAddressOf(), nullptr, nullptr);
    ctx->HSGetShader(hs.GetAddressOf(), nullptr, nullptr); ctx->DSGetShader(ds.GetAddressOf(), nullptr, nullptr);
    if (unsupported || gs || hs || ds) return false;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; UINT vc = 0, pc = 0;
    ctx->VSGetShader(vs.GetAddressOf(), nullptr, &vc); ctx->PSGetShader(ps.GetAddressOf(), nullptr, &pc);
    return vc == 0 && pc == 0 && shader_read_only(vs.Get()) && shader_read_only(ps.Get()) && !has_alias(ctx);
}
bool make_surface(Surface& s, D3D11_TEXTURE2D_DESC d, UINT bind) {
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, s.scratch.GetAddressOf()))) return false;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, s.before.GetAddressOf())) &&
           SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, s.expected.GetAddressOf())) &&
           SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, s.result.GetAddressOf()));
}
bool surfaces(const D3D11_TEXTURE2D_DESC& cd, const D3D11_TEXTURE2D_DESC& dd,
              const D3D11_DEPTH_STENCIL_VIEW_DESC& dv) {
    if (g_width == cd.Width && g_height == cd.Height && g_depthFormat == dd.Format && g_depthFlags == dv.Flags) return true;
    release_surfaces();
    if (!make_surface(g_colour, cd, D3D11_BIND_RENDER_TARGET) || !make_surface(g_depth, dd, D3D11_BIND_DEPTH_STENCIL)) return false;
    if (FAILED(g_device->CreateRenderTargetView(g_colour.scratch.Get(), nullptr, g_scratchRtv.GetAddressOf())) ||
        FAILED(g_device->CreateDepthStencilView(g_depth.scratch.Get(), &dv, g_scratchDsv.GetAddressOf()))) return false;
    g_width = cd.Width; g_height = cd.Height; g_depthFormat = dd.Format; g_depthFlags = dv.Flags;
    return true;
}
bool texture(ID3D11View* view, ComPtr<ID3D11Texture2D>& tex, D3D11_TEXTURE2D_DESC& desc) {
    ComPtr<ID3D11Resource> res; view->GetResource(res.GetAddressOf());
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf())))) return false;
    tex->GetDesc(&desc);
    return desc.ArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
           desc.Width && desc.Height && uint64_t(desc.Width) * desc.Height <= kMaxPixels;
}
bool before_draw(ID3D11DeviceContext* ctx, const DrawArgs& a) noexcept {
    if (ctx != g_context.Get() || g_stats.finished || g_prepared) return false;
    try {
        if (!g_collect && !g_auto) return false;
        if (!a.count) return false;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nv = _countof(vp);
        ctx->RSGetViewports(&nv, vp);
        if (nv != 1 || vp[0].Width < 64 || vp[0].Height < 64) return false;
        ComPtr<ID3D11RenderTargetView> rt; ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1, rt.GetAddressOf(), ds.GetAddressOf());
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; if (rt) rt->GetDesc(&rv);
        if (!ds || rv.Format != DXGI_FORMAT_R11G11B10_FLOAT) return false;
        const bool fg = vp[0].MinDepth == 0 && vp[0].MaxDepth > 0 && vp[0].MaxDepth <= 0.011f;
        g_frameFg |= fg; g_frameWorld |= vp[0].MaxDepth > 0.9f;
        if (!g_collect || !fg) return false;
        if (++g_stats.candidates > kMaxDraws || GetTickCount64() - g_started > 15000) return skip("draw/time cap");
        ID3D11RenderTargetView* targets[8] = {}; ctx->OMGetRenderTargets(8, targets, nullptr);
        bool mrt = false;
        for (UINT i = 0; i < 8; ++i) if (targets[i]) { mrt |= i != 0; targets[i]->Release(); }
        if (mrt || rv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rv.Texture2D.MipSlice != 0) return skip("target view/MRT");
        g_originalRtv = rt; g_originalDsv = ds;
        D3D11_TEXTURE2D_DESC cd{}, dd{}; D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; ds->GetDesc(&dv);
        if (!texture(rt.Get(), g_originalColour, cd) || !texture(ds.Get(), g_originalDepth, dd) ||
            cd.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
            (dd.Format != DXGI_FORMAT_R24G8_TYPELESS && dd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT) ||
            dv.Format != DXGI_FORMAT_D24_UNORM_S8_UINT || dv.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dv.Texture2D.MipSlice ||
            cd.Width != dd.Width || cd.Height != dd.Height) return skip("surface format/dimensions/budget");
        if (g_eye) {
            if (t_eye.serial <= g_eyeBoundary || (t_eye.sign != -1 && t_eye.sign != 1) ||
                !std::isfinite(t_eye.separation) || t_eye.separation <= 0 || t_eye.separation > 128 || !t_eye.positionValid ||
                (g_eyeSample && g_eyeSample != t_eye.serial)) return skip("missing/fresh same-thread render-eye tag");
            g_eyeSample = t_eye.serial; g_eyeShift = -float(t_eye.sign)*t_eye.separation;
            std::memcpy(g_eyeWorld,t_eye.worldPosition,sizeof(g_eyeWorld));
        }
        if (!frame_inspector::replay_queries_idle(ctx)) return skip("active/untracked query range");
        if (!eligible_state(ctx)) return skip("shader metadata/stages/side effects/alias/predication");
        if (g_masked && g_stats.failed) return skip("previous masked failure");
        if (g_masked && !detail::masked_eligible(ctx, dv.Flags)) return skip("native stencil active/read-only");
        if (g_masked && !detail::masked_init(g_device.Get())) { g_stats.failed = true; return skip("mask/query allocation"); }
        if (!surfaces(cd, dd, dv)) { g_stats.failed = true; return skip("scratch allocation"); }
        ctx->CopyResource(g_colour.scratch.Get(), g_originalColour.Get());
        ctx->CopyResource(g_depth.scratch.Get(), g_originalDepth.Get());
        ctx->CopyResource(g_colour.before.Get(), g_originalColour.Get());
        ctx->CopyResource(g_depth.before.Get(), g_originalDepth.Get());
        g_prepared = true;
        return true;
    } catch (...) { g_stats.failed = true; return skip("pre-draw exception"); }
}
void after_draw(ID3D11DeviceContext* ctx, const DrawArgs& a) noexcept {
    if (ctx != g_context.Get() || !g_prepared) return;
    // The unmasked replay changes only OM targets. The optional mask helper
    // restores every state it touches. Eligibility excludes implicit alias unbinds
    // and shader/SO side effects. Original targets are checked after ALL replay.
    ctx->CopyResource(g_colour.expected.Get(), g_originalColour.Get());
    ctx->CopyResource(g_depth.expected.Get(), g_originalDepth.Get());
    ID3D11RenderTargetView* scratch = g_scratchRtv.Get();
    ctx->OMSetRenderTargets(1, &scratch, g_scratchDsv.Get());
    const bool issued = frame_inspector::replay_draw(ctx, a);
    ID3D11RenderTargetView* original = g_originalRtv.Get();
    ctx->OMSetRenderTargets(1, &original, g_originalDsv.Get());
    ctx->CopyResource(g_colour.result.Get(), g_colour.scratch.Get());
    ctx->CopyResource(g_depth.result.Get(), g_depth.scratch.Get());
    uint64_t colour = 0, depth = 0, changedColour = 0, changedDepth = 0, originalColour = 0, originalDepth = 0;
    bool readback = detail::compare_pixels(ctx, g_colour.expected.Get(), g_colour.result.Get(), colour);
    readback &= detail::compare_pixels(ctx, g_depth.expected.Get(), g_depth.result.Get(), depth);
    readback &= detail::compare_pixels(ctx, g_colour.expected.Get(), g_colour.before.Get(), changedColour);
    readback &= detail::compare_pixels(ctx, g_depth.expected.Get(), g_depth.before.Get(), changedDepth);
    bool ok = issued && readback;
    if (g_masked && ok && !colour && !depth) {
        const detail::ProofSurface c{g_colour.scratch.Get(),g_colour.before.Get(),g_colour.expected.Get(),g_colour.result.Get()};
        const detail::ProofSurface d{g_depth.scratch.Get(),g_depth.before.Get(),g_depth.expected.Get(),g_depth.result.Get()};
        ok &= detail::masked_run(ctx,a,c,d,g_scratchRtv.Get(),g_scratchDsv.Get(),g_stats.masks,g_stats.tested+1);
    }
    if (g_eye && ok && !colour && !depth) {
        const detail::ProofSurface c{g_colour.scratch.Get(),g_colour.before.Get(),g_colour.expected.Get(),g_colour.result.Get()};
        const detail::ProofSurface d{g_depth.scratch.Get(),g_depth.before.Get(),g_depth.expected.Get(),g_depth.result.Get()};
        ok &= detail::eye_run(ctx,a,c,d,g_scratchRtv.Get(),g_scratchDsv.Get(),g_eyeShift,g_eyeWorld,g_stats.tested+1,g_stats.eyes);
    }
    // Reuse result staging to prove ALL replays did not write the live surfaces.
    ctx->CopyResource(g_colour.result.Get(), g_originalColour.Get());
    ctx->CopyResource(g_depth.result.Get(), g_originalDepth.Get());
    readback &= detail::compare_pixels(ctx, g_colour.expected.Get(), g_colour.result.Get(), originalColour);
    readback &= detail::compare_pixels(ctx, g_depth.expected.Get(), g_depth.result.Get(), originalDepth);
    ok &= readback;
    ++g_stats.tested; g_stats.changed += changedColour || changedDepth;
    g_stats.colourDiff += colour; g_stats.depthDiff += depth; g_stats.originalDiff += originalColour + originalDepth;
    g_stats.failed |= !ok || colour || depth || originalColour || originalDepth;
    BVR_LOG("[native-proof] draw=%u kind=%u count=%u size=%ux%u changedColour=%llu changedDepth=%llu colourDiff=%llu depthDiff=%llu originalDiff=%llu readback=%s proof=%s",
        g_stats.tested, static_cast<unsigned>(a.kind), a.count, g_width, g_height, changedColour, changedDepth,
        colour, depth, originalColour + originalDepth, readback ? "OK" : "FAILED",
        ok && !colour && !depth && !originalColour && !originalDepth ? "OK" : "INCOMPLETE");
    release_draw();
}
void present(ID3D11DeviceContext* ctx) noexcept {
    if (g_stats.finished) return;
    if (!g_context) { g_context = ctx; ctx->GetDevice(g_device.GetAddressOf()); }
    if (ctx != g_context.Get()) return;
    if (g_collect) {
        g_stats.failed |= !g_stats.tested || !g_stats.changed || g_stats.skipped != 0 || g_prepared;
        if (g_masked) {
            bool complete = !g_stats.failed;
            for (UINT i = 0; i < 5; ++i) complete &= g_stats.masks[i].tested == g_stats.tested;
            complete &= g_stats.masks[0].ps != 0 && g_stats.masks[2].selectedChanged != 0 && g_stats.masks[3].selectedChanged != 0;
            g_stats.failed |= !complete;
            BVR_LOG("[native-proof] masked %s PS unmasked=%llu empty=%llu tiles1=%llu tiles5=%llu all=%llu; synthetic workloads, same eye, no visible repair; timings are individual diagnostic samples",
                complete ? "EXACT" : "INCOMPLETE",g_stats.masks[0].ps,g_stats.masks[1].ps,g_stats.masks[2].ps,g_stats.masks[3].ps,g_stats.masks[4].ps);
        }
        if (g_eye) {
            const auto& e = g_stats.eyes;
            const bool complete = !g_stats.failed && e.tested == g_stats.tested && e.changed &&
                                  !e.cloneColourDiff && !e.cloneDepthDiff && !e.originalConstantDiff;
            g_stats.failed |= !complete;
            BVR_LOG("[native-proof] eye %s tested=%u changed=%u buffers=%u fields=%u cloneColourDiff=%llu cloneDepthDiff=%llu originalConstantDiff=%llu; constant handling only, native target-eye equality and HDR/post still unproven",
                complete ? "VERIFIED" : "INCOMPLETE",e.tested,e.changed,e.buffers,e.fields,e.cloneColourDiff,e.cloneDepthDiff,e.originalConstantDiff);
        }
        BVR_LOG("[native-proof] %s candidates=%u tested=%u changed=%u skipped=%u colourDiff=%llu depthDiff=%llu originalDiff=%llu elapsedMs=%llu; %s, no visible repair",
            g_stats.failed ? "INCOMPLETE" : g_eye ? "SOURCE_EXACT" : "EXACT", g_stats.candidates, g_stats.tested, g_stats.changed, g_stats.skipped,
            g_stats.colourDiff, g_stats.depthDiff, g_stats.originalDiff, GetTickCount64()-g_started,
            g_eye ? "source equality plus shifted constant exercise" : "same-eye scratch only");
        stop(); return;
    }
    const uint64_t now = GetTickCount64();
    if (g_auto) {
        if (!g_frameFg || !g_frameWorld) g_stableSince = 0;
        else if (!g_stableSince) g_stableSince = now;
        if (g_stableSince && now - g_stableSince >= 5000) g_pending = true;
    }
    g_frameFg = g_frameWorld = false;
    if (g_pending) {
        g_collect = true; g_pending = false; g_started = now;
        g_eyeBoundary = t_eye.serial; g_eyeSample = 0;
        BVR_LOG("[native-proof] starting one Present window (max 16 foreground draws); one-time readback hitch expected; log numbers only");
    }
}
const frame_inspector::DrawReplayProbe kProbe{before_draw, after_draw, present};
} // namespace
// All formats accepted here contain exactly four defined bytes per pixel.
// RowPitch padding is excluded. A failed Map can never masquerade as equality.
bool detail::compare_pixels(ID3D11DeviceContext* ctx, ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t& different) {
    if (!ctx || !a || !b || a == b) return false;
    D3D11_TEXTURE2D_DESC da{}, db{}; a->GetDesc(&da); b->GetDesc(&db);
    const auto format = da.Format;
    if ((format != DXGI_FORMAT_R11G11B10_FLOAT && format != DXGI_FORMAT_R24G8_TYPELESS && format != DXGI_FORMAT_D24_UNORM_S8_UINT) ||
        format != db.Format || da.Width != db.Width || da.Height != db.Height ||
        da.ArraySize != 1 || db.ArraySize != 1 || da.MipLevels != 1 || db.MipLevels != 1 ||
        da.SampleDesc.Count != 1 || db.SampleDesc.Count != 1 ||
        da.Usage != D3D11_USAGE_STAGING || db.Usage != D3D11_USAGE_STAGING ||
        !(da.CPUAccessFlags & D3D11_CPU_ACCESS_READ) || !(db.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) return false;
    D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
    if (FAILED(ctx->Map(a, 0, D3D11_MAP_READ, 0, &ma))) return false;
    if (FAILED(ctx->Map(b, 0, D3D11_MAP_READ, 0, &mb))) { ctx->Unmap(a, 0); return false; }
    different = 0;
    for (UINT y = 0; y < da.Height; ++y) {
        const auto* pa = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(ma.pData) + y * ma.RowPitch);
        const auto* pb = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mb.pData) + y * mb.RowPitch);
        for (UINT x = 0; x < da.Width; ++x) different += pa[x] != pb[x];
    }
    ctx->Unmap(b, 0); ctx->Unmap(a, 0); return true;
}
void init(const bvr::pattern_scan::ProcessImage& image) {
    HMODULE self = nullptr; wchar_t ini[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&init), &self) || !GetModuleFileNameW(self, ini, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(ini, L'\\'); if (!slash) return;
    if (wcscpy_s(slash + 1, _countof(ini) - static_cast<size_t>(slash + 1 - ini), L"BioshockVR.ini")) return;
    const UINT mode = GetPrivateProfileIntW(L"Bioshock1R", L"NativeRepairProof", 0, ini);
    if (mode < 1 || mode > 3) return;
    g_masked = mode == 2; g_eye = mode == 3;
    if (GetPrivateProfileIntW(L"Bioshock1R", L"NativeRepairAudit", 0, ini) != 0) {
        BVR_LOG("[native-proof] refused: disable NativeRepairAudit first (shared shader metadata collector)"); return;
    }
    shader_bytecode::configure(image.base, image.size);
    g_auto = true; frame_inspector::set_draw_replay_probe(&kProbe);
    BVR_LOG("[native-proof] mode=%u (2: stencil work; 3: private stereo constants)",mode);
    BVR_LOG("[native-proof] enabled from %ls; wait for 5 s world + foreground; original targets never receive the replay", ini);
}
void request(bool masked) {
    if (g_collect || g_stats.finished) return;
    g_masked = masked; g_pending = true; frame_inspector::set_draw_replay_probe(&kProbe);
}
void request_eye() {
    if (g_collect || g_stats.finished) return;
    g_eye = true; g_masked = false; g_pending = true; frame_inspector::set_draw_replay_probe(&kProbe);
}
void note_eye(int sign, float fullSeparationUu, const float worldPosition[3]) {
    if (!g_eye) return;
    ++t_eye.serial; t_eye.sign = sign; t_eye.separation = fullSeparationUu;
    t_eye.positionValid = worldPosition != nullptr;
    for (unsigned i=0;i<3;++i) {
        t_eye.worldPosition[i] = worldPosition ? worldPosition[i] : 0;
        t_eye.positionValid &= std::isfinite(t_eye.worldPosition[i]);
    }
}
Stats stats() { return g_stats; }
} // namespace bvr::b1r::native_proof
