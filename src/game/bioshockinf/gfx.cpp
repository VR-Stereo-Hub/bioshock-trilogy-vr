#include "game/bioshockinf/gfx.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

namespace bvr::bsi::gfx {
namespace {

using bvr::pattern_scan::is_memory_valid;

// ---- the HUD object ---------------------------------------------------------

// PC -> myHUD. The offset is a class-layout fact (derive once per boot,
// refused-latch); the pointer is re-read per call - the HUD object is
// recreated across loads.
void* hud_object(bool verbose) {
    void* pc = camera::last_player_controller();
    if (!pc || !is_memory_valid(pc, 4)) {
        if (verbose) BVR_LOG("[bsi] gfx: no PlayerController yet");
        return nullptr;
    }
    static uint32_t s_off = 0;
    static bool s_refused = false;
    if (!s_off && !s_refused) {
        if (!reflect::find_property_offset(pc, "myHUD", "ObjectProperty", &s_off)) {
            s_refused = true;
            BVR_LOG("[bsi] gfx: myHUD did not derive on the PC's chain - the HUD "
                    "lane stays off this boot");
            return nullptr;
        }
        BVR_LOG("[bsi] gfx: derived myHUD at PC+0x%X", s_off);
    }
    if (!s_off || !is_memory_valid(static_cast<uint8_t*>(pc) + s_off, 4)) return nullptr;
    void* hud =
        *reinterpret_cast<void**>(static_cast<uint8_t*>(pc) + s_off);
    if (!hud || !is_memory_valid(hud, 4)) return nullptr;
    return hud;
}

// ---- the element machinery (rung 3) -----------------------------------------

// HideableHUDWidgetNames (TArray<FName>, stride 8) and NumReasonsToShowElement
// (TArray<int>, stride 4) on the HUD object. Offsets derive once; the arrays
// re-read per command.
bool element_arrays(void* hud, const uint8_t** namesData, int32_t* namesNum,
                    uint8_t** cntData, int32_t* cntNum) {
    static uint32_t s_namesOff = 0, s_cntOff = 0;
    static bool s_refused = false;
    if ((!s_namesOff || !s_cntOff) && !s_refused) {
        const bool a = reflect::find_property_offset(hud, "HideableHUDWidgetNames",
                                                     "ArrayProperty", &s_namesOff);
        const bool b = reflect::find_property_offset(hud, "NumReasonsToShowElement",
                                                     "ArrayProperty", &s_cntOff);
        if (!a || !b) {
            s_refused = true;
            BVR_LOG("[bsi] gfx: element arrays did not derive (names %s, reasons %s) "
                    "- walk the HUD with bsiprop 0x<hud> * and read the real names",
                    a ? "ok" : "MISSING", b ? "ok" : "MISSING");
            return false;
        }
        BVR_LOG("[bsi] gfx: derived HideableHUDWidgetNames at hud+0x%X, "
                "NumReasonsToShowElement at hud+0x%X",
                s_namesOff, s_cntOff);
    }
    if (!s_namesOff || !s_cntOff) return false;
    if (!is_memory_valid(static_cast<uint8_t*>(hud) + s_namesOff, 8) ||
        !is_memory_valid(static_cast<uint8_t*>(hud) + s_cntOff, 8))
        return false;
    *namesData = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(hud) +
                                                          s_namesOff);
    *namesNum = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(hud) +
                                                  s_namesOff + 4);
    *cntData = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(hud) + s_cntOff);
    *cntNum =
        *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(hud) + s_cntOff + 4);
    if (*namesNum < 0 || *namesNum > 256 || *cntNum < 0 || *cntNum > 256) return false;
    if (*namesNum > 0 &&
        !is_memory_valid(*namesData, static_cast<size_t>(*namesNum) * 8))
        return false;
    if (*cntNum > 0 && !is_memory_valid(*cntData, static_cast<size_t>(*cntNum) * 4))
        return false;
    return true;
}

