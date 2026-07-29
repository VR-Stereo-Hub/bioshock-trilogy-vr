#include "framework.h"

#include "core/util/log.h"

#include <windows.h>

namespace {

DWORD WINAPI InitThread(LPVOID) {
    bvr::framework::init();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // All real work happens off the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Breadcrumb that separates an ORDERLY shutdown from a hard kill. A log
        // that just stops mid-heartbeat with no line at all (session 23's
        // external report) means neither ran - i.e. TerminateProcess or a
        // fail-fast, not an exception our filter could ever have seen.
        BVR_LOG("shutdown: DLL_PROCESS_DETACH (orderly process exit)");
    }
    return TRUE;
}
