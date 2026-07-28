#include "core/gfx/hud_capture.h"

#include "core/gfx/blit.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h" // rendered_hfov_deg (fov-watch comparison)

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>

namespace bvr::hud {
namespace {

// All state is render-thread-only except the knobs (atomics) - the game
// renders single-threaded and Present runs on the same thread.
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_force{false};
std::atomic<bool> g_gate{false};

// Our RT (created lazily to match the tonemap target's desc), plus the
// PROCESSED copy: gameswf leaves garbage in the capture's alpha channel, so
// consumers read RT2 = rgb unchanged + alpha repaired (blit::process) once
// per interval.
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11Texture2D* g_tex2 = nullptr;
ID3D11RenderTargetView* g_rtv2 = nullptr;
ID3D11ShaderResourceView* g_srv2 = nullptr;
UINT g_texW = 0, g_texH = 0;
bool g_processedThisInterval = false;

// Our own depth-stencil for the redirected draws (session 19, headset round):
// gameswf implements flash MASKS via stencil - redirecting with no DSV left
// the vending/pause odometer digit strips unclipped (user's screenshot).
// Cleared each interval; the mask write-then-test is self-contained.
ID3D11Texture2D* g_depthTex = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;

// Alpha-corrected variants of the gameswf blend states (session 19, headset
// round): gameswf's states blend the ALPHA channel like the color channel
// (a*a + (1-a)*dst), which accumulates garbage coverage - dark opaque menu
// panels ended up transparent on the quad. Each state seen on a redirected
// draw gets a variant with SrcBlendAlpha=ONE, DestBlendAlpha=INV_SRC_ALPHA
// (correct over-composite coverage), rgb ops untouched. Keyed by the
// original state pointer; variants are ours and released with the RT.
std::map<ID3D11BlendState*, ID3D11BlendState*> g_blendVariants;

// ---- per-interval classifier state ----------------------------------------
// Current binding (tracked at SetRT; resources are identity keys only).
ID3D11Resource* g_curRt = nullptr;
bool g_curRtLdr = false;      // desc matches a HUD-capable target
bool g_curDsvBound = false;
UINT g_curW = 0, g_curH = 0;
DXGI_FORMAT g_curFmt = DXGI_FORMAT_UNKNOWN;

// Scene-RT vote: the resource hosting the most DSV-bound DrawIndexed calls.
struct Vote { ID3D11Resource* res; unsigned n; };
Vote g_votes[4] = {};
ID3D11Resource* g_hudTarget = nullptr; // set when the tonemap is identified
bool g_hudThisInterval = false;
bool g_hadHudLastInterval = false;

// Lifetime counters.
std::atomic<unsigned> g_cHudDraws{0}, g_cRedirects{0}, g_cLeaks{0}, g_cIntervals{0};

// ---- Session 22: live rendered-FOV watch (see hud_capture.h) ----------------
// One 80-byte staging buffer: the first scene draw of an interval copies the
// head of its VS b0 into it (CopySubresourceRegion, async); the copy is
// mapped on a LATER present with DO_NOT_WAIT, so the pipeline never stalls.
// Tangent layout (session 21, dump-verified): floats 12..18 hold the
// screen-ray helper (2tanH, 0, -tanH, 0, 0, -2tanV, tanV); the two
// derivations must agree or the block is not a perspective pass.
constexpr UINT kFovCbBytes = 80; // floats 0..19
ID3D11Buffer* g_fovStaging = nullptr;
bool g_fovPending = false; // copied, not yet mapped
int g_fovPendingAge = 0;   // presents since the copy
int g_fovTriesThisInterval = 0;
bool g_fovCapturedThisInterval = false;
std::atomic<float> g_fovTanH{0.0f}, g_fovTanV{0.0f};
std::atomic<unsigned long long> g_fovStampMs{0};
// Mismatch verdict (render thread writes; hysteresis over present intervals).
std::atomic<bool> g_fovMismatchOn{false};
int g_fovMismatchStreak = 0;

// ---- Session 22: per-kind routing state -------------------------------------
// (a) Screen-only intervals: the hack minigame and loading screens draw PURE
// gameswf with the world pass completely absent (dump-proven: 0 DrawIndexed,
// 322/87 swf draws) - nothing classifies, so today they render in-frame
// across the whole projection FOV. The detector below (swf draws with no
// scene leader) publishes to the VR runtime, which drops those intervals to
// the readable quad screen regardless of the cinematic stereo/quad mode.
// (c) In-frame post effects: the alcohol-blur composite is a post-tonemap
// non-indexed draw from the engine's post path sampling a BACKBUFFER-SIZED
// texture (dump-proven), while gameswf HUD samples 2048x2048 UI atlases -
// srv0 size is the discriminator; those draws stay in the frame.
int g_swfDrawsThisInterval = 0;
std::atomic<bool> g_screenOnlyOn{false};
int g_screenOnlyStreak = 0;
std::atomic<unsigned> g_cPostFx{0};
struct SrvCacheEntry { ID3D11Resource* res; UINT w, h; };
SrvCacheEntry g_srvCache[8] = {};
int g_srvCacheCount = 0;

// srv0 dimensions with the same per-interval identity cache the RT descs use
// (the HUD run rebinds the same few atlases dozens of times per interval).
bool srv0_size(ID3D11DeviceContext* ctx, UINT* w, UINT* h) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) return false;
    res->Release(); // identity from here on (the PS binding holds the ref)
    for (int i = 0; i < g_srvCacheCount; ++i) {
        if (g_srvCache[i].res == res) {
            *w = g_srvCache[i].w;
            *h = g_srvCache[i].h;
            return true;
        }
    }
    UINT tw = 0, th = 0;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        tw = d.Width;
        th = d.Height;
        tex->Release();
    }
    if (g_srvCacheCount < static_cast<int>(_countof(g_srvCache))) {
        SrvCacheEntry& e = g_srvCache[g_srvCacheCount++];
        e.res = res;
        e.w = tw;
        e.h = th;
    }
    if (!tw) return false;
    *w = tw;
    *h = th;
    return true;
}

