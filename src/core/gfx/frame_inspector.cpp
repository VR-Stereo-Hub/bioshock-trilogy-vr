// One-shot frame recorder. Design constraints:
//  - Disarmed cost ~zero: every detour is one relaxed atomic load + jump.
//  - The game renders single-threaded (ENGINE_NOTES), so event recording
//    itself is unsynchronized; the arm/disarm flag is atomic because the
//    overlay thread and command seam can arm from outside the render thread.
//  - x86 has no reliable frame-pointer walks in optimized game code, so we
//    keep three callstack sources per draw: _ReturnAddress (always right),
//    RtlCaptureStackBackTrace (best effort), and a heuristic ESP scan for
//    return addresses into the exe's image preceded by CALL encodings.
//  - Dumps go to %LOCALAPPDATA%\BioshockVR (never the repo - they contain
//    game-derived data).

#include "core/gfx/frame_inspector.h"

#include "core/gfx/hud_capture.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace bvr::frame_inspector {
namespace {

// ---- state ------------------------------------------------------------------

std::atomic<int> g_armMode{0};       // 0 idle, 1 lite pending, 2 full pending
std::atomic<bool> g_recording{false};
int g_mode = 0;                      // mode of the recording in progress
thread_local int t_suppress = 0;

// Lifetime call census per hooked slot (diagnostic): are the detours even
// being called? Indexed by EventKind order below.
std::atomic<uint32_t> g_callCensus[13]{};

// Session 34: the adapter mesh veto (see the header). Atomic because it is set
// from the game thread and read on the render thread.
std::atomic<MeshSkipFn> g_meshSkip{nullptr};
std::atomic<unsigned> g_meshSkips{0};
enum CensusIdx {
    CxDrawIndexed,
    CxDraw,
    CxDrawIdxInst,
    CxDrawInst,
    CxSetRT,
    CxClearRtv,
    CxClearDsv,
    // Session 19 (the HUD hunt): draws that never showed in any window meant
    // the HUD reaches the backbuffer some other way - count every remaining
    // lane that can move pixels.
    CxDrawAuto,
    CxDispatch,
    CxCopySubRes,
    CxCopyRes,
    CxUpdateSubRes,
    CxExecCmdList,
};

constexpr size_t kMaxEvents = 20000;
constexpr size_t kMaxStack = 12;
constexpr size_t kCbFloats = 336;    // full mode: first 1344 bytes of a CB - the largest
                                     // b0 tier seen live (64 + k*256, k<=5), so a bone
                                     // palette in the tail is captured, not truncated

struct ResourceInfo {
    int id = 0;
    D3D11_TEXTURE2D_DESC desc{};
    bool isTexture2d = false;
};

enum class EventKind : uint8_t {
    DrawIndexed,
    Draw,
    DrawIndexedInstanced,
    DrawInstanced,
    ClearRtv,
    ClearDsv,
    SetRenderTargets,
    DrawAuto,
    Dispatch,
    CopySubRes,  // rtv0 = dst, srv0 = src (field reuse)
    CopyRes,     // rtv0 = dst, srv0 = src
    UpdateSubRes,// rtv0 = dst
    ExecCmdList, // a = RestoreContextState
};

struct Event {
    EventKind kind;
    uint32_t a = 0, b = 0;           // draw params (counts/starts)
    int rtv0 = -1, dsv = -1;         // resource-table ids
    int srv0 = -1;
    uint16_t vpW = 0, vpH = 0;
    uint32_t cbBytes[3] = {0, 0, 0}; // VS b0..b2 ByteWidth
    const void* cb0Object = nullptr; // identity of VS b0 (for change tracking)
    bool cb0Captured = false;        // this event carries cb0 contents
    float cb0Data[kCbFloats] = {};
    uint32_t retRva = 0;             // _ReturnAddress mapped into the exe (0 if foreign)
    uint32_t stack[kMaxStack] = {};  // exe RVAs, 0-terminated
    float clearColor[4] = {};        // ClearRtv only
    // Mode 3 only; left at -1/0 by modes 1 and 2, and only ever PRINTED in
    // mode 3, so a lite or full dump is byte-identical to before.
    int vsCbId[3] = {-1, -1, -1};
    int psCbId[4] = {-1, -1, -1, -1};
    uint32_t psCbBytes[4] = {0, 0, 0, 0};
};

// ---- mode 3: constant-buffer UPLOAD capture (session 36) -------------------
//
// WHY A THIRD MODE RATHER THAN WIDENING MODE 2. Mode 2 reads back the bound VS
// b0 once per distinct BUFFER OBJECT. That is right for a Map/WRITE_DISCARD
// engine, where every upload renames the buffer. BioShock Infinite is not one:
// it reuses a handful of buffer objects and rewrites them with
// UpdateSubresource (15.3 M lifetime calls, 251 in a single frame), so mode 2
// emits a block only at object transitions and the decoder then attributes many
// draws to a block whose contents were overwritten in between. That is worse
// than an empty dump, because it looks like data.
//
// UpdateSubresource hands us the payload AS A CALL PARAMETER. No staging
// buffer, no CopyResource, no Map stall, no readback race - and it captures
// EVERY constant buffer regardless of which stage or slot it is later bound to,
// including the 160-byte tier that carries Infinite's deferred lighting pass and
// that hud_capture's `>= 320` tier gate has always filtered out.
//
// Strictly additive: everything below is reachable only from mode 3, which
// needs the new `cb` word. Modes 1 and 2 are untouched, so BioShock 1 and 2
// cannot move.
struct CbUpload {
    int evIdx = -1;        // index into g_events, or -1
    int resId = -1;        // resource-table id of the destination buffer
    uint32_t realBytes = 0;// the buffer's full ByteWidth
    uint32_t byteOff = 0;  // pDstBox->left, or 0
    uint32_t arenaOff = 0; // index into g_cbArena
    uint32_t floats = 0;
};
// 8 MB ceiling. A frame uploads roughly 160 K floats, so this has ~50x headroom
// and costs one comparison per upload.
constexpr size_t kCbArenaFloats = 2u << 20;
std::vector<CbUpload> g_cbUploads;
std::vector<float> g_cbArena;

std::vector<Event> g_events;
std::map<ID3D11Resource*, ResourceInfo> g_resources;
int g_nextResourceId = 0;
const void* g_lastCb0Captured = nullptr;
char g_lastDumpPath[MAX_PATH] = "";
char g_status[128] = "idle";

uintptr_t g_exeBase = 0;
size_t g_exeSize = 0;

// Original function pointers.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT,
                                                        INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                      ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*,
                                            const FLOAT[4]);
