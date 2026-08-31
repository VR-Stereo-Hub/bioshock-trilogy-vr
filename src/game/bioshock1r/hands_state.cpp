#include "game/bioshock1r/hands_state.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

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
};
const StateRow kStates[] = {
    {L"HandsOffscreen", State::Offscreen},
    {L"WeaponIdling", State::Idling},
    {L"WeaponZoomedIdling", State::Idling},
    {L"AbilityIdling", State::Idling},
    {L"AbilityGenericIdling", State::Idling},
    {L"WeaponEquipping", State::Equipping},
    {L"AbilityFastEquipping", State::Equipping},
    {L"AbilitySlowEquipping", State::Equipping},
    {L"AbilityGenericEquipping", State::Equipping},
    {L"WeaponUnEquipping", State::UnEquipping},
    {L"AbilityFastUnEquipping", State::UnEquipping},
    {L"AbilitySlowUnEquipping", State::UnEquipping},
    {L"AbilityGenericUnEquipping", State::UnEquipping},
    {L"WeaponFiring", State::Firing},
    {L"WeaponZoomedFiring", State::Firing},
    {L"AbilityFiring", State::Firing},
    {L"PostWeaponFiring", State::PostFiring},
    {L"PostWeaponZoomedFiring", State::PostFiring},
    {L"FinishAbilityFiringWithEve", State::PostFiring},
    {L"FinishAbilityFiringWithoutEve", State::PostFiring},
    {L"WeaponReloading", State::Reloading},
    {L"ProceduralWeaponReloading", State::Reloading},
    {L"WeaponZoomingIn", State::Zooming},
    {L"WeaponZoomingOut", State::Zooming},
    {L"PlayingScriptedHandAnimation", State::Scripted},
    {L"InjectingEve", State::Scripted},
    {L"UsingGathererTool", State::Scripted},
    {L"ExorcisingGatherer", State::Scripted},
    {L"ProceduralLoweringHands", State::Scripted},
    {L"TransitionalState", State::Unknown},
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
