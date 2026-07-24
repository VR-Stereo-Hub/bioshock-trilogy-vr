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
std::atomic<uint32_t> g_callCensus[7]{};
enum CensusIdx { CxDrawIndexed, CxDraw, CxDrawIdxInst, CxDrawInst, CxSetRT, CxClearRtv, CxClearDsv };

constexpr size_t kMaxEvents = 20000;
constexpr size_t kMaxStack = 12;
constexpr size_t kCbFloats = 64;     // full mode: first 256 bytes of a CB

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
};

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

// Best-effort callstack: RtlCaptureStackBackTrace first, then a heuristic
// scan up the stack for exe .text return addresses preceded by a CALL.
void capture_stack(Event& ev, void* espHint) {
    void* frames[16] = {};
    USHORT n = RtlCaptureStackBackTrace(2, 16, frames, nullptr);
    size_t out = 0;
    for (USHORT i = 0; i < n && out < kMaxStack; ++i) {
        uint32_t rva = to_exe_rva(frames[i]);
        if (rva) ev.stack[out++] = rva;
    }
    if (out >= 4) return; // EBP walk was healthy enough

    // Heuristic ESP scan (FPO-resistant): any dword on the stack that points
    // into the exe image right after a plausible CALL encoding.
    const uintptr_t esp = reinterpret_cast<uintptr_t>(espHint);
    for (uintptr_t p = esp; p < esp + 2048 && out < kMaxStack; p += 4) {
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
        for (size_t i = 0; i < out; ++i)
            if (ev.stack[i] == rva) dup = true;
        if (!dup) ev.stack[out++] = rva;
    }
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
    }
    return "?";
}

bool is_draw(EventKind k) {
    return k == EventKind::DrawIndexed || k == EventKind::Draw ||
           k == EventKind::DrawIndexedInstanced || k == EventKind::DrawInstanced;
}

void write_dump() {
    wchar_t path[MAX_PATH];
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    swprintf_s(path, L"%s\\BioshockVR\\framedump_%02u%02u%02u.txt", base, st.wHour, st.wMinute,
               st.wSecond);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt") != 0 || !f) {
        sprintf_s(g_status, "dump open FAILED");
        return;
    }

    fprintf(f, "frame dump: %u events, mode=%s, exe base 0x%08X\n",
            static_cast<unsigned>(g_events.size()), g_mode == 2 ? "full" : "lite",
            static_cast<unsigned>(g_exeBase));
    fprintf(f, "lifetime call census: DrawIndexed=%u Draw=%u DrawIdxInst=%u DrawInst=%u "
               "SetRT=%u ClearRTV=%u ClearDSV=%u\n\n",
            g_callCensus[CxDrawIndexed].load(), g_callCensus[CxDraw].load(),
            g_callCensus[CxDrawIdxInst].load(), g_callCensus[CxDrawInst].load(),
            g_callCensus[CxSetRT].load(), g_callCensus[CxClearRtv].load(),
            g_callCensus[CxClearDsv].load());
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
    BVR_LOG("[gfx] frame inspector: %d/7 context slots hooked", hooked);
    return hooked == 7;
}

void arm(int mode) {
    g_armMode.store(mode == 2 ? 2 : 1, std::memory_order_relaxed);
    BVR_LOG("[gfx] frame dump armed (%s)", mode == 2 ? "full" : "lite");
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
    }
    int pending = g_armMode.exchange(0, std::memory_order_relaxed);
    if (pending) {
        g_mode = pending;
        g_events.reserve(4096);
        sprintf_s(g_status, "recording (%s)...", pending == 2 ? "full" : "lite");
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
    if (g_lastDumpPath[0]) ImGui::TextWrapped("last: %s", g_lastDumpPath);
    if (ImGui::Button("Dump next frame (lite)")) arm(1);
    ImGui::SameLine();
    if (ImGui::Button("Dump next frame (full)")) arm(2);
}

} // namespace bvr::frame_inspector
