#include "core/gfx/gfx_hud.h"

#include "core/util/log.h"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>

namespace bvr::gfx_hud {
namespace {

std::atomic<bool> g_armed{false};
std::atomic<bool> g_redirect{false};
// s52: full-screen-effect pass-through. A GFx damage flash / vignette is a
// tiny-count full-screen quad (<= 12 indices/vertices); leaving those on the
// backbuffer keeps effects across the WHOLE eye image while widgets go to
// the quad. Toggleable because the same shape could be a legit small widget
// - the in-headset A/B decides (bsihud fx on|off).
std::atomic<bool> g_fxPassthrough{true};
constexpr unsigned kFxMaxCount = 12;

// Render-thread state (the draw path is single-threaded per context).
// The backbuffer identity is the CANONICAL IUnknown pointer (COM identity
// rule - interface pointers of the same object are not comparable across
// interfaces). Weak: refreshed per present, never dereferenced.
IUnknown* g_bbUnk = nullptr;
unsigned g_bbW = 0, g_bbH = 0;

IUnknown* canonical(ID3D11Resource* r) {
    IUnknown* u = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&u))) && u) u->Release();
    return u; // weak identity only
}
bool g_onBackbuffer = false;             // current RTV0 is the backbuffer
ID3D11DepthStencilView* g_curDsv = nullptr; // weak, as last bound by the game
ID3D11RenderTargetView* g_gameRtv = nullptr; // weak, the game's own rtv0
bool g_pastBoundary = false;             // the eye blit landed this frame
bool g_ourBound = false;                 // our capture RT is currently bound
bool g_clearedThisFrame = false;

// The capture RT (owned).
ID3D11Texture2D* g_capTex = nullptr;
ID3D11RenderTargetView* g_capRtv = nullptr;
unsigned g_capW = 0, g_capH = 0;
uint32_t g_lastHudFrame = 0; // present counter value of the last frame with HUD draws
uint32_t g_presents = 0;

std::atomic<uint32_t> g_cFrames{0}, g_cBoundaries{0}, g_cHudDraws{0}, g_cRedirected{0},
    g_cBigPost{0}, g_cLastFrameHud{0};
uint32_t g_frameHud = 0;

void release_capture() {
    if (g_capRtv) { g_capRtv->Release(); g_capRtv = nullptr; }
    if (g_capTex) { g_capTex->Release(); g_capTex = nullptr; }
    g_capW = g_capH = 0;
}

bool ensure_capture(ID3D11DeviceContext* ctx) {
    if (g_capTex && g_capW == g_bbW && g_capH == g_bbH) return true;
    release_capture();
    if (!g_bbW || !g_bbH) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_bbW;
    td.Height = g_bbH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_capTex)) && g_capTex &&
              SUCCEEDED(dev->CreateRenderTargetView(g_capTex, nullptr, &g_capRtv)) && g_capRtv;
    dev->Release();
    if (!ok) {
        release_capture();
        static bool logged = false;
        if (!logged) {
            logged = true;
            BVR_LOG("gfx_hud: capture RT creation FAILED (%ux%u)", g_bbW, g_bbH);
        }
        return false;
    }
    g_capW = g_bbW;
    g_capH = g_bbH;
    BVR_LOG("gfx_hud: capture RT %ux%u created", g_capW, g_capH);
    return true;
}

// Is this draw the eye blit? Cheap gates first; the srv/viewport checks run
// only for depth-free a=6 candidates on the backbuffer (rare - one per frame).
bool is_boundary_blit(ID3D11DeviceContext* ctx, unsigned indexCount) {
    if (indexCount != 6 || g_curDsv) return false;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&n, &vp);
    if (n < 1 || static_cast<unsigned>(vp.Width) != g_bbW ||
        static_cast<unsigned>(vp.Height) != g_bbH)
        return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    bool ok = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            // A rendered full-res RT, not a UI atlas: RT-bindable and
            // backbuffer-sized (the measured blit source: 2064x2208 fmt 26,
            // bind RT|SRV).
            ok = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && td.Width == g_bbW &&
                 td.Height == g_bbH;
            tex->Release();
        }
        res->Release();
    }
    srv->Release();
    return ok;
}

