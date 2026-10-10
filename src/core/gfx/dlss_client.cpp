// See dlss_client.h and dlss_ipc.h. Ported from the Dishonored VR mod's dlss_client.cpp
// (FSR removed).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include "core/gfx/dlss_client.h"
#include "core/gfx/dlss_ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace bvr::dlss {
using namespace bvr::dlss_ipc;

namespace {
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void put(char* why, size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list a; va_start(a, fmt);
    _vsnprintf_s(why, cap, _TRUNCATE, fmt, a);
    va_end(a);
}
double now_ms() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
const uint32_t kFrameMs = 1000;   // one eye image through the pipe; a healthy round trip is < 1 ms
} // namespace

struct Client::Eye {
    ID3D11Texture2D* tex[SlotCount] = {};
    HANDLE shared[SlotCount] = {};
    ID3D11ShaderResourceView* outSrv = nullptr;
    ID3D11Fence* in = nullptr;
    ID3D11Fence* out = nullptr;
    HANDLE inShared = nullptr, outShared = nullptr;
    uint64_t value = 0;
    uint32_t w = 0, h = 0, ow = 0, oh = 0;
    DXGI_FORMAT color = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0;
    bool ready = false;
};

void Client::say(int level, const char* fmt, ...) {
    if (!log_) return;
    char line[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    log_(level, line);
}

void Client::fail(const char* fmt, ...) {
    char line[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    say(2, "dlss: transport failed (%s) - the helper is stopped and every eye takes its normal path", line);
    stop();
}

bool Client::running() const { return running_; }

bool Client::send(const void* p, uint32_t n, uint32_t ms) {
    const BYTE* c = (const BYTE*)p;
    while (n) {
        OVERLAPPED o = {};
        o.hEvent = (HANDLE)event_;
        ResetEvent(o.hEvent);
        DWORD moved = 0;
        if (!WriteFile((HANDLE)pipe_, c, n, &moved, &o)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE w[2] = {(HANDLE)event_, (HANDLE)process_};
            if (WaitForMultipleObjects(2, w, FALSE, ms) != WAIT_OBJECT_0) {
                CancelIoEx((HANDLE)pipe_, &o); GetOverlappedResult((HANDLE)pipe_, &o, &moved, TRUE); return false;
            }
            if (!GetOverlappedResult((HANDLE)pipe_, &o, &moved, FALSE)) return false;
        }
        if (!moved) return false;
        c += moved; n -= moved;
    }
    return true;
}

bool Client::recv(void* p, uint32_t n, uint32_t ms) {
    BYTE* c = (BYTE*)p;
    while (n) {
        OVERLAPPED o = {};
        o.hEvent = (HANDLE)event_;
        ResetEvent(o.hEvent);
        DWORD moved = 0;
        if (!ReadFile((HANDLE)pipe_, c, n, &moved, &o)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE w[2] = {(HANDLE)event_, (HANDLE)process_};
            if (WaitForMultipleObjects(2, w, FALSE, ms) != WAIT_OBJECT_0) {
                CancelIoEx((HANDLE)pipe_, &o); GetOverlappedResult((HANDLE)pipe_, &o, &moved, TRUE); return false;
            }
            if (!GetOverlappedResult((HANDLE)pipe_, &o, &moved, FALSE)) return false;
        }
        if (!moved) return false;
        c += moved; n -= moved;
    }
    return true;
}

bool Client::start(ID3D11Device* dev, const StartParams& sp, char* why, size_t cap) {
    stop();
    log_ = sp.log;
    if (!dev || !sp.hostExe || !sp.dataDir) { put(why, cap, "no device or paths"); return false; }
    if (GetFileAttributesW(sp.hostExe) == INVALID_FILE_ATTRIBUTES) { put(why, cap, "helper %ls not found", sp.hostExe); return false; }
    ID3D11Device5* d5 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&d5)) || !d5) {
        put(why, cap, "ID3D11Device5 unavailable (shared fences need Windows 10 1703+)"); return false;
    }
    dev_ = dev; dev_->AddRef(); dev5_ = d5;
    // The adapter the helper must use: the one these textures live on.
    LUID luid = {};
    {
        IDXGIDevice* dx = nullptr; IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && dx && SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC d = {}; ad->GetDesc(&d); luid = d.AdapterLuid;
        }
        rel(ad); rel(dx);
    }
    if (!luid.LowPart && !luid.HighPart) { put(why, cap, "cannot read the device's adapter LUID"); stop(); return false; }

    // A job with KILL_ON_JOB_CLOSE: the helper cannot outlive the game, even a crashed one.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    job_ = job;
    const DWORD pid = GetCurrentProcessId();
    wchar_t cmd[1024], dir[MAX_PATH];
    _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" %lu --luid %lX %lX --data \"%s\"", sp.hostExe, (unsigned long)pid,
                 (unsigned long)luid.HighPart, (unsigned long)luid.LowPart, sp.dataDir);
    wcscpy_s(dir, sp.hostExe);
    if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(sp.hostExe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir, &si, &pi)) {
        put(why, cap, "CreateProcess(helper) failed %lu", GetLastError()); stop(); return false;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess))
        say(1, "dlss: the helper could not join the kill-on-close job (%lu); it is still stopped on exit", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    say(0, "dlss: helper started, pid %lu, adapter LUID %08lX:%08lX", (unsigned long)pi.dwProcessId,
        (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    wchar_t name[96];
    _snwprintf_s(name, _TRUNCATE, BVR_DLSS_PIPE_FMT, (unsigned long)pid);
    const double t0 = now_ms();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (now_ms() - t0 < sp.timeoutMs) {
        pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
            put(why, cap, "the helper exited (code %lu) before opening its pipe; see its dlss_host.log", code); stop(); return false;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 50); else Sleep(10);
    }
    if (pipe == INVALID_HANDLE_VALUE) { put(why, cap, "no pipe from the helper in %u ms", sp.timeoutMs); stop(); return false; }
    pipe_ = pipe;

    Hello hello = {kMagic, kVersion, pid, luid.LowPart, luid.HighPart};
    HelloAck ack = {};
    if (!send(&hello, sizeof(hello), sp.timeoutMs) || !recv(&ack, sizeof(ack), sp.timeoutMs)) {
        put(why, cap, "handshake with the helper failed (it may have exited; see its dlss_host.log)"); stop(); return false;
    }
    if (ack.magic != kMagic || ack.version != kVersion) { put(why, cap, "helper speaks IPC v%u, the mod v%u", ack.version, kVersion); stop(); return false; }
    strcpy_s(adapter_, ack.adapter);
    if (!ack.ok) {
        if (ack.driverMin[0])
            put(why, cap, "NGX refused on %s (0x%08X, DLSS available %d, needs driver %u.%u or newer)", ack.adapter,
                ack.ngxResult, ack.dlssAvailable, ack.driverMin[0], ack.driverMin[1]);
        else
            put(why, cap, "NGX refused on %s (0x%08X, DLSS available %d) - DLSS needs an NVIDIA RTX GPU", ack.adapter,
                ack.ngxResult, ack.dlssAvailable);
        stop(); return false;
    }
    running_ = true;
    say(0, "dlss: helper ready on %s in %.0f ms (NGX 0x%08X, DLSS available)", ack.adapter, now_ms() - t0, ack.ngxResult);
    return true;
}

void Client::release_eye(int e) {
    Eye* eye = eyes_[e];
    if (!eye) return;
    for (int s = 0; s < SlotCount; ++s) {
        rel(eye->tex[s]);
        if (eye->shared[s]) CloseHandle(eye->shared[s]);
    }
    rel(eye->outSrv); rel(eye->in); rel(eye->out);
    if (eye->inShared) CloseHandle(eye->inShared);
    if (eye->outShared) CloseHandle(eye->outShared);
    bytes_ -= eye->bytes;
    delete eye;
    eyes_[e] = nullptr;
}

bool Client::built(int e, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, DXGI_FORMAT c) const {
    const Eye* eye = (e == 0 || e == 1) ? eyes_[e] : nullptr;
    return running_ && eye && eye->ready && eye->w == w && eye->h == h && eye->ow == ow && eye->oh == oh && eye->color == c;
}

bool Client::build(int e, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, DXGI_FORMAT colorFormat, int preset,
                   char* why, size_t cap) {
    if (!running_) { put(why, cap, "helper not running"); return false; }
    if ((e != 0 && e != 1) || !w || !h || ow < w || oh < h) { put(why, cap, "invalid eye %d or size %ux%u -> %ux%u", e, w, h, ow, oh); return false; }
    release_eye(e);
    Eye* eye = new Eye();
    eyes_[e] = eye;
    eye->w = w; eye->h = h; eye->ow = ow; eye->oh = oh; eye->color = colorFormat;
    ID3D11Device5* d5 = (ID3D11Device5*)dev5_;

    // Every object is created here and duplicated INTO the helper (dlss_ipc.h).
    auto make = [&](int slot, uint32_t tw, uint32_t th, DXGI_FORMAT f, UINT bind) -> bool {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = dev_->CreateTexture2D(&d, nullptr, &eye->tex[slot]);
        if (FAILED(hr)) { put(why, cap, "shared texture %d (%ux%u fmt %d) 0x%08lX", slot, tw, th, (int)f, (unsigned long)hr); return false; }
        IDXGIResource1* r = nullptr;
        hr = eye->tex[slot]->QueryInterface(__uuidof(IDXGIResource1), (void**)&r);
        if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &eye->shared[slot]);
        rel(r);
        if (FAILED(hr)) { put(why, cap, "CreateSharedHandle(texture %d) 0x%08lX", slot, (unsigned long)hr); return false; }
        const uint64_t bpp = f == DXGI_FORMAT_R8_UNORM ? 1 : (f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT ||
                              f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM) ? 4 : 8;
        eye->bytes += (uint64_t)tw * th * bpp;
        bytes_ += (uint64_t)tw * th * bpp;
        return true;
    };
    // Output: R8G8B8A8 is the UAV format every D3D11/D3D12 GPU can store to, and it is in
    // the same typeless family as the R8G8B8A8 eye swapchains (CopyResource-compatible).
    const DXGI_FORMAT outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool ok = make(Color, w, h, colorFormat, D3D11_BIND_SHADER_RESOURCE) &&
              make(Output, ow, oh, outFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) &&
              make(Depth, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE) &&
              make(Motion, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE) &&
              make(Bias, w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    if (ok && FAILED(dev_->CreateShaderResourceView(eye->tex[Output], nullptr, &eye->outSrv))) { put(why, cap, "output SRV"); ok = false; }
    if (ok) {
        HRESULT hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&eye->in);
        if (SUCCEEDED(hr)) hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&eye->out);
        if (SUCCEEDED(hr)) hr = eye->in->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &eye->inShared);
        if (SUCCEEDED(hr)) hr = eye->out->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &eye->outShared);
        if (FAILED(hr)) { put(why, cap, "shared fence 0x%08lX", (unsigned long)hr); ok = false; }
    }
    Build b = {};
    b.eye = (uint32_t)e; b.width = w; b.height = h; b.outWidth = ow; b.outHeight = oh;
    b.colorFormat = (uint32_t)colorFormat; b.outputFormat = (uint32_t)outFmt;
    b.depthInverted = 1; b.autoExposure = 1; b.preset = preset;
    auto dup = [&](HANDLE h, uint64_t* out) -> bool {
        HANDLE there = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, (HANDLE)process_, &there, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            put(why, cap, "DuplicateHandle into the helper failed %lu", GetLastError()); return false;
        }
        *out = (uint64_t)(uintptr_t)there;
        return true;
    };
    for (int s = 0; ok && s < SlotCount; ++s) ok = dup(eye->shared[s], &b.tex[s]);
    ok = ok && dup(eye->inShared, &b.fenceIn) && dup(eye->outShared, &b.fenceOut);
    if (!ok) { release_eye(e); return false; }   // handles already inside the helper die with its eye rebuild or exit
    const uint8_t tag = TagBuild;
    BuildAck ack = {};
    if (!send(&tag, 1, 5000) || !send(&b, sizeof(b), 5000) || !recv(&ack, sizeof(ack), 20000)) {
        release_eye(e); fail("eye %d build: no answer from the helper", e); put(why, cap, "the helper stopped answering"); return false;
    }
    if (!ack.ok) { put(why, cap, "the helper refused eye %d: %s (NGX 0x%08X)", e, ack.detail, ack.ngxResult); release_eye(e); return false; }
    eye->ready = true;
    say(0, "dlss: eye %d ready, %ux%u -> %ux%u (%s), colour format %d, %.1f MiB shared", e, w, h, ow, oh,
        (w == ow && h == oh) ? "DLAA" : "DLSS Super Resolution", (int)colorFormat, (double)bytes_ / (1024.0 * 1024.0));
    return true;
}

