#pragma once
// Calling the game's own UnrealScript functions by name (2026-10-20, the manual reload).
//
// UObject::FindFunction(FName, global) finds a function on an object's class (patterns.h,
// tools\ida\mr2), and ProcessEvent(UFunction*, parms, result) runs it - the engine's own path for
// every script event. Names are looked up once in GNames and cached; functions once per class.
// Every call is SEH-guarded and fails closed (false): a refused call changes nothing in the game.
// Game thread only - script runs on the game thread, and so does every caller here.
//
// The typed helpers are Actor's animation API (Actor.uc): the animation an actor is playing is a
// HANDLE, and its time, rate and pause state are readable and settable through it. That is what
// lets the manual reload drive the game's own reload clip instead of drawing one of its own.

#include <cstdint>

namespace bvr::b1r::script {

void init(void* findFunction, void* processEvent, void* findField);
bool ready();

// 2026-10-20 (the cheats): a script property's byte offset in `obj`, by name (FindField, patterns.h),
// cached per class; -1 when absent. Use only after properties_ready() said yes.
int32_t property_offset(void* obj, const wchar_t* name);
// Once: the property offsets this build reports must reproduce Actor Location and DrawScale, which
// were derived independently. False forever if they do not (no property is then trusted).
bool properties_ready(void* anyActor);

// GNames index of `text` (exact, case-insensitive), -1 when absent. Scans once per text.
int32_t name_index(const char* text);

// Run `fn` on `obj` with a parameter block (at least the function's ParmsSize; zero-filled by the
// caller). False when unresolved, not found on that class, or the call faulted.
bool call(void* obj, const char* fn, void* parms);

// Actor's animation API. `handle` is an animation instance handle the game returned.
bool anim_valid(void* actor, int32_t handle);
bool anim_name(void* actor, int32_t handle, int32_t* nameIndex);
bool anim_time(void* actor, int32_t handle, float* seconds);
bool set_anim_time(void* actor, int32_t handle, float seconds);
bool anim_rate(void* actor, int32_t handle, float* rate);
bool set_anim_rate(void* actor, int32_t handle, float rate);
bool anim_length(void* actor, int32_t nameIndex, float* seconds);
bool pause_anim(void* actor, int32_t handle);
bool resume_anim(void* actor, int32_t handle);
bool anim_paused(void* actor, int32_t handle, bool* paused);

// Counters for status lines.
uint32_t calls();
uint32_t failures();

} // namespace bvr::b1r::script
