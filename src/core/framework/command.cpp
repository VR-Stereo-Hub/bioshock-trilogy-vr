#include "command.h"

#include "core/debug/value_scan.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/igame_adapter.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace bvr::command {
namespace {

std::atomic<bool> g_presentPumpEnabled{false};
std::atomic<bool> g_gameThreadPump{false}; // handover latch, never cleared
std::atomic<bool> g_inPoll{false};

// ---- the game-thread pump's LEASE (session 36) -----------------------------
//
// The handover is still one-way in the sense that matters - the game thread
// stays the preferred pump forever - but it is now a LEASE rather than an
// eviction. If the hook the game-thread pump rides on goes quiet (a level load,
// a Scaleform menu, a scripted camera), the Present pump resumes in DEGRADED
// mode instead of leaving the mod with no command surface and no line saying so.
//
// This answers the original objection (see command.h) more precisely rather
// than overriding it. What must not happen during a load is an ENGINE-TOUCHING
// dispatch on the render thread - not any dispatch at all. So the degraded pump
// refuses exactly the vocabulary that writes engine memory and runs the rest.
//
// It is also TESTABLE ON DEMAND, which the alternative was not: `bsicam off`
// silences the camera deliberately, `vrcmd` must then report the degraded
// render pump, and `bsicam on` must hand back. An instrument that cannot be
// made to fail is not evidence.
//
// INERT FOR BS1 AND BS2 BY CONSTRUCTION: neither includes this header, neither
// calls enable_present_pump() or poll_from_game_thread(), and poll_from_present
// returns immediately unless an adapter armed the pump. Verified by grep,
// session 36 - the only callers anywhere are bioshockinf's.
std::atomic<uint64_t> g_lastGamePollMs{0};
std::atomic<bool> g_presentTookBack{false};
constexpr uint64_t kGamePumpLeaseMs = 3000;
std::atomic<uint64_t> g_lastPollMs{0};
std::atomic<uint64_t> g_polls{0};
std::atomic<uint64_t> g_dispatched{0};
std::atomic<uint64_t> g_lastDispatchMs{0};

// Guarded by the g_inPoll critical section, so plain types are fine.
FILETIME g_lastWrite{};
bool g_primed = false;
const char* g_pumpName = "none";

const wchar_t* command_path() {
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        const wchar_t* dir = bvr::log::data_dir();
        if (!dir || !dir[0]) return nullptr; // log::init failed - nothing to poll
        swprintf_s(path, L"%s\\command.txt", dir);
    }
    return path;
}

void read_and_dispatch(const wchar_t* path) {
    FILE* f = _wfsopen(path, L"rt", _SH_DENYNO);
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) dispatch_line(line);
    fclose(f);
}

// The poll body. Caller holds the g_inPoll guard and has passed the 1 Hz gate.
void poll_locked(const char* pump) {
    const wchar_t* path = command_path();
    if (!path) return;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const bool exists = GetFileAttributesExW(path, GetFileExInfoStandard, &fad) != 0;

    // THE FIRST POLL PRIMES, IT DOES NOT DISPATCH. A command.txt left over from
    // a previous run is stale by definition, and applying it at boot is a trap
    // TESTING.md records as having bitten BS1 three times - once producing a
    // false result that was chased as a real one. The harness writes to a
    // RUNNING game, so nothing legitimate is lost.
    //
    // Priming keys off the FIRST POLL, not the first file sighting: when the
    // game starts with no command.txt at all (the documented pre-launch hygiene)
    // the first file to appear is genuinely new, and an earlier version of this
    // swallowed it - verified live, session 35.
    if (!g_primed) {
        g_primed = true;
        if (!exists) return;
        g_lastWrite = fad.ftLastWriteTime;
        SYSTEMTIME st{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
        BVR_LOG("[cmd] skipping pre-existing command.txt (last written %04u-%02u-%02u "
                "%02u:%02u:%02u UTC, before we started) - write it again to run it",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return;
    }
    if (!exists) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastWrite) == 0) return;
    g_lastWrite = fad.ftLastWriteTime;
    BVR_LOG("[cmd] command.txt changed - dispatching on the %s thread", pump);
    read_and_dispatch(path);
}