void cmd_element(const char* a1, const char* a2) {
    void* hud = hud_object(true);
    if (!hud) {
        BVR_LOG("[bsi] gfx: element REFUSED - no HUD object");
        return;
    }
    const uint8_t* namesData = nullptr;
    uint8_t* cntData = nullptr;
    int32_t namesNum = 0, cntNum = 0;
    if (!element_arrays(hud, &namesData, &namesNum, &cntData, &cntNum)) return;
    if (!a1[0] || strcmp(a1, "list") == 0) {
        BVR_LOG("[bsi] gfx: %d hideable widgets, %d reason counters:", namesNum, cntNum);
        for (int32_t i = 0; i < namesNum; ++i) {
            const int32_t idx =
                *reinterpret_cast<const int32_t*>(namesData + static_cast<size_t>(i) * 8);
            char nm[patterns::kFNameTextBufMin] = {};
            patterns::fname_text(idx, nm, sizeof nm);
            const int32_t reasons =
                i < cntNum
                    ? *reinterpret_cast<const int32_t*>(cntData + static_cast<size_t>(i) * 4)
                    : -1;
            BVR_LOG("[bsi] gfx:   [%2d] %-40s reasons=%d", i, nm[0] ? nm : "?", reasons);
        }
        return;
    }
    // bsigfx element <Name> <delta>: adjust the row's reasons counter (the
    // direct lane - learn the SENSE from `list` while toggling a vigor: does
    // showing the crosshair raise or lower it?).
    int delta = 0;
    if (sscanf_s(a2, "%d", &delta) != 1 || delta == 0) {
        BVR_LOG("[bsi] gfx: element - usage: bsigfx element list | bsigfx element "
                "<Name> <+N|-N>");
        return;
    }
    for (int32_t i = 0; i < namesNum && i < cntNum; ++i) {
        const int32_t idx =
            *reinterpret_cast<const int32_t*>(namesData + static_cast<size_t>(i) * 8);
        char nm[patterns::kFNameTextBufMin] = {};
        patterns::fname_text(idx, nm, sizeof nm);
        if (_stricmp(nm, a1) != 0) continue;
        int32_t* cell = reinterpret_cast<int32_t*>(cntData + static_cast<size_t>(i) * 4);
        const int32_t before = *cell;
        *cell = before + delta;
        BVR_LOG("[bsi] gfx: element '%s' [%d] reasons %d -> %d", nm, i, before, *cell);
        return;
    }
    BVR_LOG("[bsi] gfx: element '%s' not found - run bsigfx element list", a1);
}

// ---- the movie lane (rung 4) ------------------------------------------------

// FString {Data, Num(len+1), Max} built over a static wide buffer - parms
// live only for the dispatch, and the command pump is single-threaded.
int32_t build_fstring(const char* text, void* parmsAt) {
    static wchar_t s_wbuf[128];
    int len = 0;
    for (; text[len] && len < 127; ++len) s_wbuf[len] = static_cast<wchar_t>(text[len]);
    s_wbuf[len] = 0;
    struct {
        wchar_t* data;
        int32_t num;
        int32_t max;
    } fs = {s_wbuf, len + 1, len + 1};
    memcpy(parmsAt, &fs, sizeof fs);
    return len;
}

void cmd_setb(const char* movieStr, const char* path, const char* valStr) {
    void* movie = reinterpret_cast<void*>(strtoul(movieStr, nullptr, 16));
    if (!movie) {
        BVR_LOG("[bsi] gfx: setb - usage: bsigfx setb <hexMovieObj> <path> 0|1 "
                "(the movie pointer comes from a bsifields walk of the HUD)");
        return;
    }
    static int32_t s_idx = -1;
    if (s_idx < 0) s_idx = reflect::find_function_index("SetVariableBool");
    if (s_idx < 0) {
        BVR_LOG("[bsi] gfx: setb REFUSED - 'SetVariableBool' not in GNames");
        return;
    }
    alignas(16) uint8_t parms[64] = {};
    build_fstring(path, parms);
    const uint32_t v = (valStr[0] == '1') ? 1u : 0u;
    memcpy(parms + 12, &v, sizeof v);
    const bool ok = reflect::call_on_object_by_index(movie, s_idx, parms);
    BVR_LOG("[bsi] gfx: SetVariableBool('%s', %u) on %p -> %s (acceptance is the "
            "SCREEN, not the return)",
            path, v, movie, ok ? "dispatched" : "FAILED");
}

void cmd_getb(const char* movieStr, const char* path) {
    void* movie = reinterpret_cast<void*>(strtoul(movieStr, nullptr, 16));
    if (!movie) {
        BVR_LOG("[bsi] gfx: getb - usage: bsigfx getb <hexMovieObj> <path>");
        return;
    }
    static int32_t s_idx = -1;
    if (s_idx < 0) s_idx = reflect::find_function_index("GetVariableBool");
    if (s_idx < 0) {
        BVR_LOG("[bsi] gfx: getb REFUSED - 'GetVariableBool' not in GNames");
        return;
    }
    alignas(16) uint8_t parms[64] = {};
    build_fstring(path, parms);
    const bool ok = reflect::call_on_object_by_index(movie, s_idx, parms);
    uint32_t r = 0;
    memcpy(&r, parms + 12, sizeof r); // UBOOL return past the FString
    BVR_LOG("[bsi] gfx: GetVariableBool('%s') on %p -> %s, value=%u (a 0 can also "
            "mean 'path not found' - cross-check with a known-true path)",
            path, movie, ok ? "dispatched" : "FAILED", r);
}

