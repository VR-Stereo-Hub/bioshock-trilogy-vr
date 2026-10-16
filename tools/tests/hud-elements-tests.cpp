// hud-elements-tests.cpp - host tests for src/game/bioshock1r/hud_elements_plan.h, the BS1 HUD
// split into pieces on their own anchors. Run `.\tools\host-test.ps1 hud-elements`.
//
// What has to hold:
//   - every piece on the window = no plan (core keeps the single quad, unchanged)
//   - a hidden piece is one clear and no strip, no copy, one quad (the window)
//   - a moved piece: its place cleared, copied once into a strip below the capture that stays
//     inside the image, its quad samples exactly the copied pixels, the strip itself cleared
//   - the window quad keeps the capture's whole rectangle and the old placement
//   - hands: a piece fills the panel width at the grip plus the lift, faces the head (billboard),
//     two pieces on one hand stack upward without overlapping; an untracked hand shows nothing
//   - world: parked once in front of the head, stays put when the head turns, re-parks on request
//   - the layer budget: pieces beyond it stay on the window (not cleared, not lost)
#include "game/bioshock1r/hud_elements_plan.h"

#include <cmath>
#include <cstdio>

using namespace bvr::b1r::hudel;
namespace vr = bvr::vr;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}

static vr::HudElementFrame frame() {
    vr::HudElementFrame f;
    f.texW = 2000;
    f.texH = 2000;
    f.headValid = true;
    f.head[1] = 1.6f;
    f.handValid[0] = f.handValid[1] = true;
    f.hand[0][0] = -0.2f; f.hand[0][1] = 1.1f; f.hand[0][2] = -0.3f;
    f.hand[1][0] = 0.2f;  f.hand[1][1] = 1.1f; f.hand[1][2] = -0.3f;
    return f;
}

static void table(ElementCfg* e, int n) {
    for (int i = 0; i < n; ++i) {
        e[i] = ElementCfg{};
        e[i].rect[0] = 0.05f * i; e[i].rect[1] = 0.05f * i;
        e[i].rect[2] = 0.05f * i + 0.04f; e[i].rect[3] = 0.05f * i + 0.02f;
    }
}

