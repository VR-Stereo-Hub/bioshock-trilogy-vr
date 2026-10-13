// See d3d11_debug.h.
#include "d3d11_debug.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace bvr::d3d11_debug {
namespace {

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                                    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn g_origCreate = nullptr;
CreateDeviceAndSwapChainFn g_origCreateSc = nullptr;

int g_mode = 0;                       // the marker at startup
std::atomic<bool> g_live{false};      // a device was created with the layer
std::atomic<int> g_devices{0};
ID3D11InfoQueue* g_queue = nullptr;   // the game's device's queue (first swapchain seen)
ID3D11Device* g_queueDevice = nullptr;
std::map<int, int> g_seen;            // message id -> times logged
uint32_t g_logged = 0, g_dropped = 0;
char g_status[160] = "off";
std::mutex g_mutex;
const uint32_t kPerPresent = 40, kPerRun = 600, kPerId = 4;

std::wstring marker_path() {
    const wchar_t* d = log::data_dir();
    if (!d || !d[0]) return L"";
    return std::wstring(d) + L"\\d3d11-debug-layer.txt";
}

int read_marker() {
    const std::wstring p = marker_path();
    if (p.empty()) return 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r") || !f) return 0;
    char line[64] = "";
    fgets(line, sizeof(line), f);
    fclose(f);
    return strstr(line, "break") ? 2 : 1;
}

UINT with_debug(UINT flags) { return g_mode ? (flags | D3D11_CREATE_DEVICE_DEBUG) : flags; }

void note_created(HRESULT hr, UINT flagsUsed, const char* how) {
    const bool dbg = (flagsUsed & D3D11_CREATE_DEVICE_DEBUG) != 0;
    if (SUCCEEDED(hr) && dbg) {
        g_live.store(true);
        ++g_devices;
        BVR_LOG("d3d11 debug layer: device %d created WITH the layer (%s, flags 0x%X) - %s", g_devices.load(), how, flagsUsed,
                g_mode == 2 ? "errors break on the calling thread" : "messages go to this log");
    } else {
        BVR_LOG("d3d11 debug layer: device created without the layer (%s, hr 0x%08lX, flags 0x%X)", how, (unsigned long)hr,
                flagsUsed);
    }
}

HRESULT WINAPI CreateDetour(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                            UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    UINT f = with_debug(flags);
    HRESULT hr = g_origCreate(a, t, sw, f, fl, nfl, sdk, dev, got, ctx);
    if (FAILED(hr) && f != flags) {
        BVR_LOG("d3d11 debug layer: creation with the layer failed (0x%08lX) - retrying without (SDK layers missing?)",
                (unsigned long)hr);
        f = flags;
        hr = g_origCreate(a, t, sw, f, fl, nfl, sdk, dev, got, ctx);
    }
    note_created(hr, f, "D3D11CreateDevice");
    return hr;
}

HRESULT WINAPI CreateScDetour(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                              UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev,
                              D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    UINT f = with_debug(flags);
    HRESULT hr = g_origCreateSc(a, t, sw, f, fl, nfl, sdk, sd, sc, dev, got, ctx);
    if (FAILED(hr) && f != flags) {
        BVR_LOG("d3d11 debug layer: creation with the layer failed (0x%08lX) - retrying without (SDK layers missing?)",
                (unsigned long)hr);
        f = flags;
        hr = g_origCreateSc(a, t, sw, f, fl, nfl, sdk, sd, sc, dev, got, ctx);
    }
    note_created(hr, f, "D3D11CreateDeviceAndSwapChain");
    return hr;
}

const char* sev_name(D3D11_MESSAGE_SEVERITY s) {
    switch (s) {
        case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
        case D3D11_MESSAGE_SEVERITY_ERROR: return "ERROR";
        case D3D11_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D11_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
    }
}

bool attach(IDXGISwapChain* sc) {
    if (g_queue) return true;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;
    ID3D11InfoQueue* q = nullptr;
    const HRESULT hr = dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&q);
    if (FAILED(hr) || !q) {
        dev->Release();
        static bool logged = false;
        if (!logged) { logged = true; BVR_LOG("d3d11 debug layer: the game's device has no info queue (0x%08lX) - the layer is not on it", (unsigned long)hr); }
        return false;
    }
    // Keep INFO and MESSAGE out of storage: the layer is here for errors and warnings.
    D3D11_MESSAGE_SEVERITY deny[2] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
    D3D11_INFO_QUEUE_FILTER filter = {};
    filter.DenyList.NumSeverities = 2;
    filter.DenyList.pSeverityList = deny;
    q->AddStorageFilterEntries(&filter);
    q->SetMessageCountLimit(2048);
    if (g_mode == 2) {
        q->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        q->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
    }
    g_queue = q;
    g_queueDevice = dev;   // held: the queue outlives nothing it should not
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "LIVE on the game's device (%s)", g_mode == 2 ? "break on errors" : "log");
    BVR_LOG("d3d11 debug layer: draining the game's device's info queue every present (at most %u lines per present, %u per run, "
            "%u per message id)", kPerPresent, kPerRun, kPerId);
    return true;
}

} // namespace