// ---- s54: the object-INSTANCE enumerator (the crosshair-kill hunt) ----------
// The s53 screen-model dead end: every GFx machinery NAME lives in the pool
// but no live walk reaches the HUD screen INSTANCE (myHUD is bare, the CDO
// loads null, GFxInteraction's walks found only sibling screens). This is the
// sanctioned next lane from the s53 handoff: sweep the process's committed
// private RW memory for aligned dwords equal to the target's FName INDEX at
// the derived UObject::Name offset, then validate each candidate with the
// UClass fixpoint (reflect::class_name_of - the s45b anti-fake gate). One
// index finds the whole family: UE3 instance names reuse the base FName index
// with a nonzero number ("XClikHUDCrosshair_3" = {8654, 4}), so the UClass,
// the CDO and every live instance all match. READ-ONLY, one-shot, game
// thread; costs a multi-second hitch on a big heap - never on a cadence (the
// fname_find rule), and flat lanes only while the user plays.

// SEH-guarded raw sweep (another engine thread can free pages mid-scan; a
// faulting region is abandoned, not fatal). No C++ objects inside (C2712).
int scan_region_seh(const uint8_t* base, size_t size, int32_t value,
                    const uint8_t** out, int cap, int have) {
    int n = have;
    __try {
        for (size_t o = 0; o + 8 <= size; o += 4) {
            if (*reinterpret_cast<const int32_t*>(base + o) == value) {
                if (n < cap) out[n] = base + o;
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void cmd_scan(const char* nameArg, const char* capArg) {
    const int32_t nameOff = reflect::uobject_name_offset();
    if (nameOff < 0) {
        BVR_LOG("[bsi] gfx: scan REFUSED - UObject::Name not derived yet (needs a "
                "latched PC; enter gameplay first)");
        return;
    }
    const int32_t idx = patterns::fname_find(nameArg);
    if (idx < 0) {
        BVR_LOG("[bsi] gfx: scan REFUSED - '%s' not in GNames (%d entries)", nameArg,
                patterns::fname_count());
        return;
    }
    int printCap = capArg[0] ? atoi(capArg) : 32;
    if (printCap < 1) printCap = 1;
    if (printCap > 128) printCap = 128;

    // Raw dword matches first (bounded), validation after - class_name_of does
    // FName reads and must not run inside the SEH sweep.
    constexpr int kRawCap = 1024;
    static const uint8_t* s_raw[kRawCap]; // game thread only
    const uint64_t t0 = GetTickCount64();
    int raw = 0;
    uint64_t bytes = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(0x10000);
         VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi &&
         p < reinterpret_cast<const uint8_t*>(0x7FFE0000);
         p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        const bool wantProtect =
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && wantProtect) {
            raw = scan_region_seh(static_cast<const uint8_t*>(mbi.BaseAddress),
                                  mbi.RegionSize, idx, s_raw, kRawCap, raw);
            bytes += mbi.RegionSize;
        }
    }
    const int kept = raw > kRawCap ? kRawCap : raw;
    int objects = 0;
    for (int i = 0; i < kept; ++i) {
        const uint8_t* cand = s_raw[i] - nameOff;
        char cls[64] = {};
        if (!reflect::class_name_of(cand, cls, sizeof cls) || !cls[0]) continue;
        ++objects;
        if (objects > printCap) continue;
        const int32_t number = *reinterpret_cast<const int32_t*>(s_raw[i] + 4);
        BVR_LOG("[bsi] gfx: scan hit %p  class=%-28s name=%s%s%d", cand, cls, nameArg,
                number ? "_" : " #", number ? number - 1 : 0);
    }
    BVR_LOG("[bsi] gfx: scan '%s' (fname %d): %llu MB swept in %llu ms, %d raw "
            "dword hits%s, %d validated UObjects%s - chase one with bsifields "
            "0x<hex> / bsiprop 0x<hex> *",
            nameArg, idx, static_cast<unsigned long long>(bytes >> 20),
            static_cast<unsigned long long>(GetTickCount64() - t0), raw,
            raw > kRawCap ? " (TRUNCATED at 1024 - narrow the name)" : "", objects,
            objects > printCap ? " (print-capped; bsigfx scan <Name> <cap>)" : "");
}

// s57: the scan machinery as a callable (see gfx.h). Same sweep as cmd_scan,
// but the caller wants only live INSTANCES: candidates whose class NAME
// equals the target name (the UClass reads class=Class, the CDO's own name
// is Default__X and never matches the scanned FName index).
int find_instances_impl(const char* className, void** out, int cap) {
    const int32_t nameOff = reflect::uobject_name_offset();
    if (nameOff < 0) return -1;
    const int32_t idx = patterns::fname_find(className);
    if (idx < 0) return -1;
    constexpr int kRawCap = 1024;
    static const uint8_t* s_raw[kRawCap]; // game thread only
    int raw = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(0x10000);
         VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi &&
         p < reinterpret_cast<const uint8_t*>(0x7FFE0000);
         p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        const bool wantProtect =
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && wantProtect)
            raw = scan_region_seh(static_cast<const uint8_t*>(mbi.BaseAddress),
                                  mbi.RegionSize, idx, s_raw, kRawCap, raw);
    }
    const int kept = raw > kRawCap ? kRawCap : raw;
    int n = 0;
    for (int i = 0; i < kept && n < cap; ++i) {
        uint8_t* cand = const_cast<uint8_t*>(s_raw[i]) - nameOff;
        char cls[64] = {};
        if (!reflect::class_name_of(cand, cls, sizeof cls) || !cls[0]) continue;
        if (strcmp(cls, className) != 0) continue;
        out[n++] = cand;
    }
    return n;
}