using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT,
                                            FLOAT, UINT8);

DrawIndexedFn g_origDrawIndexed = nullptr;
DrawFn g_origDraw = nullptr;
DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;
DrawInstancedFn g_origDrawInstanced = nullptr;
OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
ClearRtvFn g_origClearRtv = nullptr;
ClearDsvFn g_origClearDsv = nullptr;

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
MapFn g_origMap = nullptr;
UnmapFn g_origUnmap = nullptr;

using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using CopySubResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT,
                                              UINT, UINT, ID3D11Resource*, UINT,
                                              const D3D11_BOX*);
using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
                                           ID3D11Resource*);
using UpdateSubResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                const D3D11_BOX*, const void*, UINT, UINT);
using ExecCmdListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
DrawAutoFn g_origDrawAuto = nullptr;
DispatchFn g_origDispatch = nullptr;
CopySubResFn g_origCopySubRes = nullptr;
CopyResFn g_origCopyRes = nullptr;
UpdateSubResFn g_origUpdateSubRes = nullptr;
ExecCmdListFn g_origExecCmdList = nullptr;

// ---- cb watch (session 13) --------------------------------------------------
// The engine uploads per-draw constants through Map(WRITE_DISCARD)/memcpy/
// Unmap on the render thread; inspecting the staged bytes at Unmap time is
// free (no GPU readback). One pattern, one capture slot, seqlocked for the
// game-thread reader.
constexpr uint32_t kWatchMaxPat = 8;
constexpr uint32_t kWatchMaxCap = 32;
std::atomic<bool> g_watchArmed{false};
float g_watchPat[kWatchMaxPat] = {};
uint32_t g_watchPatFirst = 0, g_watchPatCount = 0;
uint32_t g_watchCapFirst = 0, g_watchCapCount = 0;
uint32_t g_watchBytes = 0; // exact ByteWidth filter (0 = any)
std::atomic<uint32_t> g_watchSeq{0}; // even = stable
float g_watchData[kWatchMaxCap] = {};
std::atomic<uint64_t> g_watchTickMs{0};
std::atomic<uint32_t> g_watchHits{0};
// Pending "log the writer" shots: on a fingerprint match, capture and log the
// Unmap callstack (exe RVAs) - the road to whoever BUILDS the watched values.
std::atomic<int> g_watchStackShots{0};

// Per-thread last WRITE-mapped buffer (the engine's pattern is a tight
// Map/copy/Unmap per draw, so depth-1 tracking is enough; a nested map just
// drops the outer capture for that draw).
thread_local ID3D11Resource* t_mappedRes = nullptr;
thread_local void* t_mappedPtr = nullptr;
thread_local uint32_t t_mappedBytes = 0;

size_t collect_stack(void* espHint, uint32_t* out, size_t maxOut); // fwd

void watch_inspect(ID3D11Resource* res, void* espHint) {
    if (!g_watchArmed.load(std::memory_order_relaxed)) return;
    if (!t_mappedPtr || res != t_mappedRes) return;
    if (g_watchBytes && t_mappedBytes != g_watchBytes) return;
    uint32_t needBytes = (g_watchCapFirst + g_watchCapCount) * 4;
    uint32_t patEnd = (g_watchPatFirst + g_watchPatCount) * 4;
    if (patEnd > needBytes) needBytes = patEnd;
    if (t_mappedBytes < needBytes) return;
    const float* f = static_cast<const float*>(t_mappedPtr);
    // Fingerprint match, tolerant to last-ULP recomputation drift.
    for (uint32_t i = 0; i < g_watchPatCount; ++i) {
        float d = f[g_watchPatFirst + i] - g_watchPat[i];
        if (d > 1e-3f || d < -1e-3f) return;
    }
    uint32_t seq = g_watchSeq.load(std::memory_order_relaxed);
    g_watchSeq.store(seq + 1, std::memory_order_release);
    memcpy(g_watchData, f + g_watchCapFirst, g_watchCapCount * 4);
    g_watchSeq.store(seq + 2, std::memory_order_release);
    g_watchTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_watchHits.fetch_add(1, std::memory_order_relaxed);
    int shots = g_watchStackShots.load(std::memory_order_relaxed);
    if (shots > 0 &&
        g_watchStackShots.compare_exchange_strong(shots, shots - 1,
                                                  std::memory_order_relaxed)) {
        uint32_t rvas[10] = {};
        size_t n = collect_stack(espHint, rvas, 10);
        char line[256];
        int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                              "[gfx] cbwatch writer stack (%u B cb):", t_mappedBytes);
        for (size_t i = 0; i < n && len > 0 && len < 230; ++i)
            len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " 0x%X", rvas[i]);
        BVR_LOG("%s", line);
    }
}

// ---- helpers ----------------------------------------------------------------

void capture_exe_range() {
    if (g_exeBase) return;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return;
    // SizeOfImage straight from the PE header - no psapi needed here.
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const uint8_t*>(exe) +
                                                           dos->e_lfanew);
    g_exeBase = reinterpret_cast<uintptr_t>(exe);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
}

uint32_t to_exe_rva(const void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a >= g_exeBase + 0x1000 && a < g_exeBase + g_exeSize)
        return static_cast<uint32_t>(a - g_exeBase);
    return 0;
}

// Registers a view's underlying resource in the table, returns its id.
int register_resource(ID3D11View* view) {
    if (!view) return -1;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res) return -1;

    auto it = g_resources.find(res);
    if (it != g_resources.end()) {
        res->Release();
        return it->second.id;
    }
    ResourceInfo info{};
    info.id = g_nextResourceId++;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        tex->GetDesc(&info.desc);
        info.isTexture2d = true;
        tex->Release();
    }
    g_resources.emplace(res, info);
    res->Release(); // table keys are identity only, never dereferenced
    return info.id;
}

