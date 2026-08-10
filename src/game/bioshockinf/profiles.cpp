#include "game/bioshockinf/profiles.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/aim.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/hands.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::bsi::profiles {
namespace {

// One hand's full lever set - the 12 values a weapon owns while held.
struct Slots {
    float aimTrimP, aimTrimY;
    float aimPosF, aimPosR, aimPosU;
    float trimP, trimY, trimR;
    float offF, offR, offU;
    float scale;
};
constexpr const char* kLeverNames[12] = {
    "aimTrimP", "aimTrimY", "aimPosF", "aimPosR", "aimPosU", "trimP",
    "trimY",    "trimR",    "offF",    "offR",    "offU",    "scale",
};

struct Entry {
    char key[64];
    bool valid;
    Slots s;
};
Entry g_table[64]; // game thread owns the table
int g_count = 0;

// Current identity per hand (0 = L/vigor, 1 = R/gun). Game thread writes;
// the overlay reads for display only (text tear is cosmetic).
char g_key[2][64] = {"", ""};
uint32_t g_applies = 0, g_captures = 0;
uint64_t g_lastPollMs = 0;
bool g_loaded = false;

// Overlay -> game-thread actions (render thread must not touch the table).
std::atomic<bool> g_pendSave{false};
std::atomic<bool> g_pendClearAll{false};

// s47 legacy fire-seam latch, status-line context only.
void* g_lastWeapon = reinterpret_cast<void*>(~0u);
char g_lastClass[64] = "(no shot yet)";
uint32_t g_latches = 0;

Slots read_levers(int hand) {
    Slots s{};
    s.aimTrimP = aim::trim_get(hand, 0);
    s.aimTrimY = aim::trim_get(hand, 1);
    s.aimPosF = aim::origin_get(hand, 0);
    s.aimPosR = aim::origin_get(hand, 1);
    s.aimPosU = aim::origin_get(hand, 2);
    s.trimP = hands::trim_get(hand, 0);
    s.trimY = hands::trim_get(hand, 1);
    s.trimR = hands::trim_get(hand, 2);
    s.offF = hands::offset_get(hand, 0);
    s.offR = hands::offset_get(hand, 1);
    s.offU = hands::offset_get(hand, 2);
    s.scale = hands::scale_get(hand);
    return s;
}

void write_levers(int hand, const Slots& s) {
    aim::trim_set(hand, 0, s.aimTrimP);
    aim::trim_set(hand, 1, s.aimTrimY);
    aim::origin_set(hand, 0, s.aimPosF);
    aim::origin_set(hand, 1, s.aimPosR);
    aim::origin_set(hand, 2, s.aimPosU);
    hands::trim_set(hand, 0, s.trimP);
    hands::trim_set(hand, 1, s.trimY);
    hands::trim_set(hand, 2, s.trimR);
    hands::offset_set(hand, 0, s.offF);
    hands::offset_set(hand, 1, s.offR);
    hands::offset_set(hand, 2, s.offU);
    hands::scale_set(hand, s.scale);
}

float* lever_ptr(Slots& s, int i) { return reinterpret_cast<float*>(&s) + i; }

Entry* find_entry(const char* key) {
    for (int i = 0; i < g_count; ++i)
        if (g_table[i].valid && strcmp(g_table[i].key, key) == 0) return &g_table[i];
    return nullptr;
}

Entry* find_or_create(const char* key) {
    if (Entry* e = find_entry(key)) return e;
    if (g_count >= static_cast<int>(std::size(g_table))) return nullptr;
    Entry* e = &g_table[g_count++];
    strcpy_s(e->key, key);
    e->valid = true;
    return e;
}

std::wstring file_path() {
    std::wstring p = bvr::log::data_dir();
    p += L"\\weapons.ini";
    return p;
}

void save_file() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, file_path().c_str(), L"w") != 0 || !f) {
        BVR_LOG("[bsi] profiles: save FAILED (open)");
        return;
    }
    for (int i = 0; i < g_count; ++i) {
        if (!g_table[i].valid) continue;
        Slots s = g_table[i].s;
        for (int k = 0; k < 12; ++k)
            fprintf(f, "%s.%s=%.4f\n", g_table[i].key, kLeverNames[k], *lever_ptr(s, k));
    }
    fclose(f);
    BVR_LOG("[bsi] profiles: weapons.ini saved (%d entr%s)", g_count,
            g_count == 1 ? "y" : "ies");
}

