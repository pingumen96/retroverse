# rv_tests: every GPU-free unit test. One sub-directory per module (tests/unit/<module>/test_*.cpp); sources
# are globbed so modules can add tests without editing this file.

file(GLOB_RECURSE RV_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(rv_tests ${RV_UNIT_TEST_SOURCES})
rv_set_target_defaults(rv_tests)
target_link_libraries(rv_tests PRIVATE
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_app_cli             # GPU-free part of rv_app (command line, exit codes)
    rv_render_debug_filter # GPU-free part of rv_render (GL debug message policy)
    rv_test_support
    Catch2::Catch2WithMain)
target_compile_definitions(rv_tests PRIVATE
    RV_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
rv_forbid_link(rv_tests)

catch_discover_tests(rv_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "unit."
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    PROPERTIES LABELS unit)
