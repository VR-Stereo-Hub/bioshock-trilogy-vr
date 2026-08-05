#include "crash.h"
#include "log.h"

#include "bvr_version.h" // generated: BVR_VERSION / BVR_BUILD_ID

#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bvr::crash {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
LONG g_inFilter = 0;
LONG g_teardown = 0;

// Rich but not a 1.2 GB core: stacks plus the memory those stacks point at, plus
// module data segments. That combination is what makes a smashed vtable slot or a
// recycled heap object readable after the fact - MiniDumpNormal (used until
// session 23) captures neither, which is why the external v0.2.0 report could not
// be root-caused. Set BVR_FULLDUMP=1 to ask a reporter for a full-memory dump.
constexpr MINIDUMP_TYPE kRichDump = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithIndirectlyReferencedMemory |
    MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

// "module+0xRVA" for any address inside a loaded image, else nullptr-ish text.
// Deliberately not dbghelp symbols: SymInitialize inside a crashed process is a
// good way to crash again, and module+RVA is enough to disassemble offline.
void describe(const void* addr, char* out, size_t n) {
    strncpy_s(out, n, "-", _TRUNCATE);
    if (!addr) return;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) ||
        !mod) {
        // Not in any image. Say what kind of memory it is - "heap, not executable"
        // is the whole diagnosis for a jump through a corrupted function pointer.
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            const bool exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            _snprintf_s(out, n, _TRUNCATE, "<no module: state=%X protect=%X type=%X %s>",
                        static_cast<unsigned>(mbi.State), static_cast<unsigned>(mbi.Protect),
                        static_cast<unsigned>(mbi.Type), exec ? "EXECUTABLE" : "NOT-EXECUTABLE");
        } else {
            strncpy_s(out, n, "<unmapped>", _TRUNCATE);
        }
        return;
    }
    char name[MAX_PATH]{};
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    _snprintf_s(out, n, _TRUNCATE, "%s+0x%X", base,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                      reinterpret_cast<uintptr_t>(mod)));
}

bool readable(const void* p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return reinterpret_cast<uintptr_t>(p) + bytes <= start + mbi.RegionSize;
}

#ifdef _M_IX86
// The return-address chain is what distinguishes a wild `call` from a wild `ret`,
// and it is what the old log could not answer. Walk raw stack dwords rather than
// StackWalk64 - no symbols needed, and it survives a smashed frame pointer.
void log_registers_and_stack(const CONTEXT* ctx) {
    if (!ctx) return;
    char eipWhere[160];
    describe(reinterpret_cast<void*>(ctx->Eip), eipWhere, sizeof(eipWhere));
    BVR_LOG("crash: eip=%08X [%s] esp=%08X ebp=%08X", ctx->Eip, eipWhere, ctx->Esp, ctx->Ebp);
    BVR_LOG("crash: eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X eflags=%08X", ctx->Eax,
            ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->EFlags);

    const auto* sp = reinterpret_cast<const uint32_t*>(ctx->Esp);
    int shown = 0;
    for (int i = 0; i < 64 && shown < 12; ++i) {
        if (!readable(sp + i, sizeof(uint32_t))) break;
        const uint32_t v = sp[i];
        char where[160];
        describe(reinterpret_cast<void*>(v), where, sizeof(where));
        // Only code-ish values are worth a line; a full hex dump of the stack is
        // noise, and the dump has the bytes anyway.
        if (where[0] != '-' && where[0] != '<') {
            BVR_LOG("crash:   stack[%02d] %08X -> %s", i, v, where);
            ++shown;
        }
    }
}
#endif

void report(EXCEPTION_POINTERS* info, const char* reason);

// Codes that are ALWAYS fatal and that never reach an unhandled-exception
// filter: the heap manager and /GS fail fast, and a stack overflow usually has
// no stack left to run a filter on. Deliberately NOT 0xC0000005 - our own
// pattern scans raise guarded access violations by design and would flood this.
bool always_fatal(DWORD code) {
    switch (code) {
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC00000FD: // STATUS_STACK_OVERFLOW
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (__fastfail)
    case 0xC000001D: // STATUS_ILLEGAL_INSTRUCTION
    case 0xC0000006: // STATUS_IN_PAGE_ERROR
        return true;
    default:
        return false;
    }
}

// BVR_VEH=1: log FIRST-CHANCE access violations (once per unique eip, with a
// repeat count), independent of who owns the unhandled filter. Diagnostic only
// (session 38: proved the BS2 exit fault fires with every mod hook skipped).
// First-chance means SEH-handled faults show here too - a line is "this fault
// happened", not "this fault was unhandled".
bool g_vehObserve = false;

