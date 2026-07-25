// M7 visible hands + weapons. See hands.h for the design; ENGINE_NOTES
// "Viewmodel / AHands" for the derivations.

#include "game/bioshock1r/hands.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::hands {
namespace {

const uint8_t* g_imageBase = nullptr;

std::atomic<bool> g_enabled{false};
std::atomic<int> g_pendingEnable{-1}; // overlay -> game thread (see aim.cpp)
std::atomic<int> g_handMode{2};       // 0 left, 1 right, 2 auto
std::atomic<int> g_autoHand{1};       // the latched auto choice

// Model offsets. Position is in CENTIMETRES in the grip's own frame (forward /
// right / up), so the numbers stay meaningful whatever the world scale; the
// rotation trim is degrees.
std::atomic<float> g_posFwdCm{0.0f}, g_posRightCm{0.0f}, g_posUpCm{0.0f};
std::atomic<float> g_rotPitchDeg{0.0f}, g_rotYawDeg{0.0f}, g_rotRollDeg{0.0f};

std::atomic<bool> g_writeRot{true}; // rotation write can be disabled on its own
std::atomic<bool> g_probe{false};
int32_t g_probeLeft = 0;

// The live actor, revalidated by vtable on every use (aim.cpp's hand map and
// the settings lookup take the same approach: a cached heap pointer is only
// ever trusted after its vtable still reads right).
void* g_object = nullptr;
uint64_t g_lastScanMs = 0;
void* g_lastPc = nullptr;
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_scans{0};
std::atomic<int32_t> g_lastMatches{0};

// Self-expiring synthetic offset, mirroring `vraim test`: the command file is
// polled at 1 Hz, so a hold has to outlive its command inside the DLL. This is
// the no-headset lane - it proves our write is what places the model.
struct TestOffset {
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    float distUu = 60.0f; // push the model this far along the test direction
    uint64_t deadline = 0;
};
TestOffset g_test;

// Last values written, for the overlay and the flat assertions.
std::atomic<float> g_lastX{0.0f}, g_lastY{0.0f}, g_lastZ{0.0f};
std::atomic<int32_t> g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read12(const void* src, void* out) {
    __try {
        memcpy(out, src, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write12(void* dst, const void* in) {
    __try {
        memcpy(dst, in, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- finding the actor -----------------------------------------------------

struct ScanCtx {
    float camX, camY, camZ;
    bool logEvery;  // probe: describe every instance
    bool chooseAny; // probe: choose nothing, so the whole list gets logged
};

// Runs inside the scan's SEH guard (patterns.cpp), so plain reads are safe.
// A UClass default object carries the same vtable as a live actor but sits at
// the origin with zeroed fields, so proximity to the camera is the test that
// separates the real viewmodel from it: the hands are held by the player and
// are therefore always within arm's reach of the view.
bool accept_hands(void* obj, void* user) {
    ScanCtx* c = static_cast<ScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    float loc[3];
    int32_t rot[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);
    memcpy(rot, p + patterns::kActorViewDirOffset, sizeof rot);

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (c->logEvery)
        BVR_LOG("[hands] AHands match @ %p loc=(%.1f %.1f %.1f) rot=(%d %d %d) "
                "distToCam=%.1f UU",
                obj, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2], dist);
    if (!c->chooseAny) return false;
    // Generous radius: the viewmodel may be anchored at the pawn's feet rather
    // than at the eye, and world scale varies. Anything this close to the
    // camera and not at the world origin is the live one.
    return dist < 2000.0f && (loc[0] != 0.0f || loc[1] != 0.0f || loc[2] != 0.0f);
}

bool object_valid(void* obj) {
    if (!obj) return false;
    void* vtbl = nullptr;
    if (!read_ptr(obj, &vtbl)) return false;
    return to_rva(vtbl) == patterns::kHandsVtableRva;
}

// Cached lookup with the same shape as patterns::hfov_option_ptr(): revalidate
// the cache, and rate-limit rescans so a not-yet-created actor does not scan
// the whole address space every frame.
// `probeOnly` lists every instance and chooses none, leaving the cache alone -
// it is a diagnostic, not a lookup.
void* find_object(const FrameContext& ctx, bool probeOnly) {
    if (!probeOnly) {
        if (object_valid(g_object)) return g_object;
        g_object = nullptr;
        // Rate-limit rescans so a not-yet-created actor does not walk the whole
        // address space every frame.
        uint64_t now = GetTickCount64();
        if (now - g_lastScanMs < 2000) return nullptr;
        g_lastScanMs = now;
    }

    ScanCtx sc{ctx.camX, ctx.camY, ctx.camZ, probeOnly, !probeOnly};
    int matches = 0;
    void* found = patterns::scan_for_vtable_object(
        patterns::kHandsVtableRva, patterns::kActorViewDirOffset + 12, &accept_hands, &sc,
        "AHands", &matches);
    g_scans.fetch_add(1, std::memory_order_relaxed);
    g_lastMatches.store(matches, std::memory_order_relaxed);
    if (probeOnly) return nullptr;
    g_object = found;
    return g_object;
}

// ---- which hand ------------------------------------------------------------

void load_config();
void save_config();

void log_status() {
    BVR_LOG("[hands] status: %s | actor=%p (matches %d, scans %u) | hand=%s | writes=%u",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off", g_object,
            g_lastMatches.load(std::memory_order_relaxed),
            g_scans.load(std::memory_order_relaxed),
            g_handMode.load(std::memory_order_relaxed) == 0   ? "LEFT"
            : g_handMode.load(std::memory_order_relaxed) == 1 ? "RIGHT"
                                                              : "auto",
            g_writes.load(std::memory_order_relaxed));
    BVR_LOG("[hands]   offset pos fwd%+.1f right%+.1f up%+.1f cm | rot pitch%+.1f yaw%+.1f "
            "roll%+.1f deg | writeRot=%d",
            g_posFwdCm.load(std::memory_order_relaxed),
            g_posRightCm.load(std::memory_order_relaxed),
            g_posUpCm.load(std::memory_order_relaxed),
            g_rotPitchDeg.load(std::memory_order_relaxed),
            g_rotYawDeg.load(std::memory_order_relaxed),
            g_rotRollDeg.load(std::memory_order_relaxed),
            g_writeRot.load(std::memory_order_relaxed) ? 1 : 0);
    uint64_t now = GetTickCount64();
    BVR_LOG("[hands]   last write loc=(%.1f %.1f %.1f) rot=(%d %d %d) testHold=%dms",
            g_lastX.load(std::memory_order_relaxed), g_lastY.load(std::memory_order_relaxed),
            g_lastZ.load(std::memory_order_relaxed),
            g_lastPitch.load(std::memory_order_relaxed),
            g_lastYaw.load(std::memory_order_relaxed),
            g_lastRoll.load(std::memory_order_relaxed),
            g_test.deadline > now ? static_cast<int>(g_test.deadline - now) : 0);
}

// ---- persistence -----------------------------------------------------------
// Every weapon model sits differently in the hand, so these numbers are found
// by eye in the headset and must survive the session that found them. Plain
// key=value text in the mod's own data dir - no new dependency, and the user
// can read it.

void config_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\hands.ini", bvr::log::data_dir());
}

void save_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[hands] could not write hands.ini");
        return;
    }
    fprintf(f, "# BioShock VR - M7 viewmodel offsets (cm / degrees, grip-local frame)\n");
    fprintf(f, "posFwdCm=%.2f\n", g_posFwdCm.load(std::memory_order_relaxed));
    fprintf(f, "posRightCm=%.2f\n", g_posRightCm.load(std::memory_order_relaxed));
    fprintf(f, "posUpCm=%.2f\n", g_posUpCm.load(std::memory_order_relaxed));
    fprintf(f, "rotPitchDeg=%.2f\n", g_rotPitchDeg.load(std::memory_order_relaxed));
    fprintf(f, "rotYawDeg=%.2f\n", g_rotYawDeg.load(std::memory_order_relaxed));
    fprintf(f, "rotRollDeg=%.2f\n", g_rotRollDeg.load(std::memory_order_relaxed));
    fclose(f);
    BVR_LOG("[hands] offsets saved to hands.ini");
}

void load_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file yet is normal
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[64] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
            continue;
        ++n;
        if (strcmp(key, "posFwdCm") == 0) g_posFwdCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "posRightCm") == 0) g_posRightCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "posUpCm") == 0) g_posUpCm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotPitchDeg") == 0) g_rotPitchDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotYawDeg") == 0) g_rotYawDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "rotRollDeg") == 0) g_rotRollDeg.store(v, std::memory_order_relaxed);
        else --n;
    }
    fclose(f);
    if (n) BVR_LOG("[hands] loaded %d offset value(s) from hands.ini", n);
}

} // namespace