int main() {
    const HandCfg hand;
    vr::HudElementPlan plan;
    PlanStats st;
    ElementCfg els[10];

    {   // all on the window: no plan
        table(els, 10);
        WorldPark park;
        check(!build_plan(els, 10, hand, &park, frame(), 8, &plan, &st), "all-window returns false");
    }
    {   // hidden: one clear, no strip
        table(els, 10);
        els[3].anchor = kOff;
        WorldPark park;
        check(build_plan(els, 10, hand, &park, frame(), 8, &plan, &st), "hidden builds a plan");
        check(plan.clears == 1 && plan.copies == 0 && plan.stripH == 0 && plan.quads == 1, "hidden ops",
              plan.clears * 100 + plan.copies * 10 + plan.quads, 101);
        check(plan.clear[0][0] == 300 && plan.clear[0][1] == 300 && plan.clear[0][2] == 380 &&
              plan.clear[0][3] == 340, "hidden clears its own pixels", plan.clear[0][2], 380);
        check(plan.quad[0].rect[2] == 2000 && plan.quad[0].rect[3] == 2000 && !plan.quad[0].local,
              "window quad keeps the whole capture, head-locked");
    }
    {   // one piece to the left hand
        table(els, 10);
        els[2].anchor = kHandL;
        WorldPark park;
        const auto f = frame();
        check(build_plan(els, 10, hand, &park, f, 8, &plan, &st), "hand builds a plan");
        check(plan.copies == 1 && plan.quads == 2 && plan.clears == 2, "hand ops",
              plan.copies * 100 + plan.quads * 10 + plan.clears, 122);
        const auto& c = plan.copy[0];
        check(c.dstY >= f.texH, "copy lands below the capture", c.dstY, f.texH);
        const uint32_t pw = c.src[2] - c.src[0], ph = c.src[3] - c.src[1];
        check(c.dstX + pw <= f.texW && c.dstY + ph <= f.texH + plan.stripH, "copy inside the image");
        const auto& q = plan.quad[1];
        check(q.rect[0] == c.dstX && q.rect[1] == c.dstY && q.rect[2] == c.dstX + pw && q.rect[3] == c.dstY + ph,
              "quad samples exactly the copy");
        check(plan.clear[1][1] == f.texH && plan.clear[1][3] == f.texH + plan.stripH, "the strip is cleared");
        check(std::fabs(q.widthM - hand.widthM) < 1e-6f, "fills the hand panel width", q.widthM, hand.widthM);
        check(std::fabs(q.heightM - hand.widthM * ph / pw) < 1e-5f, "keeps the piece's aspect", q.heightM,
              hand.widthM * ph / pw);
        // Billboard: the quad's +Z points at the head.
        const float z0[3] = {0, 0, 1};
        float z[3];
        bvr::xrmath::quat_rotate(q.quat[0], q.quat[1], q.quat[2], q.quat[3], z0, z);
        float to[3] = {f.head[0] - q.pos[0], f.head[1] - q.pos[1], f.head[2] - q.pos[2]};
        const float l = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        const float dot = (z[0] * to[0] + z[1] * to[1] + z[2] * to[2]) / l;
        check(dot > 0.99f, "billboard faces the head", dot, 1);
        check(q.pos[1] > f.hand[0][1] + hand.liftM - 0.001f, "sits above the grip by the lift", q.pos[1],
              f.hand[0][1] + hand.liftM);
        // Untracked: nothing shown, nothing stale.
        auto g = f;
        g.handValid[0] = false;
        build_plan(els, 10, hand, &park, g, 8, &plan, &st);
        check(plan.quads == 1 && st.dropped == 1, "untracked hand shows nothing", plan.quads, 1);
    }
    {   // two pieces on one hand stack without overlap
        table(els, 10);
        els[1].anchor = kHandR;
        els[4].anchor = kHandR;
        WorldPark park;
        build_plan(els, 10, hand, &park, frame(), 8, &plan, &st);
        const auto& a = plan.quad[1];
        const auto& b = plan.quad[2];
        // Along the panel's own up (a billboard below the eyes leans back).
        const float d[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
        const float gap = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - (a.heightM + b.heightM) * 0.5f;
        check(b.pos[1] > a.pos[1], "the second piece is above the first", b.pos[1], a.pos[1]);
        check(gap > 0.0f, "stacked pieces do not overlap", gap, 0.01);
        check(plan.copy[0].dstY + (plan.copy[0].src[3] - plan.copy[0].src[1]) <= plan.copy[1].dstY ||
              plan.copy[0].dstX + (plan.copy[0].src[2] - plan.copy[0].src[0]) <= plan.copy[1].dstX,
              "strip copies do not overlap");
    }
    {   // world: parked in front, stays when the head turns, re-parks on request
        table(els, 10);
        els[5].anchor = kWorld;
        WorldPark park;
        auto f = frame();
        build_plan(els, 10, hand, &park, f, 8, &plan, &st);
        check(park.valid, "world parks");
        const float z = plan.quad[1].pos[2];
        check(z < f.head[2] - 0.5f, "parked in front of the head", z, f.head[2] - f.winDist);
        bvr::xrmath::quat_axis_angle(0, 1, 0, 1.2f, f.headQ); // turn the head
        build_plan(els, 10, hand, &park, f, 8, &plan, &st);
        check(std::fabs(plan.quad[1].pos[2] - z) < 1e-5f, "stays when the head turns", plan.quad[1].pos[2], z);
        park.valid = false;
        build_plan(els, 10, hand, &park, f, 8, &plan, &st);
        check(std::fabs(plan.quad[1].pos[2] - z) > 0.1f, "re-parks where the head looks", plan.quad[1].pos[2], z);
    }
    {   // the layer budget: three quads allowed -> the window and two pieces
        table(els, 10);
        for (int i = 0; i < 5; ++i) els[i].anchor = kHandL;
        WorldPark park;
        build_plan(els, 10, hand, &park, frame(), 3, &plan, &st);
        check(plan.quads == 3 && st.dropped == 3, "budget caps the pieces", plan.quads, 3);
        check(plan.clears == 3, "dropped pieces are not cleared from the window", plan.clears, 3);
    }

    {   // ownership: Health (big, low priority) moved, EVE (inside it, owns) stays on the panel
        ElementCfg e[2];
        e[0].rect[0] = 0.04f; e[0].rect[1] = 0.05f; e[0].rect[2] = 0.32f; e[0].rect[3] = 0.20f;
        e[0].priority = 6; e[0].anchor = kHandL;
        e[1].rect[0] = 0.10f; e[1].rect[1] = 0.13f; e[1].rect[2] = 0.31f; e[1].rect[3] = 0.17f;
        e[1].priority = 5;
        WorldPark park;
        build_plan(e, 2, hand, &park, frame(), 8, &plan, &st);
        // The panel: Health's place cleared, EVE's pixels put back exactly where they were.
        bool restored = false;
        for (int i = 0; i < plan.copies; ++i)
            restored |= plan.copy[i].src[0] == 200 && plan.copy[i].src[1] == 260 && plan.copy[i].dstX == 200 &&
                        plan.copy[i].dstY == 260 && plan.copy[i].src[2] == 620 && plan.copy[i].src[3] == 340;
        check(restored && st.restored == 1, "the owner's pixels go back on the panel", st.restored, 1);
        // Health's copy: EVE's rectangle carved out at the same offset inside the strip copy.
        const auto& c = plan.copy[0];
        check(plan.postClears == 1 && st.carved == 1, "the owner is carved out of the moved copy", plan.postClears, 1);
        check(plan.postClear[0][0] == c.dstX + (200 - 80) && plan.postClear[0][1] == c.dstY + (260 - 100) &&
              plan.postClear[0][2] == c.dstX + (620 - 80) && plan.postClear[0][3] == c.dstY + (340 - 100),
              "carve lands on the owner's pixels in the copy", plan.postClear[0][0], c.dstX + 120);
        // Reverse: EVE moved, Health on the panel - nothing restored, nothing carved.
        e[0].anchor = kWindow; e[1].anchor = kHandR;
        build_plan(e, 2, hand, &park, frame(), 8, &plan, &st);
        check(st.restored == 0 && st.carved == 0 && plan.postClears == 0, "the owner moving needs no carve",
              st.carved + st.restored, 0);
    }
    {   // a piece on the panel with its own offset and scale gets its own quad, head-locked
        table(els, 10);
        els[6].offX = 0.1f; els[6].offY = -0.05f; els[6].scale = 2.0f;
        WorldPark park;
        auto f = frame();
        check(build_plan(els, 10, hand, &park, f, 8, &plan, &st), "placed piece builds a plan");
        const auto& q = plan.quad[1];
        const float mPerPx = f.winWidth / f.texW;
        const float cx = ((0.30f + 0.34f) * 0.5f - 0.5f) * f.winWidth + 0.1f;
        check(!q.local && std::fabs(q.pos[0] - cx) < 1e-3f, "offset on the panel plane", q.pos[0], cx);
        check(std::fabs(q.pos[2] + f.winDist) < 1e-6f, "at the panel's distance", q.pos[2], -f.winDist);
        check(std::fabs(q.widthM - 80 * mPerPx * 2.0f) < 1e-4f, "scaled", q.widthM, 80 * mPerPx * 2.0f);
    }
    {   // state rows: an inactive row is neither moved nor carves anything
        table(els, 10);
        els[2].anchor = kHandL;
        els[2].active = false;
        WorldPark park;
        check(!build_plan(els, 10, hand, &park, frame(), 8, &plan, &st), "an inactive moved row is no plan");
        els[2].active = true;
        els[3].active = false;
        els[3].rect[0] = 0.0f; els[3].rect[1] = 0.0f; els[3].rect[2] = 1.0f; els[3].rect[3] = 1.0f;
        els[3].priority = -1; // the wheel: owns everything while it is open
        build_plan(els, 10, hand, &park, frame(), 8, &plan, &st);
        check(st.carved == 0, "a closed wheel carves nothing", st.carved, 0);
        els[3].active = true;
        build_plan(els, 10, hand, &park, frame(), 8, &plan, &st);
        check(st.carved == 1, "an open wheel owns what it covers", st.carved, 1);
    }
    {   // a piece made of columns (EVE: bar + gauge staircase): its own pixels only, both ways
        ElementCfg e[2];
        e[0].rect[0] = 0.10f; e[0].rect[1] = 0.10f; e[0].rect[2] = 0.30f; e[0].rect[3] = 0.20f;
        e[0].priority = 6;                                  // Health: a plain box
        e[1].rect[0] = 0.10f; e[1].rect[1] = 0.15f; e[1].rect[2] = 0.30f; e[1].rect[3] = 0.25f;
        e[1].priority = 5;                                  // EVE: two columns inside its box
        e[1].nParts = 2;
        const float p0[4] = {0.10f, 0.18f, 0.15f, 0.25f}, p1[4] = {0.15f, 0.15f, 0.30f, 0.20f};
        for (int k = 0; k < 4; ++k) { e[1].parts[0][k] = p0[k]; e[1].parts[1][k] = p1[k]; }
        e[1].anchor = kHandL;
        WorldPark park;
        build_plan(e, 2, hand, &park, frame(), 8, &plan, &st);
        check(plan.clears == 3, "the panel loses only the columns (+ the strip)", plan.clears, 3);
        // Its copy is the box; what is not a column is carved: above column 0 and below column 1.
        check(plan.postClears == 2, "the rest of its box is carved from the copy", plan.postClears, 2);
        const auto& c = plan.copy[0];
        check(plan.postClear[0][0] == c.dstX && plan.postClear[0][1] == c.dstY &&
              plan.postClear[0][2] == c.dstX + 100 && plan.postClear[0][3] == c.dstY + 60,
              "above the first column", plan.postClear[0][3], c.dstY + 60);
        check(plan.postClear[1][0] == c.dstX + 100 && plan.postClear[1][1] == c.dstY + 100 &&
              plan.postClear[1][2] == c.dstX + 400 && plan.postClear[1][3] == c.dstY + 200,
              "below the second column", plan.postClear[1][1], c.dstY + 100);
        // Health moved instead: the columns inside its box go back on the panel, not the box.
        e[1].anchor = kWindow; e[0].anchor = kHandR;
        build_plan(e, 2, hand, &park, frame(), 8, &plan, &st);
        int restores = 0;
        for (int i = 1; i < plan.copies; ++i) ++restores;
        check(restores == 2 && plan.postClears == 2, "each column it covers restored and carved", restores, 2);
    }
    {   // trims: cut from the moved copy, cleared from the panel with the box, shown nowhere
        ElementCfg e[1];
        e[0].rect[0] = 0.10f; e[0].rect[1] = 0.10f; e[0].rect[2] = 0.30f; e[0].rect[3] = 0.20f;
        e[0].nTrims = 1;
        const float t[4] = {0.10f, 0.18f, 0.12f, 0.30f}; // hangs below the box: clipped to it
        for (int k = 0; k < 4; ++k) e[0].trims[0][k] = t[k];
        e[0].anchor = kHandL;
        WorldPark park;
        build_plan(e, 1, hand, &park, frame(), 8, &plan, &st);
        const auto& c = plan.copy[0];
        check(plan.postClears == 1 && plan.postClear[0][0] == c.dstX && plan.postClear[0][1] == c.dstY + 160 &&
              plan.postClear[0][2] == c.dstX + 40 && plan.postClear[0][3] == c.dstY + 200,
              "the trim is cut from the copy, inside its box", plan.postClear[0][1], c.dstY + 160);
        check(plan.clear[0][0] == 200 && plan.clear[0][3] == 400, "the panel still loses the whole box",
              plan.clear[0][3], 400);
        e[0].anchor = kWindow;
        check(!build_plan(e, 1, hand, &park, frame(), 8, &plan, &st), "on the panel a trim does nothing");
    }
    {   // the hand dial: the ring posed at the dial, the name above it, the panel cleared
        const auto f = frame();
        const float ring[4] = {0.3650f, 0.2000f, 0.6600f, 0.7170f};
        const float name[4] = {0.3600f, 0.1250f, 0.6700f, 0.1780f};
        const float centre[3] = {0.2f, 1.1f, -0.4f};
        float q[4];
        bvr::xrmath::quat_axis_angle(0, 1, 0, 0.6f, q); // turned and upright
        ElementCfg e[2];
        for (int k = 0; k < 4; ++k) { e[0].rect[k] = ring[k]; e[1].rect[k] = name[k]; }
        e[0].priority = e[1].priority = -1;
        dial_pose(ring, ring, f.texW, f.texH, centre, q, 0.35f, &e[0]);
        dial_pose(ring, name, f.texW, f.texH, centre, q, 0.35f, &e[1]);
        e[1].anchor = kOff; // a posed piece is shown whatever its panel anchor says
        WorldPark park;
        check(build_plan(e, 2, hand, &park, f, 8, &plan, &st), "the dial builds a plan");
        check(plan.quads == 3 && st.moved == 2, "the ring and the name each get a quad", plan.quads, 3);
        const auto& r = plan.quad[1];
        const auto& n = plan.quad[2];
        check(r.local && std::fabs(r.pos[0] - centre[0]) < 1e-5f && std::fabs(r.pos[1] - centre[1]) < 1e-5f &&
              std::fabs(r.pos[2] - centre[2]) < 1e-5f, "the ring is centred on the dial");
        check(std::fabs(r.widthM - 0.35f) < 0.003f, "the ring is the dial's width", r.widthM, 0.35);
        check(std::fabs(r.quat[1] - q[1]) < 1e-6f && std::fabs(r.quat[3] - q[3]) < 1e-6f, "the dial's orientation");
        const float u0[3] = {0, 1, 0};
        float up[3];
        bvr::xrmath::quat_rotate(q[0], q[1], q[2], q[3], u0, up);
        const float above = (n.pos[0] - r.pos[0]) * up[0] + (n.pos[1] - r.pos[1]) * up[1] + (n.pos[2] - r.pos[2]) * up[2];
        const float want = ((ring[1] + ring[3]) - (name[1] + name[3])) * 0.5f * f.texH * (0.35f / ((ring[2] - ring[0]) * f.texW));
        check(std::fabs(above - want) < 1e-4f && above > r.heightM * 0.5f, "the name sits above the ring, as drawn",
              above, want);
        check(std::fabs(n.widthM / r.widthM - (name[2] - name[0]) / (ring[2] - ring[0])) < 0.01f,
              "the name keeps its size against the ring");
        check(plan.clears >= 2, "both leave the panel", plan.clears, 2);
    }
    {   // a riding menu in the world: one quad, the whole capture, parked once
        WorldPark park;
        auto f = frame();
        check(build_screen_plan(&park, f, &plan) && plan.quads == 1 && plan.quad[0].local &&
              plan.quad[0].rect[2] == f.texW && plan.quad[0].rect[3] == f.texH && plan.clears == 0,
              "screen plan is the whole capture, world-locked");
    }

    printf("hud-elements: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