// Raw-resource variant (the Copy/Update lanes have no view). Identity-keyed
// like the view path; does NOT take a reference.
int register_resource_raw(ID3D11Resource* res) {
    if (!res) return -1;
    auto it = g_resources.find(res);
    if (it != g_resources.end()) return it->second.id;
    ResourceInfo info{};
    info.id = g_nextResourceId++;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        tex->GetDesc(&info.desc);
        info.isTexture2d = true;
        tex->Release();
    }
    g_resources.emplace(res, info);
    return info.id;
}

// Best-effort callstack: RtlCaptureStackBackTrace first, then a heuristic
// scan up the stack for exe .text return addresses preceded by a CALL.
// Only game-exe frames are kept (to_exe_rva filters ours), so the exact
// skip depth between call sites is non-critical.
size_t collect_stack(void* espHint, uint32_t* out, size_t maxOut) {
    void* frames[16] = {};
    USHORT n = RtlCaptureStackBackTrace(2, 16, frames, nullptr);
    size_t cnt = 0;
    for (USHORT i = 0; i < n && cnt < maxOut; ++i) {
        uint32_t rva = to_exe_rva(frames[i]);
        if (rva) out[cnt++] = rva;
    }
    if (cnt >= 4) return cnt; // EBP walk was healthy enough

    // Heuristic ESP scan (FPO-resistant): any dword on the stack that points
    // into the exe image right after a plausible CALL encoding.
    const uintptr_t esp = reinterpret_cast<uintptr_t>(espHint);
    for (uintptr_t p = esp; p < esp + 2048 && cnt < maxOut; p += 4) {
        uint32_t candidate = 0;
        __try {
            candidate = *reinterpret_cast<const uint32_t*>(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        uint32_t rva = to_exe_rva(reinterpret_cast<const void*>(candidate));
        if (!rva || rva < 5) continue;
        const uint8_t* code = reinterpret_cast<const uint8_t*>(g_exeBase + rva);
        bool looksLikeCall = false;
        __try {
            looksLikeCall = code[-5] == 0xE8 ||                       // CALL rel32
                            code[-6] == 0xFF || code[-2] == 0xFF ||   // CALL r/m32 forms
                            code[-3] == 0xFF;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!looksLikeCall) continue;
        bool dup = false;
        for (size_t i = 0; i < cnt; ++i)
            if (out[i] == rva) dup = true;
        if (!dup) out[cnt++] = rva;
    }
    return cnt;
}

void capture_stack(Event& ev, void* espHint) {
    size_t n = collect_stack(espHint, ev.stack, kMaxStack);
    (void)n;
}

// Query pipeline state around a draw. Expensive, but only runs while
// recording a single armed frame.
void capture_draw_state(ID3D11DeviceContext* ctx, Event& ev) {
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);
    ev.rtv0 = register_resource(rtvs[0]);
    ev.dsv = register_resource(dsv);
    for (auto* rtv : rtvs)
        if (rtv) rtv->Release();
    if (dsv) dsv->Release();

    UINT nvp = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&nvp, &vp);
    ev.vpW = static_cast<uint16_t>(vp.Width);
    ev.vpH = static_cast<uint16_t>(vp.Height);

    ID3D11Buffer* cbs[3] = {};
    ctx->VSGetConstantBuffers(0, 3, cbs);
    for (int i = 0; i < 3; ++i) {
        if (!cbs[i]) continue;
        D3D11_BUFFER_DESC bd{};
        cbs[i]->GetDesc(&bd);
        ev.cbBytes[i] = bd.ByteWidth;
    }
    ev.cb0Object = cbs[0];

    // Mode 3 only: record WHICH buffers this draw reads, so the upload table
    // can be joined to the draws that consumed it. On a deferred renderer the
    // inverse-projection terms are commonly a PIXEL-shader constant, and this
    // is the first PSGetConstantBuffers call in the codebase - modes 1 and 2
    // never reach it.
    if (g_mode == 3) {
        for (int i = 0; i < 3; ++i)
            if (cbs[i]) ev.vsCbId[i] = register_resource_raw(cbs[i]);
        ID3D11Buffer* pcbs[4] = {};
        ctx->PSGetConstantBuffers(0, 4, pcbs);
        for (int i = 0; i < 4; ++i) {
            if (!pcbs[i]) continue;
            D3D11_BUFFER_DESC pd{};
            pcbs[i]->GetDesc(&pd);
            ev.psCbBytes[i] = pd.ByteWidth;
            ev.psCbId[i] = register_resource_raw(pcbs[i]);
            pcbs[i]->Release();
        }
    }

    // Full mode: read back VS b0 contents, once per distinct buffer object
    // in a row (the engine reuses one big CB - capturing every draw would
    // balloon the dump without adding information).
    if (g_mode == 2 && cbs[0] && cbs[0] != g_lastCb0Captured) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC bd{};
            cbs[0]->GetDesc(&bd);
            D3D11_BUFFER_DESC sd = bd;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            ID3D11Buffer* staging = nullptr;
            if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging))) {
                ctx->CopyResource(staging, cbs[0]);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                    size_t n = bd.ByteWidth / 4;
                    if (n > kCbFloats) n = kCbFloats;
                    memcpy(ev.cb0Data, mapped.pData, n * 4);
                    ev.cb0Captured = true;
                    g_lastCb0Captured = cbs[0];
                    ctx->Unmap(staging, 0);
                }
                staging->Release();
            }
            dev->Release();
        }
    }
    for (auto* cb : cbs)
        if (cb) cb->Release();

    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    ev.srv0 = register_resource(srv);
    if (srv) srv->Release();
}

