// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/gpu.h - the primary adapter's name and memory, for the
// render-quality preselection. DXGI_ADAPTER_DESC's DedicatedVideoMemory is a
// SIZE_T and this is a 32-bit process, so an 8 GB card would read as 4 GB;
// IDXGIAdapter3::QueryVideoMemoryInfo reports a 64-bit budget instead.
#pragma once
#include <stdint.h>
#include <string>

namespace bvr::launcher::gpu {

struct Info {
    std::wstring name;        // "" when no adapter answered
    uint64_t budgetBytes = 0; // the local segment's budget (about 90 % of VRAM); 0 = unknown
    bool known() const { return !name.empty(); }
};
Info primary();

} // namespace bvr::launcher::gpu
