#pragma once
// s48: the SubtleFidget ProcessEvent OBSERVER (and optional filter).
//
// The s46 compose-side glue was headset-REJECTED (2026-08-09): it pins the
// driven bones but the SubtleFidget anim still owns the rest of the model.
// User directive: refuse the trigger itself. This module's vtable-slot filter
// was built as that root kill - and the clean-boot A/B then FALSIFIED the
// premise: with the filter armed from resolve, a full 8-minute idle entered
// the stance with events=1, startSeen=0, blocked=0. The `StartSubtleFidget`
// dispatch (which DOES sometimes fire, and which the filter provably blocks
// when it does) is a notification/secondary path - THE ANIM STARTS NATIVELY
// (the XFidgetAnimationSelection machinery). The surviving root is the
// engine's own gate, the bDisableSubtleFidget UBOOL - reflect.cpp's
// bsiprop/bsipropbit exist to derive its byte offset and set it; that
// derivation needs a booted save and is the next session's opener.
//
// MECHANISM (of the observer): REPLACE the ProcessEvent SLOT (+0x7C, the
// offline-derived and live-cross-checked offset) in the attachment's OWN
// vtable with a filter that recognizes exactly `StartSubtleFidget` on exactly
// the resolved attachment and tail-calls the original for everything else.
// Default PROBE (log, pass through); `bsifidget on` blocks - kept because the
// blocking machinery is proven and may serve the eventual layered kill.
//
// Scope notes:
//  - A vtable patch is CLASS-wide (every instance sharing the vtable, present
//    and future - so it survives attachment recreation across loads), but the
//    BLOCK is additionally gated to `self == bones::attachment()`, so even a
//    shared native vtable cannot silence another actor's event.
//  - The name check is one integer compare against the pre-resolved GNames
//    index (fname_find once at install; per-event text work would be a
//    stutter).
//  - Install gates: build gate, the attachment must walk as a genuine UObject
//    whose class names XFirstPersonAttachment (the s45b fixpoint walker), and
//    the slot's current occupant must be one of the two derived ProcessEvent
//    RVAs (AActor's override or the UObject base) - an unknown occupant means
//    the vtable is not what the derivation says, and we refuse.
//
// `bsifidget probe` passes everything through and logs StartSubtleFidget
// dispatches (the positive control: the stance re-onsets and the log names
// the trigger); `bsifidget on` (default) blocks them; `bsifidget off`
// restores the original slot.

namespace bvr::bsi::fidget {

// Lazy install from the camera tick (needs the resolved attachment).
bool wants_install();
bool try_install();

bool handle_command(const char* cmd, const char* args);
void draw_debug_ui();

} // namespace bvr::bsi::fidget
