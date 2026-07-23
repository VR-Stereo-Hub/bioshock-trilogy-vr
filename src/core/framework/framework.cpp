#include "framework.h"

#include "core/hooks/d3d11_hook.h"
#include "core/ui/overlay.h"
#include "core/util/crash.h"
#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>

namespace bvr::framework {

void init() {
    log::init();
    BVR_LOG("bioshockvr %s starting", BVR_VERSION);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    BVR_LOG("host process: %s (base %p)", exePath, GetModuleHandleW(nullptr));

    crash::install();

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        BVR_LOG("MH_Initialize failed: %s — mod disabled, game runs flat",
                MH_StatusToString(status));
        return;
    }
    BVR_LOG("MinHook initialized");

    if (!d3d11_hook::install()) {
        BVR_LOG("D3D11 hook install failed — mod disabled, game runs flat");
        return;
    }

    BVR_LOG("init complete; waiting for first Present");
}

} // namespace bvr::framework
