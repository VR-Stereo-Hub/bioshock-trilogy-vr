// Engine console-command execution without the (dead) Tab console: the seam
// calls the engine's own Exec-chain entries directly with a stub
// FOutputDevice. `exec <cmd>` enters at UWindowsViewport::Exec, `execc <cmd>`
// at UWindowsClient::Exec - two lanes so chain-forwarding gaps can be probed
// per link. Game thread only (called from the command seam inside the
// CalcView detour), SEH-guarded, fail-soft.

#pragma once

namespace bvr::b1r::console_exec {

// args = the raw command text after the seam verb.
void run_viewport(const char* args);
void run_client(const char* args);
void run_engine(const char* args); // UGameEngine::Exec - forwards to script

} // namespace bvr::b1r::console_exec
