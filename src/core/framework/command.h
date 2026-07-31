#pragma once
// The command-file seam, owned by core.
//
// WHY THIS IS CORE (session 35). On BioShock 1 and 2 the poller lives in the
// ADAPTER and ticks off the camera hook, so a skeleton adapter has no command
// surface at all until its first engine hook fires - the only way to talk to
// the mod is the thing that is not working yet. That circular dependency made
// the early sessions on both games materially harder. Ticked from Present
// instead, a brand-new adapter can be driven from frame one, before a single
// byte of engine knowledge exists.
//
// PUMPS. Whoever calls poll_*() owns the thread the commands run on:
//   - poll_from_present()     - the render thread. For adapters with no engine
//                               hook yet. OPT-IN: no-op until an adapter calls
//                               enable_present_pump(), so an adapter that polls
//                               for itself can never get a second poller racing
//                               its own.
//   - poll_from_game_thread() - the game thread, from an adapter's engine hook.
//                               The first call latches the handover: from then
//                               on the game thread is the pump, because
//                               engine-touching commands belong there.
//
// THE LEASE (revised session 36). The handover used to silence the Present pump
// permanently, reasoning that a "resume on stall" rule would hand the render
// thread a dispatch exactly during a load - the worst moment available. That
// hazard is real but it was stated too broadly: what must not happen during a
// load is an ENGINE-TOUCHING dispatch, not any dispatch. As written, a camera
// hook that went quiet (level load, Scaleform menu, scripted camera) left the
// mod with no command surface at all and no line saying why.
//
// So the handover is now a lease. If the game thread has not called in for
// kGamePumpLeaseMs, the Present pump resumes in DEGRADED mode: the commands
// that write engine memory (mempoke*, pokeaddr*, memrestore) are refused with
// one explanatory line, and everything else - reads, adapter commands, vrcmd,
// vroverlay - dispatches normally. The game thread reclaims the pump on its
// next call. Both transitions are logged.
//
// This also made the handover testable on demand, which the original was not:
// silence the hook, wait out the lease, and `vrcmd` must report the degraded
// render pump. An instrument that cannot be made to fail is not evidence.
//
// THREAD WARNING. While the Present pump owns the poller, every command runs on
// the RENDER thread. The mem*/fsweep scans are multi-second in the worst case,
// so there they stall presents rather than the game thread. That is a
// diagnostic-only cost and it is logged (the pump identity is in the dispatch
// line), not prevented.
//
// BS1/BS2 are deliberately untouched this session: they keep their own pollers
// and their own copies of the vocabulary below, and fold into this module in a
// later consolidation pass. The copy here is the canonical one.

#include <cstdint>

namespace bvr::command {

// Arm the Present-thread pump. Called by an adapter that has no engine hook to
// poll from. Idempotent.
void enable_present_pump();

// 1 Hz poll of <data_dir>\command.txt. Both are safe to call every frame.
void poll_from_present(uint64_t nowMs);
void poll_from_game_thread(uint64_t nowMs);

// True while the Present pump has taken back over because the game thread went
// silent. Engine-writing commands are refused in this state.
bool degraded();

// Dispatch one line: adapter first (so a game may deliberately shadow a core
// command), then the shared vocabulary, else one unknown-command line.
void dispatch_line(const char* line);

// The shared vocabulary - every branch is a pure forward into a core module:
//   value_scan: memscan/memrescan/memscani/memrescani <v>  memlist [n]
//               memread <idx>  mempoke <idx|lo-hi> <f>  mempokei ...<u>
//               memrestore  memptr <idx> [maxDeltaHex]  pokeaddr <hex> <f>
//               pokeaddri <hex> <u>  hexdump <hex> [len]  strscan <text>
//               membases  fsweep <hexaddr> <len> <lo> <hi>
//   frame:      dumpframe [full] [n]
//   input:      vrinput <args>
//   vr:         vrpace <args>  vrmirror <args>  vrcine <args>
//   ui:         vroverlay on|off
//   hud:        vrhud on|off|force on|force off|status
//   self:       vrcmd (pump owner, counters, data dir)
// Returns false when the command is not one of these.
bool core_command(const char* cmd, const char* args);

} // namespace bvr::command
