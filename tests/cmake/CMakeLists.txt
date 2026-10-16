# Tests run by ctest in CMake script mode (label "unit"): the build's own CMake scripts, and the executables whose
# contract shows only from outside the process.
#   unit.cmake/copy_assets  cmake/CopyAssets.cmake mirrors an asset tree (the Release asset deployment)
#   unit.cmake/cli_process  retroverse-cli as a process: exit codes, results on stdout, usage errors and logs on stderr

add_test(NAME "unit.cmake/copy_assets: the Release asset deployment mirrors the source tree"
    COMMAND "${CMAKE_COMMAND}"
        "-DCOPY_ASSETS=${PROJECT_SOURCE_DIR}/cmake/CopyAssets.cmake"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/copy_assets"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_copy_assets.cmake")
set_tests_properties("unit.cmake/copy_assets: the Release asset deployment mirrors the source tree"
    PROPERTIES LABELS unit)

# retroverse-cli exists with RV_BUILD_TOOLS. Its asset root is found as a user's run finds it, so RV_ASSET_ROOT is
# left out of the test's environment.
if(TARGET retroverse-cli)
    set(cli_process_test "unit.cmake/cli_process: retroverse-cli keeps its output contract as a process")
    add_test(NAME "${cli_process_test}"
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:retroverse-cli>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli_process"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_process.cmake")
    set_tests_properties("${cli_process_test}" PROPERTIES
        LABELS unit
        ENVIRONMENT_MODIFICATION "RV_ASSET_ROOT=unset:")
endif()