// bsigfx scanc <hexClass> [cap]: the CLASS-POINTER flavor - enumerate live
// INSTANCES of a UClass found by `scan` (candidate = dword address - 0x20,
// the execIsA-derived UObject::Class slot). Catches instances whose own name
// does not reuse the class's FName index (renamed widgets, sub-objects).
void cmd_scanc(const char* clsArg, const char* capArg) {
    const int32_t nameOff = reflect::uobject_name_offset();
    if (nameOff < 0) {
        BVR_LOG("[bsi] gfx: scanc REFUSED - UObject::Name not derived yet");
        return;
    }
    const uint8_t* cls =
        reinterpret_cast<const uint8_t*>(strtoul(clsArg, nullptr, 16));
    char clsName[64] = {};
    if (!cls || !reflect::class_name_of(cls, clsName, sizeof clsName) ||
        strcmp(clsName, "Class") != 0) {
        BVR_LOG("[bsi] gfx: scanc REFUSED - %p does not validate as a UClass "
                "(take the pointer from a `bsigfx scan <Name>` hit with "
                "class=Class)",
                cls);
        return;
    }
    int printCap = capArg[0] ? atoi(capArg) : 32;
    if (printCap < 1) printCap = 1;
    if (printCap > 128) printCap = 128;

    constexpr int kRawCap = 1024;
    static const uint8_t* s_raw[kRawCap]; // game thread only
    const uint64_t t0 = GetTickCount64();
    int raw = 0;
    uint64_t bytes = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(0x10000);
         VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi &&
         p < reinterpret_cast<const uint8_t*>(0x7FFE0000);
         p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        const bool wantProtect =
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && wantProtect) {
            raw = scan_region_seh(static_cast<const uint8_t*>(mbi.BaseAddress),
                                  mbi.RegionSize,
                                  static_cast<int32_t>(
                                      reinterpret_cast<intptr_t>(cls)),
                                  s_raw, kRawCap, raw);
            bytes += mbi.RegionSize;
        }
    }
    const int kept = raw > kRawCap ? kRawCap : raw;
    int objects = 0;
    for (int i = 0; i < kept; ++i) {
        const uint8_t* cand = s_raw[i] - 0x20; // UObject::Class slot
        char instCls[64] = {};
        if (!reflect::class_name_of(cand, instCls, sizeof instCls) || !instCls[0])
            continue;
        // The fixpoint validated the CLASS, not the candidate - any struct
        // holding the UClass pointer at some slot lands here (measured: 82
        // fakes in one tight stride-0x58 cluster vs 1 real HUD instance). A
        // real UObject also carries a resolvable Name; fakes read garbage.
        char nm[patterns::kFNameTextBufMin] = {};
        const int32_t nmIdx = reflect::object_name_index(cand);
        if (nmIdx <= 0 || !patterns::fname_text(nmIdx, nm, sizeof nm) || !nm[0])
            continue;
        ++objects;
        if (objects > printCap) continue;
        BVR_LOG("[bsi] gfx: scanc hit %p  instance of %-24s name=%s", cand, instCls,
                nm);
    }
    char ownName[patterns::kFNameTextBufMin] = {};
    const int32_t ownIdx = reflect::object_name_index(cls);
    if (ownIdx > 0) patterns::fname_text(ownIdx, ownName, sizeof ownName);
    BVR_LOG("[bsi] gfx: scanc %p ('%s' instances): %llu MB in %llu ms, %d raw%s, "
            "%d validated%s",
            cls, ownName[0] ? ownName : "?",
            static_cast<unsigned long long>(bytes >> 20),
            static_cast<unsigned long long>(GetTickCount64() - t0), raw,
            raw > kRawCap ? " (TRUNCATED)" : "", objects,
            objects > printCap ? " (print-capped)" : "");
}

} // namespace

