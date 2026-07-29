// Session 20 vrrec: record + replay the per-frame input state. Design notes in
// recorder.h; the acceptance contract is in docs/TESTING.md ("record/replay").
//
// Threading: every entry point here runs on the GAME thread (the CalcView tap
// and the command seam share it), so the module needs no locks of its own.
// The funnel sim slots and the pad publish are the cross-thread edges, and
// they carry their own atomics.

#include "game/bioshock1r/recorder.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/camera.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace bvr::b1r::recorder {
namespace {

#pragma pack(push, 1)
struct RecPose {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    uint8_t valid = 0;
};
struct RecHeader {
    char magic[4] = {'B', 'V', 'R', 'R'};
    uint32_t version = 1;
    uint32_t count = 0;
    uint8_t sourceVr = 0; // 1 = recorded with a real headset driving
    RecPose recenter{};   // valid flag unused; pose + the yaw units below
    int32_t recenterYawUnits = 0;
    float worldScale = 100.0f;
};
struct RecEntry {
    RecPose head{};     // what the camera drive consumed (valid = vrDrove)
    RecPose hands[4]{}; // [grip L, grip R, aim L, aim R] from the funnel
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t padActive = 0;
    int64_t predictedTime = 0;
};
#pragma pack(pop)

enum class Mode { Idle, Recording, Playing };
Mode g_mode = Mode::Idle;
std::vector<RecEntry> g_entries;
RecHeader g_header{};
uint32_t g_cursor = 0;   // next entry to replay
uint32_t g_marks = 0;    // mark lines emitted this run (record or play)
char g_fileA[MAX_PATH] = {}; // last file written/loaded, for status lines
// Runaway backstop, not a target: ~2 h at 150 CalcView/s. Never trimmed
// silently - hitting it logs and stops the recording.
constexpr size_t kMaxEntries = 1000000;

// The flat drive pose (`vrrec hand`): one static pose on all four funnel
// slots, at the sim lane's fixed hand spot.
bool g_handArmed = false;

std::wstring recordings_dir() {
    std::wstring dir = bvr::log::data_dir();
    dir += L"\\recordings";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void wide_to_narrow(const wchar_t* w, char* out, size_t cap) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(cap), nullptr, nullptr);
}

