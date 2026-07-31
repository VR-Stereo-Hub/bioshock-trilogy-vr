#include "game/adapter_registry.h"

#include "core/util/log.h"
#include "game/bioshock1r/bioshock1r_adapter.h"
#include "game/bioshock2r/bioshock2r_adapter.h"
#include "game/bioshockinf/bioshockinf_adapter.h"
#include "game/igame_adapter.h"

#include <windows.h>

namespace bvr::game {
namespace {

IGameAdapter* g_adapter = nullptr;

const wchar_t* host_exe_basename() {
    static wchar_t path[MAX_PATH] = {};
    if (!path[0]) GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

} // namespace

HostGame detect_host_game() {
    static const HostGame cached = [] {
        const wchar_t* exe = host_exe_basename();
        if (_wcsicmp(exe, L"BioshockHD.exe") == 0) return HostGame::Bioshock1;
        if (_wcsicmp(exe, L"Bioshock2HD.exe") == 0) return HostGame::Bioshock2;
        if (_wcsicmp(exe, L"BioShockInfinite.exe") == 0) return HostGame::Infinite;
        return HostGame::Unknown;
    }();
    return cached;
}

const wchar_t* host_data_subdir() {
    // BioShock 1 keeps the released flat layout; only new games get subdirs.
    switch (detect_host_game()) {
    case HostGame::Bioshock2:
        return L"bs2";
    case HostGame::Infinite:
        return L"bsi";
    default:
        return L"";
    }
}

void init_adapter() {
    IGameAdapter* instance = nullptr;
    const char* name = "";
    switch (detect_host_game()) {
    case HostGame::Bioshock1:
        instance = b1r::create_adapter();
        name = "bioshock1r";
        break;
    case HostGame::Bioshock2:
        instance = b2r::create_adapter();
        name = "bioshock2r";
        break;
    case HostGame::Infinite:
        instance = bsi::create_adapter();
        name = "bioshockinf";
        break;
    case HostGame::Unknown:
        break;
    }
    if (!instance) {
        BVR_LOG("[registry] no adapter for host %ls - game runs flat", host_exe_basename());
        return;
    }
    BVR_LOG("[registry] host %ls -> %s adapter", host_exe_basename(), name);

    pattern_scan::ProcessImage image{};
    if (!pattern_scan::capture_main_module(image)) {
        BVR_LOG("[registry] capture_main_module failed - engine features disabled, game runs flat");
    } else {
        instance->init(image); // fail-soft: failure paths log internally
    }
    // Published even on failure so the overlay can show "scan FAILED".
    // init_adapter() runs on the init thread before the D3D11 hooks install,
    // so the overlay cannot observe a half-initialized adapter.
    g_adapter = instance;
}

IGameAdapter* adapter() {
    return g_adapter;
}

} // namespace bvr::game
