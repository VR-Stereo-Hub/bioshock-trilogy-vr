#include "game/bioshock1r/gun_flip.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/temporal.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace bvr::b1r::gun_flip {
namespace {

std::atomic<void*> g_gunActor{nullptr}; // the reflected gun (null: none)
std::atomic<bool> g_static{false};      // inside the reflected gun's static-mesh draw
std::atomic<bool> g_armed{false};
std::atomic<uint32_t> g_gathers{0};

// Render thread only from here down.
struct Pair {
    ID3D11RasterizerState* orig = nullptr;  // the game's state, a reference held
    ID3D11RasterizerState* flip = nullptr;  // the same with FrontCounterClockwise turned round
};
constexpr int kCache = 16;
Pair g_cache[kCache];
int g_cacheN = 0;
ID3D11RasterizerState* g_defaultFlip = nullptr; // for the device default state (none bound)
ID3D11RasterizerState* g_applied = nullptr;     // what this put on the context, if it is still there
ID3D11RasterizerState* g_saved = nullptr;       // the game's state it replaced (a reference held)

// Per 5 s window, for the log.
uint32_t g_flipped = 0, g_staticSeen = 0, g_hands = 0, g_failed = 0;
struct Seen { unsigned count = 0; uint32_t draws = 0; bool gun = false; };
Seen g_seen[12];
uint64_t g_logMs = 0;

void note_seen(unsigned count, bool gun) {
    for (Seen& s : g_seen) {
        if (s.draws && s.count == count) { ++s.draws; return; }
        if (!s.draws) { s.count = count; s.draws = 1; s.gun = gun; return; }
    }
}

ID3D11RasterizerState* make_flip(ID3D11DeviceContext* ctx, ID3D11RasterizerState* orig) {
    D3D11_RASTERIZER_DESC d{};
    if (orig) {
        orig->GetDesc(&d);
    } else { // the device default state
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.FrontCounterClockwise = FALSE;
        d.DepthClipEnable = TRUE;
    }
    d.FrontCounterClockwise = !d.FrontCounterClockwise;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RasterizerState* f = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &f);
    dev->Release();
    if (FAILED(hr)) {
        BVR_LOG("[gunflip] could not make the turned-round rasterizer state (0x%08lX)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    return f;
}

ID3D11RasterizerState* flipped_for(ID3D11DeviceContext* ctx, ID3D11RasterizerState* orig) {
    if (!orig) {
        if (!g_defaultFlip) g_defaultFlip = make_flip(ctx, nullptr);
        return g_defaultFlip;
    }
    for (int i = 0; i < g_cacheN; ++i)
        if (g_cache[i].orig == orig) return g_cache[i].flip;
    if (g_cacheN >= kCache) return nullptr;
    ID3D11RasterizerState* f = make_flip(ctx, orig);
    if (!f) return nullptr;
    orig->AddRef(); // kept so the pointer is never reused for another state
    g_cache[g_cacheN++] = Pair{orig, f};
    return f;
}

void restore(ID3D11DeviceContext* ctx) {
    if (!g_applied) return;
    ID3D11RasterizerState* cur = nullptr;
    ctx->RSGetState(&cur);
    if (cur == g_applied) ctx->RSSetState(g_saved); // the game set nothing since: give it its own back
    if (cur) cur->Release();
    if (g_saved) g_saved->Release();
    g_saved = nullptr;
    g_applied = nullptr;
}

void maybe_log() {
    const uint64_t now = GetTickCount64();
    if (now - g_logMs < 5000) return;
    g_logMs = now;
    if (g_gunActor.load(std::memory_order_relaxed) || g_flipped || g_staticSeen) {
        char list[256] = "";
        int at = 0;
        for (const Seen& s : g_seen)
            if (s.draws)
                at += _snprintf_s(list + at, sizeof list - at, _TRUNCATE, "%s%u x%u%s", at ? ", " : "", s.count, s.draws,
                                  s.gun ? " FLIPPED" : " (hands)");
        BVR_LOG("[gunflip] 5 s: %u gun draws drawn with the other face (static %u), %u hands draws left as they are, "
                "%u gun gathers, %u could not be flipped | the viewmodel's skinned draws by index count: %s",
                g_flipped, g_staticSeen, g_hands, g_gathers.exchange(0), g_failed, at ? list : "none");
    }
    g_flipped = g_staticSeen = g_hands = g_failed = 0;
    for (Seen& s : g_seen) s = Seen{};
}

// The viewmodel's own slice of depth (its viewport's MaxDepth; the world draws to 1.0).
constexpr float kViewmodelMaxDepth = 0.0101f;
constexpr UINT kSkinnedStride = 56;

bool is_gun_draw(ID3D11DeviceContext* ctx, unsigned indexCount) {
    if (g_static.load(std::memory_order_relaxed)) {
        ++g_staticSeen;
        return true;
    }
    if (!g_gunActor.load(std::memory_order_relaxed)) return false;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, off = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
    if (vb) vb->Release();
    if (!vb || stride != kSkinnedStride) return false;
    D3D11_VIEWPORT vp{};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    if (!n || vp.MaxDepth > kViewmodelMaxDepth) return false;
    const bool gun = indexCount != patterns::kHandsIndexCount;
    note_seen(indexCount, gun);
    if (!gun) ++g_hands;
    return gun;
}

} // namespace

void note_gun_gather() { g_gathers.fetch_add(1, std::memory_order_relaxed); }

void static_draw(bool inside) { g_static.store(inside, std::memory_order_relaxed); }

void set_gun(void* actor) {
    g_gunActor.store(actor, std::memory_order_relaxed);
    if (!actor || g_armed.exchange(true, std::memory_order_relaxed)) return;
    temporal::arm_draw_tap();
    BVR_LOG("[gunflip] armed: a reflected gun's draws cull the other face");
}

void on_draw(ID3D11DeviceContext* ctx, unsigned indexCount) {
    if (!g_armed.load(std::memory_order_relaxed) || !ctx) return;
    if (!is_gun_draw(ctx, indexCount)) {
        restore(ctx);
        maybe_log();
        return;
    }
    ID3D11RasterizerState* cur = nullptr;
    ctx->RSGetState(&cur);
    if (cur && cur == g_applied) { // still ours from the gun's previous draw
        cur->Release();
        ++g_flipped;
        return;
    }
    restore(ctx); // (a stale one the game has since replaced: just let go of it)
    ID3D11RasterizerState* f = flipped_for(ctx, cur);
    if (!f) {
        if (cur) cur->Release();
        ++g_failed;
        return;
    }
    g_saved = cur; // the reference RSGetState gave
    g_applied = f;
    ctx->RSSetState(f);
    ++g_flipped;
}

void on_boundary(ID3D11DeviceContext* ctx) {
    if (!g_armed.load(std::memory_order_relaxed) || !ctx) return;
    restore(ctx);
}

} // namespace bvr::b1r::gun_flip
