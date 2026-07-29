#pragma once

namespace bvr::log {

// Opens <data_dir>\bioshockvr.log (truncated per run). subdir is an optional
// per-game folder under %LOCALAPPDATA%\BioshockVR supplied by the game layer
// (game/adapter_registry.cpp); null/empty keeps the flat layout BioShock 1
// shipped with - those paths must stay byte-identical across releases.
// Safe to call write() before init(); those lines go to OutputDebugString only.
void init(const wchar_t* subdir = nullptr);
void write(const char* fmt, ...);
const wchar_t* data_dir(); // %LOCALAPPDATA%\BioshockVR[\<subdir>], created on init

} // namespace bvr::log

#define BVR_LOG(...) ::bvr::log::write(__VA_ARGS__)
