# Standalone native launcher with per-configuration embedded payloads. Its
# renderer and F10 compile the same theme source and original art resources.
enable_language(RC)
set(BVR_LAUNCHER_ICON "${PROJECT_SOURCE_DIR}/assets/ui/launcher/icons/bioshock-vr-medallion.ico" CACHE FILEPATH "Launcher icon selected for this review build")
set(_payload "${CMAKE_BINARY_DIR}/launcher-payload/$<CONFIG>")
set(_rc "${CMAKE_BINARY_DIR}/launcher-payload/payload.rc")
set(_openvr "${PROJECT_SOURCE_DIR}/third_party/openvr_headers/bin/win32/openvr_api.dll")
file(SHA256 "${_openvr}" _openvr_hash)
file(READ "${PROJECT_SOURCE_DIR}/third_party/openvr_headers/PROVENANCE.txt" _provenance)
string(FIND "${_provenance}" "${_openvr_hash}" _pin)
if(_pin EQUAL -1)
    message(FATAL_ERROR "OpenVR loader does not match its recorded provenance")
endif()
set(_copy_commands "")
set(_copy_deps "")
foreach(_file IN ITEMS vrpreset.ini hands.ini weapons.ini)
    set(_source "${PROJECT_SOURCE_DIR}/release/preset-bs1/${_file}")
    list(APPEND _copy_commands COMMAND ${CMAKE_COMMAND} -E copy "${_source}" "${_payload}/${_file}")
    list(APPEND _copy_deps "${_source}")
endforeach()
add_custom_command(OUTPUT "${_rc}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_payload}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:bioshockvr>" "${_payload}/bioshockvr.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:xinput_proxy>" "${_payload}/xinput1_3.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:bvr_ovr_shim>" "${_payload}/bvr_steamvr32.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${_openvr}" "${_payload}/openvr_api.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/tools/launcher-collect-support.ps1" "${_payload}/collect-support.ps1"
    ${_copy_commands}
    # Plain copy advances rc.exe's timestamp when a payload changes.
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/payload.rc" "${_rc}"
    DEPENDS bioshockvr xinput_proxy bvr_ovr_shim "${_openvr}" ${_copy_deps}
        "${PROJECT_SOURCE_DIR}/tools/launcher-collect-support.ps1" payload.rc
    VERBATIM)
configure_file(version.rc.in "${CMAKE_BINARY_DIR}/launcher-payload/version.rc" @ONLY)
set(_json "${PROJECT_SOURCE_DIR}/third_party/OpenXR-SDK/src/external/jsoncpp")
set(_common
    model/installer.cpp model/settings.cpp sys/discovery.cpp
    sys/fs.cpp sys/process.cpp sys/gpu.cpp sys/game_ini.cpp sys/updates.cpp sys/resources.cpp sys/support.cpp
    "${PROJECT_SOURCE_DIR}/src/game/bioshock1r/menu_model.cpp"
    "${PROJECT_SOURCE_DIR}/src/game/bioshock1r/menu_preferences.cpp"
    "${_json}/src/lib_json/json_reader.cpp" "${_json}/src/lib_json/json_value.cpp" "${_json}/src/lib_json/json_writer.cpp")
add_library(bvr_launcher_model STATIC ${_common})
target_include_directories(bvr_launcher_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated" "${_json}/include")
target_compile_definitions(bvr_launcher_model PUBLIC UNICODE _UNICODE BVR_LAUNCHER_CONFIG="$<CONFIG>")
target_link_libraries(bvr_launcher_model PUBLIC user32 gdi32 d3d11 dxgi shell32 ole32 oleaut32 uuid comctl32 advapi32 bcrypt winhttp version)
add_dependencies(bvr_launcher_model bvr_version)
add_executable(bvr_launcher WIN32 main.cpp app/window.cpp app/render.cpp ui/screens.cpp model/fixtures.cpp
    "${PROJECT_SOURCE_DIR}/src/game/bioshock1r/menu_theme.cpp"
    "${CMAKE_BINARY_DIR}/generated/menu_resources.rc"
    "${CMAKE_BINARY_DIR}/launcher-payload/version.rc" "${_rc}" launcher.manifest)
target_include_directories(bvr_launcher PRIVATE "${_payload}")
target_link_libraries(bvr_launcher PRIVATE bvr_launcher_model imgui windowscodecs shlwapi dwmapi shcore delayimp)
target_link_options(bvr_launcher PRIVATE /DELAYLOAD:d3dcompiler_47.dll)
set_target_properties(bvr_launcher PROPERTIES OUTPUT_NAME "BioShockVR-Launcher-v${PROJECT_VERSION}" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
target_compile_options(bvr_launcher PRIVATE /W4 /permissive- /EHsc)
target_compile_options(bvr_launcher_model PRIVATE /W4 /permissive- /EHsc)

option(BIOSHOCKVR_WITH_LAUNCHER_TESTS "Build launcher host tests" ON)
if(BIOSHOCKVR_WITH_LAUNCHER_TESTS)
    add_executable(bvr_launcher_tests tests.cpp ${_common} "${_rc}")
    target_include_directories(bvr_launcher_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated" "${_json}/include" "${_payload}")
    target_compile_definitions(bvr_launcher_tests PRIVATE UNICODE _UNICODE BVR_LAUNCHER_TESTS=1 BVR_LAUNCHER_CONFIG="$<CONFIG>")
    target_link_libraries(bvr_launcher_tests PRIVATE user32 gdi32 d3d11 dxgi shell32 ole32 oleaut32 uuid comctl32 advapi32 bcrypt winhttp version)
endif()
