#pragma once
// s91: WHAT DOES THE ENGINE ATTACH TO THE HANDS? (`vrhands attach`, log only)
//
// The palette route draws the hands where the controllers are while the engine's bones stay the
// game's, so anything the ENGINE attaches to a hand bone - the held weapon's children (the Tommy
// Gun's drum magazine), muzzle flashes, the plasmid emitters (Hands.uc DamageEmitterSocket, the
// effects subsystem's AttachToSource + AttachmentBone) - is placed from the game's hand, not the
// drawn one. This lists them: every actor in the level that points at the hands actor or the held
// weapon (or at one of their children) and carries an attach name at +0xF0, with its class, the
// name, the hands bone that name resolves to, and the field that holds the link. It also logs the
// hands mesh instance's socket table and asks the palette gather which bones carry skin weight.

namespace bvr::b1r::attach_census {

// Game thread (the command seam). Logs the census; touches nothing.
void run();
// The spawn watch: snapshot the level's actors now, then for `seconds` log every NEW actor (class,
// where it sits against the hands and the left/right hand bones, its attach name, and which field
// links it to the hands, the pawn or the weapon). tick() runs it; called every CalcView.
void watch(float seconds);
// The actors attached to `hands` (linked at +0xAC or +0xB0, with an attach name at +0xF0), into out[].
// Cheap: guarded raw reads over the level's actor list, no per-actor validation calls. Game thread.
int collect_linked(void* hands, void** out, int cap);
// Every actor whose class name contains `text` (e.g. "Bolt", "Beam"), with location and links.
void find(const char* text);
void tick();

} // namespace bvr::b1r::attach_census
