#include "hud_elements.h"
#include "hud_elements_plan.h"
#include "menu_theme.h"
#include "core/input/xinput_bridge.h"
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

// The pieces, normalised to the HUD capture (y down), in menu_model.h's
// BVR_HUD_PIECES order. Derived from the movie the game plays in VR -
// HUDRadial.swf, the pad HUD (the screens log names it; HUDPC.swf is the mouse
// one) - not measured in a capture yet: its root placements in the 16:9 branch
// of SetRes (centerBox +160, rightBox +320 on a 1280x720 stage) and FFDec
// renders of the clips with their imported art (tools\flash-decode.py, then
// the movie merged with sharedlibrary.swf). They assume the stage fills the
// capture; F10 > HUD > Element rectangles > Dump the HUD checks that.
// ENGINE_NOTES "The HUD's pieces" has each one's clip and ActionScript.
//
// `priority`: the lower number owns an overlap (hud_elements_plan.h). `state`:
// rows that exist only in a game state - the ammo holder and the plasmid gear
// share one slot and SetActiveHands shows one or the other; the wheel is the
// HandsIcons radial, live while a bumper is held.
enum RowState { kAlways, kWeaponInHand, kPlasmidInHand, kWheelOpen };
struct Row {
    const char* key; float rect[4]; int priority; RowState state;
    const float (*parts)[4] = nullptr; int nParts = 0; // columns, for a piece that is not a rectangle
    const float (*trims)[4] = nullptr; int nTrims = 0; // cut off when it moves (hudel::ElementCfg::trims)
};

// Health's gauge, flush at the bottom when it moves: its lower-left tooth and
// the tail of its ring curling down at the right hang below the ring (they sit
// behind the EVE gauge where the game draws them). One-pixel columns, each
// trimmed from just below the gauge's own edge in that column (the 8x trace's
// lowest drawn run), so the cut follows the ring. Stage px, normalised.
const float kHealthTrims[][4] = {
    {72.0f / 1280, 86.000f / 720, 73.0f / 1280, 100.0f / 720},  // the lower-left tooth ...
    {73.0f / 1280, 84.625f / 720, 74.0f / 1280, 100.0f / 720},
    {74.0f / 1280, 83.625f / 720, 75.0f / 1280, 100.0f / 720},
    {75.0f / 1280, 82.500f / 720, 76.0f / 1280, 100.0f / 720},
    {76.0f / 1280, 81.500f / 720, 77.0f / 1280, 100.0f / 720},
    {77.0f / 1280, 80.250f / 720, 78.0f / 1280, 100.0f / 720},
    {78.0f / 1280, 80.500f / 720, 81.0f / 1280, 100.0f / 720},
    {81.0f / 1280, 78.875f / 720, 82.0f / 1280, 100.0f / 720},
    {82.0f / 1280, 83.375f / 720, 83.0f / 1280, 100.0f / 720},
    {83.0f / 1280, 85.750f / 720, 84.0f / 1280, 100.0f / 720},
    {84.0f / 1280, 87.500f / 720, 85.0f / 1280, 100.0f / 720},
    {85.0f / 1280, 88.250f / 720, 86.0f / 1280, 100.0f / 720},
    {86.0f / 1280, 90.250f / 720, 87.0f / 1280, 100.0f / 720},
    {109.0f / 1280, 94.000f / 720, 110.0f / 1280, 100.0f / 720}, // ... the ring's tail
    {110.0f / 1280, 93.500f / 720, 111.0f / 1280, 100.0f / 720},
    {111.0f / 1280, 92.750f / 720, 112.0f / 1280, 100.0f / 720},
    {112.0f / 1280, 90.850f / 720, 113.0f / 1280, 100.0f / 720},
    {113.0f / 1280, 90.825f / 720, 114.0f / 1280, 100.0f / 720},
    {114.0f / 1280, 90.650f / 720, 118.0f / 1280, 100.0f / 720}, // flush with the box's bottom
};
constexpr int kHealthTrimCount = static_cast<int>(sizeof kHealthTrims / sizeof kHealthTrims[0]);

