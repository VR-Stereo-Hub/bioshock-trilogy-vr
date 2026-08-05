// I6 rung 5: the config registry and named presets. See config.h for the
// adapter-local decision and the resolution-latch exception.

#include "game/bioshockinf/config.h"

#include "core/util/log.h"

#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::bsi::config {
namespace {

const KeyDesc* g_keys = nullptr;
size_t g_keyCount = 0;

// F10 -> game thread ops (file IO + appliers run on the game thread). Slot
// index 1..4; op 0 none / 1 save / 2 load / 3 save current / 4 load current.
std::atomic<int> g_pendingOp{0};
std::atomic<int> g_pendingSlot{0};

// The resolution latch (see header). Written by the resW/resH setters the
// camera registers; consumed once by the picker UI.
std::atomic<int> g_wantResW{0}, g_wantResH{0};
std::atomic<bool> g_wantResFresh{false};

// 1 Hz cached preset-directory listing for the overlay (render thread).
constexpr int kMaxList = 12;
char g_list[kMaxList][64];
int g_listCount = 0;
uint64_t g_listStampMs = 0;

std::wstring presets_dir() {
    std::wstring p = bvr::log::data_dir();
    p += L"\\presets";
    return p;
}

std::wstring preset_path(const char* name) {
    wchar_t wname[64] = L"";
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, static_cast<int>(_countof(wname)));
    std::wstring p = presets_dir();
    p += L"\\";
    p += wname;
    p += L".ini";
    return p;
}

std::wstring current_path() {
    std::wstring p = bvr::log::data_dir();
    p += L"\\vrpreset.ini";
    return p;
}

// A preset name is a filename component: letters, digits, dash, underscore.
bool name_ok(const char* name) {
    if (!name || !*name) return false;
    for (const char* c = name; *c; ++c) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
        if (!ok) return false;
    }
    return strlen(name) < 48;
}

bool save_to(const std::wstring& path, const char* what) {
    if (!g_keys) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
        BVR_LOG("[bsi] config: save FAILED (open) - %s", what);
        return false;
    }
    for (size_t i = 0; i < g_keyCount; ++i)
        fprintf(f, "%s=%.4f\n", g_keys[i].key, g_keys[i].get());
    fclose(f);
    char line[256];
    int n = _snprintf_s(line, sizeof line, _TRUNCATE, "[bsi] config: %s saved (", what);
    for (size_t i = 0; i < g_keyCount && n > 0 && n < 220; ++i)
        n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "%s%s=%.4g", i ? " " : "",
                         g_keys[i].key, g_keys[i].get());
    if (n > 0) _snprintf_s(line + n, sizeof line - n, _TRUNCATE, ")");
    BVR_LOG("%s", line);
    return true;
}

bool load_from(const std::wstring& path, const char* what) {
    if (!g_keys) return false;
    FILE* f = _wfsopen(path.c_str(), L"r", _SH_DENYNO);
    if (!f) {
        BVR_LOG("[bsi] config: %s not found - values unchanged", what);
        return false;
    }
    char line[128];
    int applied = 0, skipped = 0;
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        float v = 0.0f;
        if (sscanf_s(eq + 1, "%f", &v) != 1) continue;
        bool known = false;
        for (size_t i = 0; i < g_keyCount; ++i) {
            if (strcmp(line, g_keys[i].key) != 0) continue;
            known = true;
            if (v >= g_keys[i].lo && v <= g_keys[i].hi) {
                g_keys[i].set(v);
                ++applied;
            } else {
                ++skipped;
            }
            break;
        }
        if (!known) ++skipped; // unknown lines skip silently (forward compat)
    }
    fclose(f);
    BVR_LOG("[bsi] config: %s loaded (%d applied, %d skipped)", what, applied, skipped);
    return applied > 0;
}

