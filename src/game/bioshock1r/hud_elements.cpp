#include "hud_elements.h"
#include "hud_elements_plan.h"
#include "menu_theme.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace bvr::b1r::hud_elements {
namespace {
using S = menu::Setting;

// The pieces, normalised to the HUD capture (y down), in the F10 order.
// Derived from the HUD movie, not measured in a capture yet: HUDPC.swf's root
// placements in its 16:9 branch (SetRes shifts centreBox +160 and rightBox
// +320 on a 1280x719 stage) and FFDec's render of the movie with its imported
// art decoded. They assume the stage fills the capture; F10 > HUD > Element
// rectangles > Dump the HUD checks that, and the sliders there correct it.
// ENGINE_NOTES "The HUD's pieces" has each one's clip and ActionScript.
struct Row { const char* key; const char* name; float rect[4]; };
const Row kRows[] = {
    {"Health",    "Health bar",               {0.000f, 0.030f, 0.320f, 0.125f}}, // healthBar + hypo count
    {"Eve",       "EVE bar",                  {0.000f, 0.125f, 0.320f, 0.195f}}, // bioAmmoBar + hypo count
    {"Reticle",   "Game crosshair",           {0.460f, 0.450f, 0.540f, 0.560f}}, // centerBox.ReticleHolder
    {"Target",    "Enemy name and health",    {0.360f, 0.280f, 0.640f, 0.445f}}, // EnemyType, FrozenHealth
    {"Prompt",    "Use and hack prompts",     {0.545f, 0.360f, 0.850f, 0.620f}}, // centerBox.focusArea, ReRoll
    {"Equipment", "Weapon, ammo and plasmid", {0.000f, 0.740f, 0.330f, 0.970f}}, // ammoScrollBar, CurrentAbilityName
    {"Subtitles", "Subtitles",                {0.200f, 0.760f, 0.800f, 0.940f}}, // subtitleTextHolder, AreaNameMovie
    {"Tutorial",  "Tutorial messages",        {0.200f, 0.620f, 0.800f, 0.760f}}, // trainingTextHolder
    {"Objective", "Objective",                {0.680f, 0.000f, 1.000f, 0.150f}}, // chatBoxMovie.importantTextHolder
    {"Messages",  "Radio messages",           {0.600f, 0.150f, 1.000f, 0.330f}}, // chatBoxMovie.ChatMovie0-5
    {"Money",     "Money and ADAM",           {0.820f, 0.450f, 1.000f, 0.570f}}, // rightBox.CreditCount, AdamCount
    {"AudioLog",  "Audio log",                {0.800f, 0.860f, 1.000f, 0.970f}}, // rightBox.logHolder
    {"Escort",    "Little Sister and timers", {0.060f, 0.200f, 0.330f, 0.330f}}, // GathererHealth, NeedleHolder
};
constexpr int kCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
static_assert(static_cast<int>(S::HudElEscort) - static_cast<int>(S::HudElHealth) + 1 == kCount,
              "one F10 anchor per element row");

std::atomic<int> g_anchor[kCount] = {};
std::atomic<float> g_rect[kCount][4];
std::atomic<float> g_handWidth{0.22f}, g_handLift{0.06f}, g_handTilt{0.0f};
std::atomic<bool> g_handWatch{false};
std::atomic<bool> g_repark{false}, g_dumpRequest{false};

// Render thread only (the provider).
hudel::WorldPark g_park;
struct Cost { unsigned calls = 0, active = 0; double sumUs = 0, maxUs = 0; ULONGLONG since = 0; } g_cost;
hudel::PlanStats g_lastStats;
uint32_t g_lastStrip = 0;
int g_lastCopies = 0;
char g_dumpStatus[160] = "No dump yet.";

std::wstring ini_path() { return std::wstring(log::data_dir()) + L"\\hud-elements.ini"; }

void load_rects() {
    for (int i = 0; i < kCount; ++i)
        for (int k = 0; k < 4; ++k) g_rect[i][k].store(kRows[i].rect[k], std::memory_order_relaxed);
    const std::wstring path = ini_path();
    int overridden = 0;
    for (int i = 0; i < kCount; ++i) {
        wchar_t key[64], val[96] = {};
        swprintf_s(key, L"%hs", kRows[i].key);
        GetPrivateProfileStringW(L"Rect", key, L"", val, 96, path.c_str());
        float r[4];
        if (val[0] && swscanf_s(val, L"%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4 &&
            r[0] >= 0 && r[1] >= 0 && r[2] <= 1 && r[3] <= 1 && r[2] > r[0] && r[3] > r[1]) {
            for (int k = 0; k < 4; ++k) g_rect[i][k].store(r[k], std::memory_order_relaxed);
            ++overridden;
        }
    }
    if (overridden) BVR_LOG("[hudel] %d element rectangle(s) from hud-elements.ini", overridden);
}

bool save_rects() {
    const std::wstring path = ini_path();
    bool ok = true;
    for (int i = 0; i < kCount; ++i) {
        wchar_t key[64], val[96];
        swprintf_s(key, L"%hs", kRows[i].key);
        swprintf_s(val, L"%.4f,%.4f,%.4f,%.4f", g_rect[i][0].load(), g_rect[i][1].load(),
                   g_rect[i][2].load(), g_rect[i][3].load());
        ok &= WritePrivateProfileStringW(L"Rect", key, val, path.c_str()) != FALSE;
    }
    BVR_LOG("[hudel] element rectangles %s hud-elements.ini", ok ? "saved to" : "FAILED to save to");
    return ok;
}

// ---- One-shot HUD dump (F10 button; never runs unasked) ---------------------
// The processed capture (premultiplied) over grey as a BMP, the same with each
// element's rectangle outlined, and a text file with each rectangle's own
// content box and the box of anything no rectangle covers - the measurement
// the rectangles are corrected from. The readback stalls once, on the click.
bool write_bmp(const wchar_t* path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih;
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h); // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bgra.size());
    bool ok = fwrite(&fh, sizeof fh, 1, f) == 1 && fwrite(&ih, sizeof ih, 1, f) == 1 &&
              fwrite(bgra.data(), 1, bgra.size(), f) == bgra.size();
    ok &= fclose(f) == 0;
    return ok;
}

void dump_hud(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!rgba && !bgra) {
        sprintf_s(g_dumpStatus, "Dump refused: HUD format %u is not 8-bit RGBA.", static_cast<unsigned>(d.Format));
        return;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    const bool made = dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging));
    if (dev) dev->Release();
    if (!made) { sprintf_s(g_dumpStatus, "Dump failed: no staging texture."); return; }
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        staging->Release();
        sprintf_s(g_dumpStatus, "Dump failed: the readback did not map.");
        return;
    }
    const uint32_t W = d.Width, H = d.Height;
    std::vector<uint8_t> img(static_cast<size_t>(W) * H * 4), alpha(static_cast<size_t>(W) * H);
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* p = row + x * 4;
            const uint8_t r = rgba ? p[0] : p[2], g = p[1], b = rgba ? p[2] : p[0], a = p[3];
            const int bg = 96 * (255 - a) / 255; // premultiplied over grey
            uint8_t* o = &img[(static_cast<size_t>(y) * W + x) * 4];
            o[0] = static_cast<uint8_t>((std::min)(255, b + bg));
            o[1] = static_cast<uint8_t>((std::min)(255, g + bg));
            o[2] = static_cast<uint8_t>((std::min)(255, r + bg));
            o[3] = 255;
            alpha[static_cast<size_t>(y) * W + x] = a;
        }
    }
    ctx->Unmap(staging, 0);
    staging->Release();

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dir[MAX_PATH], base[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%s\\dumps", log::data_dir());
    CreateDirectoryW(dir, nullptr);
    swprintf_s(base, L"%s\\hud-%04u%02u%02u-%02u%02u%02u", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    swprintf_s(path, L"%s.bmp", base);
    bool ok = write_bmp(path, img, W, H);

    // Content boxes: inside each rectangle, and outside all of them.
    FILE* t = nullptr;
    swprintf_s(path, L"%s.txt", base);
    if (!_wfopen_s(&t, path, L"w") && t) {
        fprintf(t, "HUD capture %ux%u (premultiplied; content = alpha > 8)\n", W, H);
        fprintf(t, "element      rect (normalised x0,y0,x1,y1)   content inside it (normalised)\n");
        uint32_t px[kCount][4];
        bool have[kCount];
        for (int i = 0; i < kCount; ++i) {
            hudel::ElementCfg e;
            for (int k = 0; k < 4; ++k) e.rect[k] = g_rect[i][k].load(std::memory_order_relaxed);
            have[i] = hudel::pixel_rect(e, W, H, px[i]);
            uint32_t bx0 = W, by0 = H, bx1 = 0, by1 = 0;
            if (have[i])
                for (uint32_t y = px[i][1]; y < px[i][3]; ++y)
                    for (uint32_t x = px[i][0]; x < px[i][2]; ++x)
                        if (alpha[static_cast<size_t>(y) * W + x] > 8) {
                            if (x < bx0) bx0 = x; if (y < by0) by0 = y;
                            if (x + 1 > bx1) bx1 = x + 1; if (y + 1 > by1) by1 = y + 1;
                        }
            fprintf(t, "%-12s %.3f,%.3f,%.3f,%.3f   ", kRows[i].key, e.rect[0], e.rect[1], e.rect[2], e.rect[3]);
            if (bx1 > bx0) fprintf(t, "%.3f,%.3f,%.3f,%.3f\n", float(bx0) / W, float(by0) / H, float(bx1) / W, float(by1) / H);
            else fprintf(t, "empty\n");
        }
        uint32_t ux0 = W, uy0 = H, ux1 = 0, uy1 = 0, unclaimed = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                if (alpha[static_cast<size_t>(y) * W + x] <= 8) continue;
                bool in = false;
                for (int i = 0; i < kCount && !in; ++i)
                    in = have[i] && x >= px[i][0] && x < px[i][2] && y >= px[i][1] && y < px[i][3];
                if (in) continue;
                ++unclaimed;
                if (x < ux0) ux0 = x; if (y < uy0) uy0 = y;
                if (x + 1 > ux1) ux1 = x + 1; if (y + 1 > uy1) uy1 = y + 1;
            }
        if (unclaimed)
            fprintf(t, "unclaimed    %u px of content in no rectangle, box %.3f,%.3f,%.3f,%.3f\n", unclaimed,
                    float(ux0) / W, float(uy0) / H, float(ux1) / W, float(uy1) / H);
        else
            fprintf(t, "unclaimed    none\n");
        fclose(t);

        // The same picture with every rectangle outlined, one colour each.
        static const uint8_t kColours[][3] = {{60, 60, 255}, {255, 160, 40}, {255, 255, 255}, {0, 200, 255},
                                              {0, 255, 0},   {255, 0, 255},  {255, 255, 0},   {128, 255, 128},
                                              {0, 128, 255}, {255, 128, 128},{128, 128, 255}, {0, 255, 255},
                                              {200, 200, 0}};
        const uint32_t th = (std::max)(2u, W / 600);
        for (int i = 0; i < kCount; ++i) {
            if (!have[i]) continue;
            const uint8_t* c = kColours[i % (sizeof kColours / sizeof kColours[0])];
            for (uint32_t y = px[i][1]; y < px[i][3]; ++y)
                for (uint32_t x = px[i][0]; x < px[i][2]; ++x) {
                    const bool edge = x < px[i][0] + th || x + th >= px[i][2] || y < px[i][1] + th || y + th >= px[i][3];
                    if (!edge) continue;
                    uint8_t* o = &img[(static_cast<size_t>(y) * W + x) * 4];
                    o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
                }
        }
        swprintf_s(path, L"%s_boxes.bmp", base);
        ok &= write_bmp(path, img, W, H);
    } else {
        ok = false;
    }
    sprintf_s(g_dumpStatus, "%s %ls(.bmp, _boxes.bmp, .txt)", ok ? "Wrote" : "Could not write all of", base);
    BVR_LOG("[hudel] HUD dump %s: %ls (.bmp, _boxes.bmp, .txt) - %ux%u", ok ? "written" : "INCOMPLETE", base, W, H);
}