void install() {
    g_mode = read_marker();
    if (!g_mode) { _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "off (not armed)"); return; }
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "armed (%s): waiting for the game's device", g_mode == 2 ? "break" : "log");
    // d3d11.dll must be mapped for MinHook to find the exports; the game loads it anyway.
    LoadLibraryW(L"d3d11.dll");
    const MH_STATUS a = MH_CreateHookApi(L"d3d11.dll", "D3D11CreateDevice", (void*)&CreateDetour, (void**)&g_origCreate);
    const MH_STATUS b = MH_CreateHookApi(L"d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)&CreateScDetour,
                                         (void**)&g_origCreateSc);
    const MH_STATUS e = MH_EnableHook(MH_ALL_HOOKS);
    if (a != MH_OK || b != MH_OK || e != MH_OK) {
        BVR_LOG("d3d11 debug layer: ARMED but the device-creation hooks failed (%s / %s / %s) - the layer will not be on",
                MH_StatusToString(a), MH_StatusToString(b), MH_StatusToString(e));
        g_mode = 0;
        return;
    }
    BVR_LOG("d3d11 debug layer: ARMED by %ls (%s) - the game's device gets D3D11_CREATE_DEVICE_DEBUG; expect lower frame "
            "rates, this is a diagnostic run", marker_path().c_str(), g_mode == 2 ? "log + break on errors" : "log");
}

void on_present(IDXGISwapChain* sc) {
    if (!g_mode || !sc || !g_live.load()) return;
    if (!g_queue && !attach(sc)) return;
    const UINT64 n = g_queue->GetNumStoredMessages();
    if (!n) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t shown = 0;
    std::string buf;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_queue->GetMessage(i, nullptr, &len)) || !len) continue;
        buf.resize(len);
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
        if (FAILED(g_queue->GetMessage(i, m, &len))) continue;
        int& times = g_seen[(int)m->ID];
        if (times >= (int)kPerId || g_logged >= kPerRun || shown >= kPerPresent) { ++g_dropped; continue; }
        ++times; ++g_logged; ++shown;
        BVR_LOG("d3d11 debug layer [%s id %d]: %.*s", sev_name(m->Severity), (int)m->ID, (int)m->DescriptionByteLength,
                m->pDescription);
        if (g_logged == kPerRun) BVR_LOG("d3d11 debug layer: %u lines this run - further messages are counted, not logged", kPerRun);
    }
    g_queue->ClearStoredMessages();
    if (g_dropped && (g_logged % 50) == 0)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "LIVE: %u logged, %u not shown (repeats / over the cap)", g_logged, g_dropped);
}

int armed_for_next_launch() { return read_marker(); }

bool arm(int mode) {
    const std::wstring p = marker_path();
    if (p.empty()) return false;
    if (mode <= 0) {
        const bool gone = DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        BVR_LOG("d3d11 debug layer: disarmed for the next launch (%s)", gone ? "marker removed" : "marker could not be removed");
        return gone;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"w") || !f) { BVR_LOG("d3d11 debug layer: cannot write the marker %ls", p.c_str()); return false; }
    fprintf(f, "%s\n", mode == 2 ? "break" : "log");
    fclose(f);
    BVR_LOG("d3d11 debug layer: ARMED for the next launch (%s) - restart the game; the first lines of the next log say whether it took",
            mode == 2 ? "log + break on errors" : "log");
    return true;
}

bool live() { return g_live.load(); }
const char* status() { return g_status; }

} // namespace bvr::d3d11_debug