// ---- Session 22 round 2: letterbox watch -------------------------------------
// Engine cinematics (plasmid FMV sequences) clear the FINAL target to opaque
// black and tonemap the scene into a vertically SHRUNKEN quad (dump: ClearRTV
// (0,0,0,1) + a full-viewport draw whose geometry covers only the middle
// band). The bars are unpainted clear - there is no draw to classify - and
// the band content is the full render anamorphically squeezed. Watch: three
// thin columns of the backbuffer copied to a staging strip per present
// (async, DO_NOT_WAIT map a present later - the fov-watch pattern); a row is
// "bar" only if EXACTLY black across all three columns. Symmetric stable
// top/bottom runs = letterbox: the VR runtime crops the submitted subImage
// to the band (the compositor unsqueezes it back to the claimed fov - bars
// gone, geometry correct) and the adapter suspends the live head drive so
// the authored camera choreography plays (the flat-screen look, in stereo).
constexpr int kLbCols = 3;
ID3D11Texture2D* g_lbStaging = nullptr;
UINT g_lbH = 0, g_lbSrcW = 0, g_lbSrcH = 0;
bool g_lbPending = false;
int g_lbPendingAge = 0;
int g_lbStreak = 0;
unsigned g_lbLastTop = 0, g_lbLastBot = 0; // last raw measurement (render thread)
std::atomic<uint32_t> g_lbBars{0};         // packed top<<16 | bot, 0 = inactive
std::atomic<unsigned> g_cLbIntervals{0};

bool ensure_lb_staging(ID3D11DeviceContext* ctx, UINT h) {
    if (g_lbStaging && g_lbH == h) return true;
    if (g_lbStaging) {
        g_lbStaging->Release();
        g_lbStaging = nullptr;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = kLbCols;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &g_lbStaging);
    dev->Release();
    if (SUCCEEDED(hr)) g_lbH = h;
    return SUCCEEDED(hr) && g_lbStaging;
}

