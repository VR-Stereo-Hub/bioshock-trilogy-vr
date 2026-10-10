// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/process.h - the game process, elevation and launching.
#pragma once
#include <windows.h>
#include <string>

namespace bvr::launcher::process {

// Toolhelp snapshot by image name. Unknown (the snapshot failed) must be
// treated like Running by every caller: a check that cannot fail its own
// hypothesis is not a check.
enum class Running { No, Yes, Unknown };
Running is_running(const wchar_t* exeName);

bool is_elevated();
// Relaunches this exe elevated with `args`, waits, returns its exit code.
// false with err == ERROR_CANCELLED when the UAC prompt was declined.
bool run_self_elevated_wait(const std::wstring& args, DWORD* exitCode, DWORD* err);
// Runs a command line (no window), waits, returns its exit code.
bool run_wait(const std::wstring& cmdline, DWORD* exitCode, DWORD* err, DWORD timeoutMs = INFINITE,
              const std::wstring& stdoutFile = L"");   // stdout+stderr captured to this file when given
// Opens a URL or file with the shell. From an elevated process it goes through
// the desktop shell's own Automation object so the target (Steam, a browser)
// runs with the user's normal token; false when that route is unavailable.
bool open_unelevated(const std::wstring& target, const std::wstring& args = L"", const std::wstring& directory = L"");
bool open(const std::wstring& target);       // plain ShellExecute (fine when not elevated)
std::wstring quote_arg(const std::wstring& a);
bool attach_parent_console();
// Creates a per-user .lnk; no launch or elevation. The caller chooses its location.
bool write_shortcut(const std::wstring& linkPath, const std::wstring& target,
                    const std::wstring& args, DWORD* err);

} // namespace bvr::launcher::process
