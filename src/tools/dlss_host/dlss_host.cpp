// bvr_dlss_host64 - the 64-bit NVIDIA NGX helper for the BioShock VR mod.
//
// The games and the mod are 32-bit and NGX (nvngx_dlss.dll) exists only as 64-bit, so DLSS
// runs here: a hidden x64 process the mod starts (core/gfx/dlss_client.cpp), one D3D12
// device on the mod's own adapter, one DLSS feature (and so one temporal history) per eye.
// The mod creates every shared texture and fence on its D3D11 device and duplicates the
// handles into this process; the wire contract is core/gfx/dlss_ipc.h.
//
// Ported from the Dishonored VR mod's dvr_dlss_host64 (its FSR backend left out). That
// helper's route - NGX in an x64 helper, NT-handle textures, a fence pair per eye - was
// proven in a 32-bit VR mod by the BioShock VR DLSS/DLAA community fork (MIT), itself
// derived from DLSS5-Feeder (MIT) with portions from dlss5-bridge (MIT). This file is a
// rewrite for this mod, not a copy of their code; the NGX call sequence
// (Init_with_ProjectID, capability query, preset hints, the create and evaluate helpers,
// the fault guards) follows it. See src/tools/dlss_host/NOTICE.md.
//
// This software contains source code provided by NVIDIA Corporation (the NGX SDK helper
// headers it compiles against).
//
// Command line: bvr_dlss_host64.exe <game pid> --luid <high> <low> --data <dir>
// Log: <dir>\dlss_host.log (previous run: dlss_host.prev.log).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "core/gfx/dlss_ipc.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using namespace bvr::dlss_ipc;

namespace {

// A project-owned UUID for the SDK's custom-engine path; NVIDIA did not issue this project
// an application id.
const char kProjectId[] = "c81e7c99-6abf-4c23-975d-d140adcd576f";
const char kEngineVersion[] = "1.0";

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    SYSTEMTIME t; GetLocalTime(&t);
    char line[1024];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    if (g_log) {
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
        fflush(g_log);
    }
}

const char* NgxName(NVSDK_NGX_Result r) {
    switch (r) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate (driver too old)";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
    default: return NVSDK_NGX_SUCCEED(r) ? "success" : "failure";
    }
}

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------
// D3D12: device, one queue, a small ring of allocators, timestamps
// ---------------------------------------------------------------------------------------
const int kRing = 3;

struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc[kRing] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;      // our own: allocator reuse and synchronous work
    UINT64 slotValue[kRing] = {};
    UINT64 next = 0;
    HANDLE event = nullptr;
    int slot = 0;
    ID3D12QueryHeap* stamps = nullptr;
    ID3D12Resource* readback = nullptr;   // 2 x UINT64 per slot
    UINT64 freq = 0;
    bool slotTimed[kRing] = {};
    float lastGpuMs = -1.0f;
    char adapter[128] = "?";
} g;

bool WaitValue(UINT64 v, DWORD ms) {
    if (g.fence->GetCompletedValue() >= v) return true;
    ResetEvent(g.event);
    if (FAILED(g.fence->SetEventOnCompletion(v, g.event))) return false;
    return WaitForSingleObject(g.event, ms) == WAIT_OBJECT_0;
}

bool Begin() {
    g.slot = (g.slot + 1) % kRing;
    if (!WaitValue(g.slotValue[g.slot], 5000)) { Log("[gpu] allocator %d never retired", g.slot); return false; }
    if (g.slotTimed[g.slot] && g.readback && g.freq) {
        UINT64* ts = nullptr;
        D3D12_RANGE r = {g.slot * 16u, g.slot * 16u + 16u};
        if (SUCCEEDED(g.readback->Map(0, &r, (void**)&ts)) && ts) {
            const UINT64 a = ts[g.slot * 2], b = ts[g.slot * 2 + 1];
            if (b > a) g.lastGpuMs = (float)((double)(b - a) * 1000.0 / (double)g.freq);
            D3D12_RANGE none = {0, 0};
            g.readback->Unmap(0, &none);
        }
        g.slotTimed[g.slot] = false;
    }
    if (FAILED(g.alloc[g.slot]->Reset())) return false;
    return SUCCEEDED(g.list->Reset(g.alloc[g.slot], nullptr));
}