void letterbox_watch(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain) {
    // Map last present's copy first (never blocks).
    if (g_lbPending && g_lbStaging) {
        ++g_lbPendingAge;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(g_lbStaging, 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const uint8_t* base = static_cast<const uint8_t*>(m.pData);
            UINT h = g_lbH;
            auto rowBlack = [&](UINT r) {
                const uint8_t* p = base + r * m.RowPitch;
                for (int c = 0; c < kLbCols; ++c)
                    if (p[c * 4] > 2 || p[c * 4 + 1] > 2 || p[c * 4 + 2] > 2)
                        return false;
                return true;
            };
            UINT top = 0;
            while (top < h && rowBlack(top)) ++top;
            UINT bot = 0;
            while (bot < h - top && rowBlack(h - 1 - bot)) ++bot;
            ctx->Unmap(g_lbStaging, 0);
            g_lbPending = false;

            UINT minBar = h / 25;                 // >= 4% each
            bool full = top + bot >= h;           // black screen (fade/loading)
            UINT hi = top > bot ? top : bot;
            bool symmetric = (top > bot ? top - bot : bot - top) <= hi / 4 + 8;
            bool isLb = !full && top >= minBar && bot >= minBar && symmetric;
            // Stability: consecutive agreeing verdicts (bars within 3 px)
            // before switching state - the clear can flicker at scene cuts.
            bool active = g_lbBars.load(std::memory_order_relaxed) != 0;
            bool agrees = isLb == active;
            if (isLb && (top > g_lbLastTop + 3 || g_lbLastTop > top + 3 ||
                         bot > g_lbLastBot + 3 || g_lbLastBot > bot + 3))
                agrees = active && false; // moving bars: treat as disagreement
            g_lbLastTop = top;
            g_lbLastBot = bot;
            if (!agrees) {
                if (++g_lbStreak >= 5) {
                    g_lbStreak = 0;
                    g_lbBars.store(isLb ? ((top & 0xFFFF) << 16) | (bot & 0xFFFF) : 0,
                                   std::memory_order_relaxed);
                    if (isLb) g_cLbIntervals.fetch_add(1, std::memory_order_relaxed);
                    BVR_LOG("[hud] letterbox %s (top %u px, bottom %u px of %u)",
                            isLb ? "ON (engine cinematic bars)" : "off", top, bot, h);
                }
            } else {
                g_lbStreak = 0;
                if (isLb) // track slow bar animation while active
                    g_lbBars.store(((top & 0xFFFF) << 16) | (bot & 0xFFFF),
                                   std::memory_order_relaxed);
            }
        } else if (g_lbPendingAge > 8) {
            g_lbPending = false;
        }
    }

    // Queue this present's copy: three 1-px columns at 1/4, 1/2, 3/4 width.
    if (g_lbPending || !swapchain) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    if ((bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
        ensure_lb_staging(ctx, bd.Height)) {
        g_lbSrcW = bd.Width;
        g_lbSrcH = bd.Height;
        for (int c = 0; c < kLbCols; ++c) {
            UINT x = bd.Width * (c + 1) / 4;
            D3D11_BOX box{x, 0, 0, x + 1, bd.Height, 1};
            ctx->CopySubresourceRegion(g_lbStaging, 0, static_cast<UINT>(c), 0, 0,
                                       bb, 0, &box);
        }
        g_lbPending = true;
        g_lbPendingAge = 0;
    }
    bb->Release();
}

bool ensure_fov_staging(ID3D11DeviceContext* ctx) {
    if (g_fovStaging) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kFovCbBytes;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = dev->CreateBuffer(&sd, nullptr, &g_fovStaging);
    dev->Release();
    return SUCCEEDED(hr) && g_fovStaging;
}

// Map attempt + mismatch bookkeeping, once per present (from on_present).
void fov_watch_on_present(ID3D11DeviceContext* ctx) {
    if (g_fovPending && ctx && g_fovStaging) {
        ++g_fovPendingAge;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(g_fovStaging, 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const float* f = static_cast<const float*>(m.pData);
            float tanH1 = f[12] * 0.5f, tanH2 = -f[14];
            float tanV1 = -f[17] * 0.5f, tanV2 = f[18];
            ctx->Unmap(g_fovStaging, 0);
            g_fovPending = false;
            bool ok = std::isfinite(tanH1) && std::isfinite(tanH2) &&
                      std::isfinite(tanV1) && std::isfinite(tanV2) &&
                      tanH1 > 0.05f && tanH1 < 20.0f && tanV1 > 0.05f &&
                      fabsf(tanH1 - tanH2) <= 0.02f * tanH1 &&
                      fabsf(tanV1 - tanV2) <= 0.02f * tanV1;
            if (ok) {
                g_fovTanH.store(tanH1, std::memory_order_relaxed);
                g_fovTanV.store(tanV1, std::memory_order_relaxed);
                g_fovStampMs.store(GetTickCount64(), std::memory_order_relaxed);
            }
            // Non-perspective/zero blocks fail silently - the stamp just ages.
        } else if (g_fovPendingAge > 8) {
            g_fovPending = false; // copy stuck (device weirdness) - recapture
        }
    }
    g_fovCapturedThisInterval = false;
    g_fovTriesThisInterval = 0;

    // Rendered-vs-option verdict with a 3-interval hysteresis, logged on
    // transition. Session-independent by design: this is the flat-testable
    // instrument (the descent shows ON with no headset attached).
    unsigned long long stamp = g_fovStampMs.load(std::memory_order_relaxed);
    bool fresh = stamp && GetTickCount64() - stamp < 500;
    float optHfov = bvr::vr::rendered_hfov_deg();
    bool mm = false;
    if (fresh && optHfov > 1.0f) {
        float optTan = tanf(optHfov * 0.5f * 3.14159265f / 180.0f);
        if (optTan > 0.01f) {
            float r = g_fovTanH.load(std::memory_order_relaxed) / optTan;
            mm = r < 0.90f || r > 1.10f;
        }
    }
    if (mm != g_fovMismatchOn.load(std::memory_order_relaxed)) {
        if (++g_fovMismatchStreak >= 3) {
            g_fovMismatchStreak = 0;
            g_fovMismatchOn.store(mm, std::memory_order_relaxed);
            BVR_LOG("[hud] rendered-fov mismatch %s (rendered tanH %.4f = %.1f deg "
                    "vs option %.1f deg)",
                    mm ? "ON (scripted-camera fov)" : "off",
                    g_fovTanH.load(std::memory_order_relaxed),
                    2.0f * atanf(g_fovTanH.load(std::memory_order_relaxed)) * 57.29578f,
                    optHfov);
        }
    } else {
        g_fovMismatchStreak = 0;
    }
}

ID3D11Resource* scene_leader() {
    Vote* best = nullptr;
    for (Vote& v : g_votes)
        if (v.res && (!best || v.n > best->n)) best = &v;
    // A handful of DSV-bound draws is not a scene (the fg rig pass has ~14);
    // the world pass has hundreds. 32 is comfortably between.
    return (best && best->n >= 32) ? best->res : nullptr;
}

bool ensure_rt(ID3D11DeviceContext* ctx, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_tex && g_texW == w && g_texH == h) return true;
    release_resources();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_tex)) &&
              SUCCEEDED(dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) &&
              SUCCEEDED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) &&
              SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_tex2)) &&
              SUCCEEDED(dev->CreateRenderTargetView(g_tex2, nullptr, &g_rtv2)) &&
              SUCCEEDED(dev->CreateShaderResourceView(g_tex2, nullptr, &g_srv2)) &&
              SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &g_depthTex)) &&
              SUCCEEDED(dev->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv));
    dev->Release();
    if (!ok) {
        release_resources();
        BVR_LOG("[hud] RT create FAILED (%ux%u)", w, h);
        return false;
    }
    g_texW = w;
    g_texH = h;
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g_rtv, clear);
    ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    BVR_LOG("[hud] capture RT created %ux%u fmt=%d (+D24S8 for gameswf masks)", w, h, fmt);
    return true;
}