void load_file() {
    FILE* f = _wfsopen(file_path().c_str(), L"r", _SH_DENYNO);
    if (!f) {
        BVR_LOG("[bsi] profiles: weapons.ini not found - no per-weapon overrides");
        return;
    }
    char line[160];
    int applied = 0;
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        char* dot = strchr(line, '.');
        if (!eq || !dot || dot > eq) continue;
        *dot = '\0';
        *eq = '\0';
        float v = 0.0f;
        if (sscanf_s(eq + 1, "%f", &v) != 1) continue;
        int lever = -1;
        for (int k = 0; k < 12; ++k)
            if (strcmp(dot + 1, kLeverNames[k]) == 0) { lever = k; break; }
        if (lever < 0) continue; // unknown levers skip silently (forward compat)
        Entry* e = find_or_create(line);
        if (!e) break;
        *lever_ptr(e->s, lever) = v;
        ++applied;
    }
    fclose(f);
    BVR_LOG("[bsi] profiles: weapons.ini loaded (%d entr%s, %d values)", g_count,
            g_count == 1 ? "y" : "ies", applied);
}

// The equipped identity for one selector (0 gun / 1 vigor), as the archetype
// name. False when the reflection gates are closed or nothing is equipped.
bool equipped_key(int selector, char* out, size_t outSize) {
    out[0] = '\0';
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4))
        return false;
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn) return false;
    // fname_find never on a cadence (the s52 stutter) - index cached per boot.
    static int32_t s_gewIdx = -1;
    if (s_gewIdx < 0) s_gewIdx = reflect::find_function_index("GetEquippedWeapon");
    if (s_gewIdx < 0) return false;
    uint8_t parms[64] = {};
    memcpy(parms, &selector, sizeof selector);
    if (!reflect::call_on_object_by_index(pawn, s_gewIdx, parms)) return false;
    void* instance = nullptr;
    memcpy(&instance, parms + 4, sizeof instance); // return slot after the selector
    if (!instance ||
        !bvr::pattern_scan::is_memory_valid(instance, patterns::kUObjectArchetypeOffset + 4))
        return false;
    void* archetype = *reinterpret_cast<void* const*>(
        static_cast<const uint8_t*>(instance) + patterns::kUObjectArchetypeOffset);
    const int32_t ni = reflect::object_name_index(archetype);
    if (ni <= 0 || ni >= patterns::fname_count()) return false;
    return patterns::fname_text(ni, out, outSize) && out[0];
}

// selector 0 (gun) rides the RIGHT hand's levers (hand 1); selector 1
// (vigor) the LEFT's (hand 0) - this game never crosses them.
constexpr int kHandForSelector[2] = {1, 0};

void poll_hand(int selector) {
    char key[64];
    if (!equipped_key(selector, key, sizeof key)) return;
    const int hand = kHandForSelector[selector];
    char* cur = g_key[hand];
    if (strcmp(cur, key) == 0) return;
    // Switch edge: capture the live levers into the OUTGOING key, then apply
    // the INCOMING key's entry if one exists. An absent entry leaves the
    // levers untouched (the empty-profile path is byte-identical to the
    // pre-profile build).
    if (cur[0]) {
        if (Entry* old = find_or_create(cur)) {
            old->s = read_levers(hand);
            ++g_captures;
        }
    }
    if (const Entry* e = find_entry(key)) {
        write_levers(hand, e->s);
        ++g_applies;
        BVR_LOG("[bsi] profiles: '%s' -> hand %c, OVERRIDE APPLIED (capture of '%s' kept)",
                key, hand ? 'R' : 'L', cur[0] ? cur : "-");
    } else {
        BVR_LOG("[bsi] profiles: '%s' -> hand %c, no entry - levers unchanged%s", key,
                hand ? 'R' : 'L',
                cur[0] ? " (outgoing captured)" : "");
    }
    strcpy_s(g_key[hand], 64, key);
}

} // namespace