// EVE: its bar, plus its hypo gauge - the lower half of HUD_HealthEve_Cover,
// ONE shape with one bitmap fill whose two gears interlock with the first-aid
// gauge above, so no draw separates them. The cut is a SEAM traced on the
// merged HUDRadial at 8x (stage px; ENGINE_NOTES "The pieces rendered whole"):
// the cheapest left-to-right path through the transparent gap between the two
// gears (cost = coverage, never through either disc, held under the first-aid
// ring where it runs along the top of the EVE drop box). It crosses drawn art
// for 63 of 544 eighth-columns, only where the rims touch. Above it is first
// aid, below it EVE; the bar's clip (from x 111, drawn over the gauge) is EVE.
// These one-pixel columns hold every EVE pixel below the seam; 113 of 580,044
// first-aid eighth-pixels fall inside them. EVE owns them over Health (priority).
//
// The bars grow with the maximum (setBioAmmoBar / setHealthBar: BGMid._width =
// start width * max/100, the end cap follows), so the bar part runs to x 760.
const float kEveParts[][4] = {
    {0.04375f, 0.14878f, 0.04453f, 0.15104f}, // x 56..57 y 107.125..108.750
    {0.04453f, 0.14809f, 0.04531f, 0.16667f}, // x 57..58 y 106.625..120.000
    {0.04531f, 0.13542f, 0.04609f, 0.16667f}, // x 58..59 y 97.500..120.000
    {0.04609f, 0.13437f, 0.04688f, 0.16684f}, // x 59..60 y 96.750..120.125
    {0.04688f, 0.13437f, 0.04766f, 0.16719f}, // x 60..61 y 96.750..120.375
    {0.04766f, 0.13420f, 0.04922f, 0.16736f}, // x 61..63 y 96.625..120.500
    {0.04922f, 0.13368f, 0.05000f, 0.17969f}, // x 63..64 y 96.250..129.375
    {0.05000f, 0.13351f, 0.05078f, 0.17986f}, // x 64..65 y 96.125..129.500
    {0.05078f, 0.12274f, 0.05156f, 0.17969f}, // x 65..66 y 88.375..129.375
    {0.05156f, 0.12240f, 0.05312f, 0.17917f}, // x 66..68 y 88.125..129.000
    {0.05312f, 0.12309f, 0.05391f, 0.17899f}, // x 68..69 y 88.625..128.875
    {0.05391f, 0.12378f, 0.05469f, 0.17847f}, // x 69..70 y 89.125..128.500
    {0.05469f, 0.12448f, 0.05547f, 0.17847f}, // x 70..71 y 89.625..128.500
    {0.05547f, 0.12517f, 0.05625f, 0.17986f}, // x 71..72 y 90.125..129.500
    {0.05625f, 0.12587f, 0.05703f, 0.18576f}, // x 72..73 y 90.625..133.750
    {0.05703f, 0.12934f, 0.05781f, 0.18802f}, // x 73..74 y 93.125..135.375
    {0.05781f, 0.13056f, 0.05859f, 0.18802f}, // x 74..75 y 94.000..135.375
    {0.05859f, 0.13003f, 0.05937f, 0.18681f}, // x 75..76 y 93.625..134.500
    {0.05937f, 0.12934f, 0.06016f, 0.18559f}, // x 76..77 y 93.125..133.625
    {0.06016f, 0.12882f, 0.06094f, 0.18472f}, // x 77..78 y 92.750..133.000
    {0.06094f, 0.12865f, 0.06172f, 0.18333f}, // x 78..79 y 92.625..132.000
    {0.06172f, 0.12691f, 0.06250f, 0.18385f}, // x 79..80 y 91.375..132.375
    {0.06250f, 0.12639f, 0.06328f, 0.18628f}, // x 80..81 y 91.000..134.125
    {0.06328f, 0.12622f, 0.06406f, 0.18872f}, // x 81..82 y 90.875..135.875
    {0.06406f, 0.12604f, 0.06484f, 0.19097f}, // x 82..83 y 90.750..137.500
    {0.06484f, 0.12743f, 0.06641f, 0.19167f}, // x 83..85 y 91.750..138.000
    {0.06641f, 0.12743f, 0.06719f, 0.19028f}, // x 85..86 y 91.750..137.000
    {0.06719f, 0.12760f, 0.06797f, 0.18819f}, // x 86..87 y 91.875..135.500
    {0.06797f, 0.12795f, 0.06875f, 0.18576f}, // x 87..88 y 92.125..133.750
    {0.06875f, 0.12795f, 0.06953f, 0.18385f}, // x 88..89 y 92.125..132.375
    {0.06953f, 0.12882f, 0.07031f, 0.18455f}, // x 89..90 y 92.750..132.875
    {0.07031f, 0.12934f, 0.07109f, 0.18576f}, // x 90..91 y 93.125..133.750
    {0.07109f, 0.13003f, 0.07187f, 0.18715f}, // x 91..92 y 93.625..134.750
    {0.07187f, 0.13073f, 0.07266f, 0.18837f}, // x 92..93 y 94.125..135.625
    {0.07266f, 0.13142f, 0.07344f, 0.18958f}, // x 93..94 y 94.625..136.500
    {0.07344f, 0.13212f, 0.07422f, 0.19028f}, // x 94..95 y 95.125..137.000
    {0.07422f, 0.13247f, 0.07500f, 0.19045f}, // x 95..96 y 95.375..137.125
    {0.07500f, 0.13299f, 0.07578f, 0.18472f}, // x 96..97 y 95.750..133.000
    {0.07578f, 0.13351f, 0.07656f, 0.17795f}, // x 97..98 y 96.125..128.125
    {0.07656f, 0.13247f, 0.07734f, 0.17847f}, // x 98..99 y 95.375..128.500
    {0.07734f, 0.13229f, 0.07812f, 0.17865f}, // x 99..100 y 95.250..128.625
    {0.07812f, 0.13229f, 0.07969f, 0.17917f}, // x 100..102 y 95.250..129.000
    {0.07969f, 0.13247f, 0.08047f, 0.17969f}, // x 102..103 y 95.375..129.375
    {0.08047f, 0.13351f, 0.08125f, 0.17969f}, // x 103..104 y 96.125..129.375
    {0.08125f, 0.13351f, 0.08203f, 0.17917f}, // x 104..105 y 96.125..129.000
    {0.08203f, 0.13281f, 0.08359f, 0.17014f}, // x 105..107 y 95.625..122.500
    {0.08359f, 0.13212f, 0.08438f, 0.17014f}, // x 107..108 y 95.125..122.500
    {0.08438f, 0.13142f, 0.08594f, 0.17014f}, // x 108..110 y 94.625..122.500
    {0.08594f, 0.13542f, 0.08672f, 0.17014f}, // x 110..111 y 97.500..122.500
    {0.08672f, 0.13333f, 0.08984f, 0.17014f}, // x 111..115 y 96.000..122.500
    {0.08984f, 0.13316f, 0.59375f, 0.16892f}, // x 115..760 y 95.875..121.625: the bar, any maximum
};
constexpr int kEveCount = static_cast<int>(sizeof kEveParts / sizeof kEveParts[0]);

