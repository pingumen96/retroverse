# rv_test_support: helpers shared by the test executables. GPU-free: temporary folders, log capture, fixtures
# materialized under the build tree (tests/fixtures), a program stub, a child process runner (stdout and stderr apart,
# its command line built by rv_launcher), a fake system probe, junctions and folders that cannot be listed.
# Sources are globbed so independent work streams can add helpers without editing this file.

file(GLOB RV_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(rv_test_support STATIC ${RV_TEST_SUPPORT_SOURCES})
rv_set_target_defaults(rv_test_support)
# Tests include helpers as "support/<file>.hpp".
target_include_directories(rv_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# rv_platform_win: the environment of this process for the child runner (and, through it, rv_launcher's quoting).
target_link_libraries(rv_test_support PUBLIC rv_core rv_launcher rv_platform_win)
# The fixtures' sources and the build tree they are materialized in.
target_compile_definitions(rv_test_support PRIVATE
    RV_TEST_SUPPORT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_TEST_SUPPORT_BUILD_DIR="${CMAKE_BINARY_DIR}")
rv_forbid_link(rv_test_support)