Decision classify(ID3D11DeviceContext* ctx, unsigned count, bool indexed) {
    Decision d{};
    if (!g_armed.load(std::memory_order_relaxed)) return d;
    if (!g_onBackbuffer) return d;
    if (!g_pastBoundary) {
        if (indexed && is_boundary_blit(ctx, count)) {
            g_pastBoundary = true;
            g_cBoundaries.fetch_add(1, std::memory_order_relaxed);
        } else if (g_ourBound) {
            // A stale redirect binding survived into a pre-boundary draw
            // (present rolled while our RT was bound and the game has not
            // rebound yet). Hand the game's binding back.
            g_ourBound = false;
            d.restoreRtv = g_gameRtv;
            d.restoreDsv = g_curDsv;
        }
        return d;
    }
    // Post-boundary backbuffer draw = UI-run content.
    g_cHudDraws.fetch_add(1, std::memory_order_relaxed);
    ++g_frameHud;
    // The effects census + pass-through: a full-screen-ish overlay (few
    // indices/vertices) is what a GFx damage flash / vignette looks like -
    // measured live s52: ~0.3/frame at rest, ~3.6/frame during an RPG
    // self-hit. Passing them through keeps effects across the whole view.
    if (count <= kFxMaxCount) {
        g_cBigPost.fetch_add(1, std::memory_order_relaxed);
        if (g_fxPassthrough.load(std::memory_order_relaxed)) return d;
    }
    if (!g_redirect.load(std::memory_order_relaxed)) return d;
    if (!ensure_capture(ctx)) return d;
    if (!g_clearedThisFrame) {
        g_clearedThisFrame = true;
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_capRtv, clear);
    }
    g_cRedirected.fetch_add(1, std::memory_order_relaxed);
    g_ourBound = true;
    d.redirectRtv = g_capRtv;
    d.redirectDsv = g_curDsv; // depth-tested UI keeps its own depth behaviour
    return d;
}

} // namespace

void set_armed(bool on) {
    bool was = g_armed.exchange(on, std::memory_order_relaxed);
    if (was != on) BVR_LOG("gfx_hud: %s", on ? "ARMED (positional classifier live)" : "off");
}

void set_redirect(bool on) {
    bool was = g_redirect.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("gfx_hud: redirect %s", on ? "ON (UI run -> capture RT / quad)" : "off");
}

bool armed() { return g_armed.load(std::memory_order_relaxed); }
bool redirect_on() { return g_redirect.load(std::memory_order_relaxed); }

void set_fx_passthrough(bool on) {
    bool was = g_fxPassthrough.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("gfx_hud: fx passthrough %s (small-count full-screen draws %s)",
                on ? "ON" : "off", on ? "stay on the eye image" : "go to the quad");
}
bool fx_passthrough() { return g_fxPassthrough.load(std::memory_order_relaxed); }

void on_setrt(unsigned numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_curDsv = dsv;
    ID3D11RenderTargetView* rtv0 = (numViews && rtvs) ? rtvs[0] : nullptr;
    g_gameRtv = rtv0;
    g_ourBound = false; // the game rebinding always clears our substitution
    g_onBackbuffer = false;
    if (!rtv0 || !g_bbUnk) return;
    ID3D11Resource* res = nullptr;
    rtv0->GetResource(&res);
    if (res) {
        g_onBackbuffer = (canonical(res) == g_bbUnk);
        res->Release();
    }
}

Decision on_draw_indexed(ID3D11DeviceContext* ctx, unsigned indexCount) {
    return classify(ctx, indexCount, /*indexed=*/true);
}

Decision on_draw(ID3D11DeviceContext* ctx, unsigned vertexCount) {
    return classify(ctx, vertexCount, /*indexed=*/false);
}

void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain) {
    (void)ctx;
    if (!g_armed.load(std::memory_order_relaxed)) return;
    ++g_presents;
    g_cFrames.fetch_add(1, std::memory_order_relaxed);
    if (g_frameHud) g_lastHudFrame = g_presents;
    g_cLastFrameHud.store(g_frameHud, std::memory_order_relaxed);
    g_frameHud = 0;
    g_pastBoundary = false;
    g_clearedThisFrame = false;
    // Refresh the backbuffer identity latch (weak - no ref held across
    // frames; a resize simply re-latches).
    g_bbUnk = nullptr;
    if (swapchain) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
            D3D11_TEXTURE2D_DESC td{};
            bb->GetDesc(&td);
            g_bbUnk = canonical(bb);
            g_bbW = td.Width;
            g_bbH = td.Height;
            bb->Release();
        }
    }
}

ID3D11Texture2D* texture(ID3D11DeviceContext* ctx) {
    (void)ctx;
    if (!g_redirect.load(std::memory_order_relaxed) || !g_capTex) return nullptr;
    // Fresh = HUD content landed within the last few presents (menus and
    // pauses keep drawing UI; movies/loads go stale and the quad drops).
    if (g_presents - g_lastHudFrame > 8) return nullptr;
    return g_capTex;
}

Census census() {
    Census c;
    c.frames = g_cFrames.load(std::memory_order_relaxed);
    c.boundaries = g_cBoundaries.load(std::memory_order_relaxed);
    c.hudDraws = g_cHudDraws.load(std::memory_order_relaxed);
    c.redirected = g_cRedirected.load(std::memory_order_relaxed);
    c.bigPostDraws = g_cBigPost.load(std::memory_order_relaxed);
    c.lastFrameHud = g_cLastFrameHud.load(std::memory_order_relaxed);
    return c;
}

} // namespace bvr::gfx_hud