bool armed() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    return g_force.load(std::memory_order_relaxed) ||
           g_gate.load(std::memory_order_relaxed);
}

} // namespace

// Per-present cache of resource -> HUD-capable desc (the same few RTs rebind
// ~200x per interval; QI+GetDesc per bind showed up as avoidable per-frame
// COM churn in the session-19 FPS pass).
struct DescCacheEntry {
    ID3D11Resource* res;
    bool ldr;
    UINT w, h;
    DXGI_FORMAT fmt;
};
DescCacheEntry g_descCache[8] = {};
int g_descCacheCount = 0;

void on_setrt(UINT numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv) {
    g_curRt = nullptr;
    g_curRtLdr = false;
    g_curDsvBound = dsv != nullptr;
    if (!numViews || !rtvs || !rtvs[0]) return;
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    if (!res) return;
    g_curRt = res;
    res->Release(); // identity only from here on

    for (int i = 0; i < g_descCacheCount; ++i) {
        if (g_descCache[i].res == res) {
            g_curRtLdr = g_descCache[i].ldr;
            g_curW = g_descCache[i].w;
            g_curH = g_descCache[i].h;
            g_curFmt = g_descCache[i].fmt;
            return;
        }
    }
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        g_curW = d.Width;
        g_curH = d.Height;
        g_curFmt = d.Format;
        // HUD-capable: big RGBA8 render+shader target (the tonemap target's
        // shape; exact size keys off whatever the game runs at).
        g_curRtLdr = d.Width >= 640 && d.Height >= 480 &&
                     d.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                     (d.BindFlags & D3D11_BIND_RENDER_TARGET) &&
                     (d.BindFlags & D3D11_BIND_SHADER_RESOURCE);
        tex->Release();
    }
    if (g_descCacheCount < static_cast<int>(_countof(g_descCache))) {
        DescCacheEntry& e = g_descCache[g_descCacheCount++];
        e.res = res;
        e.ldr = g_curRtLdr;
        e.w = g_curW;
        e.h = g_curH;
        e.fmt = g_curFmt;
    }
}

