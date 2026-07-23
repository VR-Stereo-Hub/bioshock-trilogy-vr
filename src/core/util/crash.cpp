#include "crash.h"
#include "log.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

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
        BVR_LOG("crash: minidump written (code 0x%08X)",
                info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0);
    }

    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() {
    g_previous = SetUnhandledExceptionFilter(Filter);
}

} // namespace bvr::crash