const Row kRows[] = {
    {"Health",    {0.05596f, 0.06389f, 0.59375f, 0.13733f}, 6, kAlways, nullptr, 0, kHealthTrims, kHealthTrimCount}, // red bar + first-aid gauge
    {"Eve",       {0.04375f, 0.12240f, 0.59375f, 0.19167f}, 5, kAlways, kEveParts, kEveCount}, // bioAmmoBar + EVE gauge
    {"Reticle",   {0.455f, 0.455f, 0.525f, 0.560f}, 0,  kAlways},        // centerBox.ReticleHolder
    {"Target",    {0.360f, 0.280f, 0.640f, 0.450f}, 2,  kAlways},        // EnemyType, FrozenHealth
    {"Prompt",    {0.545f, 0.360f, 0.850f, 0.620f}, 1,  kAlways},        // centerBox.focusArea, ReRoll
    {"Ammo",      {0.035f, 0.760f, 0.380f, 0.935f}, 7,  kWeaponInHand},  // ammoScrollBar
    {"Plasmid",   {0.035f, 0.760f, 0.380f, 0.935f}, 7,  kPlasmidInHand}, // CurrentAbilityName (frame 2)
    {"Wheel",     {0.370f, 0.140f, 0.670f, 0.800f}, -1, kWheelOpen},     // HandsIcons radials, centre (662,340)
    {"Subtitles", {0.220f, 0.775f, 0.760f, 0.940f}, 3,  kAlways},        // subtitleTextHolder, AreaNameMovie
    {"Tutorial",  {0.220f, 0.650f, 0.760f, 0.755f}, 4,  kAlways},        // trainingTextHolder
    {"Objective", {0.660f, 0.000f, 1.000f, 0.130f}, 8,  kAlways},        // chatBoxMovie.importantTextHolder
    {"Messages",  {0.600f, 0.130f, 1.000f, 0.250f}, 9,  kAlways},        // chatBoxMovie.ChatMovie0-5
    {"Money",     {0.840f, 0.455f, 1.000f, 0.580f}, 10, kAlways},        // rightBox.CreditCount, AdamCount
    {"AudioLog",  {0.800f, 0.700f, 1.000f, 0.930f}, 11, kAlways},        // rightBox.logHolder
    {"Escort",    {0.050f, 0.200f, 0.390f, 0.330f}, 12, kAlways},        // GathererHealth, NeedleHolder
    {"Timers",    {0.030f, 0.440f, 0.320f, 0.690f}, 13, kAlways},        // flipTimer
};
constexpr int kCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
#define BVR_HUD_NAME(n, label, help) label,
const char* const kNames[] = {BVR_HUD_PIECES(BVR_HUD_NAME)};
#undef BVR_HUD_NAME
static_assert(sizeof kNames / sizeof kNames[0] == kCount, "one row per BVR_HUD_PIECES entry");
static_assert(kCount <= hudel::kMaxRows, "the planner's row limit");

