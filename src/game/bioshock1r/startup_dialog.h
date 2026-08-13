#pragma once
// Suppress the "revert Options?" modal the game raises after an unclean exit.
//
// WHY THIS EXISTS. Any force-kill - and, more to the point, any CRASH - makes
// the next launch open a `#32770` "Message" box asking whether to revert the
// video options. It blocks the first Present entirely (the game reports alive
// with presents=0/s until it is dismissed), and answering it needs a mouse or
// keyboard. That is fine at a desk and useless in a headset: the player is
// already wearing the HMD, the game is not rendering to it yet, and the only
// way forward is to take the headset off.
//
// The mod's own harness has always dismissed it externally (`boot.ps1` clicks
// the `&No` button through `BM_CLICK`), which is the proof that answering No is
// both correct and safe - this just moves that answer inside the process, where
// it works whether or not a harness is running.
//
// SCOPE. A watcher thread, only inside a startup window measured from mod init,
// that clicks a button only if it says No, and logs every dismissal. It does
// NOT hook the dialog APIs: MessageBoxA/W were hooked first and measured never
// to fire for this prompt (they armed 137 ms into the process, long before the
// box appears), so the window itself is the reliable handle on it.
// `vrpopup off` restores the stock behaviour for the rest of the session.

namespace bvr::b1r::startup_dialog {

// Start the watcher thread. Fail-soft: a failed thread create logs and leaves
// the stock dialog alone. Call once from the adapter's init.
void init();

bool enabled();
void set_enabled(bool on);

// "vrpopup on|off|status"
void handle_command(const char* args);

} // namespace bvr::b1r::startup_dialog
