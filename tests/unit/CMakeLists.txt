# rv_tests: every GPU-free unit test. One sub-directory per module (tests/unit/<module>/test_*.cpp); sources
# are globbed so modules can add tests without editing this file.

file(GLOB_RECURSE RV_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# tests/unit/preview and tests/unit/screenshot_diff cover the GPU-free cores of the asset preview and the screenshot
# diff tools, which only exist with RV_BUILD_TOOLS.
if(NOT TARGET rv_preview_core)
    list(FILTER RV_UNIT_TEST_SOURCES EXCLUDE REGEX "/preview/")
endif()
if(NOT TARGET rv_screenshot_diff_core)
    list(FILTER RV_UNIT_TEST_SOURCES EXCLUDE REGEX "/screenshot_diff/")
endif()

add_executable(rv_tests ${RV_UNIT_TEST_SOURCES})
rv_set_target_defaults(rv_tests)
target_link_libraries(rv_tests PRIVATE
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_app_cli             # GPU-free part of rv_app (command line, exit codes, self-check verdicts, draw plan)
    rv_devui_model         # GPU-free part of rv_devui (frame times, input routing, log view)
    rv_render_debug_filter # GPU-free part of rv_render (GL debug message policy)
    rv_render_uniforms     # GPU-free part of rv_render (std140 blocks, light and material packing, tonemap)
    rv_test_support
    Catch2::Catch2WithMain)
if(TARGET rv_preview_core)
    target_link_libraries(rv_tests PRIVATE rv_preview_core) # GPU-free core of retroverse-asset-preview
endif()
if(TARGET rv_screenshot_diff_core)
    target_link_libraries(rv_tests PRIVATE rv_screenshot_diff_core) # GPU-free core of retroverse-screenshot-diff
endif()
target_compile_definitions(rv_tests PRIVATE
    RV_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
rv_forbid_link(rv_tests)

catch_discover_tests(rv_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "unit."
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    PROPERTIES LABELS unit)