HRESULT STDMETHODCALLTYPE MapDetour(ID3D11DeviceContext* ctx, ID3D11Resource* res,
                                    UINT sub, D3D11_MAP mapType, UINT flags,
                                    D3D11_MAPPED_SUBRESOURCE* mapped) {
    HRESULT hr = g_origMap(ctx, res, sub, mapType, flags, mapped);
    if (g_watchArmed.load(std::memory_order_relaxed) && SUCCEEDED(hr) && mapped &&
        mapped->pData && sub == 0 &&
        (mapType == D3D11_MAP_WRITE_DISCARD || mapType == D3D11_MAP_WRITE ||
         mapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        ID3D11Buffer* buf = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Buffer),
                                                 reinterpret_cast<void**>(&buf)))) {
            D3D11_BUFFER_DESC bd{};
            buf->GetDesc(&bd);
            buf->Release();
            t_mappedRes = res;
            t_mappedPtr = mapped->pData;
            t_mappedBytes = bd.ByteWidth;
        }
    }
    return hr;
}

void STDMETHODCALLTYPE UnmapDetour(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub) {
    if (sub == 0 && res == t_mappedRes) {
        watch_inspect(res, &sub);
        t_mappedRes = nullptr;
        t_mappedPtr = nullptr;
        t_mappedBytes = 0;
    }
    g_origUnmap(ctx, res, sub);
}

bool should_record() {
    return g_recording.load(std::memory_order_relaxed) && t_suppress == 0 &&
           g_events.size() < kMaxEvents;
}

Event& push_event(EventKind kind, const void* retAddr, void* espHint) {
    g_events.emplace_back();
    Event& ev = g_events.back();
    ev.kind = kind;
    ev.retRva = to_exe_rva(retAddr);
    capture_stack(ev, espHint);
    return ev;
}

// ---- detours ------------------------------------------------------------

void STDMETHODCALLTYPE DrawIndexedDetour(ID3D11DeviceContext* ctx, UINT indexCount,
                                         UINT startIndex, INT baseVertex) {
    g_callCensus[CxDrawIndexed].fetch_add(1, std::memory_order_relaxed);
    if (t_suppress == 0) bvr::hud::on_draw_indexed(ctx);
    // Session 34: adapter mesh veto. Checked BEFORE recording on purpose - a
    // dropped draw did not happen, and a dump that lists it would be describing
    // a frame the GPU never saw. Null unless a game registered one.
    if (t_suppress == 0) {
        MeshSkipFn skip = g_meshSkip.load(std::memory_order_relaxed);
        if (skip && skip(indexCount)) {
            g_meshSkips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (should_record()) {
        ++t_suppress; // our own Get* calls must not recurse into recording
        Event& ev = push_event(EventKind::DrawIndexed, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.a = indexCount;
        ev.b = startIndex;
        capture_draw_state(ctx, ev);
        --t_suppress;
    }
    g_origDrawIndexed(ctx, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE DrawDetour(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex) {
    g_callCensus[CxDraw].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::Draw, _ReturnAddress(), _AddressOfReturnAddress());
        ev.a = vertexCount;
        ev.b = startVertex;
        capture_draw_state(ctx, ev);
        --t_suppress;
    }
    // HUD redirect (session 19): a gameswf-classified draw gets our RT bound
    // instead - through the ORIGINAL SetRT so the substitution does not roll
    // the classifier's own binding state. Our DSV rides along (flash masks
    // stencil against it) and the blend state swaps to its alpha-corrected
    // variant (checked per draw - gameswf changes states mid-stream).
    if (t_suppress == 0) {
        bvr::hud::DrawDecision d = bvr::hud::on_draw(ctx, vertexCount);
        if (d.verdict == bvr::hud::DrawVerdict::Redirect && d.rtv) {
            ++t_suppress;
            ID3D11DepthStencilView* dsv = bvr::hud::capture_dsv();
            g_origOMSetRenderTargets(ctx, 1, &d.rtv, dsv);
            bvr::hud::fix_blend_alpha(ctx);
            --t_suppress;
        } else if (d.verdict == bvr::hud::DrawVerdict::Skip) {
            // Session 29: the ONLY place a draw is dropped. Skipping is safe
            // where redirecting is not - we change no device state, so the
            // gameswf batch's own state machine is untouched and the next draw
            // in the batch behaves exactly as it would have.
            return;
        } else if (d.restoreRtv) {
            // Session 30: an earlier draw in this batch redirected and the game
            // has not rebound since, so "pass through" would still land on our
            // capture RT. Hand the game's own binding back first, through the
            // ORIGINAL SetRT for the same reason the substitution uses it - so
            // the classifier's own binding state does not roll.
            ++t_suppress;
            g_origOMSetRenderTargets(ctx, 1, &d.restoreRtv, d.restoreDsv);
            --t_suppress;
        }
    }
    g_origDraw(ctx, vertexCount, startVertex);
}

void STDMETHODCALLTYPE DrawIndexedInstancedDetour(ID3D11DeviceContext* ctx, UINT indexCount,
                                                  UINT instances, UINT startIndex, INT baseVertex,
                                                  UINT startInstance) {
    g_callCensus[CxDrawIdxInst].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::DrawIndexedInstanced, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.a = indexCount;
        ev.b = instances;
        capture_draw_state(ctx, ev);
        --t_suppress;
    }
    g_origDrawIndexedInstanced(ctx, indexCount, instances, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE DrawInstancedDetour(ID3D11DeviceContext* ctx, UINT vertexCount,
                                           UINT instances, UINT startVertex, UINT startInstance) {
    g_callCensus[CxDrawInst].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::DrawInstanced, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.a = vertexCount;
        ev.b = instances;
        capture_draw_state(ctx, ev);
        --t_suppress;
    }
    g_origDrawInstanced(ctx, vertexCount, instances, startVertex, startInstance);
}

void STDMETHODCALLTYPE OMSetRenderTargetsDetour(ID3D11DeviceContext* ctx, UINT numViews,
                                                ID3D11RenderTargetView* const* rtvs,
                                                ID3D11DepthStencilView* dsv) {
    g_callCensus[CxSetRT].fetch_add(1, std::memory_order_relaxed);
    if (t_suppress == 0) bvr::hud::on_setrt(numViews, rtvs, dsv);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::SetRenderTargets, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.a = numViews;
        ev.rtv0 = (numViews && rtvs) ? register_resource(rtvs[0]) : -1;
        ev.dsv = register_resource(dsv);
        --t_suppress;
    }
    g_origOMSetRenderTargets(ctx, numViews, rtvs, dsv);
}

void STDMETHODCALLTYPE ClearRtvDetour(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                                      const FLOAT color[4]) {
    g_callCensus[CxClearRtv].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::ClearRtv, _ReturnAddress(), _AddressOfReturnAddress());
        ev.rtv0 = register_resource(rtv);
        if (color) memcpy(ev.clearColor, color, sizeof(ev.clearColor));
        --t_suppress;
    }
    g_origClearRtv(ctx, rtv, color);
}

