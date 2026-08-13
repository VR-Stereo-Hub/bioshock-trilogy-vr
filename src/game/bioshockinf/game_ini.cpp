// BioShock Infinite's XUserOptions.ini resolution lane. See game_ini.h for
// the facts (DR-I8) and the write discipline. The safety chain is BS1/BS2's
// proven shape (adapter-local copy per the decoupling directive); Infinite's
// facts are simpler - one file, one governing section, no decoy sync.

#include "game/bioshockinf/game_ini.h"

#include "core/util/log.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::bsi::game_ini {
namespace {

constexpr char kSection[] = "[XCore.XUserOptionsManager]";

wchar_t g_path[MAX_PATH] = {};
bool g_searched = false;

bool file_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_all(const wchar_t* p, std::string& out) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
              got == out.size();
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool write_all(const wchar_t* p, const std::string& data) {
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) &&
              wrote == data.size();
    // Flush before the rename: a crash between the two would otherwise leave
    // a zero-length temp file to be promoted over the real config.
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// [start, end) byte span of the section BODY. Section-scoping every read and
// write is the house rule (BS2's decoy-section lesson), even though this file
// has only [IniVersion] after the target today.
bool section_span(const std::string& text, size_t& start, size_t& end) {
    size_t at = text.find(kSection);
    if (at == std::string::npos) return false;
    size_t bodyStart = text.find('\n', at);
    if (bodyStart == std::string::npos) return false;
    ++bodyStart;
    size_t p = bodyStart;
    while (p < text.size()) {
        if (text[p] == '[') break;
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) {
            p = text.size();
            break;
        }
        p = nl + 1;
    }
    start = bodyStart;
    end = p;
    return true;
}

long section_value(const std::string& text, size_t start, size_t end, const char* key) {
    const size_t keyLen = strlen(key);
    size_t p = start;
    while (p < end) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos || nl > end) ? end : nl;
        if (lineEnd - p > keyLen && text.compare(p, keyLen, key) == 0 &&
            text[p + keyLen] == '=') {
            return strtol(text.c_str() + p + keyLen + 1, nullptr, 10);
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return -1;
}

// Replace in place; false if the key is missing - this code NEVER adds keys
// (the shipped template has both, and an invented key is how a config stops
// loading).
bool set_section_value(std::string& text, size_t start, size_t& end, const char* key,
                       uint32_t value) {
    const size_t keyLen = strlen(key);
    size_t p = start;
    while (p < end) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos || nl > end) ? end : nl;
        if (lineEnd - p > keyLen && text.compare(p, keyLen, key) == 0 &&
            text[p + keyLen] == '=') {
            size_t valStart = p + keyLen + 1;
            size_t valEnd = valStart;
            while (valEnd < lineEnd && text[valEnd] != '\r' && text[valEnd] != '\n') ++valEnd;
            char buf[16];
            int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%u", value);
            if (n <= 0) return false;
            const size_t oldLen = valEnd - valStart;
            text.replace(valStart, oldLen, buf, static_cast<size_t>(n));
            end += static_cast<size_t>(n) - oldLen;
            return true;
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return false;
}

const wchar_t* path() {
    if (g_searched) return g_path;
    g_searched = true;
    wchar_t docs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        BVR_LOG("[bsi] game ini: Documents folder unresolvable - resolution lane dead");
        return g_path;
    }
    wchar_t candidate[MAX_PATH];
    if (_snwprintf_s(candidate, MAX_PATH, _TRUNCATE,
                     L"%s\\My Games\\BioShock Infinite\\XGame\\Config\\XUserOptions.ini",
                     docs) < 0)
        return g_path;
    if (!file_exists(candidate)) {
        BVR_LOG("[bsi] game ini: %ls not found (the game writes it on first exit)", candidate);
        return g_path;
    }
    std::string text;
    if (!read_all(candidate, text) || text.find(kSection) == std::string::npos) {
        BVR_LOG("[bsi] game ini: %ls exists but carries no %s - refusing it", candidate,
                kSection);
        return g_path;
    }
    wcscpy_s(g_path, candidate);
    BVR_LOG("[bsi] game ini: %ls (governs at boot; live lane is setres)", g_path);
    return g_path;
}

} // namespace

