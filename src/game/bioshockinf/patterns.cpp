#include "game/bioshockinf/patterns.h"

#include "core/util/log.h"
#include "core/util/module_id.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace bvr::bsi::patterns {
namespace {

bool g_rvaTrusted = false;
bool g_buildGateForcedOff = false;
Symbols g_symbols{};
const uint8_t* g_imageBase = nullptr;
size_t g_imageSize = 0;

// One hex line of up to 16 bytes, for the read-only probes.
void format_bytes(const uint8_t* bytes, size_t n, char* out, size_t outSize) {
    int written = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && written >= 0 && static_cast<size_t>(written) + 4 < outSize; ++i)
        written += _snprintf_s(out + written, outSize - written, _TRUNCATE, "%02X ", bytes[i]);
}

} // namespace

bool rva_trusted() {
    return g_rvaTrusted && !g_buildGateForcedOff;
}

void handle_buildgate_command(const char* args) {
    if (args && strncmp(args, "off", 3) == 0) {
        g_buildGateForcedOff = true;
        BVR_LOG("[bsi] buildgate FORCED OFF - address-dependent features now behave exactly as "
                "they would on an unrecognised build. 'buildgate on' restores.");
    } else if (args && strncmp(args, "on", 2) == 0) {
        g_buildGateForcedOff = false;
        BVR_LOG("[bsi] buildgate restored (host build itself is %s)",
                g_rvaTrusted ? "VERIFIED" : "STILL UNRECOGNISED");
    } else {
        BVR_LOG("[bsi] buildgate: host build %s, forced-off %s -> rva_trusted=%s",
                g_rvaTrusted ? "VERIFIED" : "NOT RECOGNISED",
                g_buildGateForcedOff ? "yes" : "no", rva_trusted() ? "yes" : "no");
    }
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    out = Symbols{};
    out.imageBase = image.base;
    out.imageSize = image.size;
    g_imageBase = image.base;
    g_imageSize = image.size;

    BVR_LOG("[bsi] main module: base %p size 0x%zX", image.base, image.size);
    // ASLR is off on this exe, so the base is expected to be the link-time one.
    // A different base is not fatal (every constant is an RVA) but it means an
    // assumption from I0 has changed, and that is worth one line.
    if (reinterpret_cast<uintptr_t>(image.base) != kExpectedImageBase) {
        BVR_LOG("[bsi] NOTE: image base is %p, not the expected 0x%08X - this build is rebased "
                "(ASLR on?). RVAs still apply; absolute addresses from ENGINE_NOTES do not.",
                image.base, kExpectedImageBase);
    }

    // Build gate FIRST, so everything logged after it is read in the right light.
    const bvr::module_id::Fingerprint host = bvr::module_id::host_exe();
    g_rvaTrusted =
        bvr::module_id::matches(host, kHostTimeDateStamp, kHostSizeOfImage, kHostFileBytes);
    out.buildVerified = g_rvaTrusted;
    if (g_rvaTrusted) {
        BVR_LOG("[bsi] host build VERIFIED (pe-timestamp 0x%08X, size-of-image 0x%08X, "
                "checksum 0x%08X vs expected 0x%08X) - hardcoded addresses trusted",
                host.timeDateStamp, host.sizeOfImage, host.checkSum, kHostCheckSum);
    } else {
        BVR_LOG("[bsi] ============================================================");
        BVR_LOG("[bsi] HOST BUILD NOT RECOGNISED - address-dependent features OFF");
        BVR_LOG("[bsi]   running:  pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                host.timeDateStamp, host.sizeOfImage,
                static_cast<unsigned long long>(host.fileBytes));
        BVR_LOG("[bsi]   expected: pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                kHostTimeDateStamp, kHostSizeOfImage,
                static_cast<unsigned long long>(kHostFileBytes));
        BVR_LOG("[bsi] Every offset this adapter hardcodes was derived from the 2022-05-11");
        BVR_LOG("[bsi] Steam build. Applying them to a different binary is how a mod corrupts");
        BVR_LOG("[bsi] a game rather than failing to work, so they stand down and the game");
        BVR_LOG("[bsi] stays fully playable. The log, overlay and command seam are unaffected.");
        BVR_LOG("[bsi] If you are on another storefront or a newer patch, please send this log");
        BVR_LOG("[bsi] - the three numbers above are what a per-build offset table needs.");
        BVR_LOG("[bsi] ============================================================");
    }

    // READ-ONLY probe of the I0 camera seam. No hook, no write, no scan - this
    // only turns "the disk image says 0x1E10C0 is a function" into "the LIVE
    // image has these bytes there", which is the cheapest possible head start
    // for DR-I2 and cannot destabilise anything. Nothing else consumes the RVA
    // this milestone.
    if (!rva_trusted()) {
        BVR_LOG("[bsi] camera-seam probe skipped - build gate closed");
    } else if (kGetPlayerViewPointRva + sizeof out.viewPointBytes > image.size) {
        BVR_LOG("[bsi] camera-seam probe skipped - RVA 0x%X is outside the image (size 0x%zX)",
                kGetPlayerViewPointRva, image.size);
    } else {
        const uint8_t* impl = image.base + kGetPlayerViewPointRva;
        const uint8_t* thunk = image.base + kGetPlayerViewPointThunkRva;
        if (!bvr::pattern_scan::is_memory_valid(impl, sizeof out.viewPointBytes)) {
            BVR_LOG("[bsi] camera-seam probe: RVA 0x%X not readable", kGetPlayerViewPointRva);
        } else {
            memcpy(out.viewPointBytes, impl, sizeof out.viewPointBytes);
            out.viewPointReadable = true;
            char implHex[64] = {};
            char thunkHex[64] = {};
            format_bytes(out.viewPointBytes, sizeof out.viewPointBytes, implHex, sizeof implHex);
            if (bvr::pattern_scan::is_memory_valid(thunk, 8)) {
                uint8_t thunkBytes[8];
                memcpy(thunkBytes, thunk, sizeof thunkBytes);
                format_bytes(thunkBytes, sizeof thunkBytes, thunkHex, sizeof thunkHex);
            }
            BVR_LOG("[bsi] camera-seam probe (READ-ONLY, nothing hooked): "
                    "GetPlayerViewPoint impl @ %p (RVA 0x%X) = %s| exec thunk @ %p = %s",
                    impl, kGetPlayerViewPointRva, implHex, thunk,
                    thunkHex[0] ? thunkHex : "(unreadable) ");
        }
    }

    g_symbols = out;
    return g_rvaTrusted;
}