std::atomic<int> g_anchor[kCount] = {};
std::atomic<float> g_rect[kCount][4];
std::atomic<float> g_offX[kCount], g_offY[kCount], g_scale[kCount];
std::atomic<int> g_piece{0};
std::atomic<float> g_handWidth{0.22f}, g_handLift{0.06f}, g_handTilt{0.0f};
std::atomic<bool> g_handWatch{false};
std::atomic<bool> g_repark{false}, g_dumpRequest{false};
std::atomic<bool> g_plasmidInHand{false};
// The Menus rows: 0 flat screen (as before), 1 the HUD panel, 2 the world;
// indexed by screens::PanelKind (Pause 1, Map 2, Machine 3).
std::atomic<int> g_screenAnchor[4] = {};
std::atomic<int> g_riding{0}; // the PanelKind riding the HUD now, 0 = none

// Render thread only (the provider).
hudel::WorldPark g_park, g_screenPark;
int g_wasRiding = 0;
ULONGLONG g_bumperSince = 0, g_wheelUntil = 0;
struct Cost { unsigned calls = 0, active = 0; double sumUs = 0, maxUs = 0; ULONGLONG since = 0; } g_cost;
hudel::PlanStats g_lastStats;
uint32_t g_lastStrip = 0;
int g_lastCopies = 0;
char g_dumpStatus[160] = "No dump yet.";

std::wstring ini_path() { return std::wstring(log::data_dir()) + L"\\hud-elements.ini"; }