void STDMETHODCALLTYPE ClearDsvDetour(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                      UINT flags, FLOAT depth, UINT8 stencil) {
    g_callCensus[CxClearDsv].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::ClearDsv, _ReturnAddress(), _AddressOfReturnAddress());
        ev.dsv = register_resource(dsv);
        ev.a = flags;
        --t_suppress;
    }
    g_origClearDsv(ctx, dsv, flags, depth, stencil);
}

void STDMETHODCALLTYPE DrawAutoDetour(ID3D11DeviceContext* ctx) {
    g_callCensus[CxDrawAuto].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::DrawAuto, _ReturnAddress(), _AddressOfReturnAddress());
        capture_draw_state(ctx, ev);
        --t_suppress;
    }
    g_origDrawAuto(ctx);
}

void STDMETHODCALLTYPE DispatchDetour(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    g_callCensus[CxDispatch].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::Dispatch, _ReturnAddress(), _AddressOfReturnAddress());
        ev.a = x;
        ev.b = y;
        --t_suppress;
    }
    g_origDispatch(ctx, x, y, z);
}

void STDMETHODCALLTYPE CopySubResDetour(ID3D11DeviceContext* ctx, ID3D11Resource* dst,
                                        UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
                                        ID3D11Resource* src, UINT srcSub,
                                        const D3D11_BOX* box) {
    g_callCensus[CxCopySubRes].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::CopySubRes, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.rtv0 = register_resource_raw(dst);
        ev.srv0 = register_resource_raw(src);
        ev.a = dstX;
        ev.b = dstY;
        --t_suppress;
    }
    g_origCopySubRes(ctx, dst, dstSub, dstX, dstY, dstZ, src, srcSub, box);
}

void STDMETHODCALLTYPE CopyResDetour(ID3D11DeviceContext* ctx, ID3D11Resource* dst,
                                     ID3D11Resource* src) {
    g_callCensus[CxCopyRes].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::CopyRes, _ReturnAddress(), _AddressOfReturnAddress());
        ev.rtv0 = register_resource_raw(dst);
        ev.srv0 = register_resource_raw(src);
        --t_suppress;
    }
    g_origCopyRes(ctx, dst, src);
}

void STDMETHODCALLTYPE UpdateSubResDetour(ID3D11DeviceContext* ctx, ID3D11Resource* dst,
                                          UINT dstSub, const D3D11_BOX* box, const void* data,
                                          UINT rowPitch, UINT depthPitch) {
    g_callCensus[CxUpdateSubRes].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::UpdateSubRes, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.rtv0 = register_resource_raw(dst);
        // Mode 3: keep the payload. This runs BEFORE the original, which is the
        // only unconditionally correct ordering - after it, `data` is the
        // caller's to reuse.
        if (g_mode == 3 && data && dst) {
            ID3D11Buffer* buf = nullptr;
            if (SUCCEEDED(dst->QueryInterface(__uuidof(ID3D11Buffer),
                                              reinterpret_cast<void**>(&buf)))) {
                D3D11_BUFFER_DESC bd{};
                buf->GetDesc(&bd);
                buf->Release();
                // Constant buffers only - vertex and index streams would swamp
                // the dump with megabytes of geometry.
                if (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                    // ON A BUFFER, pDstBox addresses BYTES in left/right, and
                    // SrcRowPitch is IGNORED by D3D. Engines pass junk in it -
                    // never trust it here. No box means the whole buffer.
                    uint32_t off = box ? box->left : 0u;
                    uint32_t len = box ? (box->right - box->left) : bd.ByteWidth;
                    if (off > bd.ByteWidth) {
                        len = 0;
                    } else if (off + len > bd.ByteWidth) {
                        len = bd.ByteWidth - off;
                    }
                    const uint32_t floats = len / 4;
                    if (floats > 0 && g_cbArena.size() + floats <= kCbArenaFloats) {
                        CbUpload u{};
                        u.evIdx = static_cast<int>(g_events.size()) - 1;
                        u.resId = ev.rtv0;
                        u.realBytes = bd.ByteWidth;
                        u.byteOff = off;
                        u.arenaOff = static_cast<uint32_t>(g_cbArena.size());
                        u.floats = floats;
                        const float* src = static_cast<const float*>(data);
                        g_cbArena.insert(g_cbArena.end(), src, src + floats);
                        g_cbUploads.push_back(u);
                    }
                }
            }
        }
        --t_suppress;
    }
    g_origUpdateSubRes(ctx, dst, dstSub, box, data, rowPitch, depthPitch);
}

void STDMETHODCALLTYPE ExecCmdListDetour(ID3D11DeviceContext* ctx, ID3D11CommandList* list,
                                         BOOL restore) {
    g_callCensus[CxExecCmdList].fetch_add(1, std::memory_order_relaxed);
    if (should_record()) {
        ++t_suppress;
        Event& ev = push_event(EventKind::ExecCmdList, _ReturnAddress(),
                               _AddressOfReturnAddress());
        ev.a = restore ? 1 : 0;
        --t_suppress;
    }
    g_origExecCmdList(ctx, list, restore);
}

// ---- dump writer --------------------------------------------------------

