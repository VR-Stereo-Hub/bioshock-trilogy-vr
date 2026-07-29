#pragma once

// BVR_VERSION / BVR_BUILD_ID come from the generated header (CMake project
// VERSION + git describe). Never hardcode a version here again - the old
// hand-edited #define shipped "0.1.0" across three releases.
#include "bvr_version.h"

namespace bvr::framework {

// Full mod startup, run on a dedicated thread (never under loader lock):
// logging, crash handler, MinHook, D3D11 hooks. Fails soft — on any error the
// game keeps running flat.
void init();

} // namespace bvr::framework
