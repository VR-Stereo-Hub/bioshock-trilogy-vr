// ============================================================================
//  ovrshim_input.cpp - OpenXR actions -> SteamVR input (IVRInput).
//
//  Adapted from BioVRDev/Bioshock-Remastered-VR OpenXRShim/src/shim_input.cpp
//  with the author's permission (see THIRD_PARTY_NOTICES.md). Deltas vs donor:
//    - binding manifests rewritten for THIS mod's action set ("gameplay", 19
//      actions) - SteamVR binds by ACTION NAME, so none of the donor's names
//      survive
//    - NEW: xrGetActionStatePose (the mod's locate_hand gates on it)
//    - float reads fall back to GetDigitalActionData when the analog read is
//      inactive, so a button bound to a vector1 action (Vive/WMR grip) still
//      drives the mod's FLOAT grip actions as 0/1 - the donor documented this
//      as the "WMR radials never open" hazard and left it open
//    - Index menu = left trackpad CLICK; the donor's collision (their trackpad
//      TOUCH was also their d-pad modifier) does not apply here because this
//      mod's modifier is the thumbrest, which Index lacks - thumbrest stays
//      unbound and the mod's stick-click fallback covers the ammo radial
//
//  The mod creates one action set ("gameplay") with named actions and suggests
//  bindings for oculus/touch_controller etc. SteamVR's input system wants a
//  JSON action manifest plus per-controller binding files instead, so at
//  attach time we generate them next to the DLL and hand them to
//  SetActionManifestPath. Users can rebind anything in SteamVR's controller
//  UI - that UI is the escape hatch for every gap below.
//
//  Pose components: the mod's aim_l/aim_r -> SteamVR "tip", pose_l/pose_r
//  (grip) -> "handgrip".
// ============================================================================
#include "ovrshim.h"
#include <cstring>
#include <cstdio>
#include <direct.h>
#include <vector>

extern const char* ShimModuleDir();

static ActionSetRec* g_theSet = nullptr;
static std::vector<ActionRec*> g_setActions;
static bool g_inputReady = false;

// ---------------------------------------------------------------- creation
OVRSHIM_FN(shim_CreateActionSet)(
    XrInstance, const XrActionSetCreateInfo* info, XrActionSet* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionSetRec* s = new ActionSetRec();
    strncpy_s(s->name, info->actionSetName, _TRUNCATE);
    g_theSet = s;
    *out = (XrActionSet)s;
    SLOG("xrCreateActionSet '%s'", s->name);
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_CreateAction)(
    XrActionSet set, const XrActionCreateInfo* info, XrAction* out)
{
    if (!set || !info || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = new ActionRec();
    a->set = (ActionSetRec*)set;
    strncpy_s(a->name, info->actionName, _TRUNCATE);
    a->type = info->actionType;
    g_setActions.push_back(a);
    *out = (XrAction)a;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_SuggestInteractionProfileBindings)(
    XrInstance, const XrInteractionProfileSuggestedBinding* sb)
{
    if (!sb) return XR_ERROR_VALIDATION_FAILURE;
    SLOG("xrSuggestInteractionProfileBindings profile='%s' count=%u (noted; shim "
         "authors its own SteamVR bindings)", PathToString(sb->interactionProfile),
         sb->countSuggestedBindings);
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_CreateActionSpace)(
    XrSession, const XrActionSpaceCreateInfo* info, XrSpace* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    SpaceRec* s = new SpaceRec();
    s->kind = SPACE_ACTION;
    s->action = (ActionRec*)info->action;
    *out = (XrSpace)s;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- manifest
// Index controllers. Grip via squeeze pull (analog); menu on a firm left
// trackpad click. Thumbrest is deliberately ABSENT (Index has none) - the
// mod's stick-click fallback keeps the ammo radial reachable.
static const char* kBindingsKnuckles = R"JSON({
  "bindings": {
    "/actions/gameplay": {
      "sources": [
        { "path": "/user/hand/left/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/move" },
                      "click":    { "output": "/actions/gameplay/in/stick_l" } } },
        { "path": "/user/hand/right/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/look" },
                      "click":    { "output": "/actions/gameplay/in/stick_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/plasmid" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/fire" } } },
        { "path": "/user/hand/left/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/grip_r" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_b" } } },
        { "path": "/user/hand/left/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_x" } } },
        { "path": "/user/hand/left/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_y" } } },
        { "path": "/user/hand/left/input/trackpad", "mode": "trackpad",
          "inputs": { "click": { "output": "/actions/gameplay/in/menu" } } }
      ],
      "poses": [
        { "output": "/actions/gameplay/in/aim_l",  "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gameplay/in/aim_r",  "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gameplay/in/pose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gameplay/in/pose_r", "path": "/user/hand/right/pose/handgrip" }
      ]
    }
  },
  "controller_type": "knuckles",
  "description": "BioshockVR shim default bindings for Index controllers",
  "name": "BioshockVR (shim) Index bindings"
}
)JSON";

