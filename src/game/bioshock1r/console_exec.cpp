#include "game/bioshock1r/console_exec.h"

#include "core/util/log.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace bvr::b1r::console_exec {
namespace {

// UBOOL __thiscall Exec(const wchar_t* cmd, FOutputDevice& ar), ret 8.
// Dummy-EDX __fastcall stands in for thiscall (house pattern).
using ExecFn = int(__fastcall*)(void* self, void* edx, const wchar_t* cmd,
                                void* outputDevice);

// FOutputDevice stub. The engine's Logf helper (0x6E7AC0) probes vtbl+0x10
// as a 2-arg log filter FIRST and skips formatting + Serialize entirely when
// it returns 0 - so every slot here is a callee-pop-8 no-op RETURNING 0,
// which both suppresses output and keeps direct Serialize(text, event)
// (vtbl+0x4, 2 args) balanced. A slot invoked with a different arg count
// would still unbalance the stack - accepted, SEH-guarded dev tool.
int __stdcall StubNoop2(const void*, int) { return 0; }
void* g_stubVtbl[24];
struct Stub {
    void** vptr = nullptr;
} g_stub;

void ensure_stub() {
    if (g_stub.vptr) return;
    for (auto& s : g_stubVtbl) s = reinterpret_cast<void*>(&StubNoop2);
    g_stub.vptr = g_stubVtbl;
}

uintptr_t g_faultAddr = 0;
uintptr_t g_faultIp = 0;

int filter_capture(EXCEPTION_POINTERS* ep) {
    g_faultIp = ep->ContextRecord->Eip;
    g_faultAddr = ep->ExceptionRecord->NumberParameters >= 2
                      ? ep->ExceptionRecord->ExceptionInformation[1]
                      : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

int seh_exec(ExecFn fn, void* obj, const wchar_t* wcmd) {
    __try {
        return fn(obj, nullptr, wcmd, &g_stub);
    } __except (filter_capture(GetExceptionInformation())) {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        BVR_LOG("[b1r] exec fault: eip=%p (exe+0x%X) fault-addr=%p",
                reinterpret_cast<void*>(g_faultIp),
                g_faultIp - reinterpret_cast<uintptr_t>(base),
                reinterpret_cast<void*>(g_faultAddr));
        return -1;
    }
}

// SEH-safe fetch of the UGameEngine object (vtable-identity checked).
void* resolve_engine() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    __try {
        auto* engine = *reinterpret_cast<uint8_t**>(base + patterns::kGameEnginePtrRva);
        if (!engine) return nullptr;
        if (*reinterpret_cast<uint8_t**>(engine) != base + patterns::kEngineVtableRva)
            return nullptr;
        return engine;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

enum class Entry { Viewport, Client, Engine };

void run(const char* args, uint32_t rva, const char* label, Entry entry) {
    if (!args || !args[0]) {
        BVR_LOG("[b1r] exec: missing command text");
        return;
    }
    ensure_stub();

    void* target = nullptr;
    if (entry == Entry::Engine) {
        target = resolve_engine();
        if (target) // Exec lives on the FExec subobject, not the UObject base
            target = static_cast<uint8_t*>(target) + patterns::kEngineExecThisOffset;
    } else {
        void* client = nullptr;
        void* viewport = nullptr;
        if (input_drive::resolve_engine_objects(&client, &viewport))
            target = entry == Entry::Viewport ? viewport : client;
    }
    if (!target) {
        BVR_LOG("[b1r] exec: %s object not resolvable (no world yet?)", label);
        return;
    }

    wchar_t wcmd[224];
    size_t n = 0;
    for (; n < 223 && args[n]; ++n)
        wcmd[n] = static_cast<wchar_t>(static_cast<unsigned char>(args[n]));
    while (n && (wcmd[n - 1] == L'\n' || wcmd[n - 1] == L'\r' || wcmd[n - 1] == L' '))
        --n;
    wcmd[n] = 0;
    if (n == 0) {
        BVR_LOG("[b1r] exec: empty command");
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto fn = reinterpret_cast<ExecFn>(base + rva);
    int r = seh_exec(fn, target, wcmd);
    if (r < 0)
        BVR_LOG("[b1r] %s exec FAULTED on '%ls' (SEH caught - engine state "
                "unknown, prefer a relaunch before trusting further commands)",
                label, wcmd);
    else
        BVR_LOG("[b1r] %s exec '%ls' -> %s", label, wcmd,
                r ? "HANDLED" : "unhandled (fell off this chain link)");
}

} // namespace

void run_viewport(const char* args) {
    run(args, patterns::kViewportExecRva, "viewport", Entry::Viewport);
}

void run_client(const char* args) {
    run(args, patterns::kClientExecRva, "client", Entry::Client);
}

void run_engine(const char* args) {
    run(args, patterns::kEngineExecRva, "engine", Entry::Engine);
}

} // namespace bvr::b1r::console_exec
