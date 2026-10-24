#include "core/gfx/anim_shots.h"

#include "core/gfx/blit.h"
#include "core/util/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bvr::anim_shots {
namespace {

std::atomic<bool> g_on{false};
std::mutex g_labelMutex;
std::string g_label; // guarded

struct Frame {
    ID3D11Texture2D* tex = nullptr; // kWidth x g_h, R8G8B8A8_UNORM
    uint64_t ms = 0;
    std::string label;
};
Frame g_ring[kMaxFrames];
uint32_t g_h = 0;          // the frames' height
uint32_t g_count = 0;      // frames of the event being taken
uint64_t g_lastShotMs = 0;
uint64_t g_eventStartMs = 0;
std::string g_eventLabel;  // the label the event began with
// The event being written: frames [g_writeNext, g_writeCount) still to go, into g_dir.
bool g_writing = false;
uint32_t g_writeNext = 0, g_writeCount = 0;
std::wstring g_dir;
std::string g_index;       // the event's frame list (written last)
ID3D11Texture2D* g_staging = nullptr;
std::atomic<uint32_t> g_events{0};

std::wstring dumps_dir() {
    wchar_t d[MAX_PATH];
    swprintf_s(d, L"%s\\dumps", bvr::log::data_dir());
    CreateDirectoryW(d, nullptr);
    return d;
}

std::wstring safe(const std::string& s) {
    std::wstring o;
    for (char c : s) {
        if (o.size() >= 40) break;
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        o.push_back(ok ? static_cast<wchar_t>(c) : L'_');
    }
    return o;
}

// Only the newest kKeepEvents anim-* folders stay.
void prune() {
    const std::wstring root = dumps_dir();
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\anim-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.size() <= kKeepEvents) return;
    std::sort(names.begin(), names.end()); // anim-YYYYMMDD-HHMMSS-...: oldest first
    for (size_t i = 0; i + kKeepEvents < names.size(); ++i) {
        const std::wstring dir = root + L"\\" + names[i];
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        RemoveDirectoryW(dir.c_str());
    }
}

bool write_bmp(const std::wstring& path, const uint8_t* rgba, uint32_t pitch, uint32_t w, uint32_t h) {
    const uint32_t row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h); // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = rgba + static_cast<size_t>(h - 1 - y) * pitch;
        for (uint32_t x = 0; x < w; ++x) {
            line[x * 3 + 0] = s[x * 4 + 2];
            line[x * 3 + 1] = s[x * 4 + 1];
            line[x * 3 + 2] = s[x * 4 + 0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

void release_ring() {
    for (Frame& fr : g_ring) {
        if (fr.tex) fr.tex->Release();
        fr = Frame{};
    }
    if (g_staging) g_staging->Release();
    g_staging = nullptr;
    g_h = 0;
}

// The event just ended: hand its frames to the writer.
void finish_event() {
    if (g_count == 0) return;
    if (g_writing) {
        // The last one is still being written: this one is dropped rather than overwrite its frames.
        BVR_LOG("[animshots] an event ended while the last was still being written - dropped (%u frames)", g_count);
        g_count = 0;
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\anim-%04u%02u%02u-%02u%02u%02u-%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
               safe(g_eventLabel).c_str());
    g_dir = dumps_dir() + name;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_writing = true;
    g_writeNext = 0;
    g_writeCount = g_count;
    g_index.clear();
    g_count = 0;
}

} // namespace

void set_enabled(bool on) {
    if (g_on.exchange(on) == on) return;
    BVR_LOG("[animshots] %s - frames of every animation into dumps\\anim-* (the newest %u kept)", on ? "ON" : "off",
            kKeepEvents);
}
bool enabled() { return g_on.load(std::memory_order_relaxed); }

void set_label(const char* label) {
    std::lock_guard<std::mutex> lk(g_labelMutex);
    g_label = label ? label : "";
}

void on_eye_image(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage) {
    if (!ctx || !eyeImage) return;
    std::string label;
    {
        std::lock_guard<std::mutex> lk(g_labelMutex);
        label = g_label;
    }
    if (!g_on.load(std::memory_order_relaxed)) label.clear();
    // An event is one animation: it ends when the label empties or changes its first word (the hands' state).
    auto head = [](const std::string& s) { return s.substr(0, s.find(' ')); };
    if (g_count > 0 && (label.empty() || head(label) != head(g_eventLabel))) finish_event();
    if (label.empty()) return;
    const uint64_t now = GetTickCount64();
    if (g_count >= kMaxFrames || (g_count > 0 && now - g_lastShotMs < kIntervalMs)) return;
    // The ring at the eye's aspect.
    D3D11_TEXTURE2D_DESC ed{};
    eyeImage->GetDesc(&ed);
    const uint32_t h = ed.Width ? kWidth * ed.Height / ed.Width : kWidth;
    if (h != g_h) {
        if (g_writing || g_count) return; // never resize under frames in flight
        release_ring();
        g_h = h;
    }
    Frame& fr = g_ring[g_count];
    if (!fr.tex) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kWidth;
        td.Height = g_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &fr.tex);
        dev->Release();
        if (FAILED(hr)) {
            fr.tex = nullptr;
            return;
        }
    }
    if (!bvr::blit::stretch_copy(ctx, fr.tex, eyeImage)) return;
    if (g_count == 0) {
        g_eventStartMs = now;
        g_eventLabel = label;
    }
    fr.ms = now - g_eventStartMs;
    fr.label = label;
    g_lastShotMs = now;
    ++g_count;
}

void service(ID3D11DeviceContext* ctx) {
    if (!g_writing || !ctx) return;
    if (!g_staging) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kWidth;
        td.Height = g_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &g_staging);
        dev->Release();
        if (FAILED(hr)) {
            g_staging = nullptr;
            g_writing = false;
            return;
        }
    }
    // Two frames a present: a readback stalls on the GPU, and this is a diagnostic, not a hitch worth a frame.
    for (int k = 0; k < 2 && g_writeNext < g_writeCount; ++k, ++g_writeNext) {
        const Frame& fr = g_ring[g_writeNext];
        if (!fr.tex) continue;
        ctx->CopyResource(g_staging, fr.tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) continue;
        wchar_t file[32];
        swprintf_s(file, L"\\%02u.bmp", g_writeNext);
        write_bmp(g_dir + file, static_cast<const uint8_t*>(m.pData), m.RowPitch, kWidth, g_h);
        ctx->Unmap(g_staging, 0);
        char line[320];
        sprintf_s(line, "%02u  +%5llu ms  %s\n", g_writeNext, static_cast<unsigned long long>(fr.ms), fr.label.c_str());
        g_index += line;
    }
    if (g_writeNext >= g_writeCount) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (g_dir + L"\\frames.txt").c_str(), L"wb") == 0 && f) {
            fwrite(g_index.data(), 1, g_index.size(), f);
            fclose(f);
        }
        g_writing = false;
        const uint32_t n = g_events.fetch_add(1) + 1;
        BVR_LOG("[animshots] event %u written: %u frames (%ls)", n, g_writeCount, g_dir.c_str());
        prune();
    }
}

uint32_t events_written() { return g_events.load(std::memory_order_relaxed); }

void release() {
    g_count = 0;
    g_writing = false;
    release_ring();
}

} // namespace bvr::anim_shots
