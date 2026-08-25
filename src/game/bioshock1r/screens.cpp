#include "screens.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/util/log.h"

namespace bvr::b1r::screens {
namespace {

// Substring match, case-insensitive, against the movie's full path. The names
// the engine reports are like "..\FlashMovies\PausePC.swf", so a substring is
// the right shape and "Hacking" matches "hackingPC.swf" without a second entry.
// Ported from BRVR's AnchorMovies list, plus PausePC which that list lacked
// because its own detector never resolved far enough to see it.
const char* const kPanelMovies[] = {
    "PausePC",     "ingamemanual", "Maps",         "craftingstation", "PlasmidTraining",
    "Hacking",     "GeneBank",     "PlasmidEquip", "PlasmiNow",       "Warning",
};
constexpr int kPanelMovieCount = static_cast<int>(sizeof(kPanelMovies) / sizeof(kPanelMovies[0]));

// Extra names the user can add live without a rebuild.
char g_extra[8][32];
int g_extraN = 0;

char g_top[96] = "";
std::atomic<bool> g_panelUp{false};

void* g_level = nullptr;
size_t g_levelOff = 0;
void* g_ctrl = nullptr;
void* g_lastTop = nullptr;
bool g_loggedChainStop = false;
int g_findMisses = 0;
uint64_t g_lastScanMs = 0;

// ---- guarded reads ---------------------------------------------------------

bool read_ptr_at(const void* base, size_t off, void** out) {
    __try {
        *out = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_i32_at(const void* base, size_t off, int32_t* out) {
    __try {
        *out = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An OBJECT: aligned, readable, first word is a vtable whose first entry is in
// committed executable memory.
bool looks_like_object(const void* p) {
    if (!p || (reinterpret_cast<uintptr_t>(p) & 3) ||
        reinterpret_cast<uintptr_t>(p) < 0x10000)
        return false;
    void* vtbl = nullptr;
    if (!read_ptr_at(p, 0, &vtbl)) return false;
    if (!vtbl || (reinterpret_cast<uintptr_t>(vtbl) & 3)) return false;
    void* fn = nullptr;
    if (!read_ptr_at(vtbl, 0, &fn)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!fn || VirtualQuery(fn, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    switch (mbi.Protect & 0xFF) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// A BUFFER: aligned and readable, no vtable demanded. Not pedantry - it cost
// BRVR a session. The object test on a TArray's data pointer rejects a correct
// pointer every time, because the buffer's first word is element 0, whose own
// first word is a vtable POINTER, and a vtable lives in read-only data rather
// than executable memory. Fail closed is right; failing closed on the wrong
// predicate is not.
bool looks_like_buffer(const void* p) {
    if (!p || (reinterpret_cast<uintptr_t>(p) & 3) ||
        reinterpret_cast<uintptr_t>(p) < 0x10000)
        return false;
    void* probe = nullptr;
    return read_ptr_at(p, 0, &probe);
}

// ---- LevelInfo, found structurally rather than at a literal offset ---------
// AActor::Level sits at the same offset on every actor, so the controller and
// the pawn must agree on it; both must point at the SAME object; and LevelInfo
// is itself an actor, so its own copy points at ITSELF. Nothing else passes all
// three, so no offset is trusted and a wrong guess cannot survive.
constexpr size_t kActorScanBytes = 0x1000;

void find_level(const void* controller, const void* pawn) {
    if (!controller || !pawn) return;
    for (size_t off = 0; off + 4 <= kActorScanBytes; off += 4) {
        void* fromPc = nullptr;
        void* fromPawn = nullptr;
        if (!read_ptr_at(controller, off, &fromPc)) continue;
        if (!read_ptr_at(pawn, off, &fromPawn)) continue;
        if (!fromPc || fromPc != fromPawn) continue;
        if (!looks_like_object(fromPc)) continue;
        void* selfRef = nullptr;
        if (!read_ptr_at(fromPc, off, &selfRef)) continue;
        if (selfRef != fromPc) continue;
        g_level = fromPc;
        g_levelOff = off;
        BVR_LOG("[b1r] screens: LevelInfo = %p (AActor::Level = +0x%X, self-referential)",
                fromPc, static_cast<unsigned>(off));
        return;
    }
    if (++g_findMisses == 300)
        BVR_LOG("[b1r] screens: no self-referential Level pointer after 300 frames "
                "- screen naming is OFF, placement is unchanged");
}

// ---- the GUI controller ----------------------------------------------------
// GetFlashGUIController, ecx = LevelInfo, from the getter's own instructions:
//     mov eax,[ecx+0xFC]   ; XLevel
//     mov eax,[eax+0x5C]
//     mov eax,[eax+0x4C]
//     cmp [eax+0x48],0     ; TArray ArrayNum - the getter bails when zero
//     mov eax,[eax+0x44]   ; TArray Data
//     mov eax,[eax]        ; data[0]
//     mov eax,[eax+0x7C]   ; the controller
//
// Reports WHERE it stopped rather than just failing. A six-deep walk decoded
// from instructions has six ways to be wrong, and "did not resolve" tells you
// which of them exactly never.
void* resolve_controller(const char** stopStep, void** stopVal) {
    const char* stop = nullptr;
    void* val = nullptr;
    void* ctrl = nullptr;
    const void* p = g_level;

#define SCREENS_BAIL(n, v) do { stop = (n); val = (void*)(v); goto done; } while (0)
    if (!p) SCREENS_BAIL("level null", nullptr);
    {
        static const size_t kStep[3] = {0xFC, 0x5C, 0x4C};
        static const char* const kName[3] = {"step1 +0xFC XLevel", "step2 +0x5C",
                                             "step3 +0x4C"};
        for (int i = 0; i < 3; ++i) {
            void* next = nullptr;
            if (!read_ptr_at(p, kStep[i], &next)) SCREENS_BAIL(kName[i], p);
            if (!looks_like_object(next)) SCREENS_BAIL(kName[i], next);
            p = next;
        }
        int32_t count = 0;
        if (!read_i32_at(p, 0x48, &count)) SCREENS_BAIL("step4 +0x48 unreadable", p);
        if (count == 0) SCREENS_BAIL("step4 +0x48 count == 0 (the getter bails too)", nullptr);
        void* data = nullptr;
        if (!read_ptr_at(p, 0x44, &data)) SCREENS_BAIL("step5 +0x44 unreadable", p);
        if (!looks_like_buffer(data)) SCREENS_BAIL("step5 +0x44 array data", data);
        void* elem0 = nullptr;
        if (!read_ptr_at(data, 0, &elem0)) SCREENS_BAIL("step6 data[0] unreadable", data);
        if (!looks_like_object(elem0)) SCREENS_BAIL("step6 data[0] element", elem0);
        void* c = nullptr;
        if (!read_ptr_at(elem0, 0x7C, &c)) SCREENS_BAIL("step7 +0x7C unreadable", elem0);
        if (!looks_like_object(c)) SCREENS_BAIL("step7 +0x7C controller", c);
        ctrl = c;
    }
done:
#undef SCREENS_BAIL
    if (stopStep) *stopStep = stop;
    if (stopVal) *stopVal = val;
    return ctrl;
}

// The movie names are FILENAMES, so they are strings and need no name table.
// UTF-16 first because UE2 builds TCHAR as wchar_t, then ANSI. Conservative:
// printable ASCII, must terminate, bounded.
bool try_read_string(const void* p, char* out, size_t outSz) {
    if (!out || outSz < 8 || !looks_like_buffer(p)) return false;
    unsigned char b[128];
    __try {
        memcpy(b, p, sizeof(b));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const size_t kMax = outSz - 1 < 63 ? outSz - 1 : 63;
    if (b[1] == 0 && b[0] >= 0x20 && b[0] < 0x7F) { // UTF-16LE
        size_t n = 0;
        while (n < kMax && (n * 2 + 1) < sizeof(b)) {
            const unsigned char lo = b[n * 2], hi = b[n * 2 + 1];
            if (lo == 0 && hi == 0) break;
            if (hi != 0 || lo < 0x20 || lo >= 0x7F) return false;
            out[n++] = static_cast<char>(lo);
        }
        if (n < 2) return false;
        out[n] = '\0';
        return true;
    }
    if (b[0] >= 0x20 && b[0] < 0x7F) { // ANSI
        size_t n = 0;
        while (n < kMax && n < sizeof(b)) {
            const unsigned char ch = b[n];
            if (ch == 0) break;
            if (ch < 0x20 || ch >= 0x7F) return false;
            out[n++] = static_cast<char>(ch);
        }
        if (n < 2) return false;
        out[n] = '\0';
        return true;
    }
    return false;
}

bool contains_ci(const char* hay, const char* needle) {
    const size_t nl = strlen(needle);
    if (!nl) return false;
    for (const char* p = hay; *p; ++p)
        if (_strnicmp(p, needle, nl) == 0) return true;
    return false;
}

bool is_panel_movie(const char* name) {
    for (int i = 0; i < kPanelMovieCount; ++i)
        if (contains_ci(name, kPanelMovies[i])) return true;
    for (int i = 0; i < g_extraN; ++i)
        if (contains_ci(name, g_extra[i])) return true;
    return false;
}

} // namespace


// ---- the Pauser hunt (BS1 diagnostic, always on, costs one sweep at 5 Hz) ---
//
// WHY THIS EXISTS. ENGINE_NOTES falsified `LevelInfo::Pauser` on this build -
// "read null through three real pauses". That test used `Level+0x668`, BRVR's
// number, which is exactly what the repo's own hard rule forbids: same engine
// tree, different link, derive fresh. The falsification is sound about THAT
// OFFSET and says nothing about whether the field exists.
//
// BRVR did not guess 0x668 either. It hunted it (GameState.cpp HuntPauser):
// snapshot a block of LevelInfo, then watch for the slot that goes
// null -> object -> null across one open/close of a full menu. 0x668 is what
// that hunt returned on ITS build. This is the same hunt, re-run here.
//
// WHY IT MATTERS. The Flash movie stack is exact about WHICH screen is up and
// cannot tell WHY: PausePC.swf is pushed by bathysphere rides and scripted
// scenes as well as by the player, so s64 had to stop treating it as a UI
// pause during a scene - and a real pause taken during a scene then loses the
// anchored quad and renders into the world projection. A true pause flag is
// the missing term. BRVR anchors through exactly that term (`paused` in
// Hooks.cpp's ordinaryMenu), which is why its pause menu anchors during scenes
// and this one does not.
//
// NO COMMAND, NO TOGGLE, NOTHING TO ARM. The baseline refreshes while no panel
// is up and the diff runs on the RISING EDGE of one, so simply pausing the game
// during ordinary play produces the answer. Nobody has to type anything mid-run,
// which is the only way this gets measured in a headset.
constexpr size_t kPauserScanBytes = 0x2000; // BRVR swept 4 KB; widened after a miss
constexpr int kPauserSlots = static_cast<int>(kPauserScanBytes / 4);
constexpr int kPauserMaxReport = 24;        // a wall of hits is noise, not data
void* g_pauserSnap[kPauserSlots] = {};
bool g_pauserBaseline = false;
uint64_t g_pauserNextMs = 0;
int g_pauserCandidates[kPauserMaxReport] = {};
int g_pauserCandidateN = 0;

// Refresh the baseline. Only ever called while NO panel is up, so every slot
// recorded here is a not-paused value.
void pauser_baseline(uint64_t now) {
    if (now < g_pauserNextMs) return;
    g_pauserNextMs = now + 200; // 5 Hz; the sweep is 1024 guarded reads
    const uint8_t* L = static_cast<const uint8_t*>(g_level);
    for (int i = 0; i < kPauserSlots; ++i) {
        void* v = nullptr;
        g_pauserSnap[i] = read_ptr_at(L, static_cast<size_t>(i) * 4, &v) ? v : nullptr;
    }
    g_pauserBaseline = true;
}

// A panel just came up. Report every slot that went null -> object, which is
// the Pauser shape. Anything that was already non-null is not it.
void pauser_on_panel_up() {
    if (!g_pauserBaseline) return;
    const uint8_t* L = static_cast<const uint8_t*>(g_level);
    // ANY change, classified - not just null -> object. The first cut only
    // reported null -> object, which is the Pauser SHAPE, and a real pause on
    // this build changed no such slot at all. Reporting every change says
    // whether the field is a differently-shaped one (a bool, a count, a
    // non-null handle) or whether LevelInfo is simply the wrong object.
    int found = 0;
    g_pauserCandidateN = 0;
    for (int i = 0; i < kPauserSlots; ++i) {
        void* v = nullptr;
        if (!read_ptr_at(L, static_cast<size_t>(i) * 4, &v)) continue;
        void* was = g_pauserSnap[i];
        if (v == was) continue;
        ++found;
        if (found > kPauserMaxReport) continue;
        const char* kind = (!was && v)   ? (looks_like_object(v) ? "null -> OBJECT" : "null -> value")
                           : (was && !v) ? "value -> null"
                                         : "changed";
        g_pauserCandidates[g_pauserCandidateN++] = i;
        BVR_LOG("[b1r] PAUSER HUNT: Level+0x%X %s (%p -> %p) - candidate %d",
                static_cast<unsigned>(i) * 4, kind, was, v, found);
    }
    if (found == 0)
        BVR_LOG("[b1r] PAUSER HUNT: NOTHING changed in Level+0x0..0x%X across this "
                "panel. LevelInfo is the wrong object on this build - the pause flag "
                "is not on it at all, at any shape.",
                static_cast<unsigned>(kPauserScanBytes));
    else if (found > kPauserMaxReport)
        BVR_LOG("[b1r] PAUSER HUNT: %d slots changed - too many to be meaningful. The "
                "one that also goes BACK to null on close is the answer.",
                found);
}

// The panel closed. A real Pauser goes back to null; a candidate that does not
// was something else that happened to be allocated at the same moment. This is
// the half that turns a list into an answer.
void pauser_on_panel_down() {
    if (g_pauserCandidateN == 0) return;
    const uint8_t* L = static_cast<const uint8_t*>(g_level);
    for (int n = 0; n < g_pauserCandidateN; ++n) {
        const int i = g_pauserCandidates[n];
        void* v = nullptr;
        const bool ok = read_ptr_at(L, static_cast<size_t>(i) * 4, &v);
        BVR_LOG("[b1r] PAUSER HUNT: Level+0x%X is %s on close - %s",
                static_cast<unsigned>(i) * 4, ok ? (v ? "still an object" : "back to null") : "unreadable",
                (ok && !v) ? "*** THIS IS THE PAUSER OFFSET ***" : "not it");
    }
    g_pauserCandidateN = 0;
}

void on_calcview(void* pc, void* viewActor) {
    if (!pc || !viewActor) return;

    if (!g_level) {
        find_level(pc, viewActor);
        if (!g_level) return;
    }

    // Re-prove identity every frame. A level change frees LevelInfo, which is
    // why there is no reset hook to forget to call.
    void* selfRef = nullptr;
    if (!read_ptr_at(g_level, g_levelOff, &selfRef) || selfRef != g_level) {
        BVR_LOG("[b1r] screens: LevelInfo self-reference broke - re-finding");
        g_level = nullptr;
        g_ctrl = nullptr;
        g_lastTop = nullptr;
        g_top[0] = '\0';
        g_panelUp.store(false, std::memory_order_relaxed);
        return;
    }

    // ~30 Hz. CalcView fires far above frame rate and this is a pointer walk.
    const uint64_t now = GetTickCount64();
    if (now - g_lastScanMs < 33) return;
    g_lastScanMs = now;

    if (!g_ctrl || !looks_like_object(g_ctrl)) {
        const char* stop = nullptr;
        void* val = nullptr;
        g_ctrl = resolve_controller(&stop, &val);
        if (!g_ctrl) {
            if (!g_loggedChainStop) {
                g_loggedChainStop = true;
                BVR_LOG("[b1r] screens: GUI chain stopped at %s (value %p)",
                        stop ? stop : "?", val);
            }
            return;
        }
        BVR_LOG("[b1r] screens: GUI controller = %p RESOLVED", g_ctrl);
    }

    // GetTopPlayingMovie, ecx = controller:
    //     mov edx,[ecx+0x15C]     ; count
    //     mov eax,[ecx+0x158]     ; array data
    //     mov ecx,[eax+edx*4-4]   ; data[count-1] == the TOP
    int32_t count = 0;
    void* data = nullptr;
    if (!read_i32_at(g_ctrl, 0x15C, &count)) return;
    if (!read_ptr_at(g_ctrl, 0x158, &data)) return;
    if (count <= 0 || count > 64 || !looks_like_buffer(data)) return;

    void* top = nullptr;
    if (!read_ptr_at(data, static_cast<size_t>(count - 1) * 4, &top)) return;
    if (top == g_lastTop) return;
    g_lastTop = top;

    // The name sits at +0x40 on the movie object. Measured on this build: both
    // +0x40 and +0x4C read as the path, but +0x4C degrades to
    // "NoFileSpecified" on some entries while +0x40 stayed correct.
    char name[96] = "";
    void* slot = nullptr;
    if (looks_like_object(top) && read_ptr_at(top, 0x40, &slot))
        try_read_string(slot, name, sizeof(name));

    if (!name[0]) {
        // Unknown top movie. Leave the previous verdict standing rather than
        // dropping the panel out from under a screen the player is reading.
        BVR_LOG("[b1r] screens: top movie %p has no readable name at +0x40", top);
        return;
    }

    strncpy_s(g_top, name, _TRUNCATE);
    const bool panel = is_panel_movie(name);
    const bool was = g_panelUp.exchange(panel, std::memory_order_relaxed);
    // The Pauser hunt rides the panel edges - see its banner. Baseline while
    // nothing is up, diff on the way in, confirm on the way out.
    if (panel != was) {
        if (panel) pauser_on_panel_up();
        else pauser_on_panel_down();
    }
    if (!panel) pauser_baseline(GetTickCount64());
    BVR_LOG("[b1r] screens: top = \"%s\" (%d playing) -> %s%s", name, count,
            panel ? "PANEL" : "gameplay", panel != was ? "  [changed]" : "");
}

const char* top_movie() { return g_top; }
bool panel_screen_up() { return g_panelUp.load(std::memory_order_relaxed); }
void* level() { return g_level; }

void handle_command(const char* args) {
    if (strncmp(args, "list", 4) == 0) {
        for (int i = 0; i < kPanelMovieCount; ++i)
            BVR_LOG("[b1r] screens: panel movie \"%s\"", kPanelMovies[i]);
        for (int i = 0; i < g_extraN; ++i)
            BVR_LOG("[b1r] screens: panel movie \"%s\" (added)", g_extra[i]);
    } else if (strncmp(args, "add ", 4) == 0) {
        const char* v = args + 4;
        while (*v == ' ') ++v;
        if (!*v || g_extraN >= 8) {
            BVR_LOG("[b1r] screens: cannot add (empty name, or 8 already added)");
            return;
        }
        strncpy_s(g_extra[g_extraN], v, _TRUNCATE);
        ++g_extraN;
        g_lastTop = nullptr; // force a re-evaluation of the current screen
        BVR_LOG("[b1r] screens: added \"%s\"", g_extra[g_extraN - 1]);
    } else if (strncmp(args, "clear", 5) == 0) {
        g_extraN = 0;
        g_lastTop = nullptr;
        BVR_LOG("[b1r] screens: added names cleared");
    } else {
        BVR_LOG("[b1r] screens: top = \"%s\" | panel %d | level %p | ctrl %p", g_top,
                panel_screen_up() ? 1 : 0, g_level, g_ctrl);
        BVR_LOG("[b1r] screens: usage: vrscreens list | add <name> | clear");
    }
}

} // namespace bvr::b1r::screens
