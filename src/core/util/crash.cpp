#include "crash.h"
#include "log.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>

namespace bvr::crash {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\crash", log::data_dir());
    CreateDirectoryW(dir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\bvr_%04u%02u%02u_%02u%02u%02u.dmp",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(file);

        // Log where it faulted: exception address as module+RVA (so a crash in
        // our DLL or the game is pinpointable without loading the dump).
        void* addr = info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        char where[64] = "unknown";
        HMODULE mod = nullptr;
        if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(addr), &mod) &&
            mod) {
            char name[MAX_PATH]{};
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            base = base ? base + 1 : name;
            _snprintf_s(where, _TRUNCATE, "%s+0x%X", base,
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                              reinterpret_cast<uintptr_t>(mod)));
        }
        ULONG_PTR faultAddr = 0;
        if (info->ExceptionRecord && info->ExceptionRecord->NumberParameters >= 2)
            faultAddr = info->ExceptionRecord->ExceptionInformation[1];
        BVR_LOG("crash: minidump written (code 0x%08X at %p [%s], fault addr 0x%08X)",
                info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0, addr, where,
                static_cast<unsigned>(faultAddr));
    }

    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() {
    g_previous = SetUnhandledExceptionFilter(Filter);
}

} // namespace bvr::crash