int find_instances(const char* className, void** out, int cap) {
    return find_instances_impl(className, out, cap);
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsigfx") != 0) return false;
    char sub[32] = {}, a1[96] = {}, a2[96] = {}, a3[16] = {};
    const int n = sscanf_s(args ? args : "", "%31s %95s %95s %15s", sub,
                           static_cast<unsigned>(sizeof sub), a1,
                           static_cast<unsigned>(sizeof a1), a2,
                           static_cast<unsigned>(sizeof a2), a3,
                           static_cast<unsigned>(sizeof a3));
    if (n < 1) {
        BVR_LOG("[bsi] gfx: verbs - hud | prop <Name> | cmd <FlashCommand> | element "
                "list | element <Name> <+N|-N> | setb <hexMovie> <path> 0|1 | getb "
                "<hexMovie> <path> | scan <Name> [printCap] | scanc <hexClass> "
                "[printCap] (s54 instance sweeps - a visible hitch, flat lanes only)");
        return true;
    }
    if (strcmp(sub, "scan") == 0 && n >= 2) {
        cmd_scan(a1, n >= 3 ? a2 : "");
        return true;
    }
    if (strcmp(sub, "scanc") == 0 && n >= 2) {
        cmd_scanc(a1, n >= 3 ? a2 : "");
        return true;
    }
    if (strcmp(sub, "hud") == 0) {
        void* hud = hud_object(true);
        char cls[64] = {};
        if (hud) reflect::class_name_of(hud, cls, sizeof cls);
        BVR_LOG("[bsi] gfx: hud = %p (%s) - feed it to bsifields/bsiprop/bsiarray "
                "for the walks",
                hud, cls[0] ? cls : "?");
        return true;
    }
    if (strcmp(sub, "prop") == 0 && n >= 2) {
        // Follow one ObjectProperty off the HUD by name - the movie hunt's
        // pointer-chase without hand-parsing bsifields output.
        void* hud = hud_object(true);
        if (!hud) return true;
        uint32_t off = 0;
        if (!reflect::find_property_offset(hud, a1, "ObjectProperty", &off)) {
            BVR_LOG("[bsi] gfx: prop '%s' not on the HUD's chain (as ObjectProperty)",
                    a1);
            return true;
        }
        void* obj = nullptr;
        if (is_memory_valid(static_cast<uint8_t*>(hud) + off, 4))
            obj = *reinterpret_cast<void**>(static_cast<uint8_t*>(hud) + off);
        char cls[64] = {};
        if (obj) reflect::class_name_of(obj, cls, sizeof cls);
        BVR_LOG("[bsi] gfx: hud.%s (hud+0x%X) = %p (%s)", a1, off, obj,
                cls[0] ? cls : "?");
        return true;
    }
    if (strcmp(sub, "cmd") == 0 && n >= 2) {
        char line[128];
        snprintf(line, sizeof line, "FlashCommand %s", a1);
        BVR_LOG("[bsi] gfx: exec '%s' (acceptance is the screen)", line);
        reflect::exec_console(line);
        return true;
    }
    if (strcmp(sub, "element") == 0) {
        cmd_element(n >= 2 ? a1 : "", n >= 3 ? a2 : "");
        return true;
    }
    if (strcmp(sub, "setb") == 0 && n >= 4) {
        cmd_setb(a1, a2, a3);
        return true;
    }
    if (strcmp(sub, "getb") == 0 && n >= 3) {
        cmd_getb(a1, a2);
        return true;
    }
    BVR_LOG("[bsi] gfx: unknown verb '%s' - run bsigfx for usage", sub);
    return true;
}

} // namespace bvr::bsi::gfx