// Newest = lexicographically greatest rec_*.bvrrec (names are timestamped).
bool newest_file(std::wstring& out) {
    std::wstring pat = recordings_dir() + L"\\rec_*.bvrrec";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring best;
    do {
        if (best.empty() || wcscmp(fd.cFileName, best.c_str()) > 0) best = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    out = recordings_dir() + L"\\" + best;
    return true;
}

void mark_line(const char* tag, uint32_t idx, const RecEntry& e, const FrameContext& fc) {
    // The aim-R pose through the SAME pure chain the fire ray uses
    // (frame_context.h) with the live R trim - so a record log and a replay
    // log are comparable number for number.
    // The head QUAT and the driven camera (fc.camPitch/camYaw) are the lines'
    // head-replay evidence: under simhead the head POSITION is always the
    // recenter origin, and the aim ray is deliberately head-invariant (the M7
    // decoupling), so without these a broken head replay would not show.
    const RecPose& aimR = e.hands[3];
    if (aimR.valid) {
        const float p[3] = {aimR.px, aimR.py, aimR.pz};
        const float q[4] = {aimR.qx, aimR.qy, aimR.qz, aimR.qw};
        GamePose gp = ray_pose_from_xr(fc, p, q, bvr::b1r::aim::trim_pitch_deg(1),
                                       bvr::b1r::aim::trim_yaw_deg(1));
        BVR_LOG("[rec] %s mark %u head=(%.4f %.4f %.4f | %.4f %.4f %.4f %.4f) "
                "cam=(%d %d) aimR loc=(%.3f %.3f %.3f) rot=(%d %d) "
                "pad=%04X %u %u %d %d %d %d",
                tag, idx, e.head.px, e.head.py, e.head.pz, e.head.qx, e.head.qy, e.head.qz,
                e.head.qw, fc.camPitch, fc.camYaw, gp.loc.x, gp.loc.y, gp.loc.z,
                gp.rot.pitch, gp.rot.yaw, e.buttons, e.lt, e.rt, e.lx, e.ly, e.rx, e.ry);
    } else {
        BVR_LOG("[rec] %s mark %u head=(%.4f %.4f %.4f | %.4f %.4f %.4f %.4f) "
                "cam=(%d %d) aimR INVALID pad=%04X %u %u %d %d %d %d",
                tag, idx, e.head.px, e.head.py, e.head.pz, e.head.qx, e.head.qy, e.head.qz,
                e.head.qw, fc.camPitch, fc.camYaw, e.buttons, e.lt, e.rt, e.lx, e.ly, e.rx,
                e.ry);
    }
}

void capture_pose(int hand, bool aim, RecPose& out) {
    bvr::vr::HeadPose hp{};
    if (bvr::vr::get_hand_pose(hand, aim, hp)) {
        out = {hp.px, hp.py, hp.pz, hp.qx, hp.qy, hp.qz, hp.qw, 1};
    } else {
        out = {};
    }
}

void feed_sim_slots(const RecEntry& e) {
    for (int i = 0; i < 4; ++i) {
        const RecPose& rp = e.hands[i];
        const float p[3] = {rp.px, rp.py, rp.pz};
        const float q[4] = {rp.qx, rp.qy, rp.qz, rp.qw};
        bvr::vr::set_sim_hand_pose(i & 1, i >= 2, rp.valid != 0, p, q);
    }
}

void publish_entry_pad(const RecEntry& e) {
    bvr::input::Gamepad pad{};
    pad.buttons = e.buttons;
    pad.lt = e.lt;
    pad.rt = e.rt;
    pad.lx = e.lx;
    pad.ly = e.ly;
    pad.rx = e.rx;
    pad.ry = e.ry;
    bvr::input::publish_xr_state(pad, e.padActive != 0);
}

void end_replay(const char* why) {
    bvr::vr::clear_sim_hand_poses();
    bvr::input::publish_xr_state({}, false);
    g_mode = Mode::Idle;
    BVR_LOG("[rec] play %s: %u/%u frames, %u marks", why, g_cursor,
            static_cast<uint32_t>(g_entries.size()), g_marks);
}

void write_file() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"rec_%04u%02u%02u_%02u%02u%02u.bvrrec", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    std::wstring path = recordings_dir() + L"\\" + name;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        BVR_LOG("[rec] FAILED to open recording file for write");
        return;
    }
    g_header.count = static_cast<uint32_t>(g_entries.size());
    fwrite(&g_header, sizeof g_header, 1, f);
    if (!g_entries.empty()) fwrite(g_entries.data(), sizeof(RecEntry), g_entries.size(), f);
    fclose(f);
    wide_to_narrow(path.c_str(), g_fileA, sizeof g_fileA);
    BVR_LOG("[rec] stop: %u frames (%u marks) -> %s", g_header.count, g_marks, g_fileA);
}

bool read_file(const std::wstring& path) {
    // _wfsopen with _SH_DENYNO: the fopen_s family opens files NON-SHARABLE,
    // and a freshly written recording is routinely still held by the search
    // indexer / AV scan - the shared open reads through that.
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) {
        BVR_LOG("[rec] FAILED to open recording for read (errno %d)", errno);
        return false;
    }
    RecHeader h{};
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "BVRR", 4) != 0 || h.version != 1) {
        fclose(f);
        BVR_LOG("[rec] not a BVRR v1 recording");
        return false;
    }
    g_entries.assign(h.count, {});
    if (h.count && fread(g_entries.data(), sizeof(RecEntry), h.count, f) != h.count) {
        fclose(f);
        g_entries.clear();
        BVR_LOG("[rec] recording truncated (header says %u frames)", h.count);
        return false;
    }
    fclose(f);
    g_header = h;
    wide_to_narrow(path.c_str(), g_fileA, sizeof g_fileA);
    return true;
}

} // namespace

bool playing() {
    return g_mode == Mode::Playing;
}

