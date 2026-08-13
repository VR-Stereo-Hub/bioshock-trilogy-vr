#pragma once
// s52 (I9): the Infinite HUD lane - arms core's gfx_hud (the GFx positional
// classifier, DR-I7) and owns its policy: the classifier runs whenever the
// lane is on, but the REDIRECT (HUD off the backbuffer, onto the quad) only
// engages while an XR session is live - a flat boot keeps its window HUD, so
// game-shot menu verification and non-VR launches never lose the interface.
//
// Derivations behind the classifier: ENGINE_NOTES "s52 part 3" (the HUD run =
// ret 0x492284 DrawIndexed + 0x4920FF Draw into the backbuffer AFTER the
// tonemap blit; the blit = the only full-screen depth-free a=6 there).

namespace bvr::bsi::hud {

// Adapter init: arm the classifier + register the quad texture provider.
void init();

// Game-thread tick (camera detour block): syncs the redirect gate with the
// XR session's liveness.
void tick();

// `bsihud on|off|status|redirect on|off`. False when not ours.
bool handle_command(const char* cmd, const char* args);

void draw_debug_ui();

} // namespace bvr::bsi::hud
