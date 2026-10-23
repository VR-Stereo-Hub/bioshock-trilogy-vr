#include "game/bioshock1r/room_capture.h"

#include "core/gfx/cube_snapshot.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/palette.h"
#include "game/bioshock1r/screens.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::room_capture {
namespace {

constexpr float kRadToDeg = 57.29578f;

// ---- the map watch ----------------------------------------------------------
wchar_t g_loadedFor[64] = L""; // the map whose room file was asked for (or found absent)
bool g_pendingLoad = false;    // a save just went out: load it back once it lands

bool room_path(const wchar_t* map, wchar_t* out, size_t n) {
    if (!map || !map[0]) return false;
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\rooms", bvr::log::data_dir());
    CreateDirectoryW(dir, nullptr);
    wchar_t safe[64];
    size_t k = 0;
    for (size_t i = 0; map[i] && k + 1 < sizeof(safe) / sizeof(safe[0]); ++i) {
        const wchar_t c = map[i];
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                        c == L'-' || c == L'_';
        safe[k++] = ok ? c : L'_';
    }
    safe[k] = 0;
    swprintf_s(out, n, L"%s\\%s.cube", dir, safe);
    return true;
}

bool g_clearPending = false; // the new level has no room: the old one stays until its gameplay starts

// The map name changes while the level still LOADS (the controller's Level
// pointer moves first), so a level with a room on disk shows it around the
// loading board on the way in. A level without one keeps the old room up until
// its gameplay starts, and only then begins a live cube of its own.
void watch_map(bool strictGameplay) {
    const wchar_t* map = screens::map_name();
    if (map[0] && wcscmp(map, g_loadedFor) != 0) {
        wcscpy_s(g_loadedFor, map);
        wchar_t path[MAX_PATH];
        if (!room_path(map, path, MAX_PATH)) return;
        const DWORD attr = GetFileAttributesW(path);
        const bool have = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        if (have) {
            bvr::vr::reset_room_snapshot();
            bvr::vr::request_room_file(2, path);
            g_clearPending = false;
        } else {
            g_clearPending = true;
        }
        BVR_LOG("[room] map %ls: %s", map, have ? "loading its room" : "no room on disk - the old room stays until gameplay");
    }
    if (g_clearPending && strictGameplay) {
        g_clearPending = false;
        bvr::vr::reset_room_snapshot();
        bvr::vr::request_room_file(3, nullptr);
        BVR_LOG("[room] map %ls: gameplay - a live cube of its own from here", g_loadedFor);
    }
}

// ---- the sweep --------------------------------------------------------------
// 38 directions: straight down, a ring of 6 at -60, 8 at -30, 8 level, 8 at +30,
// 6 at +60, straight up. The eye image is about 100 degrees wide and tall, so
// every texel of the sphere is covered several times over.
struct Dir { float yawDeg, pitchDeg; };
Dir g_dirs[40];
int g_dirCount = 0;
// By TIME, not ticks: CalcView runs several times per presented frame in the
// simulator, and a direction must be rendered and presented before its splat.
constexpr uint64_t kHoldMs = 200;  // a direction is held this long before its splat
constexpr uint64_t kWaitMs = 500;  // then this long for the splat to land

std::atomic<bool> g_active{false};
int g_idx = 0;
uint64_t g_aimedMs = 0;
bool g_waitSplat = false;
uint32_t g_splatsAtRequest = 0;
uint64_t g_requestedMs = 0;
uint64_t g_startMs = 0;
float g_q[4] = {0, 0, 0, 1};
float g_cam[3] = {0, 0, 0}; // the camera this frame (UE units), for the by-reach hide

void build_dirs() {
    g_dirCount = 0;
    auto ring = [&](float pitch, int n) {
        for (int i = 0; i < n; ++i) g_dirs[g_dirCount++] = {360.0f * static_cast<float>(i) / static_cast<float>(n), pitch};
    };
    ring(0.0f, 8);
    ring(30.0f, 8);
    ring(-30.0f, 8);
    ring(60.0f, 6);
    ring(-60.0f, 6);
    g_dirs[g_dirCount++] = {0.0f, 89.0f};
    g_dirs[g_dirCount++] = {0.0f, -89.0f};
}

void aim(int idx) {
    const Dir& d = g_dirs[idx];
    bvr::xrmath::xr_local_trim_quat(d.pitchDeg / kRadToDeg, d.yawDeg / kRadToDeg, 0.0f, g_q);
    bvr::vr::set_room_head_override(true, g_q);
    g_aimedMs = GetTickCount64();
    g_waitSplat = false;
}

void finish(bool saved) {
    g_active.store(false, std::memory_order_relaxed);
    palette::set_hide_all(false, nullptr, nullptr);
    bvr::vr::set_room_head_override(false, nullptr);
    bvr::vr::set_room_sweep_display(false);
    if (saved) {
        wchar_t path[MAX_PATH];
        if (room_path(screens::map_name(), path, MAX_PATH)) {
            bvr::vr::request_room_file(1, path);
            g_pendingLoad = true;
            BVR_LOG("[room] sweep done in %.1f s: %u splats, saving %ls", (GetTickCount64() - g_startMs) / 1000.0f,
                    bvr::vr::room_splats(), path);
        } else {
            BVR_LOG("[room] sweep done but the map has no name - nothing saved (the live cube keeps it)");
        }
    }
}

void tick_sweep(bool strictGameplay) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (!strictGameplay) {
        BVR_LOG("[room] sweep cancelled: not in gameplay");
        finish(false);
        return;
    }
    palette::set_hide_all(true, hands::weapon_actor(), g_cam); // the held weapon, as resolved by now
    const uint64_t now = GetTickCount64();
    if (!g_waitSplat) {
        if (now - g_aimedMs < kHoldMs) return;
        g_splatsAtRequest = bvr::vr::room_splats();
        bvr::vr::request_room_splat();
        g_waitSplat = true;
        g_requestedMs = now;
        return;
    }
    const bool landed = bvr::vr::room_splats() > g_splatsAtRequest;
    if (!landed && now - g_requestedMs < kWaitMs) return;
    if (!landed) BVR_LOG("[room] direction %d never splatted (no eye frame?) - moving on", g_idx);
    if (++g_idx >= g_dirCount) {
        finish(true);
        return;
    }
    aim(g_idx);
}

} // namespace

