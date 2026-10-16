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

    printf("hud-elements: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
