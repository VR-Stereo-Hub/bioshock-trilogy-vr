// CheatEngine-style narrowing over live process memory. The game keeps
// running while we sweep, so every raw access is SEH-guarded: a region can
// decommit between VirtualQuery and the read (other game threads allocate
// and free concurrently). SEH functions hold no C++ objects.

#include "core/debug/value_scan.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"

#include <windows.h>
#define PSAPI_VERSION 2 // K32* from kernel32; no psapi.lib (matches pattern_scan)
#include <psapi.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace bvr::value_scan {
namespace {

// Poked-original bits are stored XOR'd so the bookkeeping vector itself can
// never satisfy a later scan for the same value.
constexpr uint32_t kObfuscation = 0xB10C0DEDu;
// A float this common is useless for narrowing anyway; bail before the
// candidate vector itself becomes a memory hog.
constexpr size_t kMaxCandidates = 200000;

std::vector<uintptr_t> g_candidates;
struct Poked {
    uintptr_t addr;
    uint32_t obfuscatedBits;
};
std::vector<Poked> g_poked;

bool module_range_of(const void* anyAddrInModule, uintptr_t& lo, uintptr_t& hi) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(anyAddrInModule), &mod) ||
        !mod)
        return false;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info))) return false;
    lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    hi = lo + info.SizeOfImage;
    return true;
}

bool region_is_writable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) return false;
    switch (mbi.Protect & 0xFF) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// Calls fn(begin, end) for every committed writable private/image region in
// user space, excluding our own DLL (we would only find our own bookkeeping).
template <typename Fn>
void for_each_writable_region(Fn&& fn) {
    uintptr_t ourLo = 0, ourHi = 0;
    module_range_of(reinterpret_cast<const void*>(&scan_f32), ourLo, ourHi);

    uintptr_t p = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    // Full 4 GB walk - the game is LAA and allocates above 2 GB in long
    // sessions (same fix as patterns.cpp scan_for_vtable_object).
    while (p < 0xFFFE0000u &&
           VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end = base + mbi.RegionSize;
        if (end <= base) break; // address-space wrap guard
        if (region_is_writable(mbi) && !(base >= ourLo && base < ourHi)) fn(base, end);
        p = end;
    }
}

