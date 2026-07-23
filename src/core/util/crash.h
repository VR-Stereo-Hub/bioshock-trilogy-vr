#pragma once

namespace bvr::crash {

// Installs an unhandled-exception filter that writes a minidump to
// %LOCALAPPDATA%\BioshockVR\crash\ and then chains to the previous filter
// (the game installs its own dbghelp-based handler).
void install();

} // namespace bvr::crash
