#pragma once
// BS1 HUD elements: the PURE layout planner (no D3D calls, no engine, no
// globals), shared by the DLL and tools/tests. Given the element table's
// anchors, rectangles and placements, the HUD capture's size, the head and the
// grips, it writes the plan core/vr runs on the HUD swapchain image
// (HudElementPlan): which rectangles move into the strip below the capture,
// which places are cleared, what is put back or carved out where two pieces'
// rectangles overlap, and one quad per piece.
//
// OVERLAPS have one owner: the row with the lower `priority` number (the
// crosshair before the prompt before the subtitles ...). The owner's pixels
// go wherever the owner goes; the other row's copy has them carved out, and if
// the owner stays on the panel they are put back there. That is how Health
// keeps both hypo gauges (one bitmap in the game, HUD_HealthEve_Cover, whose
// rims interlock - no straight cut separates them) while EVE is the blue bar.
//
// Cost by construction: loops over at most kElementCount rows per present, no
// allocation, no search. With nothing moved or hidden the planner returns
// false and core takes the old single quad, unchanged.

#include "core/util/xr_math.h"
#include "core/vr/openxr_runtime.h"

#include <cmath>
#include <cstdint>

namespace bvr::b1r::hudel {

enum Anchor { kWindow = 0, kOff, kWorld, kHandL, kHandR, kAnchorCount };
constexpr int kMaxRows = 24;
constexpr int kMaxParts = 72;
constexpr int kMaxTrims = 24;

struct ElementCfg {
    int anchor = kWindow;
    float rect[4] = {};   // x0,y0,x1,y1, normalised to the capture, y down
    // A piece that is not a rectangle: the rectangles it is made of, as
    // columns sorted by x (a column may hold several runs, sorted by y, sharing
    // its exact x span), inside `rect`. Zero = the whole rect. The EVE piece is
    // its bar plus its hypo gauge traced at one-pixel columns, which interlocks
    // with the first-aid gauge in one bitmap.
    int nParts = 0;
    float parts[kMaxParts][4] = {};
    // Trimmed off when the piece moves: cut from its copy, and (inside its box)
    // cleared from the panel with the rest, so they show nowhere. Health trims
    // the two bits of its gauge that hang below the ring, to sit flush.
    int nTrims = 0;
    float trims[kMaxTrims][4] = {};
    float offX = 0.0f;    // metres: right on the panel/world plane or across the hand panel
    float offY = 0.0f;    // metres: up
    float scale = 1.0f;
    int priority = 0;     // lower owns an overlap
    bool active = true;   // false = the row does not exist this frame (game state)
};
struct HandCfg {
    float widthM = 0.22f; // Dishonored's tuned wrist panel
    float liftM = 0.06f;  // along world up, above the grip
    bool watch = false;   // false = billboard (faces the head, never rolls)
    float tiltDeg = 0.0f; // watch only: nod toward the eyes
};
struct WorldPark {
    bool valid = false;
    float pos[3] = {};            // the window's centre, XR LOCAL
    float quat[4] = {0, 0, 0, 1}; // yaw only: the window faces where the head faced
};
struct PlanStats {
    int moved = 0, hidden = 0, dropped = 0; // dropped = no quad or op left, or hand untracked
    int carved = 0, restored = 0;
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
inline bool pixel_rect(const ElementCfg& e, uint32_t w, uint32_t h, uint32_t out[4], uint32_t minPx = 2) {
    float r[4] = {e.rect[0], e.rect[1], e.rect[2], e.rect[3]};
    for (float& v : r) v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    out[0] = static_cast<uint32_t>(r[0] * w + 0.5f);
    out[1] = static_cast<uint32_t>(r[1] * h + 0.5f);
    out[2] = static_cast<uint32_t>(r[2] * w + 0.5f);
    out[3] = static_cast<uint32_t>(r[3] * h + 0.5f);
    return out[2] >= out[0] + minPx && out[3] >= out[1] + minPx;
}

inline bool intersect(const uint32_t a[4], const uint32_t b[4], uint32_t out[4]) {
    out[0] = a[0] > b[0] ? a[0] : b[0];
    out[1] = a[1] > b[1] ? a[1] : b[1];
    out[2] = a[2] < b[2] ? a[2] : b[2];
    out[3] = a[3] < b[3] ? a[3] : b[3];
    return out[2] > out[0] && out[3] > out[1];
}

// A row owns its overlap with another when its priority number is lower
// (ties: the earlier row).
inline bool owns(const ElementCfg* els, int a, int b) {
    return els[a].priority < els[b].priority || (els[a].priority == els[b].priority && a < b);
}

// Moved = it gets its own quad. On the panel with a placement of its own
// counts, since it no longer sits where the game drew it.
inline bool is_moved(const ElementCfg& e) {
    if (e.anchor == kWorld || e.anchor == kHandL || e.anchor == kHandR) return true;
    return e.anchor == kWindow && (e.offX != 0.0f || e.offY != 0.0f || e.scale != 1.0f);
}

// Park a world window where the head is looking now (yaw only).
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

// A riding screen (pause menu, map, a machine) shown in the world: the whole
// capture as one quad, parked where the head looked when it opened.
inline bool build_screen_plan(WorldPark* park, const vr::HudElementFrame& f, vr::HudElementPlan* out) {
    *out = vr::HudElementPlan{};
    if (!f.texW || !f.texH) return false;
    if (!park->valid) {
        if (!f.headValid) return false;
        park_world(f, park);
    }
    vr::HudElementQuad& q = out->quad[out->quads++];
    q.rect[0] = 0; q.rect[1] = 0; q.rect[2] = f.texW; q.rect[3] = f.texH;
    q.local = true;
    for (int k = 0; k < 3; ++k) q.pos[k] = park->pos[k];
    for (int k = 0; k < 4; ++k) q.quat[k] = park->quat[k];
    q.widthM = f.winWidth;
    q.heightM = f.winWidth * static_cast<float>(f.texH) / static_cast<float>(f.texW);
    return true;
}

// Writes the plan. False = nothing is moved or hidden: take the single quad.
inline bool build_plan(const ElementCfg* els, int count, const HandCfg& hand, WorldPark* park,
                       const vr::HudElementFrame& f, int maxQuads, vr::HudElementPlan* out,
                       PlanStats* stats) {
    *out = vr::HudElementPlan{};
    *stats = PlanStats{};
    const uint32_t W = f.texW, H = f.texH;
    if (!W || !H || maxQuads < 1 || count > kMaxRows) return false;
    constexpr int kOps = vr::kMaxHudElementOps;

    // Each row: its bounding box (what is copied and what the quad shows) and
    // its region (the rectangles it owns: the box itself, or its columns).
    uint32_t px[kMaxRows][4];
    uint32_t reg[kMaxRows][kMaxParts][4];
    int nReg[kMaxRows];
    bool valid[kMaxRows], moved[kMaxRows], gone[kMaxRows]; // gone = moved or hidden
    uint32_t dst[kMaxRows][2];                              // strip position of a moved piece
    for (int i = 0; i < count; ++i) {
        moved[i] = gone[i] = false;
        nReg[i] = 0;
        valid[i] = els[i].active && pixel_rect(els[i], W, H, px[i]);
        if (!valid[i]) continue;
        const int parts = els[i].nParts < kMaxParts ? els[i].nParts : kMaxParts;
        if (parts <= 0) {
            for (int k = 0; k < 4; ++k) reg[i][0][k] = px[i][k];
            nReg[i] = 1;
            continue;
        }
        for (int j = 0; j < parts; ++j) {
            ElementCfg part;
            for (int k = 0; k < 4; ++k) part.rect[k] = els[i].parts[j][k];
            uint32_t r[4];
            if (pixel_rect(part, W, H, r, 1) && intersect(r, px[i], reg[i][nReg[i]])) ++nReg[i];
        }
        valid[i] = nReg[i] > 0;
    }
    auto add = [&](uint32_t (*list)[4], int* n, const uint32_t r[4]) {
        if (*n >= kOps) { ++stats->dropped; return false; }
        for (int k = 0; k < 4; ++k) list[*n][k] = r[k];
        ++*n;
        return true;
    };
    // A rectangle of row p's copy, at its place in the strip.
    auto in_strip = [&](int p, const uint32_t r[4], uint32_t o[4]) {
        o[0] = dst[p][0] + (r[0] - px[p][0]);
        o[1] = dst[p][1] + (r[1] - px[p][1]);
        o[2] = dst[p][0] + (r[2] - px[p][0]);
        o[3] = dst[p][1] + (r[3] - px[p][1]);
    };

    // Pass 1: hide and move, shelf-packing the moved pieces into a strip below
    // the capture, in table order. Ops stay reserved for the strip clear.
    uint32_t shelfX = 0, shelfY = 0, shelfH = 0;
    const uint32_t kPad = 2; // keeps bilinear sampling inside each piece
    int nMoved = 0;
    for (int i = 0; i < count; ++i) {
        if (!valid[i]) continue;
        const ElementCfg& e = els[i];
        const bool hide = e.anchor == kOff;
        if (!hide && !is_moved(e)) continue;
        // One quad stays for the window itself; the clears must fit whole.
        if (out->clears + nReg[i] + 1 >= kOps || (!hide && (nMoved + 1 >= maxQuads || out->copies >= kOps))) {
            ++stats->dropped;
            continue;
        }
        for (int j = 0; j < nReg[i]; ++j) add(out->clear, &out->clears, reg[i][j]);
        gone[i] = true;
        if (hide) { ++stats->hidden; continue; }
        const uint32_t pw = px[i][2] - px[i][0], ph = px[i][3] - px[i][1];
        if (shelfX + pw > W) { shelfY += shelfH + kPad; shelfX = 0; shelfH = 0; }
        dst[i][0] = shelfX;
        dst[i][1] = H + kPad + shelfY;
        shelfX += pw + kPad;
        if (ph > shelfH) shelfH = ph;
        vr::HudElementCopy& c = out->copy[out->copies++];
        for (int k = 0; k < 4; ++k) c.src[k] = px[i][k];
        c.dstX = dst[i][0];
        c.dstY = dst[i][1];
        moved[i] = true;
        ++nMoved;
    }
    if (!out->clears) return false;
    if (nMoved) {
        // Rounded up so small layout edits do not recreate the swapchain.
        const uint32_t need = kPad + shelfY + shelfH + kPad;
        out->stripH = (need + 63u) & ~63u;
        const uint32_t strip[4] = {0, H, W, H + out->stripH};
        add(out->clear, &out->clears, strip);
    }

    // Pass 2: a moved piece's copy is its bounding box; carve out of it
    // whatever is not its own region. Parts come as columns sorted by x, a
    // column possibly holding several runs sorted by y: carve the gaps between
    // columns, and above, between and below the runs of each column.
    for (int p = 0; p < count; ++p) {
        if (!moved[p] || els[p].nParts <= 0) continue;
        uint32_t x = px[p][0], r[4], o[4];
        auto carve = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
            if (x1 <= x0 || y1 <= y0) return;
            r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
            in_strip(p, r, o);
            add(out->postClear, &out->postClears, o);
        };
        int j = 0;
        while (j < nReg[p]) {
            const uint32_t cx0 = reg[p][j][0], cx1 = reg[p][j][2];
            carve(x, px[p][1], cx0, px[p][3]); // the gap before this column
            uint32_t y = px[p][1];
            for (; j < nReg[p] && reg[p][j][0] == cx0 && reg[p][j][2] == cx1; ++j) {
                carve(cx0, y, cx1, reg[p][j][1]);
                y = reg[p][j][3];
            }
            carve(cx0, y, cx1, px[p][3]);
            x = cx1;
        }
        carve(x, px[p][1], px[p][2], px[p][3]);
    }

