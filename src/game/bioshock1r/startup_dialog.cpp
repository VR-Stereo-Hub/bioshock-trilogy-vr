#include "game/bioshock1r/startup_dialog.h"

#include "core/util/log.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace bvr::b1r::startup_dialog {
namespace {

// How long after mod init the watcher looks for the prompt. The dialog blocks
// the FIRST Present, so it always lands inside the first few seconds; the
// window is generous only because a cold shader cache can stretch startup a
// long way. Once it closes, the game's dialogs behave exactly as they always
// did - an in-game "overwrite this save?" must never be answered by a mod.
constexpr uint64_t kStartupWindowMs = 180000;
constexpr uint32_t kPollMs = 100;

// The prompt's window class and title on this build. "#32770" is the standard
// dialog class; the title really is the literal "Message", not the game name.
const char* kDialogClass = "#32770";
const char* kDialogTitle = "Message";

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_watching{false};
std::atomic<uint32_t> g_dismissed{0};
uint64_t g_initMs = 0;

// MessageBoxA/W are NOT the API behind this prompt - hooking both and watching
// them never fire is how that was established (they armed 137 ms into the
// process, long before the dialog appears). Rather than guess at the rest of
// the dialog-creation surface, the watcher answers the WINDOW, which is what
// the mod's own harness has always done from outside the process and is
// therefore already proven on this build.
bool try_dismiss() {
    HWND dlg = FindWindowA(kDialogClass, kDialogTitle);
    if (!dlg) return false;

    // Only ever click a button that says No. If the prompt ever changes shape,
    // the watcher does nothing instead of pressing something unknown.
    HWND child = nullptr;
    while ((child = FindWindowExA(dlg, child, "Button", nullptr)) != nullptr) {
        char text[64] = {};
        GetWindowTextA(child, text, sizeof text - 1);
        if (_stricmp(text, "&No") == 0 || _stricmp(text, "No") == 0) {
            SendMessageA(child, BM_CLICK, 0, 0);
            g_dismissed.fetch_add(1, std::memory_order_relaxed);
            BVR_LOG("[b1r] startup dialog: answered No to the '%s' prompt - this is "
                    "the revert-Options box the game raises after an unclean exit "
                    "(a crash or a force-kill). 'vrpopup off' leaves it alone.",
                    kDialogTitle);
            return true;
        }
    }
    return false;
}

DWORD WINAPI watch_thread(LPVOID) {
    while (GetTickCount64() - g_initMs <= kStartupWindowMs) {
        if (g_enabled.load(std::memory_order_relaxed) && try_dismiss()) break;
        Sleep(kPollMs);
    }
    g_watching.store(false, std::memory_order_relaxed);
    return 0;
}

} // namespace

void init() {
    g_initMs = GetTickCount64();
    HANDLE t = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (!t) {
        BVR_LOG("[b1r] startup dialog: watcher thread failed (%lu) - the "
                "revert-Options prompt will need a mouse as usual",
                GetLastError());
        return;
    }
    CloseHandle(t);
    g_watching.store(true, std::memory_order_relaxed);
    BVR_LOG("[b1r] startup dialog watcher armed (%u s) - the post-crash "
            "revert-Options prompt is answered No in-process, so it cannot sit "
            "between you and the headset ('vrpopup off' to disable)",
            static_cast<unsigned>(kStartupWindowMs / 1000));
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

void handle_command(const char* args) {
    if (strncmp(args, "on", 2) == 0) set_enabled(true);
    else if (strncmp(args, "off", 3) == 0) set_enabled(false);
    const uint64_t age = GetTickCount64() - g_initMs;
    BVR_LOG("[b1r] vrpopup %s | watching=%d dismissed=%u | startup window %s "
            "(%llu s of %u elapsed) (vrpopup on|off|status)",
            enabled() ? "ON" : "off", g_watching.load(std::memory_order_relaxed) ? 1 : 0,
            g_dismissed.load(std::memory_order_relaxed),
            age <= kStartupWindowMs ? "OPEN" : "closed",
            static_cast<unsigned long long>(age / 1000),
            static_cast<unsigned>(kStartupWindowMs / 1000));
}

} // namespace bvr::b1r::startup_dialog
