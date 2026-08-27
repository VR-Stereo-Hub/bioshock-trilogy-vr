#include "game/bioshock1r/hands_state.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace bvr::b1r::hands_state {
namespace {

// Every state Hands.uc declares. This list is the ACCEPTANCE TEST for the
// derivation, not documentation: an offset pair is only believed when the name
// it produces is one of these. Transcribed from
// docs/brvr-reference/research/uscript/ShockGame/Classes/Hands.uc.
struct StateRow {
    const wchar_t* name;
    State state;
    bool ability; // s68b: a PLASMID state, not a weapon one - see the header
};
const StateRow kStates[] = {
    {L"HandsOffscreen", State::Offscreen, false},
    {L"WeaponIdling", State::Idling, false},
    {L"WeaponZoomedIdling", State::Idling, false},
    {L"AbilityIdling", State::Idling, true},
    {L"AbilityGenericIdling", State::Idling, true},
    {L"WeaponEquipping", State::Equipping, false},
    {L"AbilityFastEquipping", State::Equipping, true},
    {L"AbilitySlowEquipping", State::Equipping, true},
    {L"AbilityGenericEquipping", State::Equipping, true},
    {L"WeaponUnEquipping", State::UnEquipping, false},
    {L"AbilityFastUnEquipping", State::UnEquipping, true},
    {L"AbilitySlowUnEquipping", State::UnEquipping, true},
    {L"AbilityGenericUnEquipping", State::UnEquipping, true},
    {L"WeaponFiring", State::Firing, false},
    {L"WeaponZoomedFiring", State::Firing, false},
    {L"AbilityFiring", State::Firing, true},
    {L"PostWeaponFiring", State::PostFiring, false},
    {L"PostWeaponZoomedFiring", State::PostFiring, false},
    {L"FinishAbilityFiringWithEve", State::PostFiring, true},
    {L"FinishAbilityFiringWithoutEve", State::PostFiring, true},
    {L"WeaponReloading", State::Reloading, false},
    {L"ProceduralWeaponReloading", State::Reloading, false},
    {L"WeaponZoomingIn", State::Zooming, false},
    {L"WeaponZoomingOut", State::Zooming, false},
    {L"PlayingScriptedHandAnimation", State::Scripted, false},
    {L"InjectingEve", State::Scripted, true},
    {L"UsingGathererTool", State::Scripted, false},
    {L"ExorcisingGatherer", State::Scripted, false},
    {L"ProceduralLoweringHands", State::Scripted, false},
    {L"TransitionalState", State::Unknown, false},
};

// The search windows. UE2 keeps FStateFrame near the top of UObject and
// StateNode near the top of FStateFrame; both are small structures, so a short
// sweep is enough and a long one only invites debris.
constexpr uint32_t kMaxObjScan = 0x40;
constexpr uint32_t kMaxFrameScan = 0x30;

uint32_t g_stateFrameOff = 0;
uint32_t g_stateNodeOff = 0;
bool g_located = false;
uint64_t g_nextTryMs = 0;
unsigned g_tries = 0;

bool readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.Protect & bad) return false;
    const uint8_t* base = static_cast<const uint8_t*>(mbi.BaseAddress);
    return static_cast<const uint8_t*>(p) + n <= base + mbi.RegionSize;
}

template <typename T>
bool read_at(const void* base, uint32_t off, T* out) {
    const uint8_t* p = static_cast<const uint8_t*>(base) + off;
    if (!readable(p, sizeof(T))) return false;
    memcpy(out, p, sizeof(T));
    return true;
}

// Does `maybeState` look like a UState whose name is one of ours?
const StateRow* state_row_of(const void* maybeState) {
    if (!readable(maybeState, patterns::kUObjectClassOffset + 4)) return nullptr;
    int32_t nameIdx = 0;
    if (!read_at(maybeState, patterns::kUObjectNameIndexOffset, &nameIdx)) return nullptr;
    if (nameIdx <= 0) return nullptr;
    const wchar_t* txt = patterns::fname_text(nameIdx);
    if (!txt) return nullptr;
    for (const StateRow& r : kStates)
        if (wcscmp(txt, r.name) == 0) return &r;
    return nullptr;
}

} // namespace

bool located() { return g_located; }

// s68b cache. active_hand() is called from more than one thread and has no actor
// pointer; these give it an answer without either. Staleness is the whole safety
// property: if the drive stops running the flag must not keep asserting "plasmid".
std::atomic<bool> g_lastAbility{false};
std::atomic<unsigned long long> g_lastAbilityMs{0};
constexpr unsigned long long kAbilityCacheMs = 500;

