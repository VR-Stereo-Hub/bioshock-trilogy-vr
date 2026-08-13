#pragma once
// s58: the INTERACTION-PROMPT ORACLE - a read-only instrument, never a lever.
//
// Purpose: a log-readable "is the USE prompt visible" signal, so the s58
// head-directed-interaction caller sweep is automatable flat (un-deny one
// view consumer, read the oracle, restore - no human eye needed per leg).
//
// The surface (s54 part 3 vocabulary, confirmed live mid-stall): the prompt
// is drawn by pooled XClikButtonHint widgets (XClikButtonHintsContainer holds
// them); both are CLIK widgets like XClikHUDCrosshair, whose visibility bools
// (IsShown / IsCenterpointVisible) pack one dword at a per-class offset the
// s57 xhair lane derives BY NAME per boot. This module reuses that exact
// derivation (gfx::find_instances + reflect::find_bool_property_bit) on the
// hint classes and only ever READS the bits - the game owns them; writing
// would destroy the very signal being measured.
//
// Validation contract (before the sweep trusts it): body-facing an
// interactable (prompt on screen) vs body-faced away (no prompt) must flip
// the aggregate. If XClikButtonHint's IsShown does not track the on-screen
// prompt, the fallback is the s54 enumerator lane (bsigfx scan + bsiprop
// field walks) to find the property that does - this module then moves to
// that property, still by-name-derived.

#include <cstdint>

namespace bvr::bsi::hint {

// Game thread (camera detour tail, next to xhair::tick). Idle unless watch
// mode is on; watch reads cached instances at 1 Hz and logs ON EDGE only.
void tick(uint64_t nowMs);

bool handle_command(const char* cmd, const char* args); // bsihint

} // namespace bvr::bsi::hint