// SEH-guarded primitives (plain pointers only - no unwindable objects).
bool safe_read_u32(uintptr_t addr, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_write_u32(uintptr_t addr, uint32_t value) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Collect 4-aligned dwords equal to `pattern` in [base, end) into out; a
// region vanishing mid-sweep just ends that region's contribution.
void scan_region_for_u32(uintptr_t base, uintptr_t end, uint32_t pattern,
                         std::vector<uintptr_t>* out) {
    __try {
        for (uintptr_t p = (base + 3) & ~uintptr_t{3}; p + 4 <= end; p += 4) {
            if (*reinterpret_cast<const uint32_t*>(p) == pattern) {
                out->push_back(p);
                if (out->size() >= kMaxCandidates) return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Collect 4-aligned dwords P in [base, end) with lo <= P <= hi (pointer-range
// sweep for ptr_scan). Captures location AND value at sweep time - the heap
// churns while we walk it, so a later re-read can show garbage (seen live:
// freed-fill 0xFEFEFEFE where plausible pointers stood milliseconds earlier).
struct PtrHit {
    uintptr_t location;
    uint32_t value;
};

void scan_region_for_range(uintptr_t base, uintptr_t end, uint32_t lo, uint32_t hi,
                           std::vector<PtrHit>* out, size_t cap) {
    __try {
        for (uintptr_t p = (base + 3) & ~uintptr_t{3}; p + 4 <= end; p += 4) {
            uint32_t v = *reinterpret_cast<const uint32_t*>(p);
            if (v >= lo && v <= hi) {
                out->push_back({p, v});
                if (out->size() >= cap) return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// _snprintf_s + _TRUNCATE everywhere below: this tool formats arbitrary heap
// bytes as floats, and %g/%f of a garbage dword can be long. sprintf_s ASSERTS
// on overflow in debug (a modal that hangs the render thread); _TRUNCATE just
// truncates. Learned the hard way, 2026-07-24.
const char* describe(uintptr_t addr, char* buf, size_t bufSize) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod) {
        char name[MAX_PATH]{};
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        _snprintf_s(buf, bufSize, _TRUNCATE, "%s+0x%X", base,
                    static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        _snprintf_s(buf, bufSize, _TRUNCATE, "heap");
    }
    return buf;
}

float bits_to_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

uint32_t float_to_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

size_t scan_bits(uint32_t pattern, const char* what) {
    std::vector<uintptr_t> hits;
    for_each_writable_region([&](uintptr_t base, uintptr_t end) {
        if (hits.size() < kMaxCandidates) scan_region_for_u32(base, end, pattern, &hits);
    });

    // The hit vector grows while regions are swept, so it can catch its own
    // buffer (an address that happens to share the bit pattern). Drop those.
    uintptr_t bufLo = reinterpret_cast<uintptr_t>(hits.data());
    uintptr_t bufHi = bufLo + hits.capacity() * sizeof(uintptr_t);
    std::vector<uintptr_t> cleaned;
    cleaned.reserve(hits.size());
    for (uintptr_t h : hits)
        if (h < bufLo || h >= bufHi) cleaned.push_back(h);

    g_candidates = std::move(cleaned);
    g_poked.clear();
    BVR_LOG("[vscan] scan %s (0x%08X): %u candidates%s", what, pattern,
            static_cast<unsigned>(g_candidates.size()),
            g_candidates.size() >= kMaxCandidates ? " (CAP HIT - value too common)" : "");
    return g_candidates.size();
}

size_t rescan_bits(uint32_t pattern, const char* what) {
    std::vector<uintptr_t> kept;
    kept.reserve(g_candidates.size());
    for (uintptr_t addr : g_candidates) {
        uint32_t cur = 0;
        if (safe_read_u32(addr, &cur) && cur == pattern) kept.push_back(addr);
    }
    size_t before = g_candidates.size();
    g_candidates = std::move(kept);
    BVR_LOG("[vscan] rescan %s: %u -> %u candidates", what, static_cast<unsigned>(before),
            static_cast<unsigned>(g_candidates.size()));
    return g_candidates.size();
}

bool poke_bits(size_t idx, uint32_t bits, const char* what) {
    if (idx >= g_candidates.size()) {
        BVR_LOG("[vscan] poke %u: out of range (%u candidates)", static_cast<unsigned>(idx),
                static_cast<unsigned>(g_candidates.size()));
        return false;
    }
    uintptr_t addr = g_candidates[idx];
    uint32_t original = 0;
    if (!safe_read_u32(addr, &original)) {
        BVR_LOG("[vscan] poke %u @ 0x%08X: unreadable", static_cast<unsigned>(idx),
                static_cast<unsigned>(addr));
        return false;
    }
    if (!safe_write_u32(addr, bits)) {
        BVR_LOG("[vscan] poke %u @ 0x%08X: write failed", static_cast<unsigned>(idx),
                static_cast<unsigned>(addr));
        return false;
    }
    g_poked.push_back({addr, original ^ kObfuscation});
    BVR_LOG("[vscan] poke %u @ 0x%08X: 0x%08X -> %s", static_cast<unsigned>(idx),
            static_cast<unsigned>(addr), original, what);
    return true;
}

char g_valueDesc[48];

const char* describe_f32(float v) {
    _snprintf_s(g_valueDesc, _TRUNCATE, "f32 %.4g", v);
    return g_valueDesc;
}

const char* describe_u32(uint32_t v) {
    _snprintf_s(g_valueDesc, _TRUNCATE, "u32 %u", v);
    return g_valueDesc;
}

} // namespace

size_t scan_f32(float value) {
    return scan_bits(float_to_bits(value), describe_f32(value));
}

size_t scan_u32(uint32_t value) {
    return scan_bits(value, describe_u32(value));
}

size_t rescan_f32(float value) {
    return rescan_bits(float_to_bits(value), describe_f32(value));
}

size_t rescan_u32(uint32_t value) {
    return rescan_bits(value, describe_u32(value));
}

size_t count() {
    return g_candidates.size();
}

void list(size_t n) {
    size_t shown = g_candidates.size() < n ? g_candidates.size() : n;
    BVR_LOG("[vscan] list: %u of %u candidates", static_cast<unsigned>(shown),
            static_cast<unsigned>(g_candidates.size()));
    for (size_t i = 0; i < shown; ++i) {
        uintptr_t addr = g_candidates[i];
        uint32_t cur = 0;
        char value[96];
        if (safe_read_u32(addr, &cur))
            _snprintf_s(value, _TRUNCATE, "0x%08X (f32 %.4g / u32 %u)", cur, bits_to_float(cur),
                        cur);
        else
            strcpy_s(value, "<unreadable>");
        char where[MAX_PATH + 16];
        BVR_LOG("[vscan]   %u: 0x%08X (%s) = %s", static_cast<unsigned>(i),
                static_cast<unsigned>(addr), describe(addr, where, sizeof(where)), value);
    }
}

float read_at(size_t idx) {
    if (idx >= g_candidates.size()) {
        BVR_LOG("[vscan] read %u: out of range (%u candidates)", static_cast<unsigned>(idx),
                static_cast<unsigned>(g_candidates.size()));
        return NAN;
    }
    uint32_t cur = 0;
    if (!safe_read_u32(g_candidates[idx], &cur)) {
        BVR_LOG("[vscan] read %u @ 0x%08X: unreadable", static_cast<unsigned>(idx),
                static_cast<unsigned>(g_candidates[idx]));
        return NAN;
    }
    float f = bits_to_float(cur);
    char where[MAX_PATH + 16];
    BVR_LOG("[vscan] read %u @ 0x%08X (%s) = 0x%08X (f32 %.4g / u32 %u)",
            static_cast<unsigned>(idx), static_cast<unsigned>(g_candidates[idx]),
            describe(g_candidates[idx], where, sizeof(where)), cur, f, cur);
    return f;
}

bool poke(size_t idx, float value) {
    return poke_bits(idx, float_to_bits(value), describe_f32(value));
}

bool poke_u32(size_t idx, uint32_t value) {
    return poke_bits(idx, value, describe_u32(value));
}

namespace {

size_t poke_range_bits(size_t lo, size_t hi, uint32_t bits, const char* what) {
    if (lo > hi || lo >= g_candidates.size()) {
        BVR_LOG("[vscan] poke range %u-%u: out of range (%u candidates)",
                static_cast<unsigned>(lo), static_cast<unsigned>(hi),
                static_cast<unsigned>(g_candidates.size()));
        return 0;
    }
    if (hi >= g_candidates.size()) hi = g_candidates.size() - 1;
    size_t written = 0;
    for (size_t i = lo; i <= hi; ++i) {
        uintptr_t addr = g_candidates[i];
        uint32_t original = 0;
        if (!safe_read_u32(addr, &original)) continue;
        if (!safe_write_u32(addr, bits)) continue;
        g_poked.push_back({addr, original ^ kObfuscation});
        ++written;
    }
    BVR_LOG("[vscan] poke range %u-%u = %s: %u written", static_cast<unsigned>(lo),
            static_cast<unsigned>(hi), what, static_cast<unsigned>(written));
    return written;
}

} // namespace

size_t poke_range(size_t lo, size_t hi, float value) {
    return poke_range_bits(lo, hi, float_to_bits(value), describe_f32(value));
}

size_t poke_range_u32(size_t lo, size_t hi, uint32_t value) {
    return poke_range_bits(lo, hi, value, describe_u32(value));
}

size_t restore_all() {
    size_t restored = 0;
    // Restore newest-first so double-poked addresses land back on the oldest
    // (true) original.
    for (size_t i = g_poked.size(); i-- > 0;) {
        if (safe_write_u32(g_poked[i].addr, g_poked[i].obfuscatedBits ^ kObfuscation))
            ++restored;
    }
    BVR_LOG("[vscan] restore: %u of %u restored", static_cast<unsigned>(restored),
            static_cast<unsigned>(g_poked.size()));
    g_poked.clear();
    return restored;
}

namespace {

bool poke_addr_bits(uintptr_t addr, uint32_t bits, const char* what) {
    uint32_t original = 0;
    if (!pattern_scan::is_memory_valid(reinterpret_cast<void*>(addr), 4) ||
        !safe_read_u32(addr, &original)) {
        BVR_LOG("[vscan] pokeaddr 0x%08X: unreadable", static_cast<unsigned>(addr));
        return false;
    }
    if (!safe_write_u32(addr, bits)) {
        BVR_LOG("[vscan] pokeaddr 0x%08X: write failed", static_cast<unsigned>(addr));
        return false;
    }
    g_poked.push_back({addr, original ^ kObfuscation});
    char where[MAX_PATH + 16];
    BVR_LOG("[vscan] pokeaddr 0x%08X (%s): 0x%08X -> %s", static_cast<unsigned>(addr),
            describe(addr, where, sizeof(where)), original, what);
    return true;
}

} // namespace

bool poke_addr(uintptr_t addr, float value) {
    return poke_addr_bits(addr, float_to_bits(value), describe_f32(value));
}

bool poke_addr_u32(uintptr_t addr, uint32_t value) {
    return poke_addr_bits(addr, value, describe_u32(value));
}

void hexdump(uintptr_t addr, size_t len) {
    if (len > 1024) len = 1024;
    char where[MAX_PATH + 16];
    BVR_LOG("[vscan] hexdump 0x%08X (%s) len %u:", static_cast<unsigned>(addr),
            describe(addr, where, sizeof(where)), static_cast<unsigned>(len));
    for (size_t off = 0; off < len; off += 16) {
        uint8_t row[16];
        size_t rowLen = (len - off) < 16 ? (len - off) : 16;
        bool ok = true;
        for (size_t i = 0; i < rowLen; i += 4) {
            uint32_t dw = 0;
            if (!safe_read_u32(addr + off + i, &dw)) {
                ok = false;
                break;
            }
            memcpy(row + i, &dw, 4);
        }
        if (!ok) {
            BVR_LOG("[vscan]   +0x%03X: <unreadable>", static_cast<unsigned>(off));
            continue;
        }
        char hex[3 * 16 + 1]{};
        char ascii[16 + 1]{};
        for (size_t i = 0; i < rowLen; ++i) {
            sprintf_s(hex + i * 3, 4, "%02X ", row[i]);
            ascii[i] = (row[i] >= 0x20 && row[i] < 0x7F) ? static_cast<char>(row[i]) : '.';
        }
        BVR_LOG("[vscan]   +0x%03X: %-48s |%s|", static_cast<unsigned>(off), hex, ascii);
    }
}

void float_sweep(uintptr_t addr, size_t len, float lo, float hi) {
    if (len > 4096) len = 4096;
    char where[MAX_PATH + 16];
    BVR_LOG("[vscan] fsweep 0x%08X (%s) len %u for [%.2f, %.2f]:",
            static_cast<unsigned>(addr), describe(addr, where, sizeof(where)),
            static_cast<unsigned>(len), lo, hi);
    int found = 0;
    for (size_t off = 0; off + 4 <= len; off += 4) {
        uint32_t dw = 0;
        if (!safe_read_u32(addr + off, &dw)) continue;
        float f;
        memcpy(&f, &dw, 4);
        if (f >= lo && f <= hi) {
            BVR_LOG("[vscan]   +0x%03X = %.4f", static_cast<unsigned>(off), f);
            ++found;
        }
    }
    BVR_LOG("[vscan] fsweep: %d fields in range", found);
}

size_t ptr_scan(size_t idx, uint32_t maxDelta) {
    if (idx >= g_candidates.size()) {
        BVR_LOG("[vscan] ptrscan %u: out of range", static_cast<unsigned>(idx));
        return 0;
    }
    uintptr_t target = g_candidates[idx];
    uint32_t lo = static_cast<uint32_t>(target) - maxDelta;
    uint32_t hi = static_cast<uint32_t>(target);
    constexpr size_t kCap = 4096;
    constexpr size_t kLogCap = 64;

    // Main module first (readable spans incl. .data/.rdata - a singleton
    // pointer usually lives there), then the writable heap for 2-level chains.
    std::vector<PtrHit> hits;
    pattern_scan::ProcessImage img{};
    if (pattern_scan::capture_main_module(img)) {
        scan_region_for_range(reinterpret_cast<uintptr_t>(img.base),
                              reinterpret_cast<uintptr_t>(img.base) + img.size, lo, hi, &hits,
                              kCap);
    }
    size_t moduleHits = hits.size();
    for_each_writable_region([&](uintptr_t base, uintptr_t end) {
        // Skip the module range (already swept above with read-only spans too).
        if (img.base && base >= reinterpret_cast<uintptr_t>(img.base) &&
            base < reinterpret_cast<uintptr_t>(img.base) + img.size)
            return;
        if (hits.size() < kCap) scan_region_for_range(base, end, lo, hi, &hits, kCap);
    });

    // Ignore hits inside our own bookkeeping buffers.
    uintptr_t candLo = reinterpret_cast<uintptr_t>(g_candidates.data());
    uintptr_t candHi = candLo + g_candidates.capacity() * sizeof(uintptr_t);
    uintptr_t hitLo = reinterpret_cast<uintptr_t>(hits.data());
    uintptr_t hitHi = hitLo + hits.capacity() * sizeof(PtrHit);

    BVR_LOG("[vscan] ptrscan %u (target 0x%08X, delta <= 0x%X): %u hits (%u in module)%s",
            static_cast<unsigned>(idx), static_cast<unsigned>(target), maxDelta,
            static_cast<unsigned>(hits.size()), static_cast<unsigned>(moduleHits),
            hits.size() >= kCap ? " (CAP HIT)" : "");
    size_t logged = 0;
    for (const PtrHit& h : hits) {
        if ((h.location >= candLo && h.location < candHi) ||
            (h.location >= hitLo && h.location < hitHi))
            continue;
        if (logged++ >= kLogCap) break;
        char where[MAX_PATH + 16];
        BVR_LOG("[vscan]   ptr at 0x%08X (%s): base 0x%08X -> field +0x%X",
                static_cast<unsigned>(h.location), describe(h.location, where, sizeof(where)),
                h.value, static_cast<unsigned>(target - h.value));
    }
    return hits.size();
}

void log_string_scan(const char* text) {
    pattern_scan::ProcessImage img{};
    if (!pattern_scan::capture_main_module(img)) {
        BVR_LOG("[vscan] strscan: no main module");
        return;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(img.base);
    auto report = [&](const char* kind, const std::vector<const uint8_t*>& hits) {
        BVR_LOG("[vscan] strscan '%s' %s: %u hits", text, kind,
                static_cast<unsigned>(hits.size()));
        size_t shown = hits.size() < 16 ? hits.size() : 16;
        for (size_t i = 0; i < shown; ++i) {
            uintptr_t a = reinterpret_cast<uintptr_t>(hits[i]);
            size_t xrefs = pattern_scan::find_references(img, hits[i]).size();
            BVR_LOG("[vscan]   %s+0x%X: %u xref(s)", "BioshockHD.exe",
                    static_cast<unsigned>(a - base), static_cast<unsigned>(xrefs));
        }
    };
    report("ansi", pattern_scan::find_ascii_string(img, text));
    report("wide", pattern_scan::find_wide_string(img, text));
}

void log_module_bases() {
    pattern_scan::ProcessImage img{};
    if (pattern_scan::capture_main_module(img)) {
        BVR_LOG("[vscan] main module: base 0x%08X size 0x%X",
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(img.base)),
                static_cast<unsigned>(img.size));
    }
    uintptr_t lo = 0, hi = 0;
    if (module_range_of(reinterpret_cast<const void*>(&scan_f32), lo, hi)) {
        BVR_LOG("[vscan] mod dll: base 0x%08X size 0x%X", static_cast<unsigned>(lo),
                static_cast<unsigned>(hi - lo));
    }
}

} // namespace bvr::value_scan
