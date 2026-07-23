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
    core/ui/overlay.cpp
    core/ui/overlay.h
    core/util/log.cpp
    core/util/log.h
    core/util/crash.cpp
    core/util/crash.h
)
set_target_properties(bioshockvr PROPERTIES PREFIX "")
target_include_directories(bioshockvr PRIVATE .)
target_link_libraries(bioshockvr PRIVATE
    minhook
    imgui
    d3d11
    dbghelp
)