void load_rects() {
    for (int i = 0; i < kCount; ++i) {
        for (int k = 0; k < 4; ++k) g_rect[i][k].store(kRows[i].rect[k], std::memory_order_relaxed);
        g_offX[i].store(0.0f, std::memory_order_relaxed);
        g_offY[i].store(0.0f, std::memory_order_relaxed);
        g_scale[i].store(1.0f, std::memory_order_relaxed);
    }
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
    const ULONGLONG nowMs = GetTickCount64();

    // A menu riding the HUD (camera.cpp decided it; the Menus rows say where):
    // the whole capture is the menu, so no pieces. On the panel that is the
    // single head-locked quad core already draws; in the world, one quad parked
    // where the head looked when the menu opened.
    const int riding = g_riding.load(std::memory_order_relaxed);
    if (riding != g_wasRiding) {
        g_screenPark.valid = false;
        BVR_LOG("[hudel] menu %s", riding ? (g_screenAnchor[riding].load() == 2 ? "rides the HUD, in the world"
                                                                                 : "rides the HUD panel")
                                          : "closed - the pieces are back");
        g_wasRiding = riding;
    }
    bool active = false;
    hudel::PlanStats st;
    if (riding) {
        active = g_screenAnchor[riding].load(std::memory_order_relaxed) == 2 &&
                 hudel::build_screen_plan(&g_screenPark, f, out);
    } else {
        // The wheel is open while a bumper is HELD (a tap only swaps the hand),
        // and for its close animation after.
        bool lb = false, rb = false;
        bvr::input::last_composed_bumpers(&lb, &rb);
        if (lb || rb) {
            if (!g_bumperSince) g_bumperSince = nowMs;
            if (nowMs - g_bumperSince >= 200) g_wheelUntil = nowMs + 300;
        } else {
            g_bumperSince = 0;
        }
        const bool wheel = nowMs < g_wheelUntil;
        const bool plasmid = g_plasmidInHand.load(std::memory_order_relaxed);

        hudel::ElementCfg cfg[kCount];
        for (int i = 0; i < kCount; ++i) {
            cfg[i].anchor = g_anchor[i].load(std::memory_order_relaxed);
            for (int k = 0; k < 4; ++k) cfg[i].rect[k] = g_rect[i][k].load(std::memory_order_relaxed);
            cfg[i].offX = g_offX[i].load(std::memory_order_relaxed);
            cfg[i].offY = g_offY[i].load(std::memory_order_relaxed);
            cfg[i].scale = g_scale[i].load(std::memory_order_relaxed);
            cfg[i].priority = kRows[i].priority;
            // Columns follow an edited rectangle: mapped from the default box to this one.
            cfg[i].nParts = kRows[i].nParts < hudel::kMaxParts ? kRows[i].nParts : hudel::kMaxParts;
            cfg[i].nTrims = kRows[i].nTrims < hudel::kMaxTrims ? kRows[i].nTrims : hudel::kMaxTrims;
            if (cfg[i].nParts || cfg[i].nTrims) {
                const float* d = kRows[i].rect;
                const float* c = cfg[i].rect;
                const float sx = (c[2] - c[0]) / (d[2] - d[0]), sy = (c[3] - c[1]) / (d[3] - d[1]);
                auto map = [&](const float* from, float* to) {
                    for (int k = 0; k < 4; ++k)
                        to[k] = k % 2 == 0 ? c[0] + (from[k] - d[0]) * sx : c[1] + (from[k] - d[1]) * sy;
                };
                for (int j = 0; j < cfg[i].nParts; ++j) map(kRows[i].parts[j], cfg[i].parts[j]);
                for (int j = 0; j < cfg[i].nTrims; ++j) map(kRows[i].trims[j], cfg[i].trims[j]);
            }
            switch (kRows[i].state) {
                case kWeaponInHand: cfg[i].active = !plasmid; break;
                case kPlasmidInHand: cfg[i].active = plasmid; break;
                case kWheelOpen: cfg[i].active = wheel; break;
                default: cfg[i].active = true; break;
            }
        }
        if (g_repark.exchange(false, std::memory_order_relaxed)) g_park.valid = false;
        hudel::HandCfg hand;
        hand.widthM = g_handWidth.load(std::memory_order_relaxed);
        hand.liftM = g_handLift.load(std::memory_order_relaxed);
        hand.watch = g_handWatch.load(std::memory_order_relaxed);
        hand.tiltDeg = g_handTilt.load(std::memory_order_relaxed);
        active = hudel::build_plan(cfg, kCount, hand, &g_park, f, maxQuads, out, &st);
    }
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
    const ULONGLONG now = nowMs;
    if (!g_cost.since) g_cost.since = now;
    if (now - g_cost.since >= 3000) {
        if (g_cost.active)
            BVR_LOG("[hudel] 3 s: %u presents (%u split), plan %.1f us mean / %.1f us max | moved %d hidden %d "
                    "dropped %d carved %d restored %d | strip %u rows, %d copies",
                    g_cost.calls, g_cost.active, g_cost.sumUs / g_cost.calls, g_cost.maxUs, st.moved, st.hidden,
                    st.dropped, st.carved, st.restored, out->stripH, out->copies);
        g_cost = Cost{};
        g_cost.since = now;
    }
    return active;
}