    // Trims: cut out of a moved piece's copy.
    for (int p = 0; p < count; ++p) {
        if (!moved[p]) continue;
        for (int j = 0; j < els[p].nTrims && j < kMaxTrims; ++j) {
            ElementCfg t;
            for (int k = 0; k < 4; ++k) t.rect[k] = els[p].trims[j][k];
            uint32_t r[4], c[4], o[4];
            if (!pixel_rect(t, W, H, r, 1) || !intersect(r, px[p], c)) continue;
            in_strip(p, c, o);
            add(out->postClear, &out->postClears, o);
        }
    }

    // Pass 3: overlaps. Where a gone piece P's region overlaps the region of a
    // row Q that owns the overlap: carve it out of P's copy, and put it back on
    // the panel if Q stays there.
    for (int p = 0; p < count; ++p) {
        if (!gone[p]) continue;
        for (int q = 0; q < count; ++q) {
            if (q == p || !valid[q] || !owns(els, q, p)) continue;
            bool any = false;
            for (int a = 0; a < nReg[p]; ++a)
                for (int b = 0; b < nReg[q]; ++b) {
                    uint32_t r[4];
                    if (!intersect(reg[p][a], reg[q][b], r)) continue;
                    any = true;
                    if (!gone[q]) {
                        if (out->copies >= kOps) { ++stats->dropped; continue; }
                        vr::HudElementCopy& c = out->copy[out->copies++];
                        for (int k = 0; k < 4; ++k) c.src[k] = r[k];
                        c.dstX = r[0];
                        c.dstY = r[1];
                    }
                    if (moved[p]) {
                        uint32_t o[4];
                        in_strip(p, r, o);
                        add(out->postClear, &out->postClears, o);
                    }
                }
            if (any) {
                if (!gone[q]) ++stats->restored;
                if (moved[p]) ++stats->carved;
            }
        }
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
    for (int i = 0; i < count; ++i) {
        if (!moved[i]) continue;
        const ElementCfg& e = els[i];
        const uint32_t pw = px[i][2] - px[i][0], ph = px[i][3] - px[i][1];
        vr::HudElementQuad& q = out->quad[out->quads];
        q.rect[0] = dst[i][0]; q.rect[1] = dst[i][1]; q.rect[2] = dst[i][0] + pw; q.rect[3] = dst[i][1] + ph;
        const float scale = e.scale > 0.05f ? e.scale : 0.05f;
        if (e.anchor == kWindow || e.anchor == kWorld) {
            // Its own place on the panel, plus its own offset, at its own scale.
            const float cx = (static_cast<float>(px[i][0] + px[i][2]) * 0.5f / W - 0.5f) * f.winWidth + e.offX;
            const float cy = -(static_cast<float>(px[i][1] + px[i][3]) * 0.5f / H - 0.5f) * winH + e.offY;
            q.widthM = pw * mPerPx * scale;
            q.heightM = ph * mPerPx * scale;
            if (e.anchor == kWindow) {
                q.local = false;
                q.pos[0] = cx; q.pos[1] = f.winUp + cy; q.pos[2] = -f.winDist;
            } else {
                if (!park->valid) {
                    if (!f.headValid) { ++stats->dropped; continue; }
                    park_world(f, park);
                }
                const float off[3] = {cx, cy, 0.0f};
                float o[3];
                xrmath::quat_rotate(park->quat[0], park->quat[1], park->quat[2], park->quat[3], off, o);
                q.local = true;
                for (int k = 0; k < 3; ++k) q.pos[k] = park->pos[k] + o[k];
                for (int k = 0; k < 4; ++k) q.quat[k] = park->quat[k];
            }
        } else {
            const int h = e.anchor == kHandL ? 0 : 1;
            if (!f.handValid[h] || !f.headValid) { ++stats->dropped; continue; } // no stale panel
            // On a hand a piece fills the panel's width (times its scale); pieces stack upward.
            q.local = true;
            q.widthM = hand.widthM * scale;
            q.heightM = q.widthM * static_cast<float>(ph) / static_cast<float>(pw);
            const float base[3] = {f.hand[h][0], f.hand[h][1] + hand.liftM, f.hand[h][2]};
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
            const float up0[3] = {0.0f, 1.0f, 0.0f}, right0[3] = {1.0f, 0.0f, 0.0f};
            float up[3], right[3];
            xrmath::quat_rotate(q.quat[0], q.quat[1], q.quat[2], q.quat[3], up0, up);
            xrmath::quat_rotate(q.quat[0], q.quat[1], q.quat[2], q.quat[3], right0, right);
            const float along = stackM[h] + q.heightM * 0.5f + e.offY;
            for (int k = 0; k < 3; ++k) q.pos[k] = base[k] + up[k] * along + right[k] * e.offX;
            stackM[h] += q.heightM + 0.01f;
        }
        ++out->quads;
        ++stats->moved;
    }
    return true;
}

} // namespace bvr::b1r::hudel