// ---- The provider (render thread, once per present) -------------------------
bool provide(const vr::HudElementFrame& f, int maxQuads, vr::HudElementPlan* out) {
    if (g_dumpRequest.exchange(false, std::memory_order_relaxed)) {
        if (f.ctx && f.tex) dump_hud(f.ctx, f.tex);
        else sprintf_s(g_dumpStatus, "No HUD this frame - try again during play.");
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    hudel::ElementCfg cfg[kCount];
    for (int i = 0; i < kCount; ++i) {
        cfg[i].anchor = g_anchor[i].load(std::memory_order_relaxed);
        for (int k = 0; k < 4; ++k) cfg[i].rect[k] = g_rect[i][k].load(std::memory_order_relaxed);
    }
    if (g_repark.exchange(false, std::memory_order_relaxed)) g_park.valid = false;
    hudel::HandCfg hand;
    hand.widthM = g_handWidth.load(std::memory_order_relaxed);
    hand.liftM = g_handLift.load(std::memory_order_relaxed);
    hand.watch = g_handWatch.load(std::memory_order_relaxed);
    hand.tiltDeg = g_handTilt.load(std::memory_order_relaxed);
    hudel::PlanStats st;
    const bool active = hudel::build_plan(cfg, kCount, hand, &g_park, f, maxQuads, out, &st);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);

    // The cost line: 3 s aggregates, logged only while pieces are moved, so a
    // hitch shows up as a number and an idle HUD logs nothing.
    const double us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(fq.QuadPart);
    ++g_cost.calls;
    g_cost.sumUs += us;
    if (us > g_cost.maxUs) g_cost.maxUs = us;
    if (active) ++g_cost.active;
    g_lastStats = st;
    g_lastStrip = out->stripH;
    g_lastCopies = out->copies;
    const ULONGLONG now = GetTickCount64();
    if (!g_cost.since) g_cost.since = now;
    if (now - g_cost.since >= 3000) {
        if (g_cost.active)
            BVR_LOG("[hudel] 3 s: %u presents (%u split), plan %.1f us mean / %.1f us max | moved %d hidden %d "
                    "dropped %d | strip %u rows, %d copies",
                    g_cost.calls, g_cost.active, g_cost.sumUs / g_cost.calls, g_cost.maxUs, st.moved, st.hidden,
                    st.dropped, out->stripH, out->copies);
        g_cost = Cost{};
        g_cost.since = now;
    }
    return active;
}