UINT64 End() {
    g.list->Close();
    ID3D12CommandList* l[] = {g.list};
    g.queue->ExecuteCommandLists(1, l);
    const UINT64 v = ++g.next;
    g.queue->Signal(g.fence, v);
    g.slotValue[g.slot] = v;
    return v;
}

bool InitGpu(LUID luid) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&f)) || !f) { Log("[gpu] no DXGI 1.4 factory"); return false; }
    IDXGIAdapter1* a = nullptr;
    HRESULT hr = f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a);
    f->Release();
    if (FAILED(hr) || !a) {
        Log("[gpu] no adapter with the mod's LUID %08lX:%08lX (0x%08lX) - refusing: a different adapter "
            "cannot open the mod's shared textures", (unsigned long)luid.HighPart, luid.LowPart, (unsigned long)hr);
        return false;
    }
    DXGI_ADAPTER_DESC1 d = {};
    a->GetDesc1(&d);
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g.adapter, sizeof(g.adapter), nullptr, nullptr);
    hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g.dev);
    a->Release();
    if (FAILED(hr)) { Log("[gpu] D3D12CreateDevice on %s failed 0x%08lX", g.adapter, (unsigned long)hr); return false; }
    Log("[gpu] device: %s, LUID %08lX:%08lX (the mod's adapter), vendor %04X device %04X", g.adapter,
        (unsigned long)luid.HighPart, luid.LowPart, d.VendorId, d.DeviceId);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g.queue))) return false;
    for (auto& al : g.alloc)
        if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al)))
            return false;
    if (FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), (void**)&g.list)))
        return false;
    g.list->Close();
    if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g.fence))) return false;
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Timestamps: optional; a device that refuses them only loses the gpuMs number.
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kRing * 2;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kRing * 16;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&g.stamps)) ||
        FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&g.readback)) ||
        FAILED(g.queue->GetTimestampFrequency(&g.freq))) {
        Release(g.stamps); Release(g.readback); g.freq = 0;
        Log("[gpu] timestamps unavailable; FrameAck.gpuMs stays -1");
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// NGX
// ---------------------------------------------------------------------------------------
struct Ngx {
    bool inited = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    int available = 0;
    int minDriver[2] = {};
} ngx;

bool InitNgx(const wchar_t* dataDir) {
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(exeDir, L'\\')) *(s + 1) = 0;
    // nvngx_dlss.dll is found next to this exe; NGX writes its own logs and cache in dataDir.
    const wchar_t* paths[2] = {exeDir, dataDir};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 2;
    ngx.result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                    dataDir, g.dev, &info, NVSDK_NGX_Version_API);
    Log("[ngx] Init_with_ProjectID (custom engine, API 0x%X) -> 0x%08X %s; runtime searched in %ls",
        (unsigned)NVSDK_NGX_Version_API, (unsigned)ngx.result, NgxName(ngx.result), exeDir);
    if (NVSDK_NGX_FAILED(ngx.result)) return false;
    ngx.inited = true;
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) { ngx.result = r; Log("[ngx] capability query failed 0x%08X", (unsigned)r); return false; }
    int needs = 0;
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &ngx.available);
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs);
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &ngx.minDriver[0]);
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &ngx.minDriver[1]);
    NVSDK_NGX_D3D12_DestroyParameters(caps);
    Log("[ngx] SuperSampling.Available=%d NeedsUpdatedDriver=%d MinDriver=%d.%d", ngx.available, needs,
        ngx.minDriver[0], ngx.minDriver[1]);
    if (!ngx.available) return false;
    r = NVSDK_NGX_D3D12_AllocateParameters(&ngx.params);
    if (NVSDK_NGX_FAILED(r) || !ngx.params) { ngx.result = r; Log("[ngx] AllocateParameters failed 0x%08X", (unsigned)r); return false; }
    return true;
}

