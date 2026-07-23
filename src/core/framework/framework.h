#pragma once

#define BVR_VERSION "0.1.0"

namespace bvr::framework {

// Full mod startup, run on a dedicated thread (never under loader lock):
// logging, crash handler, MinHook, D3D11 hooks. Fails soft — on any error the
// game keeps running flat.
void init();

} // namespace bvr::framework