void on_draw_indexed(ID3D11DeviceContext* ctx) {
    if (!g_curRt) return;
    if (g_curRt == g_hudTarget && g_hudTarget) {
        // The fingerprint says the world never DrawIndexes the tonemap target
        // after the tonemap - if it ever does, that is a leak we must know
        // about before trusting the redirect.
        g_cLeaks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_curDsvBound) return;

    // Session 22 fov watch: grab the first decodable scene draw's cb0 head
    // (bounded attempts - early depth-only passes can bind small buffers).
    if (!g_fovCapturedThisInterval && !g_fovPending && ctx &&
        g_fovTriesThisInterval < 8) {
        ++g_fovTriesThisInterval;
        ID3D11Buffer* cb0 = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &cb0);
        if (cb0) {
            D3D11_BUFFER_DESC bd{};
            cb0->GetDesc(&bd);
            // 320 = the smallest world-pass cb tier that carries the ray block.
            if (bd.ByteWidth >= 320 && ensure_fov_staging(ctx)) {
                D3D11_BOX box{0, 0, 0, kFovCbBytes, 1, 1};
                ctx->CopySubresourceRegion(g_fovStaging, 0, 0, 0, 0, cb0, 0, &box);
                g_fovPending = true;
                g_fovPendingAge = 0;
                g_fovCapturedThisInterval = true;
            }
            cb0->Release();
        }
    }

    for (Vote& v : g_votes) {
        if (v.res == g_curRt) {
            ++v.n;
            return;
        }
    }
    for (Vote& v : g_votes) {
        if (!v.res) {
            v.res = g_curRt;
            v.n = 1;
            return;
        }
    }
}

