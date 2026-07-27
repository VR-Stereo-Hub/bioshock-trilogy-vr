#include "core/gfx/hud_capture.h"

#include "core/util/log.h"

#include <atomic>
#include <cstring>

namespace bvr::hud {
namespace {

// All state is render-thread-only except the knobs (atomics) - the game
// renders single-threaded and Present runs on the same thread.
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_force{false};
std::atomic<bool> g_gate{false};

// Our RT (created lazily to match the tonemap target's desc).
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
UINT g_texW = 0, g_texH = 0;

// ---- per-interval classifier state ----------------------------------------
// Current binding (tracked at SetRT; resources are identity keys only).
ID3D11Resource* g_curRt = nullptr;
bool g_curRtLdr = false;      // desc matches a HUD-capable target
bool g_curDsvBound = false;
UINT g_curW = 0, g_curH = 0;
DXGI_FORMAT g_curFmt = DXGI_FORMAT_UNKNOWN;
bool g_substituted = false;   // our RTV is currently bound instead

// Scene-RT vote: the resource hosting the most DSV-bound DrawIndexed calls.
struct Vote { ID3D11Resource* res; unsigned n; };
Vote g_votes[4] = {};
ID3D11Resource* g_hudTarget = nullptr; // set when the tonemap is identified
bool g_hudThisInterval = false;
bool g_hadHudLastInterval = false;

// Lifetime counters.
std::atomic<unsigned> g_cHudDraws{0}, g_cRedirects{0}, g_cLeaks{0}, g_cIntervals{0};

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
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_tex)) &&
              SUCCEEDED(dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) &&
              SUCCEEDED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv));
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
    BVR_LOG("[hud] capture RT created %ux%u fmt=%d", w, h, fmt);
    return true;
}

bool armed() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    return g_force.load(std::memory_order_relaxed) ||
           g_gate.load(std::memory_order_relaxed);
}

} // namespace

void on_setrt(UINT numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv) {
    g_substituted = false; // any bind from the game supersedes our substitution
    g_curRt = nullptr;
    g_curRtLdr = false;
    g_curDsvBound = dsv != nullptr;
    if (!numViews || !rtvs || !rtvs[0]) return;
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    if (!res) return;
    g_curRt = res;
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
    res->Release(); // identity only from here on
}

void on_draw_indexed() {
    if (!g_curRt) return;
    if (g_curRt == g_hudTarget && g_hudTarget) {
        // The fingerprint says the world never DrawIndexes the tonemap target
        // after the tonemap - if it ever does, that is a leak we must know
        // about before trusting the redirect.
        g_cLeaks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_curDsvBound) return;
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

    // A non-indexed draw on the HUD target after the tonemap = gameswf.
    g_cHudDraws.fetch_add(1, std::memory_order_relaxed);
    g_hudThisInterval = true;
    if (!armed()) return nullptr;
    if (!ensure_rt(ctx, g_curW, g_curH, g_curFmt)) return nullptr;
    g_cRedirects.fetch_add(1, std::memory_order_relaxed);
    if (g_substituted) return nullptr; // already bound from a previous draw
    g_substituted = true;
    return g_rtv;
}

void on_present(ID3D11DeviceContext* ctx) {
    if (g_hudThisInterval) g_cIntervals.fetch_add(1, std::memory_order_relaxed);
    g_hadHudLastInterval = g_hudThisInterval;
    if (g_hudThisInterval && g_rtv && ctx) {
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_rtv, clear);
    }
    g_hudThisInterval = false;
    g_hudTarget = nullptr;
    g_substituted = false;
    g_curRt = nullptr;
    g_curRtLdr = false;
    memset(g_votes, 0, sizeof g_votes);
}

// Consumers (eye-capture-adjacent composite, quad copy) run in the present
// chain BEFORE on_present() rolls the interval - so validity keys on the
// CURRENT interval's redirects.
ID3D11ShaderResourceView* srv() {
    return g_hudThisInterval ? g_srv : nullptr;
}

ID3D11Texture2D* texture() {
    return g_hudThisInterval ? g_tex : nullptr;
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

void release_resources() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_texW = g_texH = 0;
}

} // namespace bvr::hud
