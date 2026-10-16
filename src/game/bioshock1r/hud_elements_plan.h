#pragma once
// BS1 HUD elements: the PURE layout planner (no D3D calls, no engine, no
// globals), shared by the DLL and tools/tests. Given the element table's
// anchors and rectangles, the HUD capture's size, the head and the grips, it
// writes the plan core/vr runs on the HUD swapchain image (HudElementPlan):
// which rectangles move into the strip below the capture, which places are
// cleared, and one quad per piece.
//
// Cost by construction: a loop over at most kElementCount rows per present, no
// allocation, no search. Everything left on the window costs NOTHING extra -
// it is the same single head-locked quad as before, and with every element on
// the window the planner returns false and core takes the old path unchanged.

#include "core/util/xr_math.h"
#include "core/vr/openxr_runtime.h"

#include <cmath>
#include <cstdint>

namespace bvr::b1r::hudel {

enum Anchor { kWindow = 0, kOff, kWorld, kHandL, kHandR, kAnchorCount };

struct ElementCfg {
    int anchor = kWindow;
    float rect[4] = {}; // x0,y0,x1,y1, normalised to the capture, y down
};
struct HandCfg {
    float widthM = 0.22f; // Dishonored's tuned wrist panel
    float liftM = 0.06f;  // along world up, above the grip
    bool watch = false;   // false = billboard (faces the head, never rolls)
    float tiltDeg = 0.0f; // watch only: nod toward the eyes
};
struct WorldPark {
    bool valid = false;
    float pos[3] = {};          // the window's centre, XR LOCAL
    float quat[4] = {0, 0, 0, 1}; // yaw only: the window faces where the head faced
};
struct PlanStats {
    int moved = 0, hidden = 0, dropped = 0; // dropped = no quad left in the layer budget
};

// +Z of the result points along f (unit), zero roll - what a quad layer needs
// to face the eyes. Same construction as core's quat_facing.
inline void facing_quat(const float f[3], float q[4]) {
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(f[1]) > 0.999f) { up[0] = 1.0f; up[1] = 0.0f; }
    float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) rl = 1.0f;
    r[0] /= rl; r[1] /= rl; r[2] /= rl;
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const float m00 = r[0], m01 = u[0], m02 = f[0], m10 = r[1], m11 = u[1], m12 = f[1],
                m20 = r[2], m21 = u[2], m22 = f[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
}

// The element's pixel rectangle in the capture; false = unset or degenerate.
inline bool pixel_rect(const ElementCfg& e, uint32_t w, uint32_t h, uint32_t out[4]) {
    float r[4] = {e.rect[0], e.rect[1], e.rect[2], e.rect[3]};
    for (float& v : r) v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    out[0] = static_cast<uint32_t>(r[0] * w + 0.5f);
    out[1] = static_cast<uint32_t>(r[1] * h + 0.5f);
    out[2] = static_cast<uint32_t>(r[2] * w + 0.5f);
    out[3] = static_cast<uint32_t>(r[3] * h + 0.5f);
    return out[2] > out[0] + 1 && out[3] > out[1] + 1;
}

// Park the world window where the head is looking now (yaw only).
inline void park_world(const vr::HudElementFrame& f, WorldPark* park) {
    const float fwd0[3] = {0.0f, 0.0f, -1.0f};
    float fwd[3];
    xrmath::quat_rotate(f.headQ[0], f.headQ[1], f.headQ[2], f.headQ[3], fwd0, fwd);
    fwd[1] = 0.0f;
    float l = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (l < 1e-4f) { fwd[0] = 0.0f; fwd[2] = -1.0f; l = 1.0f; }
    fwd[0] /= l; fwd[2] /= l;
    park->pos[0] = f.head[0] + fwd[0] * f.winDist;
    park->pos[1] = f.head[1] + f.winUp;
    park->pos[2] = f.head[2] + fwd[2] * f.winDist;
    const float back[3] = {-fwd[0], 0.0f, -fwd[2]}; // the quad's +Z faces the viewer
    facing_quat(back, park->quat);
    park->valid = true;
}

