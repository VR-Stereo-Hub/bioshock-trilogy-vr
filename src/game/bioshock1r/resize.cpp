// See resize.h.
#include "game/bioshock1r/resize.h"

#include "core/gfx/hud_capture.h"
#include "core/hooks/d3d11_hook.h"
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

namespace bvr::b1r::resize {
namespace {

enum State { kIdle = 0, kQueued = 1, kAwaiting = 2, kConfirmed = 3, kRefused = -1, kNotTaken = -2 };
std::atomic<int> g_state{kIdle};
std::atomic<uint64_t> g_size{0};        // (w << 32) | h
std::atomic<uint64_t> g_stampMs{0};
std::atomic<uint32_t> g_confirmedW{0}, g_confirmedH{0};
std::atomic<bool> g_windowPath{false};  // `vrres window`: the WM_SIZE route instead of SetRes
char g_status[200] = "no live resize asked this session";
char g_who[32] = "";

// The live viewport chain is re-resolved for each request. ABI and identity constants
// are BS1-only, derived in ENGINE_NOTES and centralized in patterns.h.
bvr::pattern_scan::ProcessImage g_image;
uint32_t g_vtableRva = patterns::kRenderDeviceVtableRva;
uint32_t g_setresRva = patterns::kRenderDeviceSetResRva;
using namespace patterns;
using SetResFn = int(__fastcall*)(void* device, void* edx, void* viewport, int w, int h, int fullscreen, int colorBytes,
                                  int a6, int a7);

void set_status(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(g_status, sizeof(g_status), _TRUNCATE, fmt, a);
    va_end(a);
}

void refuse(const char* why) {
    g_state.store(kRefused);
    set_status("refused: %s", why);
    BVR_LOG("[resize] REFUSED (%s): %s - the running render is unchanged", g_who, why);
}

bool readable(const void* p, size_t n) { return p && bvr::pattern_scan::is_memory_valid(p, n); }

// SEH-isolated: no C++ objects with destructors in this frame.
int seh_setres(SetResFn fn, void* device, void* viewport, int w, int h, int full, int* result) {
    __try {
        *result = fn(device, nullptr, viewport, w, h, full, 0, 0, 1);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// The engine's own door: its render device's SetRes on the live viewport.
bool apply_setres(uint32_t w, uint32_t h) {
    void* client = nullptr;
    void* viewport = nullptr;
    if (!input_drive::resolve_engine_objects(&client, &viewport) || !viewport) {
        refuse("the engine's client/viewport chain did not resolve (vtable identity failed)");
        return false;
    }
    auto* vp = static_cast<uint8_t*>(viewport);
    if (!readable(vp, kViewportFlagsOffset + 4)) { refuse("viewport unreadable"); return false; }
    void* device = *reinterpret_cast<void**>(vp + kViewportDeviceOffset);
    if (!readable(device, 4)) { refuse("no render device on the viewport (+0xA4 null or unreadable)"); return false; }
    void** vtable = *reinterpret_cast<void***>(device);
    if (!readable(vtable, (kDeviceSetResSlot + 1) * sizeof(void*))) { refuse("render device vtable unreadable"); return false; }
    const auto base = reinterpret_cast<uintptr_t>(g_image.base);
    const auto vt = reinterpret_cast<uintptr_t>(vtable);
    void* fn = vtable[kDeviceSetResSlot];
    const auto entry = reinterpret_cast<uintptr_t>(fn);
    const auto* fn8 = static_cast<const uint8_t*>(fn);
    const size_t prologueSize = sizeof patterns::kRenderDeviceSetResPrologue;
    const bool match = base && g_vtableRva && g_setresRva &&
        g_vtableRva < g_image.size && g_setresRva < g_image.size &&
        prologueSize <= g_image.size - g_setresRva &&
        vt == base + g_vtableRva && entry == base + g_setresRva &&
        readable(fn, prologueSize) &&
        memcmp(fn, patterns::kRenderDeviceSetResPrologue, prologueSize) == 0;
    if (!match) {
        BVR_LOG("[resize] identity mismatch: vtable %p (+0x%X, expected +0x%X), "
                "SetRes %p (+0x%X, expected +0x%X); prologue must match patterns.h",
                vtable, (unsigned)(vt - base), g_vtableRva, fn, (unsigned)(entry - base), g_setresRva);
        refuse("render device vtable, SetRes entry or prologue differs from the pinned build");
        return false;
    }
    const uint32_t flags = *reinterpret_cast<uint32_t*>(vp + kViewportFlagsOffset);
    const uint32_t altFull = *reinterpret_cast<uint32_t*>(vp + kViewportAltFullscreenOffset);
    const int fullscreen = ((flags & 1) || altFull) ? 1 : 0;
    BVR_LOG("[resize] engine SetRes: viewport %p, device %p (vtable +0x%X, SetRes +0x%X, first bytes %02X %02X %02X %02X), "
            "flags 0x%X -> %s %ux%u (%s)",
            viewport, device, (unsigned)(vt - base), (unsigned)(entry - base), fn8[0], fn8[1], fn8[2], fn8[3], flags,
            fullscreen ? "fullscreen" : "windowed", w, h, g_who);
    int result = 0;
    auto* reentry = reinterpret_cast<int*>(vp + kViewportReentryOffset);
    ++*reentry;
    const int faulted = seh_setres(reinterpret_cast<SetResFn>(fn), device, viewport, (int)w, (int)h, fullscreen, &result);
    --*reentry;
    if (faulted) { refuse("SetRes FAULTED (SEH caught) - the device may be in an unknown state; restart before trusting the image"); return false; }
    if (!result) { refuse("SetRes returned 0 (the engine declined the size)"); return false; }
    BVR_LOG("[resize] SetRes returned %d; waiting for the backbuffer to read %ux%u", result, w, h);
    return true;
}

// The window door (the first try, 2026-10-11: the engine did not follow it - its window
// procedure's WM_SIZE path never fired, and Windows clamped the client to the desktop).
// Kept as `vrres window <w>x<h>` for a machine where it might.
bool apply_window(uint32_t w, uint32_t h) {
    HWND hwnd = static_cast<HWND>(bvr::d3d11_hook::game_window());
    if (!hwnd || !IsWindow(hwnd)) { refuse("no game window yet (before the first Present)"); return false; }
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) { refuse("not the game window's thread"); return false; }
    RECT wr = {}, cr = {};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) { refuse("GetWindowRect/GetClientRect failed"); return false; }
    const LONG cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    const LONG dx = (wr.right - wr.left) - cw, dy = (wr.bottom - wr.top) - ch;
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    BVR_LOG("[resize] window %p: client %ldx%ld, frame +%ldx%ld, style 0x%08lX -> asking %ux%u (%s)", static_cast<void*>(hwnd), cw, ch,
            dx, dy, GetWindowLongW(hwnd, GWL_STYLE), w, h, g_who);
    if (!SetWindowPos(hwnd, nullptr, 0, 0, (int)(w + dx), (int)(h + dy), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        refuse("SetWindowPos failed");
        return false;
    }
    RECT after = {};
    GetClientRect(hwnd, &after);
    BVR_LOG("[resize] the window's client area is now %ldx%ld", after.right - after.left, after.bottom - after.top);
    return true;
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image, const wchar_t* iniPath) {
    g_image = image;
    wchar_t ini[MAX_PATH];
    if (!iniPath) {
        swprintf_s(ini, L"%s\\BioshockVR.ini", bvr::log::data_dir());
        iniPath = ini;
    }
    auto read = [&](const wchar_t* key, uint32_t fallback) {
        wchar_t text[48] = {};
        GetPrivateProfileStringW(L"Bioshock1R", key, L"", text, 48, iniPath);
        if (!text[0]) return fallback;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long value = wcstoul(text, &end, 0);
        if (errno || !end || *end || !value || value >= image.size) {
            BVR_LOG("[resize] invalid %ls override: %ls; live SetRes will refuse", key, text);
            return uint32_t{0};
        }
        return static_cast<uint32_t>(value);
    };
    g_vtableRva = read(L"RenderDeviceVtableRva", patterns::kRenderDeviceVtableRva);
    g_setresRva = read(L"RenderDeviceSetResRva", patterns::kRenderDeviceSetResRva);
}

void request(uint32_t w, uint32_t h, const char* who) {
    if (w < 640 || h < 480 || w > 8192 || h > 8192) {
        BVR_LOG("[resize] %s asked for %ux%u: refused, 640x480..8192x8192", who ? who : "?", w, h);
        return;
    }
    const int st = g_state.load();
    if (st == kQueued || st == kAwaiting) {
        BVR_LOG("[resize] %s asked for %ux%u while a resize is in flight (state %d): dropped", who ? who : "?", w, h, st);
        return;
    }
    strncpy_s(g_who, sizeof(g_who), who ? who : "?", _TRUNCATE);
    g_size.store(((uint64_t)w << 32) | h);
    g_stampMs.store(GetTickCount64());
    g_state.store(kQueued);
    set_status("queued: %ux%u", w, h);
    BVR_LOG("[resize] %s: %ux%u queued for the game thread (the engine's own SetRes; the backbuffer must follow for it to "
            "count)", g_who, w, h);
}

bool busy() {
    const int st = g_state.load();
    return st == kQueued || st == kAwaiting;
}

const char* status() { return g_status; }

void last_confirmed(uint32_t* w, uint32_t* h) {
    if (w) *w = g_confirmedW.load();
    if (h) *h = g_confirmedH.load();
}

void tick() {
    const int st = g_state.load();
    const uint64_t packed = g_size.load();
    const uint32_t w = (uint32_t)(packed >> 32), h = (uint32_t)packed;
    if (st == kAwaiting) {
        unsigned bw = 0, bh = 0;
        if (bvr::hud::backbuffer_dims(&bw, &bh) && bw == w && bh == h) {
            g_state.store(kConfirmed);
            g_confirmedW.store(w); g_confirmedH.store(h);
            set_status("live at %ux%u", w, h);
            BVR_LOG("[resize] CONFIRMED: the backbuffer is %ux%u (%s)", w, h, g_who);
        } else if (GetTickCount64() - g_stampMs.load() > 3000) {
            g_state.store(kNotTaken);
            set_status("not taken: asked %ux%u, the render is %ux%u", w, h, bw, bh);
            BVR_LOG("[resize] NOT TAKEN: asked %ux%u, the backbuffer is still %ux%u after 3 s (%s)", w, h, bw, bh, g_who);
        }
        return;
    }
    if (st != kQueued) return;
    g_state.store(kAwaiting);
    g_stampMs.store(GetTickCount64());
    unsigned bw = 0, bh = 0;
    bvr::hud::backbuffer_dims(&bw, &bh);
    if (bw == w && bh == h) {
        g_state.store(kConfirmed);
        g_confirmedW.store(w); g_confirmedH.store(h);
        set_status("already %ux%u", w, h);
        BVR_LOG("[resize] already %ux%u - nothing to do", w, h);
        return;
    }
    const bool ok = g_windowPath.load() ? apply_window(w, h) : apply_setres(w, h);
    if (ok) set_status("asked %ux%u, waiting for the backbuffer", w, h);
}

void handle_command(const char* args) {
    unsigned w = 0, h = 0;
    if (!args) args = "";
    const bool window = strncmp(args, "window", 6) == 0;
    if (window) args += 6 + (args[6] == ' ');
    if ((sscanf_s(args, "%ux%u", &w, &h) == 2 || sscanf_s(args, "%u %u", &w, &h) == 2) && w && h) {
        g_windowPath.store(window);
        request(w, h, window ? "command (window)" : "command");
    } else {
        BVR_LOG("[resize] %s | usage: vrres live <w>x<h> (the engine's SetRes) | vrres live window <w>x<h> (the window route)",
                g_status);
    }
}

} // namespace bvr::b1r::resize
