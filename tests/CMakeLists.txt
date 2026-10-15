# Tests.
#   tests/unit          GPU-free Catch2 tests (label "unit"), executable rv_tests (no SDL, no OpenGL)
#   tests/platform      SDL platform layer tests (label "unit"), executable rv_platform_tests (SDL, no OpenGL)
#   tests/support       shared helpers (temporary directories, log capture)
#   tests/input_scripts --input-script files of the verification runs, also played by tests/unit/integration
#   tests/smoke         GPU smoke tests (label "gpu"), executable rv_smoke_tests: runs retroverse.exe and the asset
#                       preview frame-limited and checks their run reports and screenshots; excluded from the default
#                       test presets (msvc-x64-debug-gpu and msvc-x64-release-gpu run them)
#   tests/cmake         tests of the build's CMake scripts (label "unit"), run by ctest in CMake script mode: the
#                       Release asset deployment (cmake/CopyAssets.cmake)

add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(platform)
add_subdirectory(smoke)
add_subdirectory(cmake)
