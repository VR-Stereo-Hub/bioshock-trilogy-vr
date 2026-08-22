#include "game/bioshock1r/game_ini.h"

#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::b1r::game_ini {
namespace {

// The PC driver section. The other three that carry identical key names are
// [XeDrv.XenonClient], [DurangoDrv.DurangoClient] and [OrbisDrv.OrbisClient] -
// console drivers, verified present in the shipped file at lines 467/499/531.
// Never touch those.
constexpr char kPcSection[] = "[WinDrv.WindowsClient]";
// The game's own options live here - same section BRVR writes Sensitivity into.
constexpr char kUserSection[] = "[ShockGame.ShockUserSettings]";

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
    // Flush before the rename: a crash between the two would otherwise leave a
    // zero-length temp file to be promoted over the real config.
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// [start, end) byte span of the PC driver section's BODY, or false.
// s63: the same span walk, for any named section. pc_section_span keeps its
// name and its caller; this is what the turn-sensitivity write needs, since the
// game's own slider lives in a different section from the viewport.
bool section_span(const std::string& text, const char* section, size_t& start,
                  size_t& end) {
    size_t at = text.find(section);
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

bool pc_section_span(const std::string& text, size_t& start, size_t& end) {
    size_t at = text.find(kPcSection);
    if (at == std::string::npos) return false;
    size_t bodyStart = text.find('\n', at);
    if (bodyStart == std::string::npos) return false;
    ++bodyStart;
    // The section ends at the next line that begins a new section.
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

// Value of `key` inside [start, end), or -1. Key must match at a line start.
long section_value(const std::string& text, size_t start, size_t end, const char* key) {
    const size_t keyLen = strlen(key);
    size_t p = start;
    while (p < end) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos || nl > end) ? end : nl;
        if (lineEnd - p > keyLen && text.compare(p, keyLen, key) == 0 && text[p + keyLen] == '=') {
            return strtol(text.c_str() + p + keyLen + 1, nullptr, 10);
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return -1;
}

// Replace `key`'s value inside [start, end) in place. Returns false if the key is
// not present in that section (this code never ADDS keys - the shipped file has
// all of them, and inventing one is how a config stops loading).
bool set_section_value(std::string& text, size_t start, size_t& end, const char* key,
                       uint32_t value) {
    const size_t keyLen = strlen(key);
    size_t p = start;
    while (p < end) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos || nl > end) ? end : nl;
        if (lineEnd - p > keyLen && text.compare(p, keyLen, key) == 0 && text[p + keyLen] == '=') {
            // Keep the line's terminator (CR and/or LF) exactly as it was.
            size_t valStart = p + keyLen + 1;
            size_t valEnd = valStart;
            while (valEnd < lineEnd && text[valEnd] != '\r' && text[valEnd] != '\n') ++valEnd;
            char buf[16];
            int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%u", value);
            if (n <= 0) return false;
            const size_t oldLen = valEnd - valStart;
            text.replace(valStart, oldLen, buf, static_cast<size_t>(n));
            end += static_cast<size_t>(n) - oldLen; // span shifts with the edit
            return true;
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return false;
}

bool section_bool(const std::string& text, size_t start, size_t end, const char* key) {
    const size_t keyLen = strlen(key);
    size_t p = start;
    while (p < end) {
        size_t nl = text.find('\n', p);
        size_t lineEnd = (nl == std::string::npos || nl > end) ? end : nl;
        if (lineEnd - p > keyLen && text.compare(p, keyLen, key) == 0 && text[p + keyLen] == '=')
            return _strnicmp(text.c_str() + p + keyLen + 1, "True", 4) == 0;
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return false;
}

// A candidate only wins if it EXISTS and actually contains the PC driver
// section: the same file name appears in layouts that belong to other titles.
bool candidate_ok(const wchar_t* p) {
    if (!file_exists(p)) return false;
    std::string text;
    if (!read_all(p, text)) return false;
    return text.find(kPcSection) != std::string::npos;
}

void search() {
    g_searched = true;
    wchar_t roaming[MAX_PATH]{}, docs[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roaming);
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);

    // Verified on the live Steam install: %APPDATA%\BioshockHD\Bioshock\.
    // The Documents variants cover layouts seen on other installs. The game
    // directory is deliberately NOT searched: under Program Files a write there
    // is silently redirected to VirtualStore, which reads back as success and
    // changes nothing.
    const wchar_t* tries[][2] = {
        {roaming, L"\\BioshockHD\\Bioshock\\Bioshock.ini"},
        {roaming, L"\\Bioshock\\Bioshock.ini"},
        {docs, L"\\BioshockHD\\Bioshock\\Bioshock.ini"},
        {docs, L"\\BioShock Remastered\\Bioshock.ini"},
    };
    for (const auto& t : tries) {
        if (!t[0][0]) continue;
        wchar_t p[MAX_PATH];
        if (_snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s%s", t[0], t[1]) < 0) continue;
        if (candidate_ok(p)) {
            wcscpy_s(g_path, p);
            BVR_LOG("[b1r] game ini: %ls", g_path);
            return;
        }
        BVR_LOG("[b1r] game ini: not here - %ls", p);
    }
    BVR_LOG("[b1r] game ini: NOT FOUND - resolution cannot be set from the mod");
}

} // namespace

const wchar_t* path() {
    if (!g_searched) search();
    return g_path;
}

Viewport read_viewport() {
    Viewport v{};
    if (!path()[0]) return v;
    std::string text;
    if (!read_all(g_path, text)) return v;
    size_t start = 0, end = 0;
    if (!pc_section_span(text, start, end)) return v;

    long wx = section_value(text, start, end, "WindowedViewportX");
    long wy = section_value(text, start, end, "WindowedViewportY");
    long fx = section_value(text, start, end, "FullscreenViewportX");
    long fy = section_value(text, start, end, "FullscreenViewportY");
    if (wx <= 0 || wy <= 0) return v;
    v.windowedW = static_cast<uint32_t>(wx);
    v.windowedH = static_cast<uint32_t>(wy);
    v.fullscreenW = fx > 0 ? static_cast<uint32_t>(fx) : 0;
    v.fullscreenH = fy > 0 ? static_cast<uint32_t>(fy) : 0;
    v.startupFullscreen = section_bool(text, start, end, "StartupFullscreen");
    v.valid = true;
    return v;
}

// s63 TURN SENSITIVITY, ported from BRVR. The game's own slider - "7" in the
// options menu is 70 here.
//
// WHY THE MOD TOUCHES IT AT ALL. The shipped default reads far too slow once
// the right stick is the only way to turn, and the mod now caps the axis at
// TurnAxisMax to kill the game's near-vertical response at full deflection. A
// low sensitivity underneath that cap leaves turning sluggish, so the cap and
// this are two halves of one fix - shipping either alone is worse than
// shipping neither.
//
// Negative means "leave whatever the player chose alone", which is the default.
bool write_turn_sensitivity(int value) {
    if (value < 0) return true; // explicitly not ours to touch
    if (value > 200) {
        BVR_LOG("[b1r] game ini: refusing Sensitivity %d (over 200)", value);
        return false;
    }
    if (!path()[0]) {
        BVR_LOG("[b1r] game ini: cannot write Sensitivity - no Bioshock.ini located");
        return false;
    }
    std::string text;
    if (!read_all(g_path, text)) {
        BVR_LOG("[b1r] game ini: read failed for Sensitivity (err %lu)", GetLastError());
        return false;
    }
    size_t start = 0, end = 0;
    if (!section_span(text, kUserSection, start, end)) {
        BVR_LOG("[b1r] game ini: %s not found - Sensitivity not written", kUserSection);
        return false;
    }
    const long before = section_value(text, start, end, "Sensitivity");
    if (before == value) {
        BVR_LOG("[b1r] game ini: Sensitivity already %d", value);
        return true;
    }
    if (!set_section_value(text, start, end, "Sensitivity", value)) return false;
    if (!write_all(g_path, text)) {
        BVR_LOG("[b1r] game ini: write failed for Sensitivity (err %lu)", GetLastError());
        return false;
    }
    BVR_LOG("[b1r] game ini: Sensitivity %ld -> %d (the in-game turn slider; pairs with "
            "TurnAxisMax)",
            before, value);
    return true;
}

bool write_viewport(uint32_t w, uint32_t h) {
    if (w < 640 || h < 480 || w > 16384 || h > 16384) {
        BVR_LOG("[b1r] game ini: refusing %ux%u (outside 640x480..16384x16384)", w, h);
        return false;
    }
    if (!path()[0]) {
        BVR_LOG("[b1r] game ini: cannot write - no Bioshock.ini located");
        return false;
    }

    std::string text;
    if (!read_all(g_path, text)) {
        BVR_LOG("[b1r] game ini: read failed (err %lu)", GetLastError());
        return false;
    }
    size_t start = 0, end = 0;
    if (!pc_section_span(text, start, end)) {
        BVR_LOG("[b1r] game ini: %s not found - refusing to guess", kPcSection);
        return false;
    }

    // Both pairs, deliberately: UE2 reads whichever mode it starts in.
    bool ok = set_section_value(text, start, end, "WindowedViewportX", w) &&
              set_section_value(text, start, end, "WindowedViewportY", h) &&
              set_section_value(text, start, end, "FullscreenViewportX", w) &&
              set_section_value(text, start, end, "FullscreenViewportY", h);
    if (!ok) {
        BVR_LOG("[b1r] game ini: a viewport key is missing from %s - nothing written",
                kPcSection);
        return false;
    }

    // One-time backup, matching the existing .bvr-bak-* convention. Never
    // overwritten, so the first backup is always the user's original.
    wchar_t bak[MAX_PATH];
    if (_snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bvr-bak-res", g_path) > 0 &&
        !file_exists(bak)) {
        if (CopyFileW(g_path, bak, TRUE))
            BVR_LOG("[b1r] game ini: original backed up to %ls", bak);
        else
            BVR_LOG("[b1r] game ini: backup FAILED (err %lu) - writing anyway", GetLastError());
    }

    // Temp file then ReplaceFileW, so an interrupted write cannot truncate the
    // user's config.
    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.bvr-tmp", g_path) < 0) return false;
    if (!write_all(tmp, text)) {
        BVR_LOG("[b1r] game ini: temp write failed (err %lu)", GetLastError());
        return false;
    }
    if (!ReplaceFileW(g_path, tmp, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp);
        BVR_LOG("[b1r] game ini: replace failed (err %lu) - config untouched", err);
        return false;
    }

    // READ BACK. A write that reports success and changes nothing is the failure
    // mode worth catching by hand - it is what UAC redirection looks like.
    Viewport after = read_viewport();
    if (!after.valid || after.windowedW != w || after.windowedH != h) {
        BVR_LOG("[b1r] game ini: WROTE %ux%u but it reads back %ux%u - the write did not stick",
                w, h, after.windowedW, after.windowedH);
        return false;
    }
    BVR_LOG("[b1r] game ini: viewport set to %ux%u (windowed AND fullscreen pairs, verified) "
            "- takes effect on the NEXT launch",
            w, h);
    return true;
}

void log_status(uint32_t liveW, uint32_t liveH) {
    Viewport v = read_viewport();
    if (!v.valid) {
        BVR_LOG("[b1r] game ini: unreadable or missing the PC driver section");
        return;
    }
    const bool agrees = liveW == 0 || (v.windowedW == liveW && v.windowedH == liveH);
    BVR_LOG("[b1r] game ini: windowed %ux%u fullscreen %ux%u startupFullscreen=%s | live "
            "backbuffer %ux%u%s",
            v.windowedW, v.windowedH, v.fullscreenW, v.fullscreenH,
            v.startupFullscreen ? "True" : "False", liveW, liveH,
            agrees ? "" : " - INI NOT HONOURED (the game rewrites this file at exit from its "
                          "own settings; change it with the game closed)");
}

} // namespace bvr::b1r::game_ini