// Vive wands. Trackpads stand in for both sticks; grip is a digital squeeze
// bound to the FLOAT grip actions (the shim's digital fallback read converts
// it to 0/1). Right menu doubles as A (use/interact); B/X/Y have no physical
// home - the SteamVR binding UI is the remedy.
static const char* kBindingsVive = R"JSON({
  "bindings": {
    "/actions/gameplay": {
      "sources": [
        { "path": "/user/hand/left/input/trackpad", "mode": "trackpad",
          "inputs": { "position": { "output": "/actions/gameplay/in/move" },
                      "click":    { "output": "/actions/gameplay/in/stick_l" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "trackpad",
          "inputs": { "position": { "output": "/actions/gameplay/in/look" },
                      "click":    { "output": "/actions/gameplay/in/stick_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/plasmid" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/fire" } } },
        { "path": "/user/hand/left/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/grip_r" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/menu" } } },
        { "path": "/user/hand/right/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_a" } } }
      ],
      "poses": [
        { "output": "/actions/gameplay/in/aim_l",  "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gameplay/in/aim_r",  "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gameplay/in/pose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gameplay/in/pose_r", "path": "/user/hand/right/pose/handgrip" }
      ]
    }
  },
  "controller_type": "vive_controller",
  "description": "BioshockVR shim bindings for Vive wands (rebind B/X/Y in the SteamVR controller UI)",
  "name": "BioshockVR (shim) Vive wand bindings"
}
)JSON";

// Touch under SteamVR (Quest via Link/Steam Link etc). Mirrors the mod's
// native touch table 1:1 including the thumbrest ammo modifier.
static const char* kBindingsTouch = R"JSON({
  "bindings": {
    "/actions/gameplay": {
      "sources": [
        { "path": "/user/hand/left/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/move" },
                      "click":    { "output": "/actions/gameplay/in/stick_l" } } },
        { "path": "/user/hand/right/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/look" },
                      "click":    { "output": "/actions/gameplay/in/stick_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/plasmid" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/fire" } } },
        { "path": "/user/hand/left/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/grip_r" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_b" } } },
        { "path": "/user/hand/left/input/x", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_x" } } },
        { "path": "/user/hand/left/input/y", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_y" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/menu" } } },
        { "path": "/user/hand/left/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/gameplay/in/thumbrest_l" } } },
        { "path": "/user/hand/right/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/gameplay/in/thumbrest_r" } } }
      ],
      "poses": [
        { "output": "/actions/gameplay/in/aim_l",  "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gameplay/in/aim_r",  "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gameplay/in/pose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gameplay/in/pose_r", "path": "/user/hand/right/pose/handgrip" }
      ]
    }
  },
  "controller_type": "oculus_touch",
  "description": "BioshockVR shim bindings for Touch controllers",
  "name": "BioshockVR (shim) Touch bindings"
}
)JSON";

// Windows Mixed Reality / Reverb G2. No face buttons: trackpad clicks feed A
// (right) and X (left), matching the mod's native WMR profile; B/Y unbound.
// UNTESTED on hardware - treat as provisional (donor's warning carried).
static const char* kBindingsWmr = R"JSON({
  "bindings": {
    "/actions/gameplay": {
      "sources": [
        { "path": "/user/hand/left/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/move" },
                      "click":    { "output": "/actions/gameplay/in/stick_l" } } },
        { "path": "/user/hand/right/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/gameplay/in/look" },
                      "click":    { "output": "/actions/gameplay/in/stick_r" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/plasmid" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/gameplay/in/fire" } } },
        { "path": "/user/hand/left/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/grip_l" } } },
        { "path": "/user/hand/right/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/grip_r" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "trackpad",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_a" } } },
        { "path": "/user/hand/left/input/trackpad", "mode": "trackpad",
          "inputs": { "click": { "output": "/actions/gameplay/in/btn_x" } } },
        { "path": "/user/hand/left/input/application_menu", "mode": "button",
          "inputs": { "click": { "output": "/actions/gameplay/in/menu" } } }
      ],
      "poses": [
        { "output": "/actions/gameplay/in/aim_l",  "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/gameplay/in/aim_r",  "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/gameplay/in/pose_l", "path": "/user/hand/left/pose/handgrip" },
        { "output": "/actions/gameplay/in/pose_r", "path": "/user/hand/right/pose/handgrip" }
      ]
    }
  },
  "controller_type": "holographic_controller",
  "description": "BioshockVR shim bindings for Windows Mixed Reality controllers (provisional, untested)",
  "name": "BioshockVR (shim) WMR bindings"
}
)JSON";

