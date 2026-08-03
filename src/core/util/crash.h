#pragma once

namespace bvr::crash {

// Installs an unhandled-exception filter that writes a minidump to
// %LOCALAPPDATA%\BioshockVR\crash\ and then chains to the previous filter
// (the game installs its own dbghelp-based handler), plus a vectored handler
// for the always-fatal codes that bypass the filter entirely.
void install();

// Re-installs our unhandled-exception filter if something else displaced it.
// SetUnhandledExceptionFilter is global last-writer-wins, and we install at DLL
// attach - the game's own handler, the Steam overlay and the 2K SDK all install
// later. Session 23: an external tester's crash produced NO dump and NO log
// line, which is exactly what a displaced filter looks like. Cheap; call from
// the present loop. Logs once when it actually had to re-arm.
void rearm();

// The host window has begun closing (WM_CLOSE/WM_DESTROY/WM_ENDSESSION seen).
// After this, a fault is treated as the host's own exit-path bug (session 38:
// BS2 faults at Bioshock2HD.exe+0x4FF0FE on EVERY close, hook-free-proven):
// one log line, no minidump, immediate TerminateProcess - which is faster and
// quieter than letting the game's chained filter retry the faulting
// instruction for seconds. Idempotent; logs once.
void note_teardown(const char* why);

// True once note_teardown has run. Cheap atomic read - adapters use it to
// park their own machinery (doubled draw, engine-field writes) per frame.
bool teardown_seen();

} // namespace bvr::crash
