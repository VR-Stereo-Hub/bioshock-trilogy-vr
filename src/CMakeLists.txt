# --- xinput1_3.dll proxy shim -------------------------------------------------
# Thin forwarder that the game loads in place of the real xinput1_3.dll; it
# forwards every export to the SysWOW64 original and loads bioshockvr.dll.
add_library(xinput_proxy SHARED
    proxy/xinput_proxy.cpp
    proxy/xinput_proxy.def
)
set_target_properties(xinput_proxy PROPERTIES
    OUTPUT_NAME "xinput1_3"
    PREFIX ""
)

# --- bioshockvr.dll — the mod -------------------------------------------------
add_library(bioshockvr SHARED
    core/framework/dllmain.cpp
    core/framework/framework.cpp
    core/framework/framework.h
    core/hooks/d3d11_hook.cpp
    core/hooks/d3d11_hook.h
    core/hooks/pattern_scan.cpp
    core/hooks/pattern_scan.h
    core/ui/overlay.cpp
    core/ui/overlay.h
    core/util/log.cpp
    core/util/log.h
    core/util/crash.cpp
    core/util/crash.h
    game/igame_adapter.h
    game/bioshock1r/bioshock1r_adapter.cpp
    game/bioshock1r/bioshock1r_adapter.h
    game/bioshock1r/patterns.cpp
    game/bioshock1r/patterns.h
)
set_target_properties(bioshockvr PROPERTIES PREFIX "")
target_include_directories(bioshockvr PRIVATE .)
target_link_libraries(bioshockvr PRIVATE
    minhook
    imgui
    d3d11
    dbghelp
)

# --- xr_hello32 - DR-1 test client --------------------------------------------
# Standalone 32-bit console app that proves (or disproves) that this machine's
# OpenXR runtimes accept a 32-bit D3D11 client. Not shipped with the mod.
if(BIOSHOCKVR_WITH_OPENXR)
    add_executable(xr_hello32 tools/xr_hello32/main.cpp)
    target_link_libraries(xr_hello32 PRIVATE openxr_loader d3d11 dxgi)
endif()