void refresh_list() {
    const uint64_t now = GetTickCount64();
    if (now - g_listStampMs < 1000) return;
    g_listStampMs = now;
    g_listCount = 0;
    std::wstring pattern = presets_dir();
    pattern += L"\\*.ini";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name - 1, nullptr,
                            nullptr);
        char* dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        if (g_listCount < kMaxList) {
            strcpy_s(g_list[g_listCount], name);
            ++g_listCount;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

void init(const KeyDesc* keys, size_t n) {
    g_keys = keys;
    g_keyCount = n;
}

void save_current() {
    save_to(current_path(), "vrpreset");
}

void load_current() {
    load_from(current_path(), "vrpreset");
}

bool save_named(const char* name) {
    if (!name_ok(name)) {
        BVR_LOG("[bsi] config: bad preset name (letters/digits/-/_ only)");
        return false;
    }
    CreateDirectoryW(presets_dir().c_str(), nullptr); // idempotent
    char what[64];
    _snprintf_s(what, sizeof what, _TRUNCATE, "preset '%s'", name);
    return save_to(preset_path(name), what);
}

bool load_named(const char* name) {
    if (!name_ok(name)) {
        BVR_LOG("[bsi] config: bad preset name (letters/digits/-/_ only)");
        return false;
    }
    char what[64];
    _snprintf_s(what, sizeof what, _TRUNCATE, "preset '%s'", name);
    return load_from(preset_path(name), what);
}

void handle_vrpreset(const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;
    char tok[64] = "", name[64] = "";
    sscanf_s(args, "%63s %63s", tok, static_cast<unsigned>(sizeof tok), name,
             static_cast<unsigned>(sizeof name));
    if (strcmp(tok, "save") == 0) {
        save_current();
    } else if (strcmp(tok, "saveas") == 0) {
        save_named(name);
    } else if (strcmp(tok, "load") == 0 && name[0]) {
        load_named(name);
    } else if (strcmp(tok, "list") == 0) {
        g_listStampMs = 0;
        refresh_list();
        BVR_LOG("[bsi] config: %d preset(s) in presets\\", g_listCount);
        for (int i = 0; i < g_listCount; ++i) BVR_LOG("[bsi]   %s", g_list[i]);
    } else if (!tok[0] || strcmp(tok, "load") == 0) {
        load_current();
    } else {
        BVR_LOG("[bsi] usage: vrpreset [save | saveas <name> | load [<name>] | list]");
    }
}

void tick() {
    const int op = g_pendingOp.exchange(0, std::memory_order_relaxed);
    if (!op) return;
    const int slot = g_pendingSlot.load(std::memory_order_relaxed);
    char name[8];
    _snprintf_s(name, sizeof name, _TRUNCATE, "slot%d", slot);
    switch (op) {
    case 1: save_named(name); break;
    case 2: load_named(name); break;
    case 3: save_current(); break;
    case 4: load_current(); break;
    default: break;
    }
}

bool wanted_resolution(int* w, int* h, bool* fresh) {
    const int ww = g_wantResW.load(std::memory_order_relaxed);
    const int hh = g_wantResH.load(std::memory_order_relaxed);
    if (w) *w = ww;
    if (h) *h = hh;
    if (fresh) *fresh = g_wantResFresh.exchange(false, std::memory_order_relaxed);
    return ww > 0 && hh > 0;
}

// The resW/resH setters the camera registers route here (config.h: loads
// LATCH resolution rather than applying it).
namespace detail {
void latch_wanted_res_w(float v) {
    g_wantResW.store(static_cast<int>(v + 0.5f), std::memory_order_relaxed);
    g_wantResFresh.store(true, std::memory_order_relaxed);
}
void latch_wanted_res_h(float v) {
    g_wantResH.store(static_cast<int>(v + 0.5f), std::memory_order_relaxed);
    g_wantResFresh.store(true, std::memory_order_relaxed);
}
} // namespace detail

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("CONFIG / PRESETS (I6)")) return;
    // Render thread: buttons post ops; the game thread's tick() does the IO.
    for (int slot = 1; slot <= 4; ++slot) {
        char label[24];
        _snprintf_s(label, sizeof label, _TRUNCATE, "Save slot %d", slot);
        if (ImGui::SmallButton(label)) {
            g_pendingSlot.store(slot, std::memory_order_relaxed);
            g_pendingOp.store(1, std::memory_order_relaxed);
        }
        ImGui::SameLine();
        _snprintf_s(label, sizeof label, _TRUNCATE, "Load slot %d", slot);
        if (ImGui::SmallButton(label)) {
            g_pendingSlot.store(slot, std::memory_order_relaxed);
            g_pendingOp.store(2, std::memory_order_relaxed);
        }
        if (slot < 4) ImGui::SameLine(0.0f, 18.0f);
    }
    if (ImGui::SmallButton("Save current (vrpreset)"))
        g_pendingOp.store(3, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload current"))
        g_pendingOp.store(4, std::memory_order_relaxed);

    refresh_list();
    if (g_listCount > 0) {
        ImGui::Text("presets on disk:");
        for (int i = 0; i < g_listCount; ++i) {
            ImGui::BulletText("%s", g_list[i]);
            ImGui::SameLine();
            char label[80];
            _snprintf_s(label, sizeof label, _TRUNCATE, "Load##%s", g_list[i]);
            if (ImGui::SmallButton(label)) {
                // Named loads reuse the slot lane only for slot names; for
                // arbitrary names post via a small static copy the tick can
                // read - simplest: match slotN, else fall back to a direct
                // note in the log (desktop verb covers arbitrary names).
                int slot = 0;
                if (sscanf_s(g_list[i], "slot%d", &slot) == 1 && slot >= 1 && slot <= 4) {
                    g_pendingSlot.store(slot, std::memory_order_relaxed);
                    g_pendingOp.store(2, std::memory_order_relaxed);
                } else {
                    BVR_LOG("[bsi] config: load non-slot presets from the desktop - "
                            "`vrpreset load %s` (in-headset buttons cover slot1..slot4)",
                            g_list[i]);
                }
            }
        }
    } else {
        ImGui::TextDisabled("no presets saved yet (Save slot 1..4, or vrpreset saveas <name>)");
    }
    if (g_keys) {
        ImGui::Separator();
        for (size_t i = 0; i < g_keyCount; ++i)
            ImGui::Text("%-14s %.4g", g_keys[i].key, g_keys[i].get());
    }
    ImGui::TextDisabled("a loaded preset's RESOLUTION is latched, not applied - use the "
                        "RENDER RESOLUTION Apply above");
}

} // namespace bvr::bsi::config