void init() {
    if (g_loaded) return;
    g_loaded = true;
    load_file();
}

void tick() {
    const uint64_t now = GetTickCount64();
    if (now - g_lastPollMs < 1000) return;
    g_lastPollMs = now;
    if (g_pendClearAll.exchange(false, std::memory_order_relaxed)) {
        g_count = 0;
        g_key[0][0] = g_key[1][0] = '\0';
        BVR_LOG("[bsi] profiles: table cleared");
    }
    poll_hand(0);
    poll_hand(1);
    if (g_pendSave.exchange(false, std::memory_order_relaxed)) {
        // Bank the live levers for the CURRENT keys first, so "save" means
        // "save what I am looking at", not "save the last switch".
        for (int hand = 0; hand < 2; ++hand)
            if (g_key[hand][0])
                if (Entry* e = find_or_create(g_key[hand])) e->s = read_levers(hand);
        save_file();
    }
}

void note_weapon_object(void* weaponObj) {
    if (weaponObj == g_lastWeapon) return;
    g_lastWeapon = weaponObj;
    ++g_latches;
    if (!weaponObj) {
        strcpy_s(g_lastClass, "(none - seam param null)");
        return;
    }
    if (!reflect::class_name_of(weaponObj, g_lastClass, sizeof g_lastClass) ||
        !g_lastClass[0])
        strcpy_s(g_lastClass, "(unreadable)");
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsiprofiles") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    auto tok = [&](const char* w) {
        const size_t n = strlen(w);
        if (strncmp(args, w, n) != 0) return false;
        const char c = args[n];
        return c == '\0' || c == ' ' || c == '\n' || c == '\r' || c == '\t';
    };
    if (tok("list")) {
        for (int i = 0; i < g_count; ++i)
            if (g_table[i].valid)
                BVR_LOG("[bsi] profiles:   %-40s aimTrim(%.1f %.1f) scale %.2f",
                        g_table[i].key, g_table[i].s.aimTrimP, g_table[i].s.aimTrimY,
                        g_table[i].s.scale);
        BVR_LOG("[bsi] profiles: %d entr%s", g_count, g_count == 1 ? "y" : "ies");
    } else if (tok("save")) {
        g_pendSave.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] profiles: save queued (next game tick)");
    } else if (tok("clear")) {
        const char* rest = args + 5;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "all", 3) == 0) {
            g_pendClearAll.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] profiles: clear-all queued");
        } else {
            char key[64] = {};
            sscanf_s(rest, "%63s", key, static_cast<unsigned>(sizeof key));
            if (Entry* e = find_entry(key)) {
                e->valid = false;
                BVR_LOG("[bsi] profiles: '%s' cleared (file rewrites on next save)", key);
            } else {
                BVR_LOG("[bsi] profiles: '%s' has no entry", key);
            }
        }
    } else {
        BVR_LOG("[bsi] profiles: gun[R]='%s' vigor[L]='%s' | %d entr%s, %u applies, "
                "%u captures | fire-seam latch '%s' (%u) | usage: bsiprofiles "
                "list|save|clear <key>|clear all",
                g_key[1][0] ? g_key[1] : "-", g_key[0][0] ? g_key[0] : "-", g_count,
                g_count == 1 ? "y" : "ies", g_applies, g_captures, g_lastClass, g_latches);
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("WEAPON PROFILES (I9)")) return;
    ImGui::Text("gun  [R]: %s", g_key[1][0] ? g_key[1] : "-");
    ImGui::Text("vigor[L]: %s", g_key[0][0] ? g_key[0] : "-");
    ImGui::Text("%d entries, %u applies, %u captures", g_count, g_applies, g_captures);
    ImGui::TextDisabled("tune with the AIM/HANDS sliders above; switching weapons");
    ImGui::TextDisabled("auto-captures the outgoing weapon's values");
    if (ImGui::Button("SAVE weapons.ini")) g_pendSave.store(true, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::Button("CLEAR all")) g_pendClearAll.store(true, std::memory_order_relaxed);
}

} // namespace bvr::bsi::profiles