const char* kind_name(EventKind k) {
    switch (k) {
        case EventKind::DrawIndexed: return "DrawIndexed";
        case EventKind::Draw: return "Draw";
        case EventKind::DrawIndexedInstanced: return "DrawIdxInst";
        case EventKind::DrawInstanced: return "DrawInst";
        case EventKind::ClearRtv: return "ClearRTV";
        case EventKind::ClearDsv: return "ClearDSV";
        case EventKind::SetRenderTargets: return "SetRT";
        case EventKind::DrawAuto: return "DrawAuto";
        case EventKind::Dispatch: return "Dispatch";
        case EventKind::CopySubRes: return "CopySubRes";
        case EventKind::CopyRes: return "CopyRes";
        case EventKind::UpdateSubRes: return "UpdateSubRes";
        case EventKind::ExecCmdList: return "ExecCmdList";
    }
    return "?";
}

bool is_draw(EventKind k) {
    return k == EventKind::DrawIndexed || k == EventKind::Draw ||
           k == EventKind::DrawIndexedInstanced || k == EventKind::DrawInstanced;
}

int g_dumpSeq = 0; // per-boot counter: consecutive-window dumps land in one second

void write_dump() {
    wchar_t path[MAX_PATH];
    // Composed from data_dir() so the per-game subdir (log.cpp) is honored;
    // for BioShock 1 the resulting string is unchanged.
    const wchar_t* base = bvr::log::data_dir();
    if (!base[0]) return; // log::init failed - no data dir
    SYSTEMTIME st{};
    GetLocalTime(&st);
    swprintf_s(path, L"%s\\framedump_%02u%02u%02u_q%d.txt", base, st.wHour,
               st.wMinute, st.wSecond, g_dumpSeq++);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt") != 0 || !f) {
        sprintf_s(g_status, "dump open FAILED");
        return;
    }

    // The mode token is the decoder's machine-readable branch key, and an old
    // dump keeps saying lite/full, so an old decoder reading a new dump is a
    // no-op rather than a misparse.
    const char* modeName = g_mode == 3 ? "cb" : (g_mode == 2 ? "full" : "lite");
    fprintf(f, "frame dump: %u events, mode=%s, exe base 0x%08X\n",
            static_cast<unsigned>(g_events.size()), modeName,
            static_cast<unsigned>(g_exeBase));
    fprintf(f, "lifetime call census: DrawIndexed=%u Draw=%u DrawIdxInst=%u DrawInst=%u "
               "SetRT=%u ClearRTV=%u ClearDSV=%u DrawAuto=%u Dispatch=%u CopySubRes=%u "
               "CopyRes=%u UpdateSubRes=%u ExecCmdList=%u\n\n",
            g_callCensus[CxDrawIndexed].load(), g_callCensus[CxDraw].load(),
            g_callCensus[CxDrawIdxInst].load(), g_callCensus[CxDrawInst].load(),
            g_callCensus[CxSetRT].load(), g_callCensus[CxClearRtv].load(),
            g_callCensus[CxClearDsv].load(), g_callCensus[CxDrawAuto].load(),
            g_callCensus[CxDispatch].load(), g_callCensus[CxCopySubRes].load(),
            g_callCensus[CxCopyRes].load(), g_callCensus[CxUpdateSubRes].load(),
            g_callCensus[CxExecCmdList].load());
    BVR_LOG("[gfx] census: DrawIndexed=%u Draw=%u DrawIdxInst=%u DrawInst=%u SetRT=%u "
            "ClearRTV=%u ClearDSV=%u",
            g_callCensus[CxDrawIndexed].load(), g_callCensus[CxDraw].load(),
            g_callCensus[CxDrawIdxInst].load(), g_callCensus[CxDrawInst].load(),
            g_callCensus[CxSetRT].load(), g_callCensus[CxClearRtv].load(),
            g_callCensus[CxClearDsv].load());

    // Resource table.
    fprintf(f, "== resources ==\n");
    for (const auto& [res, info] : g_resources) {
        if (info.isTexture2d) {
            fprintf(f, "T%d: %p Texture2D %ux%u fmt=%u mips=%u bind=0x%X\n", info.id,
                    static_cast<const void*>(res), info.desc.Width, info.desc.Height,
                    info.desc.Format, info.desc.MipLevels, info.desc.BindFlags);
        } else {
            fprintf(f, "T%d: %p (non-tex2d)\n", info.id, static_cast<const void*>(res));
        }
    }

    // Event list.
    fprintf(f, "\n== events ==\n");
    int idx = 0;
    for (const Event& ev : g_events) {
        fprintf(f, "%05d %-11s a=%-6u b=%-5u rtv0=T%-3d dsv=T%-3d vp=%ux%u cb=%u/%u/%u srv0=T%-3d "
                   "ret=0x%06X stk=",
                idx++, kind_name(ev.kind), ev.a, ev.b, ev.rtv0, ev.dsv, ev.vpW, ev.vpH,
                ev.cbBytes[0], ev.cbBytes[1], ev.cbBytes[2], ev.srv0, ev.retRva);
        for (size_t i = 0; i < kMaxStack && ev.stack[i]; ++i)
            fprintf(f, "%s0x%06X", i ? "," : "", ev.stack[i]);
        if (ev.kind == EventKind::ClearRtv)
            fprintf(f, " color=(%.3f %.3f %.3f %.3f)", ev.clearColor[0], ev.clearColor[1],
                    ev.clearColor[2], ev.clearColor[3]);
        // Mode 3 tail. APPENDED AFTER stk= ON PURPOSE: the decoder's event regex
        // is anchored from ^ through stk=(\S*) and is a PARTIAL match, so
        // anything added after that point is invisible to it, while anything
        // inserted before it would break every BioShock 1 and 2 dump parse. The
        // existing ClearRtv `color=` tail already relies on this.
        if (g_mode == 3) {
            fprintf(f, " vscb=T%d,T%d,T%d pscb=T%d,T%d,T%d,T%d pscbb=%u/%u/%u/%u", ev.vsCbId[0],
                    ev.vsCbId[1], ev.vsCbId[2], ev.psCbId[0], ev.psCbId[1], ev.psCbId[2],
                    ev.psCbId[3], ev.psCbBytes[0], ev.psCbBytes[1], ev.psCbBytes[2],
                    ev.psCbBytes[3]);
        }
        fputc('\n', f);
        if (ev.cb0Captured) {
            fprintf(f, "      cb0:");
            for (size_t i = 0; i < kCbFloats; ++i) {
                fprintf(f, " %.4f", ev.cb0Data[i]);
                if ((i & 7) == 7 && i + 1 < kCbFloats) fprintf(f, "\n          ");
            }
            fputc('\n', f);
        }
    }

    // Mode 3: the constant-buffer upload table. Records are prefixed U%05d so
    // they can collide with neither an event line (digit at column 0) nor a
    // cb0 continuation (6-space indent), and the whole section is only reachable
    // by a decoder that opts into it.
    if (g_mode == 3 && !g_cbUploads.empty()) {
        fprintf(f, "\n== cb uploads ==\n");
        int uidx = 0;
        for (const CbUpload& u : g_cbUploads) {
            fprintf(f, "U%05d ev=%05d dst=T%-3d bytes=%u off=%u n=%u\n", uidx++, u.evIdx,
                    u.resId, u.realBytes, u.byteOff, u.floats);
            for (uint32_t i = 0; i < u.floats; ++i) {
                fprintf(f, "%s%.4f", (i & 7) ? " " : "       ", g_cbArena[u.arenaOff + i]);
                if ((i & 7) == 7 && i + 1 < u.floats) fputc('\n', f);
            }
            fputc('\n', f);
        }
    }

    // Summary: draws per RT, and the return-RVA histogram over the RT with
    // the most depth-tested draws (scene-pass candidates).
    std::map<int, int> drawsPerRt;
    std::map<int, int> depthDrawsPerRt;
    for (const Event& ev : g_events) {
        if (!is_draw(ev.kind)) continue;
        drawsPerRt[ev.rtv0]++;
        if (ev.dsv >= 0) depthDrawsPerRt[ev.rtv0]++;
    }
    fprintf(f, "\n== draws per rtv0 (total / depth-tested) ==\n");
    int sceneRt = -1, sceneRtDraws = 0;
    for (const auto& [rt, n] : drawsPerRt) {
        int nd = depthDrawsPerRt.count(rt) ? depthDrawsPerRt[rt] : 0;
        fprintf(f, "T%d: %d / %d\n", rt, n, nd);
        if (nd > sceneRtDraws) {
            sceneRtDraws = nd;
            sceneRt = rt;
        }
    }

    if (sceneRt >= 0) {
        std::map<uint32_t, int> retHisto;
        for (const Event& ev : g_events) {
            if (!is_draw(ev.kind) || ev.rtv0 != sceneRt || ev.dsv < 0) continue;
            if (ev.retRva) retHisto[ev.retRva]++;
            for (size_t i = 0; i < kMaxStack && ev.stack[i]; ++i)
                retHisto[ev.stack[i]]++;
        }
        // Top 20 by count.
        fprintf(f, "\n== scene RT = T%d, stack-RVA histogram (top 20) ==\n", sceneRt);
        for (int rank = 0; rank < 20; ++rank) {
            uint32_t bestRva = 0;
            int bestN = 0;
            for (const auto& [rva, n] : retHisto) {
                if (n > bestN) {
                    bestN = n;
                    bestRva = rva;
                }
            }
            if (!bestRva) break;
            fprintf(f, "0x%06X: %d (of %d scene draws)\n", bestRva, bestN, sceneRtDraws);
            retHisto.erase(bestRva);
        }
    }

    fclose(f);
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
    strcpy_s(g_lastDumpPath, narrow);
    sprintf_s(g_status, "dumped %u events", static_cast<unsigned>(g_events.size()));
    BVR_LOG("[gfx] frame dump written: %s (%u events, %u resources)", narrow,
            static_cast<unsigned>(g_events.size()), static_cast<unsigned>(g_resources.size()));
}

} // namespace

