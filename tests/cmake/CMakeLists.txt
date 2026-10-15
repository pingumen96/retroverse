# Tests of the build's own CMake scripts (label "unit"), run by ctest in CMake script mode.
#   unit.cmake/copy_assets  cmake/CopyAssets.cmake mirrors an asset tree (the Release asset deployment)

add_test(NAME "unit.cmake/copy_assets: the Release asset deployment mirrors the source tree"
    COMMAND "${CMAKE_COMMAND}"
        "-DCOPY_ASSETS=${PROJECT_SOURCE_DIR}/cmake/CopyAssets.cmake"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/copy_assets"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_copy_assets.cmake")
set_tests_properties("unit.cmake/copy_assets: the Release asset deployment mirrors the source tree"
    PROPERTIES LABELS unit)
