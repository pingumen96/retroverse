# Tests.
#   tests/unit    GPU-free Catch2 tests (label "unit"), executable rv_tests
#   tests/support shared helpers (temporary directories, in-memory GLB builder)
#   tests/smoke   GPU smoke tests (label "gpu"), excluded from the default test preset

add_subdirectory(support)
add_subdirectory(unit)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/smoke/CMakeLists.txt")
    add_subdirectory(smoke)
endif()
