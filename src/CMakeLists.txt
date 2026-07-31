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
    core/debug/value_scan.cpp
    core/debug/value_scan.h
    core/gfx/blit.cpp
    core/gfx/blit.h
    core/gfx/frame_inspector.cpp
    core/gfx/frame_inspector.h
    core/gfx/hud_capture.cpp
    core/gfx/hud_capture.h
    core/hooks/d3d11_hook.cpp
    core/hooks/d3d11_hook.h
    core/hooks/heap_scan.cpp
    core/hooks/heap_scan.h
    core/hooks/pattern_scan.cpp
    core/hooks/pattern_scan.h
    core/input/swing.cpp
    core/input/swing.h
    core/input/xinput_bridge.cpp
    core/input/xinput_bridge.h
    core/ui/overlay.cpp
    core/ui/overlay.h
    core/vr/openxr_input.cpp
    core/vr/openxr_input.h
    core/vr/openxr_runtime.cpp
    core/vr/openxr_runtime.h
    core/util/log.cpp
    core/util/log.h
    core/util/module_id.cpp
    core/util/module_id.h
    core/util/crash.cpp
    core/util/crash.h
    game/adapter_registry.cpp
    game/adapter_registry.h
    game/igame_adapter.h
    game/bioshock1r/aim.cpp
    game/bioshock1r/aim.h
    game/bioshock1r/bioshock1r_adapter.cpp
    game/bioshock1r/bioshock1r_adapter.h
    game/bioshock1r/body.cpp
    game/bioshock1r/body.h
    game/bioshock1r/bones.cpp
    game/bioshock1r/bones.h
    game/bioshock1r/camera.cpp
    game/bioshock1r/camera.h
    game/bioshock1r/console_exec.cpp
    game/bioshock1r/game_ini.cpp
    game/bioshock1r/game_ini.h
    game/bioshock1r/console_exec.h
    game/bioshock1r/frame_context.h
    game/bioshock1r/hands.cpp
    game/bioshock1r/hands.h
    game/bioshock1r/input_drive.cpp
    game/bioshock1r/input_drive.h
    game/bioshock1r/patterns.cpp
    game/bioshock1r/patterns.h
    game/bioshock1r/recorder.cpp
    game/bioshock1r/recorder.h
    game/bioshock1r/scenedraw.cpp
    game/bioshock1r/scenedraw.h
    game/bioshock2r/bioshock2r_adapter.cpp
    game/bioshock2r/bioshock2r_adapter.h
    game/bioshock2r/camera.cpp
    game/bioshock2r/camera.h
    game/bioshock2r/game_ini.cpp
    game/bioshock2r/game_ini.h
    game/bioshock2r/patterns.cpp
    game/bioshock2r/patterns.h
    game/bioshock2r/scenedraw.cpp
    game/bioshock2r/scenedraw.h
    game/shared/ue_math.h
)
set_target_properties(bioshockvr PROPERTIES PREFIX "")

# --- generated version header --------------------------------------------------
# Regenerated before every build of the mod, so `bioshockvr X.Y.Z (build id)` in
# the log always matches the tree it was built from. Session 23: the old
# hand-edited #define shipped "0.1.0" for v0.1.0, v0.2.0 AND v0.3.0, which made a
# user's crash log unattributable to a release.
set(BVR_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/bvr_version.h")
# Once at configure time so the header exists for a fresh tree and for IDE parsing.
execute_process(COMMAND ${CMAKE_COMMAND}
        -DBVR_VERSION=${PROJECT_VERSION}
        -DBVR_SRC=${PROJECT_SOURCE_DIR}
        -DBVR_IN=${PROJECT_SOURCE_DIR}/cmake/bvr_version.h.in
        -DBVR_OUT=${BVR_VERSION_HEADER}
        -P ${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake)
add_custom_target(bvr_version ALL
    BYPRODUCTS "${BVR_VERSION_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DBVR_VERSION=${PROJECT_VERSION}
            -DBVR_SRC=${PROJECT_SOURCE_DIR}
            -DBVR_IN=${PROJECT_SOURCE_DIR}/cmake/bvr_version.h.in
            -DBVR_OUT=${BVR_VERSION_HEADER}
            -P ${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake
    COMMENT "Stamping bvr_version.h (${PROJECT_VERSION} + git describe)"
    VERBATIM)
add_dependencies(bioshockvr bvr_version)

target_include_directories(bioshockvr PRIVATE . "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(bioshockvr PRIVATE
    minhook
    imgui
    d3d11
    d3dcompiler
    dbghelp
)
if(BIOSHOCKVR_WITH_OPENXR)
    # Static loader linked straight into the mod DLL (no extra DLL shipped).
    target_link_libraries(bioshockvr PRIVATE openxr_loader)
    target_compile_definitions(bioshockvr PRIVATE BVR_WITH_OPENXR)
endif()

# --- xr_hello32 - DR-1 test client --------------------------------------------
# Standalone 32-bit console app that proves (or disproves) that this machine's
# OpenXR runtimes accept a 32-bit D3D11 client. Not shipped with the mod.
if(BIOSHOCKVR_WITH_OPENXR)
    add_executable(xr_hello32 tools/xr_hello32/main.cpp)
    target_link_libraries(xr_hello32 PRIVATE openxr_loader d3d11 dxgi)
endif()