bool current_is_ability(const void* handsActor) {
    if (!g_located || !handsActor) return false;
    const void* frame = nullptr;
    if (!read_at(handsActor, g_stateFrameOff, &frame)) return false;
    const void* node = nullptr;
    if (!read_at(frame, g_stateNodeOff, &node) || !node) return false;
    const StateRow* row = state_row_of(node);
    if (!row) return false;
    g_lastAbility.store(row->ability, std::memory_order_relaxed);
    g_lastAbilityMs.store(GetTickCount64(), std::memory_order_relaxed);
    return row->ability;
}

bool last_ability() {
    const unsigned long long at = g_lastAbilityMs.load(std::memory_order_relaxed);
    if (!at || GetTickCount64() - at > kAbilityCacheMs) return false;
    return g_lastAbility.load(std::memory_order_relaxed);
}

const char* to_string(State s) {
    switch (s) {
        case State::Idling: return "Idling";
        case State::Equipping: return "Equipping";
        case State::UnEquipping: return "UnEquipping";
        case State::Firing: return "Firing";
        case State::PostFiring: return "PostFiring";
        case State::Reloading: return "Reloading";
        case State::Zooming: return "Zooming";
        case State::Scripted: return "Scripted";
        case State::Offscreen: return "Offscreen";
        default: return "Unknown";
    }
}

bool locate(const void* handsActor) {
    if (g_located) return true;
    if (!handsActor) return false;

    // Backoff. The Hands actor exists long before it enters a named state, and
    // a sweep every frame while it sits in HandsOffscreen is the sort of
    // per-frame scan this project has a standing rule against.
    const uint64_t now = GetTickCount64();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 500;

    for (uint32_t fo = 4; fo <= kMaxObjScan; fo += 4) {
        const void* frame = nullptr;
        if (!read_at(handsActor, fo, &frame)) continue;
        if (!readable(frame, kMaxFrameScan + sizeof(void*))) continue;

        for (uint32_t no = 0; no <= kMaxFrameScan; no += 4) {
            const void* node = nullptr;
            if (!read_at(frame, no, &node)) continue;
            const StateRow* row = state_row_of(node);
            if (!row) continue;

            g_stateFrameOff = fo;
            g_stateNodeOff = no;
            g_located = true;
            BVR_LOG("[b1r] hands state LOCATED: StateFrame at +0x%X, StateNode at +0x%X, "
                    "currently '%ls' (%s). Derived, not hardcoded - the name had to "
                    "resolve to a state Hands.uc declares.",
                    fo, no, row->name, to_string(row->state));
            return true;
        }
    }

    if (++g_tries % 20 == 1)
        BVR_LOG("[b1r] hands state: no StateFrame found yet (try %u). Normal until the "
                "rig enters a named state - it starts in HandsOffscreen.",
                g_tries);
    return false;
}

State current(const void* handsActor) {
    if (!g_located || !handsActor) return State::Unknown;
    const void* frame = nullptr;
    if (!read_at(handsActor, g_stateFrameOff, &frame)) return State::Unknown;
    const void* node = nullptr;
    if (!read_at(frame, g_stateNodeOff, &node)) return State::Unknown;
    // NULL StateNode is legal: it means "no state", which the engine uses
    // between transitions. Report Unknown rather than inventing one.
    if (!node) return State::Unknown;
    const StateRow* row = state_row_of(node);
    // s68b: refresh the ability cache from here too. bones::drive() calls this
    // every frame and current_is_ability() has no other regular caller, so this
    // is what actually keeps last_ability() fresh.
    if (row) {
        g_lastAbility.store(row->ability, std::memory_order_relaxed);
        g_lastAbilityMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
    return row ? row->state : State::Unknown;
}

const wchar_t* current_name(const void* handsActor) {
    if (!g_located || !handsActor) return L"?";
    const void* frame = nullptr;
    if (!read_at(handsActor, g_stateFrameOff, &frame)) return L"?";
    const void* node = nullptr;
    if (!read_at(frame, g_stateNodeOff, &node) || !node) return L"?";
    int32_t nameIdx = 0;
    if (!read_at(node, patterns::kUObjectNameIndexOffset, &nameIdx)) return L"?";
    const wchar_t* txt = patterns::fname_text(nameIdx);
    return txt ? txt : L"?";
}

void log_status(const void* handsActor) {
    if (!g_located) {
        BVR_LOG("[b1r] hands state: NOT located yet (StateFrame offset underived)");
        return;
    }
    BVR_LOG("[b1r] hands state: StateFrame +0x%X, StateNode +0x%X | now '%ls' (%s)",
            g_stateFrameOff, g_stateNodeOff, current_name(handsActor),
            to_string(current(handsActor)));
}

} // namespace bvr::b1r::hands_state