static bool WriteTextFile(const char* path, const char* text)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) { SLOG("!!! input: cannot write %s", path); return false; }
    fputs(text, f);
    fclose(f);
    return true;
}

static const char* XrTypeToManifestType(XrActionType t)
{
    switch (t)
    {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:  return "boolean";
    case XR_ACTION_TYPE_FLOAT_INPUT:    return "vector1";
    case XR_ACTION_TYPE_VECTOR2F_INPUT: return "vector2";
    case XR_ACTION_TYPE_POSE_INPUT:     return "pose";
    case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "vibration";
    default: return "boolean";
    }
}

bool InputShim_Attach(ActionSetRec* set, ActionRec** actions, int actionCount)
{
    char dir[MAX_PATH];
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%s\\openvr_input", ShimModuleDir());
    _mkdir(dir);

    // ---- actions.json (generated from the live action list) ----------------
    // Regenerated every launch: hand-edits never stick, but a moved game
    // folder self-heals. The binding files are the user-visible surface;
    // SteamVR's own controller UI can override them per-user.
    char p[MAX_PATH];
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\actions.json", dir);
    FILE* f = nullptr;
    fopen_s(&f, p, "w");
    if (!f) { SLOG("!!! input: cannot write %s", p); return false; }

    fprintf(f, "{\n  \"default_bindings\": [\n"
        "    { \"controller_type\": \"knuckles\", \"binding_url\": \"bindings_knuckles.json\" },\n"
        "    { \"controller_type\": \"vive_controller\", \"binding_url\": \"bindings_vive_controller.json\" },\n"
        "    { \"controller_type\": \"oculus_touch\", \"binding_url\": \"bindings_oculus_touch.json\" },\n"
        "    { \"controller_type\": \"holographic_controller\", \"binding_url\": \"bindings_holographic_controller.json\" }\n"
        "  ],\n  \"actions\": [\n");
    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        fprintf(f, "    { \"name\": \"/actions/%s/%s/%s\", \"type\": \"%s\", \"requirement\": \"optional\" }%s\n",
            set->name, out ? "out" : "in", actions[i]->name,
            XrTypeToManifestType(actions[i]->type),
            (i + 1 < actionCount) ? "," : "");
    }
    fprintf(f, "  ],\n  \"action_sets\": [\n"
        "    { \"name\": \"/actions/%s\", \"usage\": \"leftright\" }\n"
        "  ],\n  \"localization\": [\n    { \"language_tag\": \"en_US\"", set->name);
    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        fprintf(f, ",\n      \"/actions/%s/%s/%s\": \"%s\"",
            set->name, out ? "out" : "in", actions[i]->name, actions[i]->name);
    }
    fprintf(f, "\n    }\n  ]\n}\n");
    fclose(f);

    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\bindings_knuckles.json", dir);
    WriteTextFile(p, kBindingsKnuckles);
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\bindings_vive_controller.json", dir);
    WriteTextFile(p, kBindingsVive);
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\bindings_oculus_touch.json", dir);
    WriteTextFile(p, kBindingsTouch);
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\bindings_holographic_controller.json", dir);
    WriteTextFile(p, kBindingsWmr);

    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\actions.json", dir);
    EVRInputError ie = g_vr.input->SetActionManifestPath((char*)p);
    if (ie != EVRInputError_VRInputError_None)
    {
        SLOG("!!! input: SetActionManifestPath('%s') -> %d", p, (int)ie);
        return false;
    }
    SLOG("input: action manifest set: %s", p);

    char handlePath[160];
    _snprintf_s(handlePath, sizeof(handlePath), _TRUNCATE, "/actions/%s", set->name);
    ie = g_vr.input->GetActionSetHandle(handlePath, &set->vrHandle);
    if (ie != EVRInputError_VRInputError_None || !set->vrHandle)
    {
        SLOG("!!! input: GetActionSetHandle -> %d", (int)ie);
        return false;
    }

    for (int i = 0; i < actionCount; ++i)
    {
        const bool out = actions[i]->type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
        _snprintf_s(handlePath, sizeof(handlePath), _TRUNCATE, "/actions/%s/%s/%s",
                  set->name, out ? "out" : "in", actions[i]->name);
        ie = g_vr.input->GetActionHandle(handlePath, &actions[i]->vrHandle);
        if (ie != EVRInputError_VRInputError_None)
            SLOG("!!! input: GetActionHandle('%s') -> %d", handlePath, (int)ie);
    }

    set->attached = true;
    g_inputReady = true;
    SLOG("input: %d actions attached to SteamVR input", actionCount);
    return true;
}

