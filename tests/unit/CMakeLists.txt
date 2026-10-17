# rv_tests: every GPU-free unit test. One sub-directory per module (tests/unit/<module>/test_*.cpp); sources
# are globbed so modules can add tests without editing this file.

file(GLOB_RECURSE RV_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# tests/unit/preview, tests/unit/screenshot_diff and tests/unit/cli cover the GPU-free cores of the asset preview, the
# screenshot diff and retroverse-cli, which only exist with RV_BUILD_TOOLS.
if(NOT TARGET rv_preview_core)
    list(FILTER RV_UNIT_TEST_SOURCES EXCLUDE REGEX "/preview/")
endif()
if(NOT TARGET rv_screenshot_diff_core)
    list(FILTER RV_UNIT_TEST_SOURCES EXCLUDE REGEX "/screenshot_diff/")
endif()
if(NOT TARGET rv_cli_core)
    list(FILTER RV_UNIT_TEST_SOURCES EXCLUDE REGEX "/cli/")
endif()

add_executable(rv_tests ${RV_UNIT_TEST_SOURCES})
rv_set_target_defaults(rv_tests)
rv_enable_long_paths(rv_tests)
target_link_libraries(rv_tests PRIVATE
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_app_cli             # GPU-free part of rv_app (command line, exit codes, self-check verdicts, draw plan)
    rv_app_catalog         # GPU-free part of rv_app over the ES-DE catalog (the catalog worker, its source; I2 step 13)
    rv_devui_model         # GPU-free part of rv_devui (frame times, input routing, log view)
    rv_render_debug_filter # GPU-free part of rv_render (GL debug message policy)
    rv_render_uniforms     # GPU-free part of rv_render (std140 blocks, light and material packing, tonemap)
    rv_media               # video decoding with FFmpeg (CPU frames)
    rv_launcher            # emulator launch policy
    rv_ui_model            # GPU-free part of the product UI (navigation and focus)
    rv_catalog             # the catalog domain and its ports (I2)
    rv_esde                # the ES-DE adapter (I2)
    rv_catalog_db          # the SQLite store of the catalog (I2)
    rv_platform_win        # the Windows services without SDL: system probe, process runner (I2)
    rv_test_support
    Catch2::Catch2WithMain)
if(TARGET rv_preview_core)
    target_link_libraries(rv_tests PRIVATE rv_preview_core) # GPU-free core of retroverse-asset-preview
endif()
if(TARGET rv_screenshot_diff_core)
    target_link_libraries(rv_tests PRIVATE rv_screenshot_diff_core) # GPU-free core of retroverse-screenshot-diff
endif()
if(TARGET rv_cli_core)
    target_link_libraries(rv_tests PRIVATE rv_cli_core) # GPU-free core of retroverse-cli
endif()
target_compile_definitions(rv_tests PRIVATE
    RV_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
rv_forbid_link(rv_tests)
# The process runner tests (tests/unit/platform_win) and the child runner's (tests/unit/support) start the fake emulator
# from the directory of rv_tests.exe; it is only started, never linked.
add_dependencies(rv_tests rv_fake_emulator)
# The play session tests (tests/unit/cli/test_cli_sessions.cpp) start retroverse-cli.exe as a process, which they end
# mid-game (crash recovery) or run again (a restart): it is only started, never linked.
if(TARGET retroverse-cli)
    add_dependencies(rv_tests retroverse-cli)
endif()

catch_discover_tests(rv_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "unit."
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    PROPERTIES LABELS unit)