void veh_observe_av(const EXCEPTION_RECORD* rec) {
    static void* s_eips[8];
    static LONG s_counts[8];
    void* eip = rec->ExceptionAddress;
    for (int i = 0; i < 8; ++i) {
        if (s_eips[i] == eip) {
            LONG n = InterlockedIncrement(&s_counts[i]);
            if (n == 2 || n == 100 || n == 10000)
                BVR_LOG("crash: [veh] first-chance AV at %p seen %ld times", eip, n);
            return;
        }
        if (!s_eips[i] && InterlockedCompareExchangePointer(&s_eips[i], eip, nullptr) == nullptr) {
            char where[160];
            describe(eip, where, sizeof(where));
            ULONG_PTR target = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
            BVR_LOG("crash: [veh] first-chance AV at %p [%s] touching 0x%08X (tid=%u)",
                    eip, where, static_cast<unsigned>(target), GetCurrentThreadId());
            InterlockedIncrement(&s_counts[i]);
            return;
        }
    }
}

LONG CALLBACK VectoredFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
    if (rec && g_vehObserve && rec->ExceptionCode == 0xC0000005)
        veh_observe_av(rec);
    if (rec && always_fatal(rec->ExceptionCode))
        report(info, "vectored (fatal class - would never reach the filter)");
    return EXCEPTION_CONTINUE_SEARCH; // observe only, never swallow
}