void on_calcview(bool strictGameplay) {
    watch_map(strictGameplay);
    if (g_pendingLoad) {
        const int r = bvr::vr::room_file_result();
        if (r != 0) {
            g_pendingLoad = false;
            wchar_t path[MAX_PATH];
            if (r == 1 && room_path(screens::map_name(), path, MAX_PATH)) bvr::vr::request_room_file(2, path);
        }
    }
    tick_sweep(strictGameplay);
}

bool override_head(bvr::vr::HeadPose* hp) {
    if (!g_active.load(std::memory_order_relaxed) || !hp) return false;
    hp->qx = g_q[0];
    hp->qy = g_q[1];
    hp->qz = g_q[2];
    hp->qw = g_q[3];
    return true;
}

void publish_world_yaw(int32_t bodyYawUnits, const float camLoc[3]) {
    // UE yaw is positive to the right, XR yaw (about +Y) positive to the left.
    bvr::vr::set_room_world_yaw(-static_cast<float>(bodyYawUnits) / kRotUnitsPerRadian);
    if (camLoc)
        for (int i = 0; i < 3; ++i) g_cam[i] = camLoc[i];
}

bool hide_rig() { return g_active.load(std::memory_order_relaxed); }

void start() {
    if (g_active.load(std::memory_order_relaxed)) return;
    if (!bvr::vr::room_snapshot()) {
        BVR_LOG("[room] capture refused: the room snapshot is off (F10 Display, or vrsnap on)");
        return;
    }
    build_dirs();
    g_idx = 0;
    g_startMs = GetTickCount64();
    g_active.store(true, std::memory_order_relaxed);
    palette::set_hide_all(true, hands::weapon_actor(), g_cam);
    bvr::vr::reset_room_snapshot();
    bvr::vr::set_room_sweep_display(true);
    aim(0);
    BVR_LOG("[room] capture sweep: %d directions, hands and gun hidden, map %ls", g_dirCount, screens::map_name());
}

void cancel() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    BVR_LOG("[room] sweep cancelled");
    finish(false);
}

bool active() { return g_active.load(std::memory_order_relaxed); }

float circle() { return bvr::cube_snapshot::screen_circle(); }
void set_circle(float radiusFrac) { bvr::cube_snapshot::set_screen_circle(radiusFrac); }

void handle_command(const char* args) {
    wchar_t path[MAX_PATH];
    if (strncmp(args, "capture", 7) == 0) {
        start();
    } else if (strncmp(args, "cancel", 6) == 0) {
        cancel();
    } else if (strncmp(args, "save", 4) == 0) {
        if (room_path(screens::map_name(), path, MAX_PATH)) {
            bvr::vr::request_room_file(1, path);
            g_pendingLoad = true;
        }
    } else if (strncmp(args, "load", 4) == 0) {
        if (room_path(screens::map_name(), path, MAX_PATH)) bvr::vr::request_room_file(2, path);
    } else if (strncmp(args, "clear", 5) == 0) {
        bvr::vr::request_room_file(3, nullptr);
    } else if (strncmp(args, "circle", 6) == 0) {
        float r = 0.0f;
        if (sscanf_s(args + 6, "%f", &r) == 1) set_circle(r);
    }
    BVR_LOG("[room] map %ls, sweep %s (%d/%d), saved room %s, circle %.3f, %u splats "
            "(vrroom capture|cancel|save|load|clear|circle <r>|status)",
            screens::map_name(), active() ? "RUNNING" : "idle", g_idx, g_dirCount,
            bvr::vr::room_saved_loaded() ? "loaded" : "none", circle(), bvr::vr::room_splats());
}

} // namespace bvr::b1r::room_capture