// ---- public api -----------------------------------------------------------

bool install(void** ctxVtable) {
    if (!ctxVtable) return false;
    capture_exe_range();

    struct Slot {
        int index;
        void* detour;
        void** original;
        const char* name;
    };
    const Slot slots[] = {
        {12, reinterpret_cast<void*>(&DrawIndexedDetour),
         reinterpret_cast<void**>(&g_origDrawIndexed), "DrawIndexed"},
        {13, reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&g_origDraw), "Draw"},
        {20, reinterpret_cast<void*>(&DrawIndexedInstancedDetour),
         reinterpret_cast<void**>(&g_origDrawIndexedInstanced), "DrawIndexedInstanced"},
        {21, reinterpret_cast<void*>(&DrawInstancedDetour),
         reinterpret_cast<void**>(&g_origDrawInstanced), "DrawInstanced"},
        {33, reinterpret_cast<void*>(&OMSetRenderTargetsDetour),
         reinterpret_cast<void**>(&g_origOMSetRenderTargets), "OMSetRenderTargets"},
        {50, reinterpret_cast<void*>(&ClearRtvDetour), reinterpret_cast<void**>(&g_origClearRtv),
         "ClearRenderTargetView"},
        {53, reinterpret_cast<void*>(&ClearDsvDetour), reinterpret_cast<void**>(&g_origClearDsv),
         "ClearDepthStencilView"},
        {14, reinterpret_cast<void*>(&MapDetour), reinterpret_cast<void**>(&g_origMap), "Map"},
        {15, reinterpret_cast<void*>(&UnmapDetour), reinterpret_cast<void**>(&g_origUnmap),
         "Unmap"},
        // Session 19 (the HUD hunt): every other lane that can move pixels.
        {38, reinterpret_cast<void*>(&DrawAutoDetour),
         reinterpret_cast<void**>(&g_origDrawAuto), "DrawAuto"},
        {41, reinterpret_cast<void*>(&DispatchDetour),
         reinterpret_cast<void**>(&g_origDispatch), "Dispatch"},
        {46, reinterpret_cast<void*>(&CopySubResDetour),
         reinterpret_cast<void**>(&g_origCopySubRes), "CopySubresourceRegion"},
        {47, reinterpret_cast<void*>(&CopyResDetour),
         reinterpret_cast<void**>(&g_origCopyRes), "CopyResource"},
        {48, reinterpret_cast<void*>(&UpdateSubResDetour),
         reinterpret_cast<void**>(&g_origUpdateSubRes), "UpdateSubresource"},
        {58, reinterpret_cast<void*>(&ExecCmdListDetour),
         reinterpret_cast<void**>(&g_origExecCmdList), "ExecuteCommandList"},
    };

    int hooked = 0;
    for (const Slot& s : slots) {
        MH_STATUS status = MH_CreateHook(ctxVtable[s.index], s.detour, s.original);
        if (status == MH_OK) status = MH_EnableHook(ctxVtable[s.index]);
        if (status == MH_OK) {
            ++hooked;
        } else {
            BVR_LOG("[gfx] inspector hook %s FAILED: %s", s.name, MH_StatusToString(status));
        }
    }
    BVR_LOG("[gfx] frame inspector: %d/15 context slots hooked", hooked);
    return hooked == 15;
}

