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
//                               The first call latches the handover and silences
//                               the Present pump PERMANENTLY: engine-touching
//                               commands belong on the game thread, and a
//                               "resume on stall" rule would hand the render
//                               thread a dispatch exactly during a load, which
//                               is the worst moment available.
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
