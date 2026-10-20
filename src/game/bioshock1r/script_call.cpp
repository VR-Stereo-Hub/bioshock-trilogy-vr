#include "game/bioshock1r/script_call.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <utility>

namespace bvr::b1r::script {
namespace {

using FindFunctionFn = void*(__thiscall*)(void* obj, int32_t nameIndex, int32_t nameNumber, int32_t global);
using ProcessEventFn = void(__thiscall*)(void* obj, void* fn, void* parms, void* result);
FindFunctionFn g_find = nullptr;
ProcessEventFn g_process = nullptr;
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

void init(void* findFunction, void* processEvent) {
    g_find = reinterpret_cast<FindFunctionFn>(findFunction);
    g_process = reinterpret_cast<ProcessEventFn>(processEvent);
    BVR_LOG("[script] calls by name %s", ready() ? "ready" : "UNAVAILABLE (a prologue did not match)");
}

bool ready() { return g_find && g_process; }

int32_t name_index(const char* text) {
    const auto it = g_names.find(text);
    if (it != g_names.end()) return it->second;
    int32_t found = -1;
    const int32_t n = patterns::fname_count();
    wchar_t want[96] = {};
    for (int i = 0; i < 95 && text[i]; ++i) want[i] = static_cast<wchar_t>(text[i]);
    for (int32_t i = 0; i < n; ++i) {
        const wchar_t* t = patterns::fname_text(i);
        if (t && _wcsicmp(t, want) == 0) {
            found = i;
            break;
        }
    }
    if (n > 0) g_names[text] = found; // an empty table (too early) is not cached as "absent"
    if (found < 0) BVR_LOG("[script] name '%s' not in GNames (%d names)", text, n);
    return found;
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
