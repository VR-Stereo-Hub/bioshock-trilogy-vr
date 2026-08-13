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

// ---- generalised native-table lookup (session 36) --------------------------

namespace {

// True when p starts a NUL-terminated run of printable ASCII whose length is in
// [minLen, maxLen). Used only to judge whether a pointer LOOKS like a native
// registration name, never to decide anything the game depends on.
bool looks_like_ascii_name(const uint8_t* p, size_t minLen = 4, size_t maxLen = 192) {
    for (size_t i = 0; i < maxLen; ++i) {
        if (!is_memory_valid(p + i, 1)) return false;
        const uint8_t c = p[i];
        if (c == 0) return i >= minLen;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return false;
}

// One entry of `shape` at `entry` is well formed: its name pointer resolves to
// a plausible name inside the image, and its impl pointer lands inside the
// image. Deliberately cheap - this is a neighbour sanity test, not a decision.
bool native_entry_well_formed(const ProcessImage& img, const NativeTableShape& shape,
                              const uint8_t* entry) {
    if (!is_memory_valid(entry, shape.entryStride)) return false;
    uint32_t nameAddr = 0;
    uint32_t implAddr = 0;
    memcpy(&nameAddr, entry, sizeof nameAddr);
    memcpy(&implAddr, entry + shape.implOffset, sizeof implAddr);
    const uint8_t* name = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(nameAddr));
    const uint8_t* impl = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(implAddr));
    if (name < img.base || name >= img.base + img.size) return false;
    if (impl < img.base || impl >= img.base + img.size) return false;
    if (shape.wideNames) return is_memory_valid(name, 2);
    return looks_like_ascii_name(name);
}

} // namespace

bool find_native_function_ex(const ProcessImage& img, const NativeTableShape& shape,
                             const char* className, const char* funcName,
                             NativeScanResult& out, bool verifyNeighbours) {
    out = {};
    if (shape.entryStride < 8 || shape.implOffset + 4 > shape.entryStride) return false;
    if (!className || !funcName || !shape.nameFormat) return false;

    char name[192];
    if (_snprintf_s(name, sizeof name, _TRUNCATE, shape.nameFormat, className, funcName) < 0)
        return false;
    const size_t len = strlen(name);
    if (len < 6) return false; // a short needle floods the match list; refuse it

    std::vector<const uint8_t*> strings =
        shape.wideNames ? find_wide_string(img, name) : find_ascii_string(img, name);
    out.stringMatches = strings.size();

    const size_t charBytes = shape.wideNames ? 2u : 1u;
    for (const uint8_t* s : strings) {
        // THE LINKER POOLS STRING LITERALS BY SUFFIX, so a substring match
        // proves nothing. Two distinct failures need two distinct guards:
        //
        //   PREFIX position - searching "AActorexecTrace" also matches inside
        //     "AActorexecTraceActors", whose table entry points at the SAME
        //     address. Only requiring OUR terminator at OUR end rejects that.
        //   SUFFIX position - "PlayerControllerexecCalcFOV" is stored inside
        //     "AXPlayerControllerexecCalcFOV" and SHARES its terminator, so the
        //     check above cannot save us. The reference step kills it instead:
        //     a pooled suffix has its own address, and only its own entry (if
        //     any) points there.
        const uint8_t* term = s + charBytes * len;
        if (!is_memory_valid(term, charBytes) || term[0] != 0 ||
            (shape.wideNames && term[1] != 0)) {
            ++out.terminatorRejects;
            continue;
        }

        std::vector<const uint8_t*> refs = find_references(img, s);
        out.tableRefs += refs.size();

        for (const uint8_t* entry : refs) {
            if (!is_memory_valid(entry, shape.entryStride)) continue;
            if (verifyNeighbours) {
                // ONE well-formed side is enough. Requiring both would silently
                // drop the table's first and last entries, which is a hole
                // rather than a safety property.
                const uint8_t* prev = entry - shape.entryStride;
                const bool prevOk =
                    prev >= img.base && native_entry_well_formed(img, shape, prev);
                const bool nextOk =
                    native_entry_well_formed(img, shape, entry + shape.entryStride);
                if (!prevOk && !nextOk) {
                    ++out.neighbourRejects;
                    continue;
                }
            }
            uint32_t implAddr = 0;
            memcpy(&implAddr, entry + shape.implOffset, sizeof implAddr);
            const uint8_t* fn =
                reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(implAddr));
            if (fn < img.base || fn >= img.base + img.size || !is_memory_valid(fn, 16)) {
                ++out.implRejects;
                continue;
            }
            out.tableEntry = entry;
            out.function = const_cast<uint8_t*>(fn);
            return true;
        }
    }
    return false;
}

bool native_table_bounds(const ProcessImage& img, const NativeTableShape& shape,
                         const uint8_t* seedEntry, NativeTableBounds& out) {
    out = {};
    if (!seedEntry || !native_entry_well_formed(img, shape, seedEntry)) return false;
    constexpr size_t kSanityCap = 65536; // no engine registers a million natives

    const uint8_t* lo = seedEntry;
    size_t back = 0;
    while (back < kSanityCap) {
        const uint8_t* p = lo - shape.entryStride;
        if (p < img.base || !native_entry_well_formed(img, shape, p)) break;
        lo = p;
        ++back;
    }
    const uint8_t* hi = seedEntry;
    size_t fwd = 0;
    while (fwd < kSanityCap) {
        const uint8_t* p = hi + shape.entryStride;
        if (!native_entry_well_formed(img, shape, p)) break;
        hi = p;
        ++fwd;
    }
    out.base = lo;
    out.count = back + fwd + 1;
    out.seedIndex = back;
    return out.count > 1;
}

bool find_native_in_table(const ProcessImage& img, const NativeTableShape& shape,
                          const NativeTableBounds& table, const char* className,
                          const char* funcName, NativeScanResult& out) {
    out = {};
    if (!table.base || table.count == 0 || shape.wideNames) return false; // ASCII path only
    if (!className || !funcName || !shape.nameFormat) return false;

    char name[192];
    if (_snprintf_s(name, sizeof name, _TRUNCATE, shape.nameFormat, className, funcName) < 0)
        return false;

    for (size_t i = 0; i < table.count; ++i) {
        const uint8_t* entry = table.base + i * shape.entryStride;
        if (!is_memory_valid(entry, shape.entryStride)) continue;
        uint32_t nameAddr = 0;
        memcpy(&nameAddr, entry, sizeof nameAddr);
        const uint8_t* n = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(nameAddr));
        if (!looks_like_ascii_name(n)) continue;
        if (strcmp(reinterpret_cast<const char*>(n), name) != 0) continue;
        ++out.stringMatches;
        uint32_t implAddr = 0;
        memcpy(&implAddr, entry + shape.implOffset, sizeof implAddr);
        const uint8_t* fn =
            reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(implAddr));
        if (fn < img.base || fn >= img.base + img.size || !is_memory_valid(fn, 16)) {
            ++out.implRejects;
            continue;
        }
        out.tableEntry = entry;
        out.function = const_cast<uint8_t*>(fn);
        return true;
    }
    return false;
}

} // namespace bvr::pattern_scan