ID3D11RenderTargetView* on_draw(ID3D11DeviceContext* ctx) {
    if (!g_curRt || !g_curRtLdr) return nullptr;
    ++g_swfDrawsThisInterval; // session 22: screen-only interval detector

    if (!g_hudTarget) {
        // Tonemap check: first non-indexed draw on an LDR target sampling the
        // scene-vote leader marks this target as the HUD host.
        ID3D11Resource* leader = scene_leader();
        if (!leader) return nullptr;
        ID3D11ShaderResourceView* srv0 = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv0);
        if (!srv0) return nullptr;
        ID3D11Resource* srvRes = nullptr;
        srv0->GetResource(&srvRes);
        srv0->Release();
        if (srvRes) srvRes->Release(); // identity compare only
        if (srvRes == leader) g_hudTarget = g_curRt;
        return nullptr; // the tonemap itself always passes through
    }

    if (g_curRt != g_hudTarget) return nullptr;

    // Session 22 kind (c): the engine's own post effects (alcohol blur) are
    // also post-tonemap non-indexed draws here, but they sample a
    // BACKBUFFER-SIZED texture (gameswf samples UI atlases). They belong IN
    // the frame - per-eye correct by construction - so they never redirect
    // and never count as HUD content.
    {
        UINT sw = 0, sh = 0;
        if (srv0_size(ctx, &sw, &sh) && sw == g_curW && sh == g_curH) {
            g_cPostFx.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // A non-indexed draw on the HUD target after the tonemap = gameswf.
    g_cHudDraws.fetch_add(1, std::memory_order_relaxed);
    g_hudThisInterval = true;
    if (!armed()) return nullptr;
    if (!ensure_rt(ctx, g_curW, g_curH, g_curFmt)) return nullptr;
    g_cRedirects.fetch_add(1, std::memory_order_relaxed);
    // Returned for EVERY redirected draw - the caller re-binds each time
    // (cheap, self-healing against mid-stream game binds) and swaps the
    // blend state to its alpha-corrected variant.
    return g_rtv;
}

void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain) {
    fov_watch_on_present(ctx);      // session 22: map last interval's cb0 copy
    letterbox_watch(ctx, swapchain); // session 22 round 2: cinematic bars

    // Session 22 kind (a): screen-only interval verdict (hysteresis over
    // present intervals, transitions logged - the flat instrument). Computed
    // BEFORE the vote reset below. The 20-draw floor keeps single stray swf
    // draws (subtitle fade, console line) from tripping it.
    {
        bool screenOnly = scene_leader() == nullptr && g_swfDrawsThisInterval >= 20;
        if (screenOnly != g_screenOnlyOn.load(std::memory_order_relaxed)) {
            if (++g_screenOnlyStreak >= 3) {
                g_screenOnlyStreak = 0;
                g_screenOnlyOn.store(screenOnly, std::memory_order_relaxed);
                BVR_LOG("[hud] screen-only interval %s (swf draws %d, world pass %s)",
                        screenOnly ? "ON (hack/loading/FMV-class screen)" : "off",
                        g_swfDrawsThisInterval, screenOnly ? "absent" : "present");
            }
        } else {
            g_screenOnlyStreak = 0;
        }
        g_swfDrawsThisInterval = 0;
        g_srvCacheCount = 0;
    }

    if (g_hudThisInterval) g_cIntervals.fetch_add(1, std::memory_order_relaxed);
    g_hadHudLastInterval = g_hudThisInterval;
    if (g_hudThisInterval && g_rtv && ctx) {
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_rtv, clear);
        if (g_dsv)
            ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                       1.0f, 0);
    }
    g_hudThisInterval = false;
    g_processedThisInterval = false;
    g_hudTarget = nullptr;
    g_curRt = nullptr;
    g_curRtLdr = false;
    memset(g_votes, 0, sizeof g_votes);
    g_descCacheCount = 0;
}

// Consumers (eye-capture-adjacent composite, quad copy) run in the present
// chain BEFORE on_present() rolls the interval - so validity keys on the
// CURRENT interval's redirects. Both accessors serve the PROCESSED copy
// (alpha repaired), running the repair pass at most once per interval.
ID3D11ShaderResourceView* srv(ID3D11DeviceContext* ctx) {
    if (!g_hudThisInterval || !g_srv2) return nullptr;
    if (!g_processedThisInterval && ctx) {
        if (!bvr::blit::process(ctx, g_rtv2, g_srv, g_texW, g_texH)) return nullptr;
        g_processedThisInterval = true;
    }
    return g_processedThisInterval ? g_srv2 : nullptr;
}

ID3D11Texture2D* texture(ID3D11DeviceContext* ctx) {
    return srv(ctx) ? g_tex2 : nullptr;
}

bool redirected_this_interval() {
    return g_hudThisInterval;
}

