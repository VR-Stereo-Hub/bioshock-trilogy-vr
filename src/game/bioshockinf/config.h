#pragma once
// I6 rung 5: the adapter-local config registry and named presets.
//
// DELIBERATELY ADAPTER-LOCAL, not core (ARCHITECTURE decision log,
// session 41): the ROADMAP's "extract bvr::config into core" is deferred to
// the healing session per the decoupling directive - BS1/BS2 are
// headset-accepted and their hand-rolled vrpreset writers must not move to
// serve Infinite. Duplicate code is fine.
//
// One registration table serves everything: the CURRENT-settings store
// (vrpreset.ini - same file, same keys as the hand-rolled I4/I5 writer, so
// legacy 3-key files keep loading), named preset files
// (%LOCALAPPDATA%\BioshockVR\bsi\presets\<name>.ini), the seam verbs and the
// F10 readout loop. Every value is a float on the wire; a key's set()
// applies through whatever posting machinery the value needs - fov and
// resolution NEVER take naked stores into engine-coupled state.
//
// Resolution keys are the one deliberate exception to load-applies: a
// surprise live resize mid-headset is a session hazard, so a loaded preset
// LATCHES its resolution as "wanted" and the RENDER RESOLUTION section (and
// the log) surface it behind the existing overlay-clickable Apply.

#include <cstdint>

namespace bvr::bsi::config {

struct KeyDesc {
    const char* key;      // ini key, also the F10 readout label
    float (*get)();       // current value (called at save)
    void (*set)(float v); // apply a loaded value (must be render-thread-safe)
    float lo, hi;         // load-time range guard; out-of-range values skip
};

// Register the table once (adapter init, before any load). The pointer must
// stay valid for the process lifetime (camera.cpp owns a static table).
void init(const KeyDesc* keys, size_t n);

// Current-settings store (vrpreset.ini). load is fail-soft: missing file =
// defaults stand; unknown lines skip; out-of-range values skip.
void save_current();
void load_current();

// Named presets under presets\<name>.ini. Slot names are "slot1".."slot4"
// (the F10 buttons); the seam verbs take arbitrary names. Game thread only
// (file IO + the same appliers load_current uses).
bool save_named(const char* name);
bool load_named(const char* name);

// `vrpreset [save|saveas <name>|load <name>|list]` - the whole verb family.
// Bare `vrpreset` loads the current store (the legacy behaviour).
void handle_vrpreset(const char* args);

// Game thread pump: consumes F10-posted save/load ops. Called from the
// camera detour tail.
void tick();

// The resolution a loaded preset wants, if any (consumed-once flag so the
// picker UI can preselect it exactly once per load).
bool wanted_resolution(int* w, int* h, bool* fresh);

// Overlay section: slot buttons, preset list, current-values readout.
void draw_debug_ui();

namespace detail {
// The setters the camera registers for resW/resH: they LATCH the wanted
// resolution (see the header comment), never apply it.
void latch_wanted_res_w(float v);
void latch_wanted_res_h(float v);
} // namespace detail

} // namespace bvr::bsi::config
