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

} // namespace bvr::bsi::patterns
