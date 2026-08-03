#include "game/bioshock2r/aim.h"

#include "core/util/log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace bvr::b2r::aim {

std::atomic<bool> g_probeArmed{false};

namespace {

// ---- fire-watch -------------------------------------------------------------
// One row per fire-chain name: the Lane-A index global (null = none exists in
// this exe), the UFunction* learned when FindFunctionChecked resolves that
// index, and hit counters on both sides of the dispatch. Counters are plain
// uint32 - ProcessEvent is game-thread-only and so is every reader.
struct WatchRow {
    const char* name = nullptr;
    const uint8_t* indexGlobal = nullptr;
    void* fn = nullptr;    // learned UFunction*, null until seen
    uint32_t ffHits = 0;   // FindFunctionChecked resolutions of this index
    uint32_t peHits = 0;   // ProcessEvent dispatches of the learned pointer
};
WatchRow g_watch[patterns::FireNames::kMax];
int g_watchCount = 0;

// ---- census -----------------------------------------------------------------
// Dedup table of every dispatched function-name index while armed. Fixed and
// linear-probed; 0xFFFFFFFF marks an empty slot (index 0 = 'None' is a valid
// FName). Game thread only.
constexpr uint32_t kCensusSlots = 256;
struct CensusSlot {
    uint32_t index;
    uint32_t hits;
};
CensusSlot g_census[kCensusSlots];
uint32_t g_censusUsed = 0;
uint32_t g_censusOverflow = 0; // dispatches dropped because the table filled
uint32_t g_censusUnreadable = 0;

// UFunction name-field offset, self-derived (-1 = not yet). Derivation: scan
// the PlayerCalcView UFunction (known pointer, known index from
// *g_calcViewIndexGlobal) for {index, 0} - the 8-byte FName {index, number}.
int g_nameOffset = -1;
const uint8_t* g_calcViewIndexGlobal = nullptr; // patterns::Symbols.fnameIndexGlobal
uint32_t g_nameOffsetCandidates = 0;            // >1 = ambiguous, logged

uint64_t g_lastSummaryMs = 0;
uint32_t g_peSeen = 0; // total ProcessEvent dispatches while armed

void census_clear() {
    memset(g_census, 0xFF, sizeof(g_census));
    for (auto& s : g_census) s.hits = 0;
    g_censusUsed = 0;
    g_censusOverflow = 0;
    g_censusUnreadable = 0;
    g_peSeen = 0;
}

void census_add(uint32_t index) {
    uint32_t h = (index * 2654435761u) & (kCensusSlots - 1);
    for (uint32_t i = 0; i < kCensusSlots; ++i) {
        CensusSlot& s = g_census[(h + i) & (kCensusSlots - 1)];
        if (s.index == index) {
            ++s.hits;
            return;
        }
        if (s.index == 0xFFFFFFFF) {
            s.index = index;
            s.hits = 1;
            ++g_censusUsed;
            return;
        }
    }
    ++g_censusOverflow;
}

// Try to pin the UFunction name-field offset from a known (fn, index) pair.
// Requires the number half to be zero right behind the index - every
// fire-chain name is number-less. First match wins; every match is counted
// so an ambiguity names itself in the log (the census text output is the
// cross-check: a wrong offset prints garbage names).
void derive_name_offset(void* fn, uint32_t knownIndex) {
    if (!bvr::pattern_scan::is_memory_valid(fn, 0x100)) return;
    const uint8_t* p = static_cast<const uint8_t*>(fn);
    int first = -1;
    uint32_t matches = 0;
    for (int off = 0; off + 8 <= 0x100; off += 4) {
        uint32_t idx = 0, num = 0;
        memcpy(&idx, p + off, 4);
        memcpy(&num, p + off + 4, 4);
        if (idx == knownIndex && num == 0) {
            ++matches;
            if (first < 0) first = off;
        }
    }
    if (first >= 0) {
        g_nameOffset = first;
        g_nameOffsetCandidates = matches;
        BVR_LOG("[b2r] aim probe: UFunction name offset = +0x%X (%u candidate(s) in the "
                "first 0x100 bytes%s)",
                first, matches,
                matches > 1 ? " - AMBIGUOUS, census text is the cross-check" : "");
    }
}

bool token(const char* args, const char* word, const char** rest = nullptr) {
    size_t n = strlen(word);
    if (strncmp(args, word, n) != 0) return false;
    char t = args[n];
    if (t != '\0' && t != ' ' && t != '\t' && t != '\r' && t != '\n') return false;
    if (rest) {
        const char* r = args + n;
        while (*r == ' ' || *r == '\t') ++r;
        *rest = r;
    }
    return true;
}

void log_status() {
    BVR_LOG("[b2r] vraim status: probe %s, %d fire-name(s), name offset %s, census "
            "%u/%u used (overflow %u, unreadable %u), pe seen %u",
            g_probeArmed.load(std::memory_order_relaxed) ? "ARMED" : "off", g_watchCount,
            g_nameOffset >= 0 ? "derived" : "PENDING", g_censusUsed, kCensusSlots,
            g_censusOverflow, g_censusUnreadable, g_peSeen);
    for (int i = 0; i < g_watchCount; ++i) {
        const WatchRow& w = g_watch[i];
        BVR_LOG("[b2r]   %-20s global=%s fn=%p ff=%u pe=%u", w.name,
                w.indexGlobal ? "yes" : "no", w.fn, w.ffHits, w.peHits);
    }
}

void census_dump() {
    BVR_LOG("[b2r] aim probe census: %u distinct name(s), overflow %u, unreadable %u, "
            "pe seen %u, name offset %s",
            g_censusUsed, g_censusOverflow, g_censusUnreadable, g_peSeen,
            g_nameOffset >= 0 ? "derived" : "PENDING (dump is empty until the probe "
                                            "sees PlayerCalcView resolve once)");
    for (uint32_t i = 0; i < kCensusSlots; ++i) {
        const CensusSlot& s = g_census[i];
        if (s.index == 0xFFFFFFFF) continue;
        char text[48];
        if (!patterns::fname_text(s.index, text, sizeof(text)))
            snprintf(text, sizeof(text), "<unreadable>");
        BVR_LOG("[b2r]   census idx=%-6u hits=%-6u %s", s.index, s.hits, text);
    }
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols) {
    patterns::FireNames names{};
    patterns::resolve_fire_names(image, names);
    g_watchCount = names.count;
    for (int i = 0; i < names.count; ++i) {
        g_watch[i].name = names.name[i];
        g_watch[i].indexGlobal = names.indexGlobal[i];
    }
    g_calcViewIndexGlobal = symbols.fnameIndexGlobal;
    census_clear();

    // GNames smoke test - index 0 is 'None' on every UE2 build; the
    // PlayerCalcView index (live in the cached global) must read back as
    // itself. Failure is loud and non-fatal: the census degrades to raw
    // indexes, the fire-watch is unaffected.
    char t0[48], t1[48];
    bool ok0 = patterns::fname_text(0, t0, sizeof(t0));
    uint32_t cvIdx = g_calcViewIndexGlobal
                         ? *reinterpret_cast<const uint32_t*>(g_calcViewIndexGlobal)
                         : 0;
    bool ok1 = cvIdx ? patterns::fname_text(cvIdx, t1, sizeof(t1)) : false;
    BVR_LOG("[b2r] aim probe init: %d fire-name(s); GNames[0]=%s GNames[%u]=%s",
            g_watchCount, ok0 ? t0 : "<FAIL>", cvIdx, ok1 ? t1 : "<FAIL>");
    if (ok0 && strcmp(t0, "None") != 0)
        BVR_LOG("[b2r] aim probe: GNames[0] is not 'None' - RVA suspect, census text "
                "untrusted");
}

void probe_findfunc(uint32_t nameIndex, uint32_t nameNumber, void* fn) {
    if (!fn) return;
    for (int i = 0; i < g_watchCount; ++i) {
        WatchRow& w = g_watch[i];
        if (!w.indexGlobal) continue;
        if (nameIndex == *reinterpret_cast<const uint32_t*>(w.indexGlobal)) {
            ++w.ffHits;
            if (w.fn != fn) {
                w.fn = fn;
                BVR_LOG("[b2r] aim probe: %s UFunction learned: %p", w.name, fn);
            }
        }
    }
    // Self-derive the UFunction name offset off the one (fn, index) pair we
    // know for certain - PlayerCalcView resolves hundreds of times a second,
    // so this lands within a frame of arming.
    if (g_nameOffset < 0 && nameNumber == 0 && g_calcViewIndexGlobal &&
        nameIndex == *reinterpret_cast<const uint32_t*>(g_calcViewIndexGlobal))
        derive_name_offset(fn, nameIndex);
}

void probe_process_event(void* fn) {
    ++g_peSeen;
    if (!fn) return;
    for (int i = 0; i < g_watchCount; ++i) {
        WatchRow& w = g_watch[i];
        if (w.fn && w.fn == fn) ++w.peHits;
    }
    if (g_nameOffset >= 0) {
        const uint8_t* p = static_cast<const uint8_t*>(fn) + g_nameOffset;
        if (bvr::pattern_scan::is_memory_valid(p, 4)) {
            uint32_t idx = 0;
            memcpy(&idx, p, 4);
            census_add(idx);
        } else {
            ++g_censusUnreadable;
        }
    }
}

void poll_tick(uint64_t now) {
    if (!g_probeArmed.load(std::memory_order_relaxed)) return;
    if (now - g_lastSummaryMs < 1000) return;
    g_lastSummaryMs = now;

    static uint32_t s_lastPeSeen = 0;
    static uint32_t s_lastFf[patterns::FireNames::kMax] = {};
    static uint32_t s_lastPe[patterns::FireNames::kMax] = {};
    char line[256];
    int pos = 0;
    for (int i = 0; i < g_watchCount; ++i) {
        const WatchRow& w = g_watch[i];
        if (w.ffHits == s_lastFf[i] && w.peHits == s_lastPe[i]) continue;
        pos += snprintf(line + pos, sizeof(line) - pos, " %s ff+%u pe+%u", w.name,
                        w.ffHits - s_lastFf[i], w.peHits - s_lastPe[i]);
        s_lastFf[i] = w.ffHits;
        s_lastPe[i] = w.peHits;
        if (pos >= static_cast<int>(sizeof(line)) - 48) break;
    }
    if (pos > 0)
        BVR_LOG("[b2r] aim probe:%s (pe/s=%u census=%u)", line, g_peSeen - s_lastPeSeen,
                g_censusUsed);
    s_lastPeSeen = g_peSeen;
}

bool handle_command(const char* args) {
    const char* rest = nullptr;
    if (*args == '\0' || token(args, "status")) {
        log_status();
        return true;
    }
    if (token(args, "probe", &rest)) {
        if (token(rest, "on")) {
            census_clear();
            g_probeArmed.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim probe on (fire-watch + census armed)");
        } else if (token(rest, "off")) {
            g_probeArmed.store(false, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vraim probe off");
        } else if (token(rest, "clear")) {
            census_clear();
            for (int i = 0; i < g_watchCount; ++i) {
                g_watch[i].ffHits = 0;
                g_watch[i].peHits = 0;
            }
            BVR_LOG("[b2r] command: vraim probe clear");
        } else if (token(rest, "dump")) {
            log_status();
            census_dump();
        } else {
            BVR_LOG("[b2r] vraim probe on|off|clear|dump");
        }
        return true;
    }
    BVR_LOG("[b2r] vraim: status | probe on|off|clear|dump (the seam hook lands after "
            "the probe's verdict)");
    return true;
}

} // namespace bvr::b2r::aim
