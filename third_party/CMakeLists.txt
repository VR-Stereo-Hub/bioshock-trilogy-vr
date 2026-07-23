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

# --- OpenXR loader (Apache-2.0) — wired on demand -----------------------------
# The loader is only needed from M2 (headset bring-up) onward. Enable with
# -DBIOSHOCKVR_WITH_OPENXR=ON once tools/xr-hello32 (DR-1) has validated 32-bit
# runtime support.
option(BIOSHOCKVR_WITH_OPENXR "Build the OpenXR loader" OFF)
if(BIOSHOCKVR_WITH_OPENXR)
    set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    add_subdirectory(OpenXR-SDK)
endif()