// NGX faults are caught here rather than taking the helper down with an unexplained exit.
NVSDK_NGX_Result SafeCreate(NVSDK_NGX_Handle** h, NVSDK_NGX_DLSS_Create_Params* cp, DWORD* code) {
    __try { return NGX_D3D12_CREATE_DLSS_EXT(g.list, 1, 1, h, ngx.params, cp); }
    __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
NVSDK_NGX_Result SafeEvaluate(NVSDK_NGX_Handle* h, NVSDK_NGX_D3D12_DLSS_Eval_Params* ep, DWORD* code) {
    __try { return NGX_D3D12_EVALUATE_DLSS_EXT(g.list, h, ngx.params, ep); }
    __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
void SafeReleaseFeature(NVSDK_NGX_Handle* h) {
    __try { NVSDK_NGX_D3D12_ReleaseFeature(h); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------------------
// One eye: its shared resources, fences and DLSS feature
// ---------------------------------------------------------------------------------------
struct Eye {
    ID3D12Resource* tex[SlotCount] = {};
    ID3D12Fence* in = nullptr;
    ID3D12Fence* out = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Build b = {};
    uint64_t frames = 0, failures = 0;
    double gpuSum = 0; uint64_t gpuN = 0;
} eyes[2];

void ReleaseEye(Eye& e) {
    if (e.feature) {
        // The feature may still be referenced by work in flight.
        WaitValue(g.next, 5000);
        SafeReleaseFeature(e.feature);
        e.feature = nullptr;
    }
    for (auto& t : e.tex) Release(t);
    Release(e.in); Release(e.out);
}

bool OpenHandle(uint64_t value, REFIID iid, void** out, const char* what, char* why, size_t cap) {
    HANDLE h = (HANDLE)(uintptr_t)value;
    if (!h) { _snprintf_s(why, cap, _TRUNCATE, "%s handle missing", what); return false; }
    const HRESULT hr = g.dev->OpenSharedHandle(h, iid, out);
    CloseHandle(h);
    if (FAILED(hr) || !*out) { _snprintf_s(why, cap, _TRUNCATE, "OpenSharedHandle(%s) 0x%08lX", what, (unsigned long)hr); return false; }
    return true;
}

void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.list->ResourceBarrier(1, &b);
}

bool DoBuild(const Build& b, BuildAck& ack) {
    ack.eye = b.eye;
    if (b.eye > 1) { strcpy_s(ack.detail, "invalid eye"); return false; }
    Eye& e = eyes[b.eye];
    ReleaseEye(e);
    e = Eye{};
    e.b = b;
    static const char* kNames[SlotCount] = {"Color", "Output", "Depth", "Motion", "Bias"};
    bool ok = true;
    for (uint32_t s = 0; s < SlotCount; ++s)
        ok = OpenHandle(b.tex[s], __uuidof(ID3D12Resource), (void**)&e.tex[s], kNames[s], ack.detail, sizeof(ack.detail)) && ok;
    ok = OpenHandle(b.fenceIn, __uuidof(ID3D12Fence), (void**)&e.in, "fence in", ack.detail, sizeof(ack.detail)) && ok;
    ok = OpenHandle(b.fenceOut, __uuidof(ID3D12Fence), (void**)&e.out, "fence out", ack.detail, sizeof(ack.detail)) && ok;
    if (!ok) { Log("[eye%u] build refused: %s", b.eye, ack.detail); ReleaseEye(e); return false; }

    const bool dlaa = b.outWidth == b.width && b.outHeight == b.height;
    // The quality mode follows the per-axis ratio the mod chose: 1.5 Quality, 1.72 Balanced,
    // 2 Performance, 3 Ultra Performance. Ultra Quality (1.3) is created as MaxQuality with the
    // larger render: the 310.7 runtime refuses NGX's own UltraQuality value (the Dishonored
    // mod's host test, CreateFeature 0xBAD00010 UnsupportedParameter).
    const float ratio = b.width ? (float)b.outWidth / (float)b.width : 1.0f;
    const NVSDK_NGX_PerfQuality_Value pq = dlaa ? NVSDK_NGX_PerfQuality_Value_DLAA
        : ratio < 1.6f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
        : ratio < 1.85f ? NVSDK_NGX_PerfQuality_Value_Balanced
        : ratio < 2.5f ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    // Presets set explicitly so an NVIDIA App override or a swapped runtime cannot change what
    // the log claims. Default K for every mode; a nonzero mod preset overrides all.
    const int pk = b.preset > 0 ? b.preset : (int)NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pk);
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pk);
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pk);
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pk);
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pk);
    ngx.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, pk);

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = b.width;
    cp.Feature.InHeight = b.height;
    cp.Feature.InTargetWidth = b.outWidth;
    cp.Feature.InTargetHeight = b.outHeight;
    cp.Feature.InPerfQualityValue = pq;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;   // vectors at render size
    if (b.depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (b.autoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    cp.InFeatureCreateFlags = flags;
    if (!Begin()) { strcpy_s(ack.detail, "command list unavailable"); ReleaseEye(e); return false; }
    DWORD code = 0;
    const NVSDK_NGX_Result r = SafeCreate(&e.feature, &cp, &code);
    ack.ngxResult = (uint32_t)r;
    const UINT64 v = End();
    if (!WaitValue(v, 10000)) { strcpy_s(ack.detail, "feature creation did not complete"); ReleaseEye(e); return false; }
    if (code || NVSDK_NGX_FAILED(r) || !e.feature) {
        _snprintf_s(ack.detail, _TRUNCATE, "CreateFeature 0x%08X %s%s", (unsigned)r, NgxName(r), code ? " (fault)" : "");
        Log("[eye%u] %s, exception 0x%08lX", b.eye, ack.detail, (unsigned long)code);
        e.feature = nullptr;
        ReleaseEye(e);
        return false;
    }
    Log("[eye%u] feature ready: %ux%u -> %ux%u %s (%.2fx, perf quality %d), preset %d, flags 0x%X (MVLowRes%s%s), colour fmt %u, output fmt %u",
        b.eye, b.width, b.height, b.outWidth, b.outHeight, dlaa ? "DLAA" : "DLSS SR", ratio, (int)pq, pk, flags,
        b.depthInverted ? " DepthInverted" : "", b.autoExposure ? " AutoExposure" : "", b.colorFormat, b.outputFormat);
    ack.ok = 1;
    return true;
}

bool DoFrame(const Frame& f, FrameAck& ack) {
    ack.eye = f.eye; ack.value = f.value; ack.gpuMs = -1.0f;
    if (f.eye > 1 || !eyes[f.eye].feature) {
        // Still signal: the mod waits on this value, and a refusal must not hang its queue.
        if (f.eye <= 1 && eyes[f.eye].out) g.queue->Signal(eyes[f.eye].out, f.value);
        ack.ok = 0;
        return false;
    }
    Eye& e = eyes[f.eye];
    // The mod's copies into the inputs are ordered before this evaluate on the GPU.
    g.queue->Wait(e.in, f.value);
    if (!Begin()) { g.queue->Signal(e.out, f.value); ack.ok = 0; return false; }
    const bool timed = g.stamps && g.freq;
    if (timed) g.list->EndQuery(g.stamps, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2);
    // Shared textures arrive (and leave) in COMMON: explicit transitions, never implicit
    // promotion to UAV, which a non-simultaneous-access texture does not get.
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Transition(e.tex[Color], D3D12_RESOURCE_STATE_COMMON, read);
    Transition(e.tex[Depth], D3D12_RESOURCE_STATE_COMMON, read);
    Transition(e.tex[Motion], D3D12_RESOURCE_STATE_COMMON, read);
    if (f.useBias) Transition(e.tex[Bias], D3D12_RESOURCE_STATE_COMMON, read);
    Transition(e.tex[Output], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = e.tex[Color];
    ep.Feature.pInOutput = e.tex[Output];
    ep.Feature.InSharpness = f.sharpness;
    ep.pInDepth = e.tex[Depth];
    ep.pInMotionVectors = e.tex[Motion];
    ep.pInBiasCurrentColorMask = f.useBias ? e.tex[Bias] : nullptr;
    ep.InJitterOffsetX = f.jitterX;
    ep.InJitterOffsetY = f.jitterY;
    ep.InRenderSubrectDimensions.Width = e.b.width;
    ep.InRenderSubrectDimensions.Height = e.b.height;
    ep.InReset = f.reset ? 1 : 0;
    ep.InMVScaleX = f.mvScaleX;
    ep.InMVScaleY = f.mvScaleY;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    DWORD code = 0;
    const NVSDK_NGX_Result r = SafeEvaluate(e.feature, &ep, &code);
    Transition(e.tex[Color], read, D3D12_RESOURCE_STATE_COMMON);
    Transition(e.tex[Depth], read, D3D12_RESOURCE_STATE_COMMON);
    Transition(e.tex[Motion], read, D3D12_RESOURCE_STATE_COMMON);
    if (f.useBias) Transition(e.tex[Bias], read, D3D12_RESOURCE_STATE_COMMON);
    Transition(e.tex[Output], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (timed) {
        g.list->EndQuery(g.stamps, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2 + 1);
        g.list->ResolveQueryData(g.stamps, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2, 2, g.readback, g.slot * 16);
        g.slotTimed[g.slot] = true;
    }
    End();
    // Always signal: the mod waits on this value, and a refused evaluate must not hang it.
    g.queue->Signal(e.out, f.value);
    ack.ngxResult = (uint32_t)r;
    ack.ok = (!code && NVSDK_NGX_SUCCEED(r)) ? 1 : 0;
    ack.gpuMs = g.lastGpuMs;
    ++e.frames;
    if (g.lastGpuMs >= 0) { e.gpuSum += g.lastGpuMs; ++e.gpuN; }
    if (!ack.ok) {
        ++e.failures;
        if (e.failures <= 5 || e.failures % 300 == 0)
            Log("[eye%u] evaluate refused 0x%08X %s (exception 0x%08lX), %llu refusals", f.eye, (unsigned)r, NgxName(r),
                (unsigned long)code, (unsigned long long)e.failures);
    }
    if (e.frames % 900 == 0)
        Log("[eye%u] %llu frames, %llu refused, evaluate GPU avg %.3f ms", f.eye, (unsigned long long)e.frames,
            (unsigned long long)e.failures, e.gpuN ? e.gpuSum / e.gpuN : -1.0);
    return ack.ok != 0;
}

// ---------------------------------------------------------------------------------------
// The pipe
// ---------------------------------------------------------------------------------------
HANDLE g_pipe = INVALID_HANDLE_VALUE;

bool ReadAll(void* p, DWORD n) {
    BYTE* c = (BYTE*)p;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(g_pipe, c, n, &got, nullptr) || !got) return false;
        c += got; n -= got;
    }
    return true;
}
bool WriteAll(const void* p, DWORD n) {
    DWORD put = 0;
    return WriteFile(g_pipe, p, n, &put, nullptr) && put == n;
}

int Serve(DWORD pid, LUID luid, const wchar_t* dataDir) {
    wchar_t name[96];
    _snwprintf_s(name, _TRUNCATE, BVR_DLSS_PIPE_FMT, (unsigned long)pid);
    g_pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) { Log("[pipe] CreateNamedPipe failed %lu", GetLastError()); return 3; }
    Log("[pipe] waiting for the mod on %ls", name);
    if (!ConnectNamedPipe(g_pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        Log("[pipe] ConnectNamedPipe failed %lu", GetLastError()); return 3;
    }
    Hello hello = {};
    if (!ReadAll(&hello, sizeof(hello)) || hello.magic != kMagic || hello.version != kVersion) {
        Log("[pipe] bad hello (magic %08X version %u, want v%u)", hello.magic, hello.version, kVersion);
        return 4;
    }
    if (hello.luidLow != luid.LowPart || hello.luidHigh != luid.HighPart)
        Log("[pipe] WARNING: hello LUID %08lX:%08lX differs from the command line's", (unsigned long)hello.luidHigh,
            (unsigned long)hello.luidLow);

    HelloAck ha = {};
    ha.magic = kMagic; ha.version = kVersion;
    const bool up = InitGpu(luid) && InitNgx(dataDir);
    ha.ok = up ? 1 : 0;
    ha.ngxResult = (uint32_t)ngx.result;
    ha.dlssAvailable = ngx.available;
    ha.driverMin[0] = (uint32_t)ngx.minDriver[0]; ha.driverMin[1] = (uint32_t)ngx.minDriver[1];
    strcpy_s(ha.adapter, g.adapter);
    if (!WriteAll(&ha, sizeof(ha)) || !up) { Log("[host] %s", up ? "hello ack write failed" : "not available, exiting"); return up ? 3 : 5; }

    for (;;) {
        uint8_t tag = 0;
        if (!ReadAll(&tag, 1)) { Log("[pipe] the mod closed the pipe"); break; }
        if (tag == TagBuild) {
            Build b = {};
            BuildAck ack = {};
            if (!ReadAll(&b, sizeof(b))) break;
            DoBuild(b, ack);
            if (!WriteAll(&ack, sizeof(ack))) break;
        } else if (tag == TagFrame) {
            Frame f = {};
            FrameAck ack = {};
            if (!ReadAll(&f, sizeof(f))) break;
            DoFrame(f, ack);
            if (!WriteAll(&ack, sizeof(ack))) break;
        } else if (tag == TagQuit) {
            Log("[pipe] quit requested");
            break;
        } else {
            Log("[pipe] unknown tag 0x%02X - desynchronised, exiting", tag);
            break;
        }
    }
    return 0;
}

void Shutdown() {
    if (g.fence) WaitValue(g.next, 3000);
    for (auto& e : eyes) ReleaseEye(e);
    if (ngx.params) { NVSDK_NGX_D3D12_DestroyParameters(ngx.params); ngx.params = nullptr; }
    if (ngx.inited && g.dev) { NVSDK_NGX_D3D12_Shutdown1(g.dev); ngx.inited = false; }
    Release(g.readback); Release(g.stamps); Release(g.list);
    for (auto& a : g.alloc) Release(a);
    Release(g.fence); Release(g.queue); Release(g.dev);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    LUID luid = {};
    bool haveLuid = false;
    const wchar_t* dataDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--luid") && i + 2 < argc) {
            luid.HighPart = (LONG)wcstol(argv[i + 1], nullptr, 16);
            luid.LowPart = (DWORD)wcstoul(argv[i + 2], nullptr, 16);
            haveLuid = true; i += 2;
        } else if (!wcscmp(argv[i], L"--data") && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (!pid) {
            pid = wcstoul(argv[i], nullptr, 10);
        }
    }
    if (!pid || !haveLuid || !dataDir) {
        MessageBoxW(nullptr, L"This is the BioShock VR DLSS helper. The VR mod starts it; there is nothing to run here.",
                    L"BioShock VR DLSS helper", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    CreateDirectoryW(dataDir, nullptr);
    wchar_t logPath[MAX_PATH], prevPath[MAX_PATH];
    _snwprintf_s(logPath, _TRUNCATE, L"%s\\dlss_host.log", dataDir);
    _snwprintf_s(prevPath, _TRUNCATE, L"%s\\dlss_host.prev.log", dataDir);
    MoveFileExW(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
    _wfopen_s(&g_log, logPath, L"w");
    Log("bvr_dlss_host64 (built %s %s), game pid %lu, IPC v%u", __DATE__, __TIME__, (unsigned long)pid, kVersion);
    const int rc = Serve(pid, luid, dataDir);
    Shutdown();
    Log("[host] exit %d", rc);
    if (g_log) fclose(g_log);
    return rc;
}