const Symbols& symbols() {
    return g_symbols;
}

const uint8_t* image_base() {
    return g_imageBase;
}

size_t image_size() {
    return g_imageSize;
}

const uint8_t* rva_to_address(uint32_t rva, size_t needBytes) {
    if (!rva_trusted() || !g_imageBase) return nullptr;
    if (static_cast<size_t>(rva) + needBytes > g_imageSize) return nullptr;
    const uint8_t* p = g_imageBase + rva;
    if (!bvr::pattern_scan::is_memory_valid(p, needBytes)) return nullptr;
    return p;
}

// ---- GNames ---------------------------------------------------------------

namespace {

// Reads the { Data, Num, Max } triple. Returns false when the gate is closed,
// when resolve() has not run, or when the array is not yet populated - which is
// the NORMAL state until the engine's static initializers have run, and is why
// nothing here may be called from init().
bool gnames(const uint8_t* const** dataOut, int32_t* numOut) {
    const uint8_t* p = rva_to_address(kGNamesDataRva, 12);
    if (!p) return false;
    const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(p);
    int32_t num = *reinterpret_cast<const int32_t*>(p + 4);
    if (!data || num <= 0) return false;
    if (!bvr::pattern_scan::is_memory_valid(data, sizeof(void*))) return false;
    if (dataOut) *dataOut = data;
    if (numOut) *numOut = num;
    return true;
}

// The entry for `index`, validated against its OWN recorded index. UE3 packs
// (index << 1) | isWide at +0x8, so a slot that disagrees means the layout
// assumption broke or the slot is recycled garbage - refuse rather than read.
const uint8_t* fname_entry(int32_t index, bool* wideOut) {
    const uint8_t* const* data = nullptr;
    int32_t num = 0;
    if (!gnames(&data, &num)) return nullptr;
    if (index < 0 || index >= num) return nullptr;
    if (!bvr::pattern_scan::is_memory_valid(data + index, sizeof(void*))) return nullptr;
    const uint8_t* entry = data[index];
    if (!entry) return nullptr;
    if (!bvr::pattern_scan::is_memory_valid(entry, kFNameEntryTextOffset + 2)) return nullptr;
    const uint32_t packed =
        *reinterpret_cast<const uint32_t*>(entry + kFNameEntryIndexFlagsOffset);
    if (static_cast<int32_t>(packed >> 1) != index) return nullptr;
    if (wideOut) *wideOut = (packed & kFNameEntryWideBit) != 0;
    return entry;
}

char printable(uint32_t c) {
    return (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
}

} // namespace

int32_t fname_count() {
    int32_t num = 0;
    return gnames(nullptr, &num) ? num : 0;
}

int32_t fname_max() {
    const uint8_t* p = rva_to_address(kGNamesMaxRva, 4);
    return p ? *reinterpret_cast<const int32_t*>(p) : 0;
}

bool fname_is_wide(int32_t index, bool& outWide) {
    bool wide = false;
    if (!fname_entry(index, &wide)) return false;
    outWide = wide;
    return true;
}

bool fname_text(int32_t index, char* out, size_t outSize) {
    if (!out || outSize < kFNameTextBufMin) return false;
    out[0] = '\0';
    bool wide = false;
    const uint8_t* entry = fname_entry(index, &wide);
    if (!entry) return false;

    const uint8_t* text = entry + kFNameEntryTextOffset;
    const size_t charBytes = wide ? 2u : 1u;
    const size_t cap = (outSize - 1 < kFNameMaxChars) ? outSize - 1 : kFNameMaxChars;

    size_t n = 0;
    for (; n < cap; ++n) {
        const uint8_t* c = text + n * charBytes;
        if (!bvr::pattern_scan::is_memory_valid(c, charBytes)) {
            out[0] = '\0';
            return false;
        }
        const uint32_t ch = wide ? static_cast<uint32_t>(c[0] | (c[1] << 8)) : c[0];
        if (ch == 0) {
            out[n] = '\0';
            return true;
        }
        out[n] = printable(ch);
    }
    // No terminator inside the window: refuse rather than hand back a truncated
    // name, which would silently mis-key any per-name filter built on it.
    out[0] = '\0';
    return false;
}

int32_t fname_find(const char* text) {
    if (!text || !*text) return -1;
    const int32_t num = fname_count();
    if (num <= 0) return -1;
    char buf[256];
    for (int32_t i = 0; i < num; ++i) {
        if (!fname_text(i, buf, sizeof buf)) continue;
        if (_stricmp(buf, text) == 0) return i;
    }
    return -1;
}

} // namespace bvr::bsi::patterns