void poll(uint64_t nowMs, const char* pump) {
    uint64_t last = g_lastPollMs.load(std::memory_order_relaxed);
    if (nowMs - last < 1000) return;
    bool expected = false;
    if (!g_inPoll.compare_exchange_strong(expected, true, std::memory_order_acquire)) return;
    g_lastPollMs.store(nowMs, std::memory_order_relaxed);
    g_polls.fetch_add(1, std::memory_order_relaxed);
    g_pumpName = pump;
    poll_locked(pump);
    g_inPoll.store(false, std::memory_order_release);
}

} // namespace

void enable_present_pump() {
    if (!g_presentPumpEnabled.exchange(true))
        BVR_LOG("[cmd] Present-thread command pump ARMED (no engine hook needed)");
}

void poll_from_present(uint64_t nowMs) {
    if (!g_presentPumpEnabled.load(std::memory_order_relaxed)) return;
    if (g_gameThreadPump.load(std::memory_order_relaxed)) {
        const uint64_t lastGame = g_lastGamePollMs.load(std::memory_order_relaxed);
        if (nowMs - lastGame < kGamePumpLeaseMs) return; // lease healthy, stand down
        if (!g_presentTookBack.exchange(true))
            BVR_LOG("[cmd] game-thread pump silent for >%llu ms - Present pump RESUMING in "
                    "DEGRADED mode. Commands that write engine memory are refused until the "
                    "game thread returns; everything else dispatches normally.",
                    static_cast<unsigned long long>(kGamePumpLeaseMs));
    }
    poll(nowMs, g_presentTookBack.load(std::memory_order_relaxed) ? "render(degraded)"
                                                                 : "render");
}

void poll_from_game_thread(uint64_t nowMs) {
    // Stamped on EVERY call, not just the ones that pass the 1 Hz gate inside
    // poll(): the lease measures whether the game thread is alive, which is a
    // different question from whether it polled.
    g_lastGamePollMs.store(nowMs, std::memory_order_relaxed);
    if (g_presentTookBack.exchange(false))
        BVR_LOG("[cmd] game thread resumed - Present pump standing down again");
    if (!g_gameThreadPump.exchange(true))
        BVR_LOG("[cmd] game-thread pump took over - commands now run on the game thread, where "
                "anything touching the engine belongs. The Present pump stands down, but keeps "
                "a %llu ms lease so a silent game thread cannot strand the command surface.",
                static_cast<unsigned long long>(kGamePumpLeaseMs));
    poll(nowMs, "game");
}

bool degraded() {
    return g_presentTookBack.load(std::memory_order_relaxed);
}

void dispatch_line(const char* line) {
    char cmd[32];
    int consumed = 0;
    if (sscanf_s(line, "%31s%n", cmd, static_cast<unsigned>(sizeof cmd), &consumed) != 1) return;
    const char* args = line + consumed;
    while (*args == ' ' || *args == '\t') ++args;
    g_dispatched.fetch_add(1, std::memory_order_relaxed);
    g_lastDispatchMs.store(GetTickCount64(), std::memory_order_relaxed);

    // Degraded mode: the game thread is silent, so we are dispatching from the
    // render thread during whatever silenced it - most likely a level load.
    // Refuse the vocabulary that WRITES ENGINE MEMORY and let the rest through.
    // This is the precise form of the original "never resume" rule: the hazard
    // was always an engine-touching dispatch at the worst moment, not reading a
    // counter. Costs nothing while the game thread is healthy.
    if (g_presentTookBack.load(std::memory_order_relaxed)) {
        static const char* const kEngineWriters[] = {"mempoke", "mempokei", "memrestore",
                                                     "pokeaddr", "pokeaddri"};
        for (const char* w : kEngineWriters) {
            if (strcmp(cmd, w) != 0) continue;
            BVR_LOG("[cmd] '%s' REFUSED - the game thread is silent (loading?) and this command "
                    "writes engine memory. Retry once the game thread resumes.", cmd);
            return;
        }
    }

    // Adapter FIRST: a game may deliberately shadow a core command, and this
    // ordering is what lets BS1/BS2 keep their divergent branches when they
    // fold into this module. This is a control-plane call (once a second, on
    // the pump thread), not the per-frame state query the core/adapter contract
    // forbids - same shape as overlay.cpp's drawDebugUi() call.
    if (auto* adapter = bvr::game::adapter())
        if (adapter->handleCommand(cmd, args)) return;
    if (core_command(cmd, args)) return;
    BVR_LOG("[cmd] unknown command: %s (core vocabulary is listed in "
            "core/framework/command.h; the adapter did not claim it either)", cmd);
}