bool Client::evaluate(ID3D11DeviceContext* ctx, int e, const EyeInputs& in, char* why, size_t cap) {
    if (!running_) { put(why, cap, "helper not running"); return false; }
    Eye* eye = (e == 0 || e == 1) ? eyes_[e] : nullptr;
    if (!eye || !eye->ready) { put(why, cap, "eye %d not built", e); return false; }
    if (!ctx || !in.color || !in.depth || !in.motion) { put(why, cap, "missing input"); return false; }
    ID3D11DeviceContext4* c4 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c4)) || !c4) { put(why, cap, "ID3D11DeviceContext4 unavailable"); return false; }
    const double t0 = now_ms();
    ctx->CopyResource(eye->tex[Color], in.color);
    ctx->CopyResource(eye->tex[Depth], in.depth);
    ctx->CopyResource(eye->tex[Motion], in.motion);
    if (in.bias) ctx->CopyResource(eye->tex[Bias], in.bias);
    const uint64_t v = ++eye->value;
    HRESULT hr = c4->Signal(eye->in, v);
    ctx->Flush();   // the helper's queue waits on this value: it must reach the GPU now
    if (FAILED(hr)) { c4->Release(); fail("Signal(in) 0x%08lX", (unsigned long)hr); put(why, cap, "signal failed"); return false; }
    Frame f = {};
    f.eye = (uint32_t)e; f.value = v; f.reset = (in.reset || v == 1) ? 1u : 0u;
    f.jitterX = in.jitterX; f.jitterY = in.jitterY;
    f.mvScaleX = (float)eye->w; f.mvScaleY = (float)eye->h;
    f.sharpness = in.sharpness;
    f.useBias = in.bias ? 1u : 0u;
    const uint8_t tag = TagFrame;
    FrameAck ack = {};
    if (!send(&tag, 1, kFrameMs) || !send(&f, sizeof(f), kFrameMs) || !recv(&ack, sizeof(ack), kFrameMs)) {
        c4->Release(); fail("eye %d frame %llu: the helper did not answer in %u ms", e, (unsigned long long)v, kFrameMs);
        put(why, cap, "the helper stopped answering"); return false;
    }
    if (ack.eye != (uint32_t)e || ack.value != v) {
        c4->Release(); fail("eye %d frame %llu: answer for eye %u frame %llu (desynchronised)", e, (unsigned long long)v, ack.eye,
                            (unsigned long long)ack.value);
        put(why, cap, "desynchronised"); return false;
    }
    // The ack means the helper has queued its Signal(out, v). The GPU wait orders the output
    // read after the evaluate without a CPU stall on the present thread.
    hr = c4->Wait(eye->out, v);
    c4->Release();
    if (FAILED(hr)) { fail("Wait(out) 0x%08lX", (unsigned long)hr); put(why, cap, "wait failed"); return false; }
    const double ms = now_ms() - t0;
    stats.cpuMsSum[e] += ms;
    if (ms > stats.cpuMsMax[e]) stats.cpuMsMax[e] = ms;
    if (ack.gpuMs >= 0) { stats.gpuMsSum[e] += ack.gpuMs; ++stats.gpuN[e]; }
    if (!ack.ok) { ++stats.refused[e]; put(why, cap, "NGX refused the evaluate (0x%08X)", ack.ngxResult); return false; }
    ++stats.frames[e];
    return true;
}