void report(EXCEPTION_POINTERS* info, const char* reason) {
    // A fault inside MiniDumpWriteDump used to recurse straight back into here.
    if (InterlockedCompareExchange(&g_inFilter, 1, 0) != 0) return;

    // Exit-path fault (session 38): once the window began closing, a fault is
    // the host's own teardown bug (BS2 faults at +0x4FF0FE on every close,
    // reproduced with every mod hook skipped). A dump would be noise that eats
    // the session cap, and returning to the chained filter spins the faulting
    // instruction for seconds (86k retries observed). Log one line and end the
    // process now - the user asked to close it.
    if (InterlockedCompareExchange(&g_teardown, 0, 0) != 0) {
        const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
        char where[160];
        describe(rec ? rec->ExceptionAddress : nullptr, where, sizeof(where));
        BVR_LOG("crash: fault during window teardown (code 0x%08X at %s) - known "
                "host exit-path bug; no dump, terminating cleanly",
                rec ? rec->ExceptionCode : 0, where);
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // Repeat-fault suppressor (session 25): a chained filter (BS2's
    // CSERHelper) can CONTINUE_EXECUTION a persistent fault, re-executing the
    // faulting instruction forever - observed as one 55 MB dump per second
    // for 40 minutes (2083 dumps, 115 GB) on one BS2 crash. The first report
    // of an address has full detail + dump; repeats at the SAME address log a
    // heartbeat line only. Statics are safe: the g_inFilter latch makes this
    // section single-flight.
    static void* s_lastEip = nullptr;
    static unsigned s_repeats = 0;
    void* eip = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress
                                              : nullptr;
    if (eip && eip == s_lastEip) {
        ++s_repeats;
        if (s_repeats == 1)
            BVR_LOG("crash: same fault at %p again - suppressing repeat dumps and detail "
                    "(a chained filter is retrying the faulting instruction)",
                    eip);
        else if (s_repeats % 500 == 0)
            BVR_LOG("crash: fault at %p repeated %u times", eip, s_repeats);
        InterlockedExchange(&g_inFilter, 0);
        return;
    }
    s_lastEip = eip;
    s_repeats = 0;

    // Distinct-fault dump cap: a cascade of different crash addresses (heap
    // corruption walking) must not fill the disk either. Crash LINES keep
    // logging; only the minidump writes stop.
    static unsigned s_dumpsWritten = 0;
    const bool wantDump = s_dumpsWritten < 3;
    if (s_dumpsWritten == 3) {
        ++s_dumpsWritten; // log the cap notice once
        BVR_LOG("crash: dump cap (3 per session) reached - further faults are logged "
                "without minidumps");
    }

    BVR_LOG("crash: caught via %s", reason);

    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\crash", log::data_dir());
    CreateDirectoryW(dir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\bvr_%04u%02u%02u_%02u%02u%02u.dmp",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    MINIDUMP_TYPE type = kRichDump;
    if (GetEnvironmentVariableW(L"BVR_FULLDUMP", nullptr, 0) != 0)
        type = static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                          MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

    BOOL wrote = FALSE;
    if (wantDump) {
        ++s_dumpsWritten; // count attempts, not successes - a failing write
                          // storm must hit the cap too
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      &mei, nullptr, nullptr);
            CloseHandle(file);
        }
    }

    const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
    void* addr = rec ? rec->ExceptionAddress : nullptr;
    char where[160];
    describe(addr, where, sizeof(where));

    ULONG_PTR faultAddr = 0;
    const char* access = "";
    if (rec && rec->NumberParameters >= 2) {
        faultAddr = rec->ExceptionInformation[1];
        // ExceptionInformation[0]: 0 read, 1 write, 8 DEP execute violation. The 8
        // case means "jumped into non-executable memory", a completely different
        // bug class from a null deref, and the old log did not distinguish them.
        switch (rec->ExceptionInformation[0]) {
        case 0: access = " (read)"; break;
        case 1: access = " (write)"; break;
        case 8: access = " (DEP: EXECUTE from non-executable memory)"; break;
        default: break;
        }
    }

    BVR_LOG("crash: %s (code 0x%08X at %p [%s], fault addr 0x%08X%s) tid=%u build %s (%s)",
            wrote ? "minidump written"
                  : (wantDump ? "MINIDUMP WRITE FAILED" : "minidump skipped (session cap)"),
            rec ? rec->ExceptionCode : 0, addr, where, static_cast<unsigned>(faultAddr), access,
            GetCurrentThreadId(), BVR_VERSION, BVR_BUILD_ID);
    if (wrote) {
        char dumpPath[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, dumpPath, sizeof(dumpPath), nullptr, nullptr);
        BVR_LOG("crash: dump at %s", dumpPath);
    }

    char faultWhere[160];
    describe(reinterpret_cast<void*>(faultAddr), faultWhere, sizeof(faultWhere));
    BVR_LOG("crash: fault address region: %s", faultWhere);

#ifdef _M_IX86
    log_registers_and_stack(info ? info->ContextRecord : nullptr);
#endif

    InterlockedExchange(&g_inFilter, 0);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    report(info, "unhandled-exception filter");
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// Exit watchdog. The host's exit path can end two ways, and session 38 saw
// both on BS2: a fault (absorbed above) or a DEADLOCK - the in-game quit
// blocked after WM_DESTROY with zero CPU and one thread left, surviving well
// past two minutes. Either way the user asked to close the game, so a close
// that has not completed within the grace period is ended here. The grace is
// deliberately longer than any healthy exit measured (vanilla BS2 takes 5-9 s;
// BS1 exits well inside that), so this never truncates a working shutdown.
DWORD WINAPI TeardownWatchdog(LPVOID) {
    constexpr DWORD kGraceMs = 15000;
    Sleep(kGraceMs);
    BVR_LOG("crash: still alive %u ms after the window began closing - the host "
            "exit path is stuck; ending the process",
            kGraceMs);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

void note_teardown(const char* why) {
    if (InterlockedExchange(&g_teardown, 1) != 0) return;
    BVR_LOG("crash: window teardown noted (%s) - exit-path faults will be "
            "logged without dumps and end the process directly",
            why);
    // Own thread: the close message runs on the game thread, which is exactly
    // the thread that can deadlock.
    HANDLE t = CreateThread(nullptr, 0, &TeardownWatchdog, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

bool teardown_seen() {
    return InterlockedCompareExchange(&g_teardown, 0, 0) != 0;
}

void install() {
    g_previous = SetUnhandledExceptionFilter(Filter);
    g_vehObserve = GetEnvironmentVariableW(L"BVR_VEH", nullptr, 0) != 0;
    // Fatal-class codes (heap corruption, stack overflow, __fastfail) never
    // reach an unhandled-exception filter. First in the chain, observe-only.
    AddVectoredExceptionHandler(1, &VectoredFilter);
}

void rearm() {
    // Read the current filter by setting ours and looking at what came back.
    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(Filter);
    if (current == Filter) return; // still ours, nothing happened
    // Someone displaced us. Keep THEIR filter as our chain target so their
    // handler still runs after ours, and say so once - a silent displacement is
    // why an external crash produced no dump and no log line at all.
    static bool logged = false;
    g_previous = current;
    if (!logged) {
        logged = true;
        char where[160];
        describe(reinterpret_cast<void*>(current), where, sizeof(where));
        BVR_LOG("crash: our exception filter had been displaced by %p [%s] - re-armed "
                "(chaining to it); dumps would have been lost until now",
                current, where);
    }
}

} // namespace bvr::crash
