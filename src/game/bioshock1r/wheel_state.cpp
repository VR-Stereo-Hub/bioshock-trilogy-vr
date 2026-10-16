#include "wheel_state.h"
#include "wheel_mirror.h"
#include "swap_hold.h"
#include "patterns.h"
#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace bvr::b1r::wheel_state {
namespace {

using CallMethodFn = void(__cdecl*)(void* movie, const char* method, const char* fmt, ...);
CallMethodFn g_original = nullptr;
const uint8_t* g_image = nullptr; // the exe's base, for swap_hold's pointer checks
unsigned g_axisCalls = 0, g_checkCalls = 0; // the game thread: this opening's stick calls

// The mirror is written by the hook (the game thread: UnrealScript and the C++ HUD
// updaters) and read whole only by the F10 status; the dial reads the atomics.
CRITICAL_SECTION g_lock;
wheel::Mirror g_mirror;
std::atomic<bool> g_installed{false}, g_known{false};
std::atomic<int> g_count[2] = {};
std::atomic<int> g_open{-1};

// Which movie is the HUD: its filename (movie+0x40) names HUDRadial or HUDPC. Cached
// per pointer; a new pointer (a reloaded movie) is read once.
void* g_hudMovie = nullptr;
void* g_notHud[8] = {};
unsigned g_notHudNext = 0;

bool read_text(const void* p, char* out, size_t n, bool wide) {
    __try {
        size_t i = 0;
        if (wide) {
            const wchar_t* w = static_cast<const wchar_t*>(p);
            for (; i + 1 < n && w[i]; ++i) out[i] = (w[i] >= 0x20 && w[i] < 0x7F) ? static_cast<char>(w[i]) : '?';
        } else {
            const char* c = static_cast<const char*>(p);
            for (; i + 1 < n && c[i]; ++i) out[i] = c[i];
        }
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool is_hud(void* movie) {
    if (!movie) return false;
    if (movie == g_hudMovie) return true;
    for (void* p : g_notHud)
        if (p == movie) return false;
    char name[96] = {};
    void* slot = nullptr;
    __try {
        slot = *reinterpret_cast<void**>(static_cast<uint8_t*>(movie) + patterns::kFlashMovieFilenameOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (slot && read_text(slot, name, sizeof name, true) &&
        (strstr(name, "HUDRadial") || strstr(name, "HUDPC"))) {
        if (g_hudMovie != movie)
            BVR_LOG("[wheel] the HUD movie is %p (\"%s\")", movie, name);
        g_hudMovie = movie;
        return true;
    }
    g_notHud[g_notHudNext++ % 8] = movie;
    return false;
}

void log_state() {
    char w[400] = {}, p[400] = {};
    for (int k = 0; k < 2; ++k) {
        char* out = k ? p : w;
        const wheel::List& l = g_mirror.list[k];
        int n = sprintf_s(out, 400, "%d", l.count);
        for (int i = 0; i < l.count && n > 0 && n < 360; ++i)
            n += sprintf_s(out + n, 400 - n, "%s%s", i ? ", " : " [", l.item[i]);
        if (l.count && n > 0 && n < 398) strcat_s(out, 400, "]");
    }
    BVR_LOG("[wheel] weapons %s | plasmids %s | open: %s", w, p,
            g_mirror.open == wheel::kWeapon ? "WEAPON wheel" : g_mirror.open == wheel::kPlasmid ? "PLASMID wheel" : "none");
}

void observe(void* movie, const char* method, const char* fmt, va_list ap) {
    if (!is_hud(movie)) return;
    // The first string argument, when the format starts with one.
    char arg0[64] = {};
    if (fmt && fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 's') {
        const wchar_t* s = va_arg(ap, const wchar_t*);
        if (s) read_text(s, arg0, sizeof arg0, true);
    } else if (fmt && fmt[0] == '%' && fmt[1] == 's') {
        const char* s = va_arg(ap, const char*);
        if (s) read_text(s, arg0, sizeof arg0, false);
    }
    EnterCriticalSection(&g_lock);
    const bool changed = g_mirror.on_call(method, arg0);
    g_count[0].store(g_mirror.list[0].count, std::memory_order_relaxed);
    g_count[1].store(g_mirror.list[1].count, std::memory_order_relaxed);
    g_open.store(g_mirror.open, std::memory_order_relaxed);
    if (!g_known.exchange(true, std::memory_order_relaxed))
        BVR_LOG("[wheel] the HUD movie's calls are arriving (first: %s) - the wheel stage is known", method);
    // The lists, and each opening and closing - one line each, so a run says whether
    // the game opened a wheel at all, whatever the dial did with it. Selections are
    // per stick step and stay quiet.
    if (changed && strncmp(method, "Select", 6) != 0) log_state();
    LeaveCriticalSection(&g_lock);
}

void __cdecl detour(void* movie, const char* method, const char* fmt, ...) {
    bool want = false;
    __try {
        want = wheel::interesting(method);
        // While a wheel is open: how often the game hands the stick to it. Each axis
        // change runs CheckFor*RadialChange; only some go on to UpdateRadialAxis, the
        // call that moves the pick (ENGINE_NOTES "The weapon and plasmid wheels").
        if (g_open.load(std::memory_order_relaxed) >= 0 && method) {
            if (method[0] == 'U' && !strcmp(method, "UpdateRadialAxis")) ++g_axisCalls;
            else if (method[0] == 'C' && !strncmp(method, "CheckFor", 8)) ++g_checkCalls;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        want = false;
    }
    swap_hold::poll(g_image); // one pointer compare once it is in
    if (want) {
        va_list ap;
        va_start(ap, fmt);
        __try {
            observe(movie, method, fmt, ap);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        va_end(ap);
        // The release reaching the HUD movie: what the swap hold kept back goes first,
        // so the game sees the order it would have (swap, pick, then the release).
        if (is_hud(movie)) {
            if (!strcmp(method, "RBReleased") || !strcmp(method, "LBReleased")) {
                BVR_LOG("[wheel] closed: the game passed the stick to the wheel %u time(s) (%u change checks)",
                        g_axisCalls, g_checkCalls);
                g_axisCalls = g_checkCalls = 0;
                swap_hold::on_release();
            }
            else if (!strcmp(method, "InitializeMovie")) swap_hold::clear();
        }
    }
    // Forward unchanged. The original reads only what its format asks for (at most 27
    // dwords at the 158 known call sites: SetHackingParameters' five doubles and 17
    // ints), so passing the caller's next 40 stack dwords along is exact for every
    // call; the extra ones are the caller's own frame and are never read.
    const uint32_t* a = reinterpret_cast<const uint32_t*>(&fmt) + 1;
    g_original(movie, method, fmt, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
               a[12], a[13], a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23], a[24],
               a[25], a[26], a[27], a[28], a[29], a[30], a[31], a[32], a[33], a[34], a[35], a[36], a[37],
               a[38], a[39]);
}

} // namespace

bool install(const bvr::pattern_scan::ProcessImage& image) {
    InitializeCriticalSection(&g_lock);
    g_image = image.base;
    uint8_t* target = const_cast<uint8_t*>(image.base) + patterns::kFlashCallMethodRva;
    const uint8_t* tail = target + patterns::kFlashCallMethodTailAt;
    if (!bvr::pattern_scan::is_memory_valid(target, patterns::kFlashCallMethodTailAt + sizeof patterns::kFlashCallMethodTail) ||
        memcmp(target, patterns::kFlashCallMethodHead, sizeof patterns::kFlashCallMethodHead) != 0 ||
        memcmp(tail, patterns::kFlashCallMethodTail, sizeof patterns::kFlashCallMethodTail) != 0) {
        BVR_LOG("[wheel] the HUD movie call routine's prologue does not match at RVA 0x%X - the wheel stage is "
                "unknown and the hand dial stays off", patterns::kFlashCallMethodRva);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_original));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        BVR_LOG("[wheel] hooking the HUD movie call routine failed: %s - the hand dial stays off", MH_StatusToString(st));
        return false;
    }
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[wheel] HUD movie calls observed at %p (RVA 0x%X; forwards every call unchanged)", target,
            patterns::kFlashCallMethodRva);
    return true;
}

bool live() { return g_installed.load(std::memory_order_relaxed) && g_known.load(std::memory_order_relaxed); }
int count(int kind) { return kind == 0 || kind == 1 ? g_count[kind].load(std::memory_order_relaxed) : 0; }
int open_kind() { return g_open.load(std::memory_order_relaxed); }

void draw_status() {
    if (!g_installed.load(std::memory_order_relaxed)) {
        ImGui::TextWrapped("The wheel's stage is unknown: the HUD movie's call routine was not found in this build.");
        return;
    }
    if (!g_known.load(std::memory_order_relaxed)) {
        ImGui::TextWrapped("Waiting for the HUD movie (load a save).");
        return;
    }
    EnterCriticalSection(&g_lock);
    for (int k = 0; k < 2; ++k) {
        const wheel::List& l = g_mirror.list[k];
        ImGui::Text("%s: %d%s", k ? "Plasmids" : "Weapons", l.count,
                    g_mirror.open == k ? "   (wheel open)" : "");
        for (int i = 0; i < l.count; ++i) {
            static const char* const kPos[8] = {"up", "down", "left", "right", "up-left", "up-right",
                                                "down-right", "down-left"};
            ImGui::BulletText("%s  %s%s", kPos[i], l.item[i], !strcmp(l.item[i], l.selected) ? "  (equipped)" : "");
        }
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace bvr::b1r::wheel_state