Resolution read_resolution() {
    Resolution r{};
    if (!path()[0]) return r;
    std::string text;
    if (!read_all(g_path, text)) return r;
    size_t start = 0, end = 0;
    if (!section_span(text, start, end)) return r;
    const long x = section_value(text, start, end, "ResolutionX");
    const long y = section_value(text, start, end, "ResolutionY");
    if (x <= 0 || y <= 0) return r;
    r.x = static_cast<int>(x);
    r.y = static_cast<int>(y);
    r.valid = true;
    return r;
}

bool write_resolution(int w, int h) {
    if (w < 640 || h < 480 || w > 16384 || h > 16384) {
        BVR_LOG("[bsi] game ini: refusing %dx%d (outside 640x480..16384x16384)", w, h);
        return false;
    }
    if (!path()[0]) {
        BVR_LOG("[bsi] game ini: cannot write - XUserOptions.ini not located");
        return false;
    }
    std::string text;
    if (!read_all(g_path, text)) {
        BVR_LOG("[bsi] game ini: read failed on %ls (err %lu)", g_path, GetLastError());
        return false;
    }
    size_t start = 0, end = 0;
    if (!section_span(text, start, end)) {
        BVR_LOG("[bsi] game ini: %s not found - refusing to guess", kSection);
        return false;
    }
    if (!set_section_value(text, start, end, "ResolutionX", static_cast<uint32_t>(w)) ||
        !set_section_value(text, start, end, "ResolutionY", static_cast<uint32_t>(h))) {
        BVR_LOG("[bsi] game ini: ResolutionX/Y missing from %s - nothing written", kSection);
        return false;
    }

    wchar_t bak[MAX_PATH];
    if (_snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bvr-bak-res", g_path) > 0 &&
        !file_exists(bak)) {
        if (CopyFileW(g_path, bak, TRUE))
            BVR_LOG("[bsi] game ini: original backed up to %ls", bak);
        else
            BVR_LOG("[bsi] game ini: backup FAILED (err %lu) - writing anyway", GetLastError());
    }

    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.bvr-tmp", g_path) < 0) return false;
    if (!write_all(tmp, text)) {
        BVR_LOG("[bsi] game ini: temp write failed (err %lu)", GetLastError());
        return false;
    }
    if (!ReplaceFileW(g_path, tmp, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                      nullptr)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp);
        BVR_LOG("[bsi] game ini: replace failed (err %lu) - config untouched", err);
        return false;
    }

    // READ BACK - proves the FILE took the value, never that the engine
    // honours it (DR-I8's whole lesson). Acceptance is the backbuffer at
    // first Present after a relaunch.
    const Resolution after = read_resolution();
    if (!after.valid || after.x != w || after.y != h) {
        BVR_LOG("[bsi] game ini: WROTE %dx%d but it reads back %dx%d - the write did not stick",
                w, h, after.x, after.y);
        return false;
    }
    BVR_LOG("[bsi] game ini: ResolutionX/Y set to %dx%d (file verified - NOT acceptance; "
            "confirm against the first-Present backbuffer after the next boot)",
            w, h);
    return true;
}

void log_status(unsigned liveW, unsigned liveH) {
    const Resolution r = read_resolution();
    if (!r.valid) {
        BVR_LOG("[bsi] game ini: XUserOptions.ini unreadable or missing %s", kSection);
        return;
    }
    const bool agrees =
        liveW == 0 || (static_cast<unsigned>(r.x) == liveW && static_cast<unsigned>(r.y) == liveH);
    BVR_LOG("[bsi] game ini: XUserOptions.ini %dx%d | live backbuffer %ux%u%s", r.x, r.y, liveW,
            liveH,
            agrees ? ""
                   : " - DIFFERS (a live setres does not persist itself; Apply writes the ini "
                     "so the next boot agrees)");
}

} // namespace bvr::bsi::game_ini