const char* anchor_name(int a) {
    static const char* const kNames[] = {"panel", "hidden", "world", "left hand", "right hand"};
    return a >= 0 && a < hudel::kAnchorCount ? kNames[a] : "?";
}

} // namespace

void init() {
    load_rects();
    vr::set_hud_elements_provider(&provide);
    BVR_LOG("[hudel] %d HUD pieces registered; every one starts on the HUD panel (F10 > HUD > HUD pieces)",
            kCount);
}

float menu_read(menu::Setting id) {
    const int i = static_cast<int>(id) - static_cast<int>(S::HudElHealth);
    if (i >= 0 && i < kCount) return static_cast<float>(g_anchor[i].load(std::memory_order_relaxed));
    switch (id) {
        case S::HudHandWidth: return g_handWidth.load(std::memory_order_relaxed);
        case S::HudHandLift: return g_handLift.load(std::memory_order_relaxed);
        case S::HudHandFace: return g_handWatch.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case S::HudHandTilt: return g_handTilt.load(std::memory_order_relaxed);
        default: return std::numeric_limits<float>::quiet_NaN();
    }
}

bool menu_write(menu::Setting id, float value) {
    const int i = static_cast<int>(id) - static_cast<int>(S::HudElHealth);
    if (i >= 0 && i < kCount) {
        const int a = static_cast<int>(value);
        if (a < 0 || a >= hudel::kAnchorCount) return false;
        const int was = g_anchor[i].exchange(a, std::memory_order_relaxed);
        if (was != a) {
            if (a == hudel::kWorld) g_repark.store(true, std::memory_order_relaxed); // park where you look now
            BVR_LOG("[hudel] %s: %s -> %s", kRows[i].key, anchor_name(was), anchor_name(a));
        }
        return true;
    }
    switch (id) {
        case S::HudHandWidth: g_handWidth.store(value, std::memory_order_relaxed); return true;
        case S::HudHandLift: g_handLift.store(value, std::memory_order_relaxed); return true;
        case S::HudHandFace: g_handWatch.store(value != 0, std::memory_order_relaxed); return true;
        case S::HudHandTilt: g_handTilt.store(value, std::memory_order_relaxed); return true;
        default: return false;
    }
}

