// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/gpu.cpp - see gpu.h.
#include "sys/gpu.h"
#include <windows.h>
#include <dxgi1_4.h>

namespace bvr::launcher::gpu {

Info primary()
{
    Info info;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return info;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); adapter = nullptr; continue; }
        info.name = d.Description;
        IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a3))) && a3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO m = {};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m))) info.budgetBytes = m.Budget;
            a3->Release();
        }
        adapter->Release();
        break;
    }
    factory->Release();
    return info;
}

} // namespace bvr::launcher::gpu
