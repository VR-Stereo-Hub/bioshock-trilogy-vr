#include "game/bioshock2r/game_ini.h"

#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::b2r::game_ini {
namespace {

// THE section that governs (Shared.ini). Measured, not assumed - see the
// header's truth table.
constexpr char kSharedSection[] = "[SharedOptions]";

// The PC driver section of Bioshock2SP.ini. FOUR others carry identical
// viewport key names in the shipped BS2 file - [XeDrv.XenonClient] (509),
// [PS3Drv.PS3Client] (541), [DurangoDrv.DurangoClient] (573) and
// [OrbisDrv.OrbisClient] (605), all verified present at those lines on the live
// install. BS1 has only three decoys; the PS3 one is BS2's addition, and it is
// also the marker that tells the two games' config files apart (BS1 ships
// GNMDrv, never PS3Drv). Never touch any of them. These keys do NOT drive the
// engine on BS2 - they are kept in sync, not relied on.
constexpr char kPcSection[] = "[WinDrv.WindowsClient]";
constexpr char kBs2Marker[] = "[PS3Drv.PS3Client]";

// Shared.ini is the governing file; Bioshock2SP.ini sits beside it.
wchar_t g_path[MAX_PATH] = {};   // Shared.ini
wchar_t g_spPath[MAX_PATH] = {}; // Bioshock2SP.ini ("" if absent)
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

// [start, end) byte span of `section`'s BODY, or false. Scoping every read and
// every write through this is the whole defence against the decoy sections.
bool section_span(const std::string& text, const char* section, size_t& start, size_t& end) {
    size_t at = text.find(section);
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

// Replace `key`'s value inside [start, end) in place. Returns false if the key
// is not present in that section - this code never ADDS keys. The shipped file
// has all of them, and inventing one is how a config stops loading.
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

// A candidate only wins if it EXISTS and actually carries the named section.
bool candidate_has(const wchar_t* p, const char* section) {
    if (!file_exists(p)) return false;
    std::string text;
    if (!read_all(p, text)) return false;
    return text.find(section) != std::string::npos;
}

void search() {
    g_searched = true;
    wchar_t roaming[MAX_PATH]{}, docs[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roaming);
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);

    // Verified on the live Steam install: %APPDATA%\BioshockHD\Bioshock2\ holds
    // Shared.ini (the pair that governs), Bioshock2SP.ini and User.ini; the
    // matching Documents\BioshockHD\BioShock2\ holds ONLY SaveGames, so there is
    // no competing candidate there today. The Documents entries cover layouts
    // other installs may use. The game directory is deliberately NOT searched:
    // under Program Files a write there is silently redirected to VirtualStore,
    // which reads back as success and changes nothing.
    const wchar_t* dirs[][2] = {
        {roaming, L"\\BioshockHD\\Bioshock2"},
        {roaming, L"\\Bioshock2"},
        {docs, L"\\BioshockHD\\Bioshock2"},
        {docs, L"\\BioShock 2 Remastered"},
    };
    for (const auto& d : dirs) {
        if (!d[0][0]) continue;
        wchar_t shared[MAX_PATH], sp[MAX_PATH];
        if (_snwprintf_s(shared, MAX_PATH, _TRUNCATE, L"%s%s\\Shared.ini", d[0], d[1]) < 0)
            continue;
        if (!candidate_has(shared, kSharedSection)) {
            BVR_LOG("[b2r] game ini: not here - %ls", shared);
            continue;
        }
        wcscpy_s(g_path, shared);
        // The secondary file is optional: it is kept in sync when present and
        // its absence must never block the write that actually matters.
        if (_snwprintf_s(sp, MAX_PATH, _TRUNCATE, L"%s%s\\Bioshock2SP.ini", d[0], d[1]) > 0 &&
            candidate_has(sp, kPcSection)) {
            wcscpy_s(g_spPath, sp);
        }
        bool marker = g_spPath[0] && candidate_has(g_spPath, kBs2Marker);
        BVR_LOG("[b2r] game ini: %ls (governs) | %ls%s", g_path,
                g_spPath[0] ? g_spPath : L"<no Bioshock2SP.ini - not needed>",
                g_spPath[0] && !marker
                    ? " (note: no [PS3Drv.PS3Client] - the BS2 section layout may have "
                      "changed; writes stay section-scoped regardless)"
                    : "");
        return;
    }
    BVR_LOG("[b2r] game ini: Shared.ini NOT FOUND - resolution cannot be set from the mod");
}

// Replace one key inside one section of one file, with the whole safety chain:
// section-scoped, never adds a key, one-time backup, temp + ReplaceFileW.
// `keys`/`values` are parallel arrays so a file is rewritten exactly once.
bool edit_section_keys(const wchar_t* file, const char* section, const char* const* keys,
                       const uint32_t* values, int n) {
    std::string text;
    if (!read_all(file, text)) {
        BVR_LOG("[b2r] game ini: read failed on %ls (err %lu)", file, GetLastError());
        return false;
    }
    size_t start = 0, end = 0;
    if (!section_span(text, section, start, end)) {
        BVR_LOG("[b2r] game ini: %s not found in %ls - refusing to guess", section, file);
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (!set_section_value(text, start, end, keys[i], values[i])) {
            BVR_LOG("[b2r] game ini: key %s missing from %s in %ls - nothing written", keys[i],
                    section, file);
            return false;
        }
    }

    // One-time backup, matching the existing .bvr-bak-* convention. Never
    // overwritten, so the first backup is always the user's original.
    wchar_t bak[MAX_PATH];
    if (_snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bvr-bak-res", file) > 0 && !file_exists(bak)) {
        if (CopyFileW(file, bak, TRUE))
            BVR_LOG("[b2r] game ini: original backed up to %ls", bak);
        else
            BVR_LOG("[b2r] game ini: backup FAILED (err %lu) - writing anyway", GetLastError());
    }

    // Temp file then ReplaceFileW, so an interrupted write cannot truncate the
    // user's config.
    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.bvr-tmp", file) < 0) return false;
    if (!write_all(tmp, text)) {
        BVR_LOG("[b2r] game ini: temp write failed (err %lu)", GetLastError());
        return false;
    }
    if (!ReplaceFileW(file, tmp, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp);
        BVR_LOG("[b2r] game ini: replace failed on %ls (err %lu) - config untouched", file, err);
        return false;
    }
    return true;
}

} // namespace