// BioShock holds ONE thing at a time (the trigger that fires also switches
// hands - XENON_RT/LT), so the viewmodel belongs to whichever hand last fired.
// Seeded from the triggers the bridge itself composes, exactly like aim.cpp's
// object map. Shared with the aim laser so the beam leaves the hand that is
// actually holding the weapon.
int active_hand() {
    int mode = g_handMode.load(std::memory_order_relaxed);
    if (mode == 0 || mode == 1) return mode;

    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    if (rt >= 64 && lt < 64) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lt >= 64 && rt < 64) g_autoHand.store(0, std::memory_order_relaxed);
    return g_autoHand.load(std::memory_order_relaxed);
}

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    load_config();
    BVR_LOG("[hands] init: AHands vtable RVA 0x%X (actor located lazily)",
            patterns::kHandsVtableRva);
}

void on_calcview(const FrameContext& ctx) {
    // Overlay request, applied from THIS thread (same rule as aim.cpp: the
    // render thread must never touch engine state directly).
    int pending = g_pendingEnable.exchange(-1, std::memory_order_relaxed);
    if (pending == 1) {
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[hands] ON (overlay) - viewmodel follows the controller");
    } else if (pending == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF (overlay) - engine placement restored");
    }

    // World change: the old actor died with the old world, and a recycled heap
    // address must never be written to.
    if (ctx.pc != g_lastPc) {
        if (g_lastPc && g_object) BVR_LOG("[hands] world changed - actor cache cleared");
        g_lastPc = ctx.pc;
        g_object = nullptr;
        g_lastScanMs = 0;
    }

    // One-shot probe: describe every instance, choose none.
    if (g_probeLeft > 0) {
        --g_probeLeft;
        find_object(ctx, true);
    }

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Cutscene guard, same predicate the aim ray uses: during normal play the
    // view actor is the player's own pawn.
    bool gameplayView = false;
    if (ctx.viewActor) {
        void* vtbl = nullptr;
        if (read_ptr(ctx.viewActor, &vtbl))
            gameplayView = (to_rva(vtbl) == patterns::kShockPlayerVtableRva);
        if (!gameplayView && ctx.viewActor == ctx.pc) gameplayView = true;
    }
    if (!gameplayView) return;

    void* obj = find_object(ctx, false);
    if (!obj) return;

    // Where the model goes.
    GamePose gp{};
    uint64_t now = GetTickCount64();
    if (now < g_test.deadline) {
        // Synthetic lane, the no-headset test: put the model a fixed distance
        // in front of the camera along a direction the test picks. If the
        // visible gun moves when the yaw changes, our write is what places it.
        gp.rot.yaw = ctx.camYaw + static_cast<int32_t>(g_test.yawDeg * kRotUnitsPerDegree);
        gp.rot.pitch = ctx.camPitch + static_cast<int32_t>(g_test.pitchDeg * kRotUnitsPerDegree);
        gp.rot.roll = 0;
        float dir[3];
        ue_rot_to_dir(gp.rot, dir);
        gp.loc = {ctx.camX + dir[0] * g_test.distUu, ctx.camY + dir[1] * g_test.distUu,
                  ctx.camZ + dir[2] * g_test.distUu};
    } else {
        bvr::vr::HeadPose hp{};
        // GRIP pose - where the hand is, which is what a model wants.
        if (!ctx.vrDriving || !bvr::vr::get_hand_pose(active_hand(), false, hp)) return;
        const float pos[3] = {hp.px, hp.py, hp.pz};
        const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        gp = xr_pose_to_game(ctx, pos, quat);
    }

    // Rotation trim first: the position offset rides the TRIMMED frame, so
    // "2 cm forward" means forward along the barrel as finally oriented.
    gp.rot.pitch += static_cast<int32_t>(g_rotPitchDeg.load(std::memory_order_relaxed) *
                                         kRotUnitsPerDegree);
    gp.rot.yaw += static_cast<int32_t>(g_rotYawDeg.load(std::memory_order_relaxed) *
                                       kRotUnitsPerDegree);
    gp.rot.roll += static_cast<int32_t>(g_rotRollDeg.load(std::memory_order_relaxed) *
                                        kRotUnitsPerDegree);

    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    float uuPerCm = ctx.worldScale / 100.0f;
    float of = g_posFwdCm.load(std::memory_order_relaxed) * uuPerCm;
    float orr = g_posRightCm.load(std::memory_order_relaxed) * uuPerCm;
    float ou = g_posUpCm.load(std::memory_order_relaxed) * uuPerCm;
    float loc[3] = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
                    gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
                    gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};

    uint8_t* p = static_cast<uint8_t*>(obj);
    bool wrote = write12(p + patterns::kActorLocOffset, loc);
    if (g_writeRot.load(std::memory_order_relaxed)) {
        int32_t rot[3] = {gp.rot.pitch, gp.rot.yaw, gp.rot.roll};
        wrote = write12(p + patterns::kActorViewDirOffset, rot) || wrote;
    }
    if (!wrote) {
        g_object = nullptr; // the write faulted - stop trusting this pointer
        return;
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastX.store(loc[0], std::memory_order_relaxed);
    g_lastY.store(loc[1], std::memory_order_relaxed);
    g_lastZ.store(loc[2], std::memory_order_relaxed);
    g_lastPitch.store(gp.rot.pitch, std::memory_order_relaxed);
    g_lastYaw.store(gp.rot.yaw, std::memory_order_relaxed);
    g_lastRoll.store(gp.rot.roll, std::memory_order_relaxed);
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        log_status();
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[hands] ON - viewmodel follows the controller");
        log_status();
    } else if (strcmp(verb, "off") == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF - engine placement restored");
    } else if (strcmp(verb, "probe") == 0) {
        int n = 1;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 1;
        if (n > 30) n = 30;
        g_probeLeft = n;
        BVR_LOG("[hands] probe armed for %d frame(s) - listing every AHands instance", n);
    } else if (strcmp(verb, "hand") == 0) {
        int mode = rest[0] == 'l' ? 0 : rest[0] == 'r' ? 1 : 2;
        g_handMode.store(mode, std::memory_order_relaxed);
        BVR_LOG("[hands] hand = %s", mode == 0 ? "LEFT" : mode == 1 ? "RIGHT" : "auto");
    } else if (strcmp(verb, "pos") == 0) {
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &f, &r, &u) == 3) {
            g_posFwdCm.store(f, std::memory_order_relaxed);
            g_posRightCm.store(r, std::memory_order_relaxed);
            g_posUpCm.store(u, std::memory_order_relaxed);
            BVR_LOG("[hands] pos offset fwd%+.1f right%+.1f up%+.1f cm", f, r, u);
        } else {
            BVR_LOG("[hands] usage: vrhands pos <fwdCm> <rightCm> <upCm>");
        }
    } else if (strcmp(verb, "rot") == 0) {
        float p = 0.0f, y = 0.0f, r = 0.0f;
        if (sscanf_s(rest, "%f %f %f", &p, &y, &r) == 3) {
            g_rotPitchDeg.store(p, std::memory_order_relaxed);
            g_rotYawDeg.store(y, std::memory_order_relaxed);
            g_rotRollDeg.store(r, std::memory_order_relaxed);
            BVR_LOG("[hands] rot trim pitch%+.1f yaw%+.1f roll%+.1f deg", p, y, r);
        } else {
            BVR_LOG("[hands] usage: vrhands rot <pitchDeg> <yawDeg> <rollDeg>");
        }
    } else if (strcmp(verb, "writerot") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_writeRot.store(on, std::memory_order_relaxed);
        BVR_LOG("[hands] rotation write %s", on ? "ON" : "off (position only)");
    } else if (strcmp(verb, "save") == 0) {
        save_config();
    } else if (strcmp(verb, "reload") == 0) {
        load_config();
        log_status();
    } else if (strcmp(verb, "test") == 0) {
        float yaw = 0.0f, pitch = 0.0f, dist = 60.0f;
        int hold = 0;
        int n = sscanf_s(rest, "%f %f %f %d", &yaw, &pitch, &dist, &hold);
        if (n < 2) {
            BVR_LOG("[hands] usage: vrhands test <yawDeg> <pitchDeg> [distUU] [holdMs]");
            return;
        }
        if (n < 3 || dist <= 0.0f) dist = 60.0f;
        if (hold <= 0) hold = 30000;
        if (hold > 120000) hold = 120000;
        g_test.yawDeg = yaw;
        g_test.pitchDeg = pitch;
        g_test.distUu = dist;
        g_test.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[hands] test placement: yaw %+.1f pitch %+.1f dist %.0f UU for %d ms", yaw,
                pitch, dist, hold);
    } else if (strcmp(verb, "testclear") == 0) {
        g_test.deadline = 0;
        BVR_LOG("[hands] test placement cleared");
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else {
        BVR_LOG("[hands] unknown command '%s' "
                "(on|off|probe|hand|pos|rot|writerot|save|reload|test|status)",
                verb);
    }
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) && g_object != nullptr;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Hands + weapon (M7)")) return;

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Viewmodel follows the controller", &on))
        g_pendingEnable.store(on ? 1 : 0, std::memory_order_relaxed);

    int mode = g_handMode.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("left", &mode, 0)) g_handMode.store(0, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("right", &mode, 1)) g_handMode.store(1, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("auto", &mode, 2)) g_handMode.store(2, std::memory_order_relaxed);

    float f = g_posFwdCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset forward (cm)", &f, -30.0f, 30.0f))
        g_posFwdCm.store(f, std::memory_order_relaxed);
    float r = g_posRightCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset right (cm)", &r, -30.0f, 30.0f))
        g_posRightCm.store(r, std::memory_order_relaxed);
    float u = g_posUpCm.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset up (cm)", &u, -30.0f, 30.0f))
        g_posUpCm.store(u, std::memory_order_relaxed);

    float rp = g_rotPitchDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim pitch (deg)", &rp, -90.0f, 90.0f))
        g_rotPitchDeg.store(rp, std::memory_order_relaxed);
    float ry = g_rotYawDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim yaw (deg)", &ry, -90.0f, 90.0f))
        g_rotYawDeg.store(ry, std::memory_order_relaxed);
    float rr = g_rotRollDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim roll (deg)", &rr, -90.0f, 90.0f))
        g_rotRollDeg.store(rr, std::memory_order_relaxed);

    if (ImGui::Button("Save offsets")) save_config();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) load_config();

    ImGui::Text("actor %p (matches %d) | writes %u", g_object,
                g_lastMatches.load(std::memory_order_relaxed),
                g_writes.load(std::memory_order_relaxed));
    ImGui::Text("last loc (%.0f %.0f %.0f) rot (%d %d %d)",
                g_lastX.load(std::memory_order_relaxed),
                g_lastY.load(std::memory_order_relaxed),
                g_lastZ.load(std::memory_order_relaxed),
                g_lastPitch.load(std::memory_order_relaxed),
                g_lastYaw.load(std::memory_order_relaxed),
                g_lastRoll.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::hands
