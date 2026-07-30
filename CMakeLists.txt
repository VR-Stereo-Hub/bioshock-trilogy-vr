cmake_minimum_required(VERSION 3.21)

# VERSION is the single source of truth for the version the mod logs and shows.
# Bump it here and nowhere else; src/CMakeLists.txt generates bvr_version.h from
# it at build time, stamped with `git describe`.
project(BioshockVR VERSION 0.6.0 LANGUAGES C CXX)

# BioshockHD.exe is 32-bit x86. Everything we inject must be too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "This mod targets a 32-bit game. Configure with the 'win32' preset:\n"
        "  cmake --preset win32\n"
        "(or pass -G \"Visual Studio 17 2022\" -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: no VC redist dependency inside the game process.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

add_subdirectory(third_party)
add_subdirectory(src)
