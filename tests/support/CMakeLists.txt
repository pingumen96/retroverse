# rv_test_support: helpers shared by the test executables. GPU-free: temporary folders, log capture, fixtures
# materialized under the build tree (tests/fixtures), a program stub, a child process runner (stdout and stderr apart,
# its command line built by rv_launcher), a fake system probe, junctions and folders that cannot be listed, generated
# catalog databases.
# Sources are globbed so independent work streams can add helpers without editing this file.

file(GLOB RV_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(rv_test_support STATIC ${RV_TEST_SUPPORT_SOURCES})
rv_set_target_defaults(rv_test_support)
# Tests include helpers as "support/<file>.hpp".
target_include_directories(rv_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# rv_platform_win: the environment of this process for the child runner (and, through it, rv_launcher's quoting).
# rv_catalog and rv_catalog_db: the generated catalog databases of the query tests and benchmarks (I2 step 9).
# rv_assets and rv_media: the fixtures' generated covers (images) and clips (media::test_video) of I3 step 1c.
target_link_libraries(rv_test_support PUBLIC
    rv_core rv_launcher rv_platform_win rv_catalog rv_catalog_db rv_assets rv_media)
# The fixtures' sources and the build tree they are materialized in.
target_compile_definitions(rv_test_support PRIVATE
    RV_TEST_SUPPORT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_TEST_SUPPORT_BUILD_DIR="${CMAKE_BINARY_DIR}")
rv_forbid_link(rv_test_support)
