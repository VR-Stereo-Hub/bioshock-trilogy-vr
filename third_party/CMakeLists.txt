if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/minhook/CMakeLists.txt")
    message(FATAL_ERROR
        "Submodules missing. Run:\n  git submodule update --init --recursive")
endif()

# --- MinHook (BSD-2-Clause) ---------------------------------------------------
add_subdirectory(minhook)

# --- Dear ImGui (MIT) — no upstream CMakeLists; hand-rolled static lib --------
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC imgui imgui/backends)

# --- OpenXR loader (Apache-2.0) -----------------------------------------------
# Statically linked (DYNAMIC_LOADER off) so we ship no extra loader DLL.
option(BIOSHOCKVR_WITH_OPENXR "Build the OpenXR loader" ON)
if(BIOSHOCKVR_WITH_OPENXR)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    add_subdirectory(OpenXR-SDK)
    # Upstream forces dynamic CRT for static-lib builds; match our /MT so the
    # loader links into the mod DLL and test exes without CRT mismatch.
    set_target_properties(openxr_loader PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