bool core_command(const char* cmd, const char* args) {
    float v = 0.0f;
    unsigned lo = 0, hi = 0, n = 0;
    unsigned addr = 0, len = 0;

    if (strcmp(cmd, "memscan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::scan_f32(v);
    } else if (strcmp(cmd, "memrescan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::rescan_f32(v);
    } else if (strcmp(cmd, "memscani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::scan_u32(n);
    } else if (strcmp(cmd, "memrescani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::rescan_u32(n);
    } else if (strcmp(cmd, "memlist") == 0) {
        bvr::value_scan::list(sscanf_s(args, "%u", &n) == 1 ? n : 32);
    } else if (strcmp(cmd, "memread") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::read_at(n);
    } else if (strcmp(cmd, "mempoke") == 0) {
        if (sscanf_s(args, "%u-%u %f", &lo, &hi, &v) == 3)
            bvr::value_scan::poke_range(lo, hi, v);
        else if (sscanf_s(args, "%u %f", &n, &v) == 2)
            bvr::value_scan::poke(n, v);
    } else if (strcmp(cmd, "mempokei") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%u-%u %u", &lo, &hi, &iv) == 3)
            bvr::value_scan::poke_range_u32(lo, hi, iv);
        else if (sscanf_s(args, "%u %u", &n, &iv) == 2)
            bvr::value_scan::poke_u32(n, iv);
    } else if (strcmp(cmd, "memrestore") == 0) {
        bvr::value_scan::restore_all();
    } else if (strcmp(cmd, "memptr") == 0) {
        unsigned maxDelta = 0x400;
        if (sscanf_s(args, "%u %x", &n, &maxDelta) >= 1)
            bvr::value_scan::ptr_scan(n, maxDelta);
    } else if (strcmp(cmd, "pokeaddr") == 0) {
        if (sscanf_s(args, "%x %f", &addr, &v) == 2) bvr::value_scan::poke_addr(addr, v);
    } else if (strcmp(cmd, "pokeaddri") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%x %u", &addr, &iv) == 2) bvr::value_scan::poke_addr_u32(addr, iv);
    } else if (strcmp(cmd, "hexdump") == 0) {
        if (sscanf_s(args, "%x %u", &addr, &len) >= 1)
            bvr::value_scan::hexdump(addr, len ? len : 64);
    } else if (strcmp(cmd, "strscan") == 0) {
        char text[96];
        if (sscanf_s(args, "%95s", text, static_cast<unsigned>(sizeof text)) == 1)
            bvr::value_scan::log_string_scan(text);
    } else if (strcmp(cmd, "membases") == 0) {
        bvr::value_scan::log_module_bases();
    } else if (strcmp(cmd, "fsweep") == 0) {
        float flo = 0.0f, fhi = 0.0f;
        if (sscanf_s(args, "%x %u %f %f", &addr, &len, &flo, &fhi) == 4)
            bvr::value_scan::float_sweep(addr, len, flo, fhi);
        else
            BVR_LOG("[cmd] usage: fsweep <hexaddr> <len> <lo> <hi>");
    } else if (strcmp(cmd, "dumpframe") == 0) {
        // dumpframe [full] [n] - n > 1 records consecutive present windows
        // (files suffixed _qN); the dump lands in this game's data dir.
        bool full = strncmp(args, "full", 4) == 0;
        int count = 1;
        sscanf_s(full ? args + 4 : args, " %d", &count);
        bvr::frame_inspector::arm(full ? 2 : 1, count);
    } else if (strcmp(cmd, "vrinput") == 0) {
        bvr::input::handle_command(args); // logs its own echoes
    } else if (strcmp(cmd, "vrpace") == 0) {
        bvr::vr::handle_pace_command(args);
    } else if (strcmp(cmd, "vrmirror") == 0) {
        bvr::vr::handle_mirror_command(args);
    } else if (strcmp(cmd, "vrcine") == 0) {
        bvr::vr::handle_cine_command(args);
    } else if (strcmp(cmd, "vroverlay") == 0) {
        // An explicit "off" is off; anything else (including a bare command) is
        // on. BS2's reading, kept because "vroverlay" alone meaning "hide it"
        // is the more surprising of the two.
        bool on = strncmp(args, "off", 3) != 0;
        bvr::overlay::set_visible(on);
        BVR_LOG("[cmd] overlay %s (seam request)", on ? "ON" : "off");
    } else if (strcmp(cmd, "vrhud") == 0) {
        if (strncmp(args, "force on", 8) == 0) {
            bvr::hud::set_force(true);
        } else if (strncmp(args, "force off", 9) == 0) {
            bvr::hud::set_force(false);
        } else if (strncmp(args, "on", 2) == 0) {
            bvr::hud::set_enabled(true);
        } else if (strncmp(args, "off", 3) == 0) {
            bvr::hud::set_enabled(false);
        } else {
            unsigned hd = 0, rd = 0, lk = 0, iv = 0;
            bvr::hud::get_counters(&hd, &rd, &lk, &iv);
            unsigned lbT = 0, lbB = 0;
            bool lb = bvr::hud::letterbox(&lbT, &lbB);
            BVR_LOG("[hud] status: %s force=%d | hudDraws=%u redirects=%u leaks=%u "
                    "hudIntervals=%u | postFx=%u screenOnly=%d letterbox=%d(%u/%u) "
                    "(vrhud on|off|force on|force off|status)",
                    bvr::hud::enabled() ? "ON" : "off", bvr::hud::force() ? 1 : 0, hd, rd, lk,
                    iv, bvr::hud::postfx_count(), bvr::hud::screen_only() ? 1 : 0, lb ? 1 : 0,
                    lbT, lbB);
            bvr::hud::RouteStats rs{};
            bvr::hud::get_route_stats(&rs);
            char buf[320];
            int written = 0;
            for (int i = 0; i < bvr::hud::kRoutePassCount && written >= 0 && written < 300; ++i) {
                if (!rs.pass[i] && !rs.stranded[i]) continue;
                written += _snprintf_s(buf + written, sizeof buf - written, _TRUNCATE, "%s=%u/%u ",
                                       bvr::hud::route_reason_name(i), rs.pass[i], rs.stranded[i]);
            }
            BVR_LOG("[hud] routes (pass/STRANDED): %s| postFxRejected=%u effectsInFrame=%u "
                    "effectsOverBound=%u square=%d",
                    written > 0 ? buf : "(none) ", rs.postFxRejected, rs.effectsInFrame,
                    rs.effectsRejected, rs.squareTarget ? 1 : 0);
        }
    } else if (strcmp(cmd, "vrcmd") == 0) {
        // The seam describing itself. Names the pump because it is also the
        // thread every command above runs on.
        uint64_t last = g_lastDispatchMs.load(std::memory_order_relaxed);
        BVR_LOG("[cmd] status: pump=%s (present=%s gameThread=%s) polls=%llu dispatched=%llu "
                "lastDispatch=%llums ago | file=%ls\\command.txt",
                g_pumpName,
                g_presentPumpEnabled.load(std::memory_order_relaxed) ? "armed" : "off",
                g_gameThreadPump.load(std::memory_order_relaxed) ? "OWNS" : "no",
                static_cast<unsigned long long>(g_polls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_dispatched.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(last ? GetTickCount64() - last : 0),
                bvr::log::data_dir());
    } else {
        return false;
    }
    return true;
}

} // namespace bvr::command