const wchar_t* path() {
    if (!g_searched) search();
    return g_path;
}

Viewport read_viewport() {
    Viewport v{};
    if (!path()[0]) return v;

    // The governing pair.
    std::string text;
    if (!read_all(g_path, text)) return v;
    size_t start = 0, end = 0;
    if (!section_span(text, kSharedSection, start, end)) return v;
    long sx = section_value(text, start, end, "ViewportX");
    long sy = section_value(text, start, end, "ViewportY");
    if (sx <= 0 || sy <= 0) return v;
    v.w = static_cast<uint32_t>(sx);
    v.h = static_cast<uint32_t>(sy);
    v.startupFullscreen = section_bool(text, start, end, "StartupFullscreen");
    v.valid = true;

    // The ignored-but-kept-in-sync pair, reported only.
    std::string sp;
    if (g_spPath[0] && read_all(g_spPath, sp)) {
        size_t s2 = 0, e2 = 0;
        if (section_span(sp, kPcSection, s2, e2)) {
            long wx = section_value(sp, s2, e2, "WindowedViewportX");
            long wy = section_value(sp, s2, e2, "WindowedViewportY");
            long fx = section_value(sp, s2, e2, "FullscreenViewportX");
            long fy = section_value(sp, s2, e2, "FullscreenViewportY");
            v.windowedW = wx > 0 ? static_cast<uint32_t>(wx) : 0;
            v.windowedH = wy > 0 ? static_cast<uint32_t>(wy) : 0;
            v.fullscreenW = fx > 0 ? static_cast<uint32_t>(fx) : 0;
            v.fullscreenH = fy > 0 ? static_cast<uint32_t>(fy) : 0;
        }
    }
    return v;
}

