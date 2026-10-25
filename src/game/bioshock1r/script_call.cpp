#include "game/bioshock1r/script_call.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <cctype>
#include <map>
#include <unordered_map>
#include <string>
#include <utility>

namespace bvr::b1r::script {
namespace {

using FindFunctionFn = void*(__thiscall*)(void* obj, int32_t nameIndex, int32_t nameNumber, int32_t global);
using ProcessEventFn = void(__thiscall*)(void* obj, void* fn, void* parms, void* result);
using FindFieldFn = void*(__fastcall*)(void* strct, const wchar_t* name);
FindFunctionFn g_find = nullptr;
ProcessEventFn g_process = nullptr;
FindFieldFn g_findField = nullptr;
std::map<std::pair<const void*, std::wstring>, int32_t> g_props; // (class, name) -> offset, -1 absent
std::atomic<uint32_t> g_calls{0}, g_failures{0};

// Game thread only (every caller is), so plain maps.
std::map<std::string, int32_t> g_names;
std::map<std::pair<const void*, int32_t>, void*> g_functions; // (class, name) -> UFunction*

bool read_ptr(const void* p, void** out) {
    __try {
        *out = *static_cast<void* const*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* find_guarded(void* obj, int32_t name) {
    __try {
        return g_find(obj, name, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* find_field_guarded(void* cls, const wchar_t* name) {
    __try {
        return g_findField(cls, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool process_guarded(void* obj, void* fn, void* parms) {
    __try {
        g_process(obj, fn, parms, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* function_of(void* obj, const char* fname) {
    const int32_t name = name_index(fname);
    if (name < 0) return nullptr;
    void* cls = nullptr;
    if (!read_ptr(static_cast<uint8_t*>(obj) + patterns::kUObjectClassOffset, &cls) || !cls) return nullptr;
    const auto key = std::make_pair(static_cast<const void*>(cls), name);
    const auto it = g_functions.find(key);
    if (it != g_functions.end()) return it->second;
    void* fn = find_guarded(obj, name);
    g_functions[key] = fn;
    if (!fn) BVR_LOG("[script] %s: not found on this object's class (%p)", fname, cls);
    return fn;
}

struct HandleOnly { int32_t handle; int32_t ret[4]; };
} // namespace

void init(void* findFunction, void* processEvent, void* findField) {
    g_find = reinterpret_cast<FindFunctionFn>(findFunction);
    g_process = reinterpret_cast<ProcessEventFn>(processEvent);
    g_findField = reinterpret_cast<FindFieldFn>(findField);
    BVR_LOG("[script] calls by name %s, properties by name %s", ready() ? "ready" : "UNAVAILABLE (a prologue did not match)",
            g_findField ? "pending the live check" : "UNAVAILABLE (FindField did not match)");
}

bool ready() { return g_find && g_process; }

int32_t property_offset(void* obj, const wchar_t* name) {
    if (!g_findField || !obj || !name) return -1;
    void* cls = nullptr;
    if (!read_ptr(static_cast<uint8_t*>(obj) + patterns::kUObjectClassOffset, &cls) || !cls) return -1;
    const auto key = std::make_pair(static_cast<const void*>(cls), std::wstring(name));
    const auto it = g_props.find(key);
    if (it != g_props.end()) return it->second;
    int32_t off = -1;
    void* prop = find_field_guarded(cls, name);
    void* raw = nullptr;
    if (prop && read_ptr(static_cast<uint8_t*>(prop) + patterns::kPropertyOffsetOffset, &raw))
        off = static_cast<int32_t>(reinterpret_cast<intptr_t>(raw));
    if (off < 0 || off > 0x10000) off = -1;
    g_props[key] = off;
    if (off < 0) BVR_LOG("[script] property %S: not found on this object's class (%p)", name, cls);
    return off;
}

int32_t class_property_offset(void* cls, const wchar_t* name) {
    if (!g_findField || !cls || !name) return -1;
    void* prop = find_field_guarded(cls, name);
    void* raw = nullptr;
    if (!prop || !read_ptr(static_cast<uint8_t*>(prop) + patterns::kPropertyOffsetOffset, &raw)) return -1;
    const int32_t off = static_cast<int32_t>(reinterpret_cast<intptr_t>(raw));
    return off >= 0 && off <= 0x10000 ? off : -1;
}

void* class_defaults(void* cls) {
    void* data = nullptr;
    return cls && read_ptr(static_cast<uint8_t*>(cls) + patterns::kClassDefaultsOffset, &data) ? data : nullptr;
}

bool properties_ready(void* anyActor) {
    static int s_state = 0; // 0 unchecked, 1 good, -1 refused
    if (s_state || !anyActor) return s_state > 0;
    // Two answers known independently (ENGINE_NOTES: Location +0x1D8 from the viewmodel actor, DrawScale
    // +0x2AC from AActor::SetDrawScale) - the property offset field must reproduce both.
    const int32_t loc = property_offset(anyActor, L"Location"), ds = property_offset(anyActor, L"DrawScale");
    s_state = loc == static_cast<int32_t>(patterns::kActorLocOffset) &&
                      ds == static_cast<int32_t>(patterns::kActorDrawScaleOffset)
                  ? 1
                  : -1;
    BVR_LOG("[script] properties by name %s: Actor.Location at +0x%X (want 0x%X), DrawScale at +0x%X (want 0x%X)",
            s_state > 0 ? "CHECKED" : "REFUSED", loc, patterns::kActorLocOffset, ds, patterns::kActorDrawScaleOffset);
    return s_state > 0;
}

// 2026-10-20: the whole name table in ONE pass, lower-cased (the first headset eject froze a quarter
// of a second: each new name was a full scan of GNames, and the first drop needs about ten). Rebuilt
// when a name is missing and the table has grown since (a level load adds names).
static std::unordered_map<std::string, int32_t> g_index;
static int32_t g_indexedCount = 0;
static void build_index() {
    const uint64_t t0 = GetTickCount64();
    const int32_t n = patterns::fname_count();
    g_index.clear();
    g_index.reserve(static_cast<size_t>(n > 0 ? n : 0));
    std::string key;
    for (int32_t i = 0; i < n; ++i) {
        const wchar_t* t = patterns::fname_text(i);
        if (!t || !*t) continue;
        key.clear();
        for (const wchar_t* c = t; *c && key.size() < 128; ++c)
            key.push_back(static_cast<char>(*c < 128 ? tolower(static_cast<int>(*c)) : '?'));
        g_index.emplace(key, i); // the first index of a name wins, as the scan's did
    }
    g_indexedCount = n;
    BVR_LOG("[script] name index: %d names in %llu ms", n, static_cast<unsigned long long>(GetTickCount64() - t0));
}

int32_t name_index(const char* text) {
    const auto it = g_names.find(text);
    if (it != g_names.end()) return it->second;
    const int32_t n = patterns::fname_count();
    if (n <= 0) return -1; // too early: nothing is cached as "absent"
    std::string key(text);
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (g_index.empty() || g_indexedCount <= 0) build_index();
    auto hit = g_index.find(key);
    if (hit == g_index.end() && n > g_indexedCount) {
        build_index();
        hit = g_index.find(key);
    }
    const int32_t found = hit != g_index.end() ? hit->second : -1;
    g_names[text] = found;
    if (found < 0) BVR_LOG("[script] name '%s' not in GNames (%d names)", text, n);
    return found;
}

void warm_names() {
    if (patterns::fname_count() > 0 && g_index.empty()) build_index();
}

bool call(void* obj, const char* fname, void* parms) {
    if (!ready() || !obj) return false;
    void* fn = function_of(obj, fname);
    if (!fn) return false;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (process_guarded(obj, fn, parms)) return true;
    if (g_failures.fetch_add(1, std::memory_order_relaxed) < 5) BVR_LOG("[script] %s FAULTED (caught)", fname);
    return false;
}

bool anim_valid(void* a, int32_t h) {
    HandleOnly p{h, {}};
    return call(a, "IsAnimationHandleValid", &p) && p.ret[0] != 0;
}
bool anim_name(void* a, int32_t h, int32_t* name) {
    struct { int32_t handle; int32_t ret[2]; int32_t pad[2]; } p{h, {-1, 0}, {}};
    if (!call(a, "GetAnimationName", &p)) return false;
    *name = p.ret[0];
    return true;
}
bool anim_time(void* a, int32_t h, float* s) {
    struct { int32_t handle; float ret; int32_t pad[2]; } p{h, 0.0f, {}};
    if (!call(a, "GetAnimationCurrentTime", &p)) return false;
    *s = p.ret;
    return true;
}
bool set_anim_time(void* a, int32_t h, float s) {
    struct { int32_t handle; float t; int32_t ret; int32_t pad; } p{h, s, 0, 0};
    return call(a, "SetAnimationCurrentTime", &p) && p.ret != 0;
}
bool anim_rate(void* a, int32_t h, float* r) {
    struct { int32_t handle; float ret; int32_t pad[2]; } p{h, 0.0f, {}};
    if (!call(a, "GetAnimationPlaybackRate", &p)) return false;
    *r = p.ret;
    return true;
}
bool set_anim_rate(void* a, int32_t h, float r) {
    struct { int32_t handle; float rate; int32_t ret; int32_t pad; } p{h, r, 0, 0};
    return call(a, "SetAnimationPlaybackRate", &p) && p.ret != 0;
}
bool anim_length(void* a, int32_t name, float* s) {
    struct { int32_t name[2]; float ret; int32_t pad; } p{{name, 0}, 0.0f, 0};
    if (!call(a, "GetAnimationLength", &p)) return false;
    *s = p.ret;
    return true;
}
bool pause_anim(void* a, int32_t h) {
    HandleOnly p{h, {}};
    return call(a, "PauseAnimation", &p) && p.ret[0] != 0;
}
bool resume_anim(void* a, int32_t h) {
    HandleOnly p{h, {}};
    return call(a, "ResumeAnimation", &p) && p.ret[0] != 0;
}
bool anim_paused(void* a, int32_t h, bool* paused) {
    HandleOnly p{h, {}};
    if (!call(a, "IsAnimationPaused", &p)) return false;
    *paused = p.ret[0] != 0;
    return true;
}

uint32_t calls() { return g_calls.load(std::memory_order_relaxed); }
uint32_t failures() { return g_failures.load(std::memory_order_relaxed); }

} // namespace bvr::b1r::script
