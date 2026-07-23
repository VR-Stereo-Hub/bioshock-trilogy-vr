#pragma once

namespace bvr::log {

// Opens %LOCALAPPDATA%\BioshockVR\bioshockvr.log (truncated per run).
// Safe to call write() before init(); those lines go to OutputDebugString only.
void init();
void write(const char* fmt, ...);
const wchar_t* data_dir(); // %LOCALAPPDATA%\BioshockVR, created on init

} // namespace bvr::log

#define BVR_LOG(...) ::bvr::log::write(__VA_ARGS__)
