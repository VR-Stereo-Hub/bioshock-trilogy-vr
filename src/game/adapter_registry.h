#pragma once
// Multi-game dispatch: which adapter runs is decided here, by host exe name.
// Host detection is split from adapter construction because framework init
// needs the per-game data-dir subfolder BEFORE the log exists, so
// detect_host_game()/host_data_subdir() must never log.

namespace bvr::game {

enum class HostGame {
    Bioshock1, // BioshockHD.exe
    Bioshock2, // Bioshock2HD.exe
    Infinite,  // BioShockInfinite.exe (UE3 - a different engine entirely)
    Unknown,
};

// Cached exe-basename match. Safe to call before log::init - never logs.
HostGame detect_host_game();

// Data-dir subfolder under %LOCALAPPDATA%\BioshockVR for this host. Empty for
// BioShock 1 (released flat layout - those paths must stay byte-identical) and
// for unknown hosts; L"bs2" for BioShock 2, L"bsi" for Infinite. The exe-name
// knowledge lives only in this module - core stays game-agnostic.
const wchar_t* host_data_subdir();

} // namespace bvr::game
