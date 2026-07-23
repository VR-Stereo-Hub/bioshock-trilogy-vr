#pragma once
// BioShock 1 Remastered signature table. This header and patterns.cpp are the
// ONLY files allowed to contain raw addresses/offsets for this game (see
// ARCHITECTURE.md). Every entry is documented in docs/ENGINE_NOTES.md with
// its derivation method.

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b1r::patterns {

// Live horizontal FOV in degrees (float) inside APlayerController.
// Derivation: itsloopyo/bioshock-remastered-headtracking (MIT)
// FOV_LIVE_OFFSET; verified live in DR-4. Telemetry-only: the renderer never
// reads it (ENGINE_NOTES 2026-07-24).
inline constexpr uint32_t kFovLiveOffset = 0xE0;

// UShockUserSettings singleton - the remaster settings object that backs the
// [ShockGame.ShockUserSettings] ini section. Its HorizontalFOV int is what
// the renderer consumes EVERY FRAME, with no cap beyond the options UI's
// 75-130 range. Derivation (2026-07-24, ENGINE_NOTES symbol table): runtime
// value-scan narrowing through the command seam + poke/screenshot A/B to find
// the consumed copy + RTTI walk (vtable -> COL -> TypeDescriptor) for the
// class name. There is NO stable static pointer to the object (the .data
// slots the ptr-scan surfaced were coincidental range matches that a float
// later overwrote); the object itself is stable for the session, so we locate
// it by scanning the heap for its fixed-RVA vtable and caching the instance.
inline constexpr uint32_t kUserSettingsVtableRva = 0xDA3878;  // .?AVUShockUserSettings@@
inline constexpr uint32_t kUserSettingsHfovOffset = 0x8C;     // int32, degrees

struct Symbols {
    // void __thiscall(APlayerController* this, AActor** viewActor,
    //                 FVector* camLoc, FRotator* camRot)
    void* eventPlayerCalcView = nullptr;
};

// Runs all scans, logging each stage. False if anything failed to resolve.
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The live HorizontalFOV field of the UShockUserSettings singleton, or null.
// Lazy by design: the object exists only after engine init, so this validates
// on every call (null global / dead memory / foreign vtable all return null)
// instead of caching at resolve() time. Game thread only.
int32_t* hfov_option_ptr();

} // namespace bvr::b1r::patterns