void set_enabled(bool on) {
    bool was = g_enabled.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("[hud] capture %s", on ? "ON (gameswf HUD redirects while gated)"
                                       : "off (HUD stays in the game frame)");
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_gate(bool stereoActive) {
    g_gate.store(stereoActive, std::memory_order_relaxed);
}

void set_force(bool on) {
    g_force.store(on, std::memory_order_relaxed);
    BVR_LOG("[hud] force %s (redirect %s the stereo gate - flat testing)",
            on ? "ON" : "off", on ? "ignores" : "respects");
}

bool force() { return g_force.load(std::memory_order_relaxed); }

void get_counters(unsigned* hudDraws, unsigned* redirects, unsigned* leaks,
                  unsigned* intervalsWithHud) {
    if (hudDraws) *hudDraws = g_cHudDraws.load(std::memory_order_relaxed);
    if (redirects) *redirects = g_cRedirects.load(std::memory_order_relaxed);
    if (leaks) *leaks = g_cLeaks.load(std::memory_order_relaxed);
    if (intervalsWithHud) *intervalsWithHud = g_cIntervals.load(std::memory_order_relaxed);
}

bool fov_watch(float* tanH, float* tanV, unsigned long long* ageMs) {
    unsigned long long s = g_fovStampMs.load(std::memory_order_relaxed);
    if (!s) return false;
    if (tanH) *tanH = g_fovTanH.load(std::memory_order_relaxed);
    if (tanV) *tanV = g_fovTanV.load(std::memory_order_relaxed);
    if (ageMs) *ageMs = GetTickCount64() - s;
    return true;
}

bool fov_mismatch() {
    return g_fovMismatchOn.load(std::memory_order_relaxed);
}

bool screen_only() {
    return g_screenOnlyOn.load(std::memory_order_relaxed);
}

bool letterbox(unsigned* topPx, unsigned* botPx) {
    uint32_t packed = g_lbBars.load(std::memory_order_relaxed);
    if (!packed) return false;
    if (topPx) *topPx = packed >> 16;
    if (botPx) *botPx = packed & 0xFFFF;
    return true;
}

unsigned postfx_count() {
    return g_cPostFx.load(std::memory_order_relaxed);
}

ID3D11DepthStencilView* capture_dsv() {
    return g_dsv;
}

void fix_blend_alpha(ID3D11DeviceContext* ctx) {
    ID3D11BlendState* cur = nullptr;
    FLOAT bf[4] = {};
    UINT mask = 0xFFFFFFFF;
    ctx->OMGetBlendState(&cur, bf, &mask);
    // Already one of our variants (consecutive redirected draws with no
    // game state change in between): nothing to do.
    for (const auto& [orig, variant] : g_blendVariants) {
        if (variant == cur) {
            cur->Release();
            return;
        }
    }
    auto it = g_blendVariants.find(cur);
    ID3D11BlendState* variant = nullptr;
    if (it != g_blendVariants.end()) {
        variant = it->second;
    } else {
        D3D11_BLEND_DESC d{};
        if (cur) {
            cur->GetDesc(&d);
        } else {
            // Default state: blending off - output alpha is already the
            // shader's (correct coverage); still force the alpha write on.
            d.RenderTarget[0].BlendEnable = FALSE;
        }
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            if (SUCCEEDED(dev->CreateBlendState(&d, &variant)))
                g_blendVariants.emplace(cur, variant);
            dev->Release();
        }
    }
    if (variant) ctx->OMSetBlendState(variant, bf, mask);
    if (cur) cur->Release();
}

void release_resources() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_srv2) { g_srv2->Release(); g_srv2 = nullptr; }
    if (g_rtv2) { g_rtv2->Release(); g_rtv2 = nullptr; }
    if (g_tex2) { g_tex2->Release(); g_tex2 = nullptr; }
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    for (auto& [orig, variant] : g_blendVariants)
        if (variant) variant->Release();
    g_blendVariants.clear();
    g_texW = g_texH = 0;
    g_processedThisInterval = false;
    if (g_fovStaging) { g_fovStaging->Release(); g_fovStaging = nullptr; }
    g_fovPending = false;
    g_fovCapturedThisInterval = false;
    if (g_lbStaging) { g_lbStaging->Release(); g_lbStaging = nullptr; }
    g_lbH = 0;
    g_lbPending = false;
}

} // namespace bvr::hud
