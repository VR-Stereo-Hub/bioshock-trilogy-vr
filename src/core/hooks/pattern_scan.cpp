// FName-chain scan technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT license), src/memory.rs.
// Same algorithm and constants, reimplemented in C++.
//
// Scan-at-load safety: everything the chain touches is static once the loader
// has run - the UTF-16 name string (.rdata), the code bytes (.text), and the
// *address* of the FName index global (a link-time constant, fixed up by the
// loader). We never read the global's runtime value, so it does not matter
// that FName registration has not executed yet. Writable .data is also swept;
// at load time it is still in its static initial state, so results are
// deterministic at load but a mid-game rescan could theoretically differ.

#include "core/hooks/pattern_scan.h"

#include <windows.h>
#define PSAPI_VERSION 2 // K32GetModuleInformation from kernel32; no psapi.lib
#include <psapi.h>

#include <cstdio>
#include <cstring>

namespace bvr::pattern_scan {
namespace {

bool region_is_readable(DWORD state, DWORD protect) {
    if (state != MEM_COMMIT) return false;
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// Calls fn(begin, end) for every readable committed span of the image, so a
// stray unreadable region can't fault the sweep. A needle never spans two
// regions in practice (it lives inside one PE section).
template <typename Fn>
void for_each_readable_span(const ProcessImage& img, Fn&& fn) {
    const uint8_t* p = img.base;
    const uint8_t* imageEnd = img.base + img.size;
    while (p < imageEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uint8_t* regionEnd = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd > imageEnd) regionEnd = imageEnd;
        if (regionEnd <= p) break;
        if (region_is_readable(mbi.State, mbi.Protect)) fn(p, regionEnd);
        p = regionEnd;
    }
}

void find_all(const ProcessImage& img, const uint8_t* needle, size_t n,
              std::vector<const uint8_t*>& out) {
    if (n == 0) return;
    for_each_readable_span(img, [&](const uint8_t* begin, const uint8_t* end) {
        const uint8_t* p = begin;
        while (p + n <= end) {
            const void* hit = memchr(p, needle[0], static_cast<size_t>(end - p) - (n - 1));
            if (!hit) break;
            const uint8_t* h = static_cast<const uint8_t*>(hit);
            if (memcmp(h, needle, n) == 0) out.push_back(h);
            p = h + 1;
        }
    });
}

} // namespace

bool capture_main_module(ProcessImage& out) {
    HMODULE module = GetModuleHandleA(nullptr);
    if (!module) return false;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
    out.base = static_cast<const uint8_t*>(info.lpBaseOfDll);
    out.size = info.SizeOfImage;
    return out.base != nullptr && out.size != 0;
}

bool is_memory_valid(const void* addr, size_t size) {
    if (reinterpret_cast<uintptr_t>(addr) < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (!region_is_readable(mbi.State, mbi.Protect)) return false;
    const uint8_t* regionEnd = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(addr) + size <= regionEnd;
}

std::vector<const uint8_t*> find_wide_string(const ProcessImage& img, const char* ascii) {
    std::vector<uint8_t> needle;
    for (const char* c = ascii; *c; ++c) {
        needle.push_back(static_cast<uint8_t>(*c));
        needle.push_back(0x00);
    }
    std::vector<const uint8_t*> out;
    find_all(img, needle.data(), needle.size(), out);
    return out;
}

std::vector<const uint8_t*> find_ascii_string(const ProcessImage& img, const char* ascii) {
    std::vector<const uint8_t*> out;
    find_all(img, reinterpret_cast<const uint8_t*>(ascii), strlen(ascii), out);
    return out;
}

std::vector<const uint8_t*> find_references(const ProcessImage& img, const void* target) {
    uint32_t value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
    uint8_t needle[4];
    memcpy(needle, &value, sizeof(needle));
    std::vector<const uint8_t*> out;
    find_all(img, needle, sizeof(needle), out);
    return out;
}

const uint8_t* find_fname_index_global(const ProcessImage& img, const uint8_t* stringXref,
                                       const uint8_t** ctorOut) {
    constexpr size_t kForwardWindow = 96;
    const uint8_t* imageEnd = img.base + img.size;
    const uint8_t* windowEnd = stringXref + kForwardWindow;
    if (windowEnd > imageEnd) windowEnd = imageEnd;

    const uint8_t* call = nullptr;
    for (const uint8_t* p = stringXref; p < windowEnd; ++p) {
        if (*p == 0xE8) { // CALL rel32 into the FName constructor
            call = p;
            break;
        }
    }
    if (!call) return nullptr;
    if (ctorOut) {
        // Resolve the rel32: target = end-of-instruction + displacement. This
        // is the FName constructor itself - previously computed and thrown
        // away; session 20 keeps it (GNames falls out of its disassembly).
        int32_t rel;
        memcpy(&rel, call + 1, sizeof rel);
        *ctorOut = call + 5 + rel;
    }

    for (const uint8_t* p = call + 5; p + 6 <= windowEnd; ++p) {
        if (p[0] == 0x89 && p[1] == 0x0D) { // MOV [imm32], ECX - index store
            uint32_t addr;
            memcpy(&addr, p + 2, sizeof(addr));
            return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
        }
    }
    return nullptr;
}

const uint8_t* find_function_start(const ProcessImage& img, const uint8_t* xref) {
    constexpr size_t kLookback = 512;
    static constexpr uint8_t kPrologue[] = {0xCC, 0xCC, 0xCC, 0x55, 0x8B, 0xEC};

    uintptr_t lowest = reinterpret_cast<uintptr_t>(img.base);
    uintptr_t start = reinterpret_cast<uintptr_t>(xref);
    if (start - lowest > kLookback) lowest = start - kLookback;

    const uint8_t* imageEnd = img.base + img.size;
    for (uintptr_t p = start; p >= lowest; --p) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(p);
        if (c + sizeof(kPrologue) <= imageEnd && memcmp(c, kPrologue, sizeof(kPrologue)) == 0)
            return c + 3; // the 55 (push ebp)
    }
    return nullptr;
}

bool find_event_function(const ProcessImage& img, const char* eventName, EventScanResult& out) {
    constexpr uintptr_t kInitSiteRadius = 200;
    out = {};

    std::vector<const uint8_t*> strings = find_wide_string(img, eventName);
    out.stringMatches = strings.size();

    for (const uint8_t* ws : strings) {
        std::vector<const uint8_t*> stringXrefs = find_references(img, ws);
        out.stringXrefs += stringXrefs.size();

        for (const uint8_t* sx : stringXrefs) {
            const uint8_t* ctor = nullptr;
            const uint8_t* global = find_fname_index_global(img, sx, &ctor);
            if (!global) continue;
            out.fnameIndexGlobal = global;
            out.fnameCtor = ctor;

            std::vector<const uint8_t*> globalXrefs = find_references(img, global);
            out.globalXrefs = globalXrefs.size();

            for (const uint8_t* gx : globalXrefs) {
                uintptr_t a = reinterpret_cast<uintptr_t>(gx);
                uintptr_t b = reinterpret_cast<uintptr_t>(sx);
                if ((a > b ? a - b : b - a) <= kInitSiteRadius) continue; // FName init site
                ++out.candidates;

                const uint8_t* fn = find_function_start(img, gx);
                if (fn && is_memory_valid(fn, 16)) {
                    out.function = const_cast<uint8_t*>(fn);
                    return true;
                }
            }
        }
    }
    return false;
}

bool find_native_function(const ProcessImage& img, const char* className,
                          const char* funcName, NativeScanResult& out) {
    out = {};

    char name[160];
    if (_snprintf_s(name, sizeof name, _TRUNCATE, "int%sexec%s", className, funcName) < 0)
        return false;

    std::vector<const uint8_t*> strings = find_wide_string(img, name);
    out.stringMatches = strings.size();

    for (const uint8_t* ws : strings) {
        // A shorter registration name can be a SUFFIX of a longer one (the
        // linker pools wide strings by suffix), so only an occurrence whose
        // own terminator follows the last character is the real entry.
        size_t len = strlen(name);
        const uint8_t* term = ws + 2 * len;
        if (!is_memory_valid(term, 2) || term[0] != 0 || term[1] != 0) continue;

        std::vector<const uint8_t*> refs = find_references(img, ws);
        out.tableRefs += refs.size();

        for (const uint8_t* entry : refs) {
            if (!is_memory_valid(entry, 12)) continue;
            uint32_t impl;
            memcpy(&impl, entry + 4, sizeof(impl));
            const uint8_t* fn = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(impl));
            if (fn < img.base || fn >= img.base + img.size) continue; // not code in this image
            if (!is_memory_valid(fn, 16)) continue;
            out.tableEntry = entry;
            out.function = const_cast<uint8_t*>(fn);
            return true;
        }
    }
    return false;
}

} // namespace bvr::pattern_scan
