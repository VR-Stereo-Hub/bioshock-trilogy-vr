# Regenerates bvr_version.h so the logged version can never drift from the tree.
# Run via `cmake -P` from a build-time custom target with BVR_VERSION, BVR_SRC,
# BVR_IN and BVR_OUT defined.
#
# Writes through a temp file and copy_if_different so an unchanged git state does
# not force a rebuild of every TU that includes the header.

set(BVR_BUILD_ID "nogit")

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${BVR_SRC}"
        OUTPUT_VARIABLE _describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _describe)
        set(BVR_BUILD_ID "${_describe}")
    endif()
endif()

configure_file("${BVR_IN}" "${BVR_OUT}.tmp" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BVR_OUT}.tmp" "${BVR_OUT}")
file(REMOVE "${BVR_OUT}.tmp")