bool write_viewport(uint32_t w, uint32_t h) {
    if (w < 640 || h < 480 || w > 16384 || h > 16384) {
        BVR_LOG("[b2r] game ini: refusing %ux%u (outside 640x480..16384x16384)", w, h);
        return false;
    }
    if (!path()[0]) {
        BVR_LOG("[b2r] game ini: cannot write - no Shared.ini located");
        return false;
    }

    // 1. The pair that actually governs.
    const char* sharedKeys[] = {"ViewportX", "ViewportY"};
    const uint32_t sharedVals[] = {w, h};
    if (!edit_section_keys(g_path, kSharedSection, sharedKeys, sharedVals, 2)) return false;

    // 2. Keep the ignored pair in sync. Best effort: a failure here cannot undo
    // the write that matters, so it is logged and swallowed rather than
    // reported as failure. Both windowed and fullscreen, because UE2 keeps two
    // and reads whichever mode it starts in.
    if (g_spPath[0]) {
        const char* spKeys[] = {"WindowedViewportX", "WindowedViewportY",
                                "FullscreenViewportX", "FullscreenViewportY"};
        const uint32_t spVals[] = {w, h, w, h};
        if (!edit_section_keys(g_spPath, kPcSection, spKeys, spVals, 4))
            BVR_LOG("[b2r] game ini: Bioshock2SP.ini not synced - harmless (the engine ignores "
                    "those keys on BS2), but a config regeneration could revert the resolution");
    }

    // 3. READ BACK. A write that reports success and changes nothing is the
    // failure mode worth catching by hand - it is what UAC redirection looks
    // like. Note this proves the FILE took the value, never that the ENGINE
    // honours it: on BS2 the WinDrv keys verify perfectly and do nothing, which
    // is how they were caught. The only real acceptance is the backbuffer at
    // first Present after a relaunch.
    Viewport after = read_viewport();
    if (!after.valid || after.w != w || after.h != h) {
        BVR_LOG("[b2r] game ini: WROTE %ux%u but it reads back %ux%u - the write did not stick",
                w, h, after.w, after.h);
        return false;
    }
    // Deliberately does NOT spell out the startup line's exact wording: quoting
    // it here made this message match every `grep "first Present: backbuffer"`
    // and hid the real one.
    BVR_LOG("[b2r] game ini: viewport set to %ux%u in Shared.ini [SharedOptions] (verified) "
            "- takes effect on the NEXT launch; confirm against the startup backbuffer line",
            w, h);
    return true;
}

void log_status(uint32_t liveW, uint32_t liveH) {
    Viewport v = read_viewport();
    if (!v.valid) {
        BVR_LOG("[b2r] game ini: Shared.ini unreadable or missing %s", kSharedSection);
        return;
    }
    const bool agrees = liveW == 0 || (v.w == liveW && v.h == liveH);
    BVR_LOG("[b2r] game ini: Shared.ini %ux%u (GOVERNS) startupFullscreen=%s | "
            "Bioshock2SP.ini windowed %ux%u fullscreen %ux%u (ignored by the engine, synced) | "
            "live backbuffer %ux%u%s",
            v.w, v.h, v.startupFullscreen ? "True" : "False", v.windowedW, v.windowedH,
            v.fullscreenW, v.fullscreenH, liveW, liveH,
            agrees ? "" : " - NOT HONOURED YET (a change takes effect on the next launch; if it "
                          "persists past a relaunch, something else is winning - measure, do "
                          "not assume)");
}

} // namespace bvr::b2r::game_ini