// Writes the plan. False = nothing is moved or hidden: take the single quad.
inline bool build_plan(const ElementCfg* els, int count, const HandCfg& hand, WorldPark* park,
                       const vr::HudElementFrame& f, int maxQuads, vr::HudElementPlan* out,
                       PlanStats* stats) {
    *out = vr::HudElementPlan{};
    *stats = PlanStats{};
    const uint32_t W = f.texW, H = f.texH;
    if (!W || !H || maxQuads < 1) return false;

    // Pass 1: what moves, what hides. Shelf-pack the moved pieces into a strip
    // below the capture, as wide as the capture, in table order.
    struct Piece { int el; uint32_t src[4]; uint32_t dstX, dstY; };
    Piece pieces[vr::kMaxHudElementQuads];
    int nPieces = 0;
    uint32_t shelfX = 0, shelfY = 0, shelfH = 0;
    const uint32_t kPad = 2; // keeps bilinear sampling inside each piece
    for (int i = 0; i < count; ++i) {
        const ElementCfg& e = els[i];
        if (e.anchor == kWindow) continue;
        uint32_t px[4];
        if (!pixel_rect(e, W, H, px)) continue;
        if (e.anchor == kOff) {
            if (out->clears + 1 < vr::kMaxHudElementOps) { // one op stays for the strip
                for (int k = 0; k < 4; ++k) out->clear[out->clears][k] = px[k];
                ++out->clears;
                ++stats->hidden;
            }
            continue;
        }
        // One quad stays for the window itself; one slot of ops for the strip clear.
        if (nPieces + 1 >= maxQuads || out->clears + 1 >= vr::kMaxHudElementOps) {
            ++stats->dropped;
            continue;
        }
        const uint32_t pw = px[2] - px[0], ph = px[3] - px[1];
        if (shelfX + pw > W) { shelfY += shelfH + kPad; shelfX = 0; shelfH = 0; }
        Piece& p = pieces[nPieces++];
        p.el = i;
        for (int k = 0; k < 4; ++k) p.src[k] = px[k];
        p.dstX = shelfX;
        p.dstY = H + kPad + shelfY;
        shelfX += pw + kPad;
        if (ph > shelfH) shelfH = ph;
        for (int k = 0; k < 4; ++k) out->clear[out->clears][k] = px[k];
        ++out->clears;
    }
    if (!nPieces && !out->clears) return false;

    if (nPieces) {
        // Rounded up so small layout edits do not recreate the swapchain.
        const uint32_t need = kPad + shelfY + shelfH + kPad;
        out->stripH = (need + 63u) & ~63u;
        const uint32_t strip[4] = {0, H, W, H + out->stripH};
        for (int k = 0; k < 4; ++k) out->clear[out->clears][k] = strip[k];
        ++out->clears;
    }

    // The window: the capture minus what moved or hid, exactly where it was.
    const float winH = f.winWidth * static_cast<float>(H) / static_cast<float>(W);
    vr::HudElementQuad& win = out->quad[out->quads++];
    win.rect[0] = 0; win.rect[1] = 0; win.rect[2] = W; win.rect[3] = H;
    win.local = false;
    win.pos[0] = 0.0f; win.pos[1] = f.winUp; win.pos[2] = -f.winDist;
    win.widthM = f.winWidth;
    win.heightM = winH;
    const float mPerPx = f.winWidth / static_cast<float>(W);

    float stackM[2] = {0.0f, 0.0f}; // how far up each hand's stack has grown
    for (int i = 0; i < nPieces; ++i) {
        const Piece& p = pieces[i];
        const ElementCfg& e = els[p.el];
        const uint32_t pw = p.src[2] - p.src[0], ph = p.src[3] - p.src[1];
        vr::HudElementCopy& c = out->copy[out->copies++];
        for (int k = 0; k < 4; ++k) c.src[k] = p.src[k];
        c.dstX = p.dstX;
        c.dstY = p.dstY;
        vr::HudElementQuad& q = out->quad[out->quads];
        q.rect[0] = p.dstX; q.rect[1] = p.dstY; q.rect[2] = p.dstX + pw; q.rect[3] = p.dstY + ph;
        q.local = true;
        if (e.anchor == kWorld) {
            if (!park->valid) {
                if (!f.headValid) { ++stats->dropped; continue; }
                park_world(f, park);
            }
            // Its own place on the window, on the parked window.
            const float cx = (static_cast<float>(p.src[0] + p.src[2]) * 0.5f / W - 0.5f) * f.winWidth;
            const float cy = -(static_cast<float>(p.src[1] + p.src[3]) * 0.5f / H - 0.5f) * winH;
            const float off[3] = {cx, cy, 0.0f};
            float o[3];
            xrmath::quat_rotate(park->quat[0], park->quat[1], park->quat[2], park->quat[3], off, o);
            for (int k = 0; k < 3; ++k) q.pos[k] = park->pos[k] + o[k];
            for (int k = 0; k < 4; ++k) q.quat[k] = park->quat[k];
            q.widthM = pw * mPerPx;
            q.heightM = ph * mPerPx;
        } else {
            const int h = e.anchor == kHandL ? 0 : 1;
            if (!f.handValid[h] || !f.headValid) { ++stats->dropped; continue; } // no stale panel
            // On a hand a piece fills the panel's width; pieces stack upward.
            q.widthM = hand.widthM;
            q.heightM = hand.widthM * static_cast<float>(ph) / static_cast<float>(pw);
            float base[3] = {f.hand[h][0], f.hand[h][1] + hand.liftM, f.hand[h][2]};
            if (hand.watch) {
                float tilt[4];
                xrmath::quat_axis_angle(1.0f, 0.0f, 0.0f, (-90.0f + hand.tiltDeg) * 0.01745329f, tilt);
                xrmath::quat_mul(f.handQ[h], tilt, q.quat);
            } else {
                float toHead[3] = {f.head[0] - base[0], f.head[1] - base[1], f.head[2] - base[2]};
                const float l = std::sqrt(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
                if (l < 0.02f) { ++stats->dropped; continue; } // inside the head
                for (float& v : toHead) v /= l;
                facing_quat(toHead, q.quat);
            }
            const float up0[3] = {0.0f, 1.0f, 0.0f};
            float up[3];
            xrmath::quat_rotate(q.quat[0], q.quat[1], q.quat[2], q.quat[3], up0, up);
            const float along = stackM[h] + q.heightM * 0.5f;
            for (int k = 0; k < 3; ++k) q.pos[k] = base[k] + up[k] * along;
            stackM[h] += q.heightM + 0.01f;
        }
        ++out->quads;
        ++stats->moved;
    }
    return true;
}

} // namespace bvr::b1r::hudel