bool replay_head(bvr::vr::HeadPose& hp) {
    if (g_mode != Mode::Playing || g_cursor >= g_entries.size()) return false;
    const RecPose& h = g_entries[g_cursor].head;
    if (!h.valid) return false;
    hp = {h.px, h.py, h.pz, h.qx, h.qy, h.qz, h.qw};
    return true;
}

void on_tick(const FrameContext& fc, const bvr::vr::HeadPose& consumedHead, bool vrDrove,
             bool liveHead) {
    if (g_mode == Mode::Recording) {
        if (g_entries.size() >= kMaxEntries) {
            BVR_LOG("[rec] entry cap %u hit - stopping the recording",
                    static_cast<uint32_t>(kMaxEntries));
            write_file();
            g_mode = Mode::Idle;
            return;
        }
        RecEntry e{};
        e.head = {consumedHead.px, consumedHead.py, consumedHead.pz, consumedHead.qx,
                  consumedHead.qy, consumedHead.qz, consumedHead.qw,
                  static_cast<uint8_t>(vrDrove ? 1 : 0)};
        capture_pose(0, false, e.hands[0]);
        capture_pose(1, false, e.hands[1]);
        capture_pose(0, true, e.hands[2]);
        capture_pose(1, true, e.hands[3]);
        if (vrDrove && liveHead) g_header.sourceVr = 1;
        bvr::input::Gamepad pad{};
        bool active = false;
        bvr::input::last_xr_pad(&pad, &active);
        e.buttons = pad.buttons;
        e.lt = pad.lt;
        e.rt = pad.rt;
        e.lx = pad.lx;
        e.ly = pad.ly;
        e.rx = pad.rx;
        e.ry = pad.ry;
        e.padActive = active ? 1 : 0;
        e.predictedTime = bvr::vr::last_predicted_time();
        uint32_t idx = static_cast<uint32_t>(g_entries.size());
        g_entries.push_back(e);
        if (idx % 10 == 0) {
            ++g_marks;
            mark_line("REC ", idx, e, fc);
        }
    } else if (g_mode == Mode::Playing) {
        if (g_cursor >= g_entries.size()) {
            end_replay("done");
            return;
        }
        // The head for THIS tick was already served by replay_head at the
        // camera gate (cursor untouched there); now feed the same entry's
        // hands + pad so aim/hands - which run right after this call -
        // consume one consistent frame, then advance.
        const RecEntry& e = g_entries[g_cursor];
        feed_sim_slots(e);
        publish_entry_pad(e);
        if (g_cursor % 10 == 0) {
            ++g_marks;
            mark_line("PLAY", g_cursor, e, fc);
        }
        ++g_cursor;
    } else if (g_handArmed) {
        // Flat drive pose: keep the funnel slots armed (a session appearing
        // mid-arm would otherwise overwrite them at the next input_sync -
        // re-arming per tick keeps the sim overlay authoritative).
        float q[4];
        xr_local_trim_quat(0.0f, 0.0f, 0.0f, q);
        const float p[3] = {0.15f, -0.20f, -0.35f};
        for (int i = 0; i < 4; ++i) bvr::vr::set_sim_hand_pose(i & 1, i >= 2, true, p, q);
    }
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[rec] usage: vrrec start|stop|play [file]|hand [p y r|off]|status");
        return;
    }
    // The command seam delivers the raw line INCLUDING its trailing newline -
    // skip all whitespace or `play` with no argument reads "\n" as a file name
    // (errno 22, the first live failure of this module).
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') ++rest;

    if (strcmp(verb, "start") == 0) {
        if (g_mode != Mode::Idle) {
            BVR_LOG("[rec] start REFUSED: %s", g_mode == Mode::Recording ? "already recording"
                                                                         : "a replay is running");
            return;
        }
        g_entries.clear();
        g_entries.reserve(65536);
        g_header = {};
        bvr::vr::HeadPose rp{};
        int32_t yawUnits = 0;
        float ws = 100.0f;
        camera::get_recenter_state(&rp, &yawUnits, &ws);
        g_header.recenter = {rp.px, rp.py, rp.pz, rp.qx, rp.qy, rp.qz, rp.qw, 1};
        g_header.recenterYawUnits = yawUnits;
        g_header.worldScale = ws;
        g_marks = 0;
        g_mode = Mode::Recording;
        BVR_LOG("[rec] START (recenter yaw %d units, worldScale %.1f - recenter BEFORE "
                "start, not during)",
                yawUnits, ws);
    } else if (strcmp(verb, "stop") == 0) {
        if (g_mode == Mode::Recording) {
            write_file();
            g_mode = Mode::Idle;
        } else if (g_mode == Mode::Playing) {
            end_replay("ABORTED");
        } else {
            BVR_LOG("[rec] nothing to stop");
        }
    } else if (strcmp(verb, "play") == 0) {
        if (g_mode != Mode::Idle) {
            BVR_LOG("[rec] play REFUSED: busy");
            return;
        }
        if (bvr::vr::session_live()) {
            BVR_LOG("[rec] play REFUSED: an XR session is live (two writers on the "
                    "funnel) - close the headset session first");
            return;
        }
        std::wstring path;
        if (*rest) {
            int n = MultiByteToWideChar(CP_UTF8, 0, rest, -1, nullptr, 0);
            std::wstring name(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, rest, -1, name.data(), n);
            name.resize(wcslen(name.c_str()));
            while (!name.empty() && (name.back() == L'\r' || name.back() == L'\n' ||
                                     name.back() == L' ' || name.back() == L'\t'))
                name.pop_back();
            path = recordings_dir() + L"\\" + name;
        } else if (!newest_file(path)) {
            BVR_LOG("[rec] no recordings found");
            return;
        }
        if (!read_file(path)) return;
        // Restore the mapping's hidden state - without this every comparison
        // fails (the whole xr->game map routes through it).
        bvr::vr::HeadPose rp{g_header.recenter.px, g_header.recenter.py, g_header.recenter.pz,
                             g_header.recenter.qx, g_header.recenter.qy, g_header.recenter.qz,
                             g_header.recenter.qw};
        camera::set_recenter_state(rp, g_header.recenterYawUnits, g_header.worldScale);
        g_cursor = 0;
        g_marks = 0;
        g_mode = Mode::Playing;
        BVR_LOG("[rec] PLAY %s: %u frames, source=%s (recenter yaw %d, worldScale %.1f "
                "restored)",
                g_fileA, static_cast<uint32_t>(g_entries.size()),
                g_header.sourceVr ? "headset" : "flat", g_header.recenterYawUnits,
                g_header.worldScale);
    } else if (strcmp(verb, "hand") == 0) {
        if (strncmp(rest, "off", 3) == 0) {
            g_handArmed = false;
            bvr::vr::clear_sim_hand_poses();
            BVR_LOG("[rec] hand sim OFF (funnel back to live slots)");
        } else {
            // Optional pitch/yaw/roll degrees; default = straight ahead.
            float p = 0.0f, y = 0.0f, r = 0.0f;
            sscanf_s(rest, "%f %f %f", &p, &y, &r);
            float q[4];
            xr_local_trim_quat(p / kRadToDeg, y / kRadToDeg, r / kRadToDeg, q);
            const float pos[3] = {0.15f, -0.20f, -0.35f};
            for (int i = 0; i < 4; ++i)
                bvr::vr::set_sim_hand_pose(i & 1, i >= 2, true, pos, q);
            g_handArmed = true;
            BVR_LOG("[rec] hand sim ON: all four funnel slots at p=%.1f y=%.1f r=%.1f "
                    "(model+ray+laser follow; 'vrrec hand off' clears)",
                    p, y, r);
        }
    } else if (strcmp(verb, "status") == 0) {
        const char* m = g_mode == Mode::Recording ? "RECORDING"
                        : g_mode == Mode::Playing ? "PLAYING"
                                                  : "idle";
        BVR_LOG("[rec] status: %s | entries=%u cursor=%u marks=%u handSim=%d | last file %s",
                m, static_cast<uint32_t>(g_entries.size()), g_cursor, g_marks,
                g_handArmed ? 1 : 0, g_fileA[0] ? g_fileA : "(none)");
    } else {
        BVR_LOG("[rec] unknown 'vrrec %s' (start|stop|play [file]|hand [p y r|off]|status)",
                verb);
    }
}

} // namespace bvr::b1r::recorder