void draw_panel() {
    if (menu::theme::section("HUD pieces: status")) {
        int moved = 0, hidden = 0;
        for (int i = 0; i < kCount; ++i) {
            const int a = g_anchor[i].load(std::memory_order_relaxed);
            moved += a == hudel::kWorld || a == hudel::kHandL || a == hudel::kHandR;
            hidden += a == hudel::kOff;
        }
        ImGui::Text("%d piece(s) moved, %d hidden, %d on the panel", moved, hidden, kCount - moved - hidden);
        if (g_lastStats.dropped)
            ImGui::Text("%d piece(s) not shown last frame (hand not tracked, or no layer left)", g_lastStats.dropped);
        if (menu::theme::button("Move the world pieces in front of me")) g_repark.store(true, std::memory_order_relaxed);
        menu::theme::note("Pieces set to In the world stay where they were placed. This moves them to where you are looking now.");
    }
    if (menu::theme::section("Element rectangles")) {
        static int sel = 0;
        const char* names[kCount];
        for (int i = 0; i < kCount; ++i) names[i] = kRows[i].name;
        ImGui::Combo("Piece", &sel, names, kCount);
        static const char* const kEdge[4] = {"Left", "Top", "Right", "Bottom"};
        for (int k = 0; k < 4; ++k) {
            float v = g_rect[sel][k].load(std::memory_order_relaxed);
            char id[32];
            sprintf_s(id, "%s##hudrect%d", kEdge[k], k);
            ImGui::TextUnformatted(kEdge[k]);
            if (menu::theme::slider(id, &v, 0.0f, 1.0f, "%.3f")) g_rect[sel][k].store(v, std::memory_order_relaxed);
        }
        if (menu::theme::button("Save rectangles")) save_rects();
        ImGui::SameLine();
        if (menu::theme::button("Restore this piece")) {
            for (int k = 0; k < 4; ++k) g_rect[sel][k].store(kRows[sel].rect[k], std::memory_order_relaxed);
        }
        if (menu::theme::button("Dump the HUD")) g_dumpRequest.store(true, std::memory_order_relaxed);
        ImGui::TextWrapped("%s", g_dumpStatus);
        menu::theme::note("Each piece is a rectangle of the captured HUD, as a share of its width and height. "
                          "Dump the HUD writes the HUD as it is now, the same picture with every rectangle drawn on "
                          "it, and the content each rectangle holds, to the dumps folder. Do it during play with the "
                          "pieces showing; it pauses the game for a moment. Saved rectangles go to hud-elements.ini.");
    }
}

} // namespace bvr::b1r::hud_elements