ID3D11ShaderResourceView* Client::output(int e) const {
    return (e == 0 || e == 1) && eyes_[e] ? eyes_[e]->outSrv : nullptr;
}
ID3D11Texture2D* Client::output_texture(int e) const {
    return (e == 0 || e == 1) && eyes_[e] ? eyes_[e]->tex[Output] : nullptr;
}

void Client::stop() {
    const bool was = running_ || process_;
    running_ = false;
    if (pipe_) {
        const uint8_t tag = TagQuit;
        DWORD put = 0;
        OVERLAPPED o = {}; o.hEvent = (HANDLE)event_;
        if (event_ && (WriteFile((HANDLE)pipe_, &tag, 1, &put, &o) || GetLastError() == ERROR_IO_PENDING))
            WaitForSingleObject((HANDLE)event_, 200);
        CancelIoEx((HANDLE)pipe_, nullptr);
        CloseHandle((HANDLE)pipe_);
        pipe_ = nullptr;
    }
    if (process_) {
        // The helper drains its queue and releases the features on Quit; a stuck one is killed.
        if (WaitForSingleObject((HANDLE)process_, 3000) != WAIT_OBJECT_0) {
            say(1, "dlss: the helper did not exit in 3 s - terminating it");
            TerminateProcess((HANDLE)process_, 0xD155);
            WaitForSingleObject((HANDLE)process_, 1000);
        }
        CloseHandle((HANDLE)process_);
        process_ = nullptr;
    }
    // Only now: the helper is gone, so no GPU work of its can still read these.
    release_eye(0); release_eye(1);
    if (job_) { CloseHandle((HANDLE)job_); job_ = nullptr; }
    if (event_) { CloseHandle((HANDLE)event_); event_ = nullptr; }
    if (dev5_) { ((ID3D11Device5*)dev5_)->Release(); dev5_ = nullptr; }
    rel(dev_);
    if (was) say(0, "dlss: helper stopped");
}

} // namespace bvr::dlss
