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

void init() {
    std::lock_guard lock(g_mutex);
    if (g_file) return;

    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        return;
    swprintf_s(g_dataDir, L"%s\\BioshockVR", local);
    CreateDirectoryW(g_dataDir, nullptr);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\bioshockvr.log", g_dataDir);
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