OVRSHIM_FN(shim_AttachSessionActionSets)(
    XrSession, const XrSessionActionSetsAttachInfo* info)
{
    if (!info || info->countActionSets < 1) return XR_ERROR_VALIDATION_FAILURE;
    ActionSetRec* set = (ActionSetRec*)info->actionSets[0];
    if (!set) return XR_ERROR_HANDLE_INVALID;
    if (!InputShim_Attach(set, g_setActions.data(), (int)g_setActions.size()))
    {
        // Attach "succeeds" so the mod keeps running; every action just reads
        // inactive, which the mod already handles by publishing a neutral pad.
        SLOG("!!! input: attach degraded - controllers will be inactive");
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- sync/state
OVRSHIM_FN(shim_SyncActions)(XrSession, const XrActionsSyncInfo* info)
{
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_inputReady) return XR_SUCCESS;

    if (g_vr.ovl && g_vr.ovl->IsDashboardVisible())
        return XR_SESSION_NOT_FOCUSED;

    vr::VRActiveActionSet_t as = {};
    as.ulActionSet = ((ActionSetRec*)info->activeActionSets[0].actionSet)->vrHandle;
    as.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    as.nPriority = 0;
    const EVRInputError ie = g_vr.input->UpdateActionState(
        (VRActiveActionSet_t*)&as, sizeof(vr::VRActiveActionSet_t), 1);
    if (ie != EVRInputError_VRInputError_None)
    {
        static EVRInputError last = EVRInputError_VRInputError_None;
        if (ie != last) { last = ie; SLOG("!!! input: UpdateActionState -> %d", (int)ie); }
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetActionStateBoolean)(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState = XR_FALSE;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputDigitalActionData_t d = {};
    if (g_vr.input->GetDigitalActionData(a->vrHandle, (InputDigitalActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState = d.bState ? XR_TRUE : XR_FALSE;
        out->changedSinceLastSync = d.bChanged ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetActionStateFloat)(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState = 0.f;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputAnalogActionData_t d = {};
    if (g_vr.input->GetAnalogActionData(a->vrHandle, (InputAnalogActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState = d.x;
        out->changedSinceLastSync = (d.deltaX != 0.f) ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
        return XR_SUCCESS;
    }

    // Digital fallback (delta vs donor): a button bound to a vector1 action
    // (Vive/WMR grip -> the mod's FLOAT grip actions) reads inactive through
    // the analog path on some SteamVR versions. Convert 0/1 here so the
    // weapon/plasmid radials still open - the donor documented this as the
    // "WMR radials never open" hazard and shipped it open.
    vr::InputDigitalActionData_t dd = {};
    if (g_vr.input->GetDigitalActionData(a->vrHandle, (InputDigitalActionData_t*)&dd,
            sizeof(dd), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && dd.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState = dd.bState ? 1.f : 0.f;
        out->changedSinceLastSync = dd.bChanged ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetActionStateVector2f)(
    XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    out->currentState.x = out->currentState.y = 0.f;
    out->changedSinceLastSync = XR_FALSE;
    out->lastChangeTime = 0;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputAnalogActionData_t d = {};
    if (g_vr.input->GetAnalogActionData(a->vrHandle, (InputAnalogActionData_t*)&d,
            sizeof(d), vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && d.bActive)
    {
        out->isActive = XR_TRUE;
        out->currentState.x = d.x;
        out->currentState.y = d.y;
        out->changedSinceLastSync = (d.deltaX != 0.f || d.deltaY != 0.f) ? XR_TRUE : XR_FALSE;
        out->lastChangeTime = g_st.lastPredictedTime;
    }
    return XR_SUCCESS;
}

// NEW vs donor: the mod's locate_hand refuses to locate a hand whose pose
// action is not active, so without this the hands never appear. Called four
// times per frame - GetPoseActionDataForNextFrame is a cheap local read.
OVRSHIM_FN(shim_GetActionStatePose)(
    XrSession, const XrActionStateGetInfo* gi, XrActionStatePose* out)
{
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)gi->action;
    out->isActive = XR_FALSE;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;

    vr::InputPoseActionData_t pd = {};
    if (g_vr.input->GetPoseActionDataForNextFrame(a->vrHandle,
            ETrackingUniverseOrigin_TrackingUniverseStanding,
            (InputPoseActionData_t*)&pd, sizeof(pd),
            vr::k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None
        && pd.bActive)
        out->isActive = XR_TRUE;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- haptics
// The mod does not call this today, but the GIPA-based dispatch makes it free
// to keep (in the donor's static-import model a MISSING export here stopped
// the whole mod from loading - error 127, 2026-08-12).
//
// OpenXR gives nanoseconds and may ask for XR_FREQUENCY_UNSPECIFIED; SteamVR
// wants seconds and a real frequency. 160 Hz is the mid-band both Touch and
// Index reproduce well. XR_MIN_HAPTIC_DURATION (-1) -> 40 ms.
OVRSHIM_FN(shim_ApplyHapticFeedback)(
    XrSession, const XrHapticActionInfo* hi, const XrHapticBaseHeader* header)
{
    if (!hi || !header) return XR_ERROR_VALIDATION_FAILURE;
    ActionRec* a = (ActionRec*)hi->action;
    if (!a || !a->vrHandle || !g_inputReady) return XR_SUCCESS;
    if (header->type != XR_TYPE_HAPTIC_VIBRATION) return XR_ERROR_VALIDATION_FAILURE;

    const XrHapticVibration* v = (const XrHapticVibration*)header;

    float seconds = (v->duration == XR_MIN_HAPTIC_DURATION)
        ? 0.04f : (float)((double)v->duration / 1e9);
    if (seconds < 0.01f) seconds = 0.01f;
    if (seconds > 2.0f)  seconds = 2.0f;      // a stuck buzz is worse than none

    float freq = v->frequency;
    if (freq == XR_FREQUENCY_UNSPECIFIED || freq <= 0.f) freq = 160.0f;

    float amp = v->amplitude;
    if (amp < 0.f) amp = 0.f;
    if (amp > 1.f) amp = 1.f;

    const EVRInputError ie = g_vr.input->TriggerHapticVibrationAction(
        a->vrHandle, 0.0f, seconds, freq, amp, vr::k_ulInvalidInputValueHandle);

    static bool told = false;
    if (!told)
    {
        told = true;
        SLOG("input: first haptic pulse -> %d  (%.0f Hz, %.2f amp, %.0f ms). "
             "Result 0 means SteamVR ACCEPTED it; if nothing is felt the binding "
             "for /actions/gameplay/out/* is not live on this controller.",
             (int)ie, freq, amp, seconds * 1000.0f);
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- locate
static bool SpacePoseInOrigin(SpaceRec* s, M34* out)
{
    switch (s->kind)
    {
    case SPACE_REF_LOCAL:
        *out = M34_Identity();
        return true;
    case SPACE_REF_VIEW:
        if (!g_st.hmdValid) return false;
        *out = g_st.hmd;
        return true;
    case SPACE_ACTION:
    {
        if (!s->action || !s->action->vrHandle || !g_inputReady || !g_st.haveOrigin)
            return false;
        vr::InputPoseActionData_t pd = {};
        const EVRInputError ie = g_vr.input->GetPoseActionDataForNextFrame(
            s->action->vrHandle, ETrackingUniverseOrigin_TrackingUniverseStanding,
            (InputPoseActionData_t*)&pd, sizeof(pd), vr::k_ulInvalidInputValueHandle);
        if (ie != EVRInputError_VRInputError_None || !pd.bActive || !pd.pose.bPoseIsValid)
            return false;
        *out = M34_Mul(g_st.originInv, M34_FromVr(pd.pose.mDeviceToAbsoluteTracking));
        return true;
    }
    }
    return false;
}

OVRSHIM_FN(shim_LocateSpace)(
    XrSpace space, XrSpace base, XrTime, XrSpaceLocation* out)
{
    if (!space || !base || !out) return XR_ERROR_VALIDATION_FAILURE;
    out->locationFlags = 0;
    out->pose.orientation = { 0, 0, 0, 1 };
    out->pose.position = { 0, 0, 0 };

    M34 t, b;
    if (!SpacePoseInOrigin((SpaceRec*)space, &t)) return XR_SUCCESS;
    if (!SpacePoseInOrigin((SpaceRec*)base, &b)) return XR_SUCCESS;

    const M34 rel = M34_Mul(M34_InvRigid(b), t);
    float q[4], p[3];
    M34_ToQuatPos(rel, q, p);
    out->pose.orientation = { q[0], q[1], q[2], q[3] };
    out->pose.position = { p[0], p[1], p[2] };
    out->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}
