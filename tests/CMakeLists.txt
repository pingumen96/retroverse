# Tests.
#   tests/unit          GPU-free Catch2 tests (label "unit"), executable rv_tests (no SDL, no OpenGL)
#   tests/platform      SDL platform layer tests (label "unit"), executable rv_platform_tests (SDL, no OpenGL)
#   tests/support       shared helpers (temporary directories, log capture)
#   tests/input_scripts --input-script files of the verification runs, also played by tests/unit/integration
#   tests/smoke         GPU smoke tests (label "gpu"), excluded from the default test preset

add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(platform)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/smoke/CMakeLists.txt")
    add_subdirectory(smoke)
endif()
