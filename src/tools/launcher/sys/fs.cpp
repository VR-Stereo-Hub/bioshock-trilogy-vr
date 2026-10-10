// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/fs.cpp - see fs.h.
#include "sys/fs.h"
#include <shlobj.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "bcrypt.lib")

namespace bvr::launcher::fs {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_file(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool make_dir(const std::wstring& path, DWORD* err)
{
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS && is_dir(path)) return true;
    if (err) *err = GetLastError();
    return false;
}

bool make_dirs(const std::wstring& path, DWORD* err)
{
    if (is_dir(path)) return true;
    const int code=SHCreateDirectoryExW(nullptr,path.c_str(),nullptr);
    if (code==ERROR_SUCCESS || ((code==ERROR_ALREADY_EXISTS || code==ERROR_FILE_EXISTS) && is_dir(path))) return true;
    if(err)*err=(DWORD)code;
    return false;
}

std::wstring strip_trailing_slashes(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}
std::wstring join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = strip_trailing_slashes(a);
    if (r.back() != L'\\') r += L'\\';
    return r + (b.front() == L'\\' ? b.substr(1) : b);
}
std::wstring parent(const std::wstring& path)
{
    const std::wstring p = strip_trailing_slashes(path);
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
std::wstring filename(const std::wstring& path)
{
    const std::wstring p = strip_trailing_slashes(path);
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

bool read_file(const std::wstring& path, std::vector<uint8_t>* out, DWORD* err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { if (err) *err = GetLastError(); CloseHandle(h); return false; }
    if (sz.QuadPart < 0 || sz.QuadPart > 512ll * 1024 * 1024) {
        if (err) *err = ERROR_FILE_TOO_LARGE;
        CloseHandle(h); return false;
    }
    out->resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out->size()) {
        DWORD got = 0;
        const DWORD want = (DWORD)((out->size() - done) > (1u << 20) ? (1u << 20) : (out->size() - done));
        if (!ReadFile(h, out->data() + done, want, &got, nullptr)) { if (err) *err = GetLastError(); CloseHandle(h); return false; }
        if (got == 0) break;
        done += got;
    }
    CloseHandle(h);
    if (done != out->size()) { if (err) *err = ERROR_HANDLE_EOF; return false; }
    return true;
}

bool write_file_atomic(const std::wstring& path, const void* data, size_t size, DWORD* err)
{
    // CREATE_NEW never follows or overwrites a pre-existing temporary file.
    static volatile LONG sequence = 0;
    const std::wstring tmp = path + L".bvr-tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(InterlockedIncrement(&sequence));
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < size) {
        DWORD put = 0;
        const DWORD want = (DWORD)((size - done) > (1u << 20) ? (1u << 20) : (size - done));
        if (!WriteFile(h, p + done, want, &put, nullptr) || put == 0) {
            if (err) *err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            CloseHandle(h); DeleteFileW(tmp.c_str()); return false;
        }
        done += put;
    }
    if (!FlushFileBuffers(h)) {
        if (err) *err = GetLastError();
        CloseHandle(h); DeleteFileW(tmp.c_str()); return false;
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = GetLastError();
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool copy_file(const std::wstring& from, const std::wstring& to, DWORD* err)
{
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    if (err) *err = GetLastError();
    return false;
}
bool move_file(const std::wstring& from, const std::wstring& to, DWORD* err)
{
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return true;
    if (err) *err = GetLastError();
    return false;
}
bool delete_file(const std::wstring& path, DWORD* err)
{
    if (DeleteFileW(path.c_str())) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
    if (err) *err = e;
    return false;
}

bool probe_writable(const std::wstring& dir, DWORD* err)
{
    const std::wstring probe = join(dir, L".bvr-launcher-probe.tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
    CloseHandle(h);
    return true;
}

uint64_t file_size(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) return;
        ok = true;
    }
    ~Sha256()
    {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool update(const void* p, size_t n) { return BCryptHashData(hash, (PUCHAR)p, (ULONG)n, 0) == 0; }
    bool finish(std::string* hex)
    {
        UCHAR out[32];
        if (BCryptFinishHash(hash, out, sizeof(out), 0) != 0) return false;
        static const char* d = "0123456789abcdef";
        hex->clear();
        for (UCHAR b : out) { hex->push_back(d[b >> 4]); hex->push_back(d[b & 15]); }
        return true;
    }
};
}

bool sha256_bytes(const void* data, size_t size, std::string* hexLower)
{
    Sha256 s;
    return s.ok && s.update(data, size) && s.finish(hexLower);
}
bool sha256_file(const std::wstring& path, std::string* hexLower, DWORD* err)
{
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes, err)) return false;
    return sha256_bytes(bytes.data(), bytes.size(), hexLower);
}

bool contains_ascii(const void* data, size_t size, const char* needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || size < n) return false;
    const char* p = (const char*)data;
    for (size_t i = 0; i + n <= size; ++i)
        if (p[i] == needle[0] && memcmp(p + i, needle, n) == 0) return true;
    return false;
}

std::wstring known_folder(const KNOWNFOLDERID& id)
{
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
std::wstring env(const wchar_t* name)
{
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return (n == 0 || n >= 4096) ? L"" : std::wstring(buf, n);
}
std::wstring system_dir()
{
    wchar_t buf[MAX_PATH];
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"";
}
std::wstring temp_dir()
{
    wchar_t buf[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return n ? strip_trailing_slashes(std::wstring(buf, n)) : L".";
}
std::wstring module_path()
{
    wchar_t buf[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    return std::wstring(buf, n);
}
std::wstring timestamp_local()
{
    SYSTEMTIME t; GetLocalTime(&t);
    return wformat(L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string utc_now_iso()
{
    SYSTEMTIME t; GetSystemTime(&t);
    return format("%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::string narrow(const std::wstring& s)
{
    if (s.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring widen(const std::string& s)
{
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
std::string narrow_acp(const std::wstring& s)
{
    if (s.empty()) return "";
    const int n = WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring win_error_text(DWORD err)
{
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, nullptr);
    std::wstring r = n && msg ? std::wstring(msg, n) : L"";
    if (msg) LocalFree(msg);
    while (!r.empty() && (r.back() == L'\r' || r.back() == L'\n' || r.back() == L' ')) r.pop_back();
    return r.empty() ? wformat(L"Windows error %lu", (unsigned long)err) : wformat(L"%s (%lu)", r.c_str(), (unsigned long)err);
}
std::string format(const char* fmt, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    const int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    return std::string(buf, n < 0 ? sizeof(buf) - 1 : (size_t)n);
}
std::wstring wformat(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    const int n = _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return std::wstring(buf, n < 0 ? 2047 : (size_t)n);
}
bool iequals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

} // namespace bvr::launcher::fs
