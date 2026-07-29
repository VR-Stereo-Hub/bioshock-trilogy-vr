#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace bvr::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
wchar_t g_dataDir[MAX_PATH] = {};

} // namespace

void init(const wchar_t* subdir) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;

    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        return;
    swprintf_s(g_dataDir, L"%s\\BioshockVR", local);
    CreateDirectoryW(g_dataDir, nullptr);
    if (subdir && subdir[0]) {
        wchar_t withSub[MAX_PATH];
        swprintf_s(withSub, L"%s\\%s", g_dataDir, subdir);
        wcscpy_s(g_dataDir, withSub);
        CreateDirectoryW(g_dataDir, nullptr);
    }

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\bioshockvr.log", g_dataDir);

    // Keep ONE generation of history. The log is truncated on every launch, so
    // a user who crashes and then relaunches to check something destroys the
    // only evidence - which is exactly what happened to the first external
    // crash report of session 23 (they sent a later, healthy run by mistake and
    // it cost a session to untangle). Now the crashing run survives as
    // bioshockvr.prev.log until the launch after next.
    wchar_t prev[MAX_PATH];
    swprintf_s(prev, L"%s\\bioshockvr.prev.log", g_dataDir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING); // no-op on first run

    // _SH_DENYNO so tools/tail-log.ps1 can follow the file while the game runs
    // (fopen_s-style opens deny all sharing).
    g_file = _wfsopen(path, L"w", _SH_DENYNO);
}

const wchar_t* data_dir() {
    return g_dataDir;
}

void write(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(message, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard lock(g_mutex);
    if (g_file) {
        fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
        fflush(g_file); // the process may die at any moment — never lose lines
    }
    char dbg[2100];
    sprintf_s(dbg, "[bioshockvr] %s\n", message);
    OutputDebugStringA(dbg);
}

} // namespace bvr::log