void set_cb_watch(const float* pattern, uint32_t patFirst, uint32_t patCount,
                  uint32_t capFirst, uint32_t capCount, uint32_t requiredBytes) {
    g_watchArmed.store(false, std::memory_order_relaxed);
    if (!pattern || patCount == 0 || patCount > kWatchMaxPat || capCount == 0 ||
        capCount > kWatchMaxCap) {
        BVR_LOG("[gfx] cb watch cleared");
        return;
    }
    memcpy(g_watchPat, pattern, patCount * 4);
    g_watchPatFirst = patFirst;
    g_watchPatCount = patCount;
    g_watchCapFirst = capFirst;
    g_watchCapCount = capCount;
    g_watchBytes = requiredBytes;
    g_watchArmed.store(true, std::memory_order_release);
    BVR_LOG("[gfx] cb watch armed: %u pattern floats @ f%u -> capture f%u..f%u (bytes=%u)",
            patCount, patFirst, capFirst, capFirst + capCount - 1, requiredBytes);
}

uint32_t cb_watch_hits() {
    return g_watchHits.load(std::memory_order_relaxed);
}

void cb_watch_log_stacks(int n) {
    g_watchStackShots.store(n, std::memory_order_relaxed);
}

bool latest_cb_watch(float* out, uint32_t count, uint64_t* ageMs) {
    if (!out || count > kWatchMaxCap) return false;
    uint64_t tick = g_watchTickMs.load(std::memory_order_relaxed);
    if (!tick) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s0 = g_watchSeq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        float tmp[kWatchMaxCap];
        memcpy(tmp, g_watchData, count * 4);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_watchSeq.load(std::memory_order_relaxed) != s0) continue;
        memcpy(out, tmp, count * 4);
        if (ageMs) *ageMs = GetTickCount64() - tick;
        return true;
    }
    return false;
}

std::atomic<int> g_armCount{0}; // windows left to record (consecutive presents)

void set_mesh_skip(MeshSkipFn fn) {
    g_meshSkip.store(fn, std::memory_order_relaxed);
}

unsigned mesh_skips() {
    return g_meshSkips.load(std::memory_order_relaxed);
}

void arm(int mode, int count) {
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    g_armCount.store(count, std::memory_order_relaxed);
    if (mode < 1 || mode > 3) mode = 1;
    g_armMode.store(mode, std::memory_order_relaxed);
    const char* name = mode == 3 ? "cb" : (mode == 2 ? "full" : "lite");
    BVR_LOG("[gfx] frame dump armed (%s, %d window%s)", name, count, count == 1 ? "" : "s");
}

void on_present(IDXGISwapChain*) {
    if (g_recording.load(std::memory_order_relaxed)) {
        g_recording.store(false, std::memory_order_relaxed);
        write_dump();
        g_events.clear();
        g_events.shrink_to_fit();
        g_resources.clear();
        g_nextResourceId = 0;
        g_lastCb0Captured = nullptr;
        g_cbUploads.clear();
        g_cbUploads.shrink_to_fit();
        g_cbArena.clear();
        g_cbArena.shrink_to_fit();
        // Consecutive-window capture (session 19): a command armed at CalcView
        // always opens on the SAME phase of the stereo pair, so a single
        // window can never see what the other pair half draws (the HUD hunt
        // hit exactly this). Roll straight into the next window.
        if (g_armCount.fetch_sub(1, std::memory_order_relaxed) - 1 > 0) {
            g_events.reserve(4096);
            g_recording.store(true, std::memory_order_relaxed);
            return;
        }
    }
    int pending = g_armMode.exchange(0, std::memory_order_relaxed);
    if (pending) {
        g_mode = pending;
        g_events.reserve(4096);
        // Fixed literals only - the Debug CRT's sprintf_s pops a MODAL dialog
        // that freezes the game on overflow (TESTING.md).
        sprintf_s(g_status, "recording (%s)...",
                  pending == 3 ? "cb" : (pending == 2 ? "full" : "lite"));
        g_recording.store(true, std::memory_order_relaxed);
    }
}

ScopedSuppress::ScopedSuppress() {
    ++t_suppress;
}

ScopedSuppress::~ScopedSuppress() {
    --t_suppress;
}

void draw_debug_ui() {
    ImGui::Text("Frame inspector: %s", g_status);
    if (g_watchArmed.load(std::memory_order_relaxed))
        ImGui::Text("cb watch: %u hits", g_watchHits.load(std::memory_order_relaxed));
    if (g_lastDumpPath[0]) ImGui::TextWrapped("last: %s", g_lastDumpPath);
    if (ImGui::Button("Dump next frame (lite)")) arm(1);
    ImGui::SameLine();
    if (ImGui::Button("Dump next frame (full)")) arm(2);
}

uint64_t draw_call_census() {
    return static_cast<uint64_t>(g_callCensus[CxDrawIndexed].load(std::memory_order_relaxed)) +
           g_callCensus[CxDraw].load(std::memory_order_relaxed) +
           g_callCensus[CxDrawIdxInst].load(std::memory_order_relaxed) +
           g_callCensus[CxDrawInst].load(std::memory_order_relaxed);
}

} // namespace bvr::frame_inspector