const char* anchor_name(int a) {
    static const char* const kAnchorNames[] = {"panel", "hidden", "world", "left hand", "right hand"};
    return a >= 0 && a < hudel::kAnchorCount ? kAnchorNames[a] : "?";
}

} // namespace

void init() {
    load_rects();
    vr::set_hud_elements_provider(&provide);
    BVR_LOG("[hudel] %d HUD pieces registered; every one starts on the HUD panel (F10 > HUD > HUD pieces)",
            kCount);
}

void set_plasmid_in_hand(bool plasmid) { g_plasmidInHand.store(plasmid, std::memory_order_relaxed); }

bool screen_rides(int panelKind) {
    return panelKind >= 1 && panelKind <= 3 && g_screenAnchor[panelKind].load(std::memory_order_relaxed) != 0;
}

void set_riding_screen(int panelKind) { g_riding.store(panelKind, std::memory_order_relaxed); }

float menu_read(menu::Setting id) {
    const int i = static_cast<int>(id) - static_cast<int>(S::HudElHealth);
    if (i >= 0 && i < kCount) return static_cast<float>(g_anchor[i].load(std::memory_order_relaxed));
    const int p = static_cast<int>(id) - static_cast<int>(S::HudXHealth);
    if (p >= 0 && p < kCount * 3) {
        const int row = p / 3;
        return p % 3 == 0 ? g_offX[row].load(std::memory_order_relaxed)
             : p % 3 == 1 ? g_offY[row].load(std::memory_order_relaxed)
                          : g_scale[row].load(std::memory_order_relaxed);
    }
    switch (id) {
        case S::HudHandWidth: return g_handWidth.load(std::memory_order_relaxed);
        case S::HudHandLift: return g_handLift.load(std::memory_order_relaxed);
        case S::HudHandFace: return g_handWatch.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case S::HudHandTilt: return g_handTilt.load(std::memory_order_relaxed);
        case S::HudPiece: return static_cast<float>(g_piece.load(std::memory_order_relaxed));
        case S::HudScreenPause: return static_cast<float>(g_screenAnchor[1].load(std::memory_order_relaxed));
        case S::HudScreenMap: return static_cast<float>(g_screenAnchor[2].load(std::memory_order_relaxed));
        case S::HudScreenMachines: return static_cast<float>(g_screenAnchor[3].load(std::memory_order_relaxed));
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
    const int p = static_cast<int>(id) - static_cast<int>(S::HudXHealth);
    if (p >= 0 && p < kCount * 3) {
        const int row = p / 3;
        (p % 3 == 0 ? g_offX[row] : p % 3 == 1 ? g_offY[row] : g_scale[row]).store(value, std::memory_order_relaxed);
        return true;
    }
    auto screen = [&](int kind) {
        const int was = g_screenAnchor[kind].exchange(static_cast<int>(value), std::memory_order_relaxed);
        if (was != static_cast<int>(value))
            BVR_LOG("[hudel] menus: %s -> %s", kind == 1 ? "pause menu" : kind == 2 ? "map" : "machines",
                    value == 0 ? "flat screen" : value == 1 ? "HUD panel" : "world");
        return true;
    };
    switch (id) {
        case S::HudHandWidth: g_handWidth.store(value, std::memory_order_relaxed); return true;
        case S::HudHandLift: g_handLift.store(value, std::memory_order_relaxed); return true;
        case S::HudHandFace: g_handWatch.store(value != 0, std::memory_order_relaxed); return true;
        case S::HudHandTilt: g_handTilt.store(value, std::memory_order_relaxed); return true;
        case S::HudPiece: g_piece.store(static_cast<int>(value), std::memory_order_relaxed); return true;
        case S::HudScreenPause: return screen(1);
        case S::HudScreenMap: return screen(2);
        case S::HudScreenMachines: return screen(3);
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
        for (int i = 0; i < kCount; ++i) names[i] = kNames[i];
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
