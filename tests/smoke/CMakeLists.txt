# rv_smoke_tests: the GPU smoke tests (label "gpu"; ctest names "gpu.<test case>"). Each test case starts the
# executables of this build frame-limited, with --hidden and a data directory under <build>/smoke/<case>, and checks
# what the runs report (--run-report: exit code, OpenGL debug messages, frame statistics, the assets read) and the
# screenshots they write: the reference scenes, the instance buffer growing mid-run, the rendering self-checks, the key
# light's shadow and the ambient occlusion on test scenes (tests/smoke/scenes), the developer overlay and the asset
# preview (developer builds), golden G01 within the tolerance of retroverse-screenshot-diff (on the reference machine
# only). A test case skipped because no OpenGL 4.5 context can be created exits with 77 (SKIP_RETURN_CODE).
#
# The default test presets leave the label out (msvc-x64-debug, msvc-x64-release); msvc-x64-debug-gpu and
# msvc-x64-release-gpu run them, and scripts/gate.ps1 runs them for Gate A. Sources are globbed like rv_tests.

if(NOT TARGET rv_screenshot_diff_core)
    message(STATUS "GPU smoke tests need the screenshot diff of the tools (RV_BUILD_TOOLS); skipping them")
    return()
endif()

file(GLOB RV_SMOKE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(rv_smoke_tests ${RV_SMOKE_TEST_SOURCES})
rv_set_target_defaults(rv_smoke_tests)
# GPU-free itself: it starts the executables under test and reads their reports, scenes and screenshots.
target_link_libraries(rv_smoke_tests PRIVATE
    rv_core
    rv_assets
    rv_scene
    rv_app_cli               # the run report
    rv_screenshot_diff_core  # the golden comparison
    Catch2::Catch2
    shell32)                 # CommandLineToArgvW: the quoting of child process arguments
rv_forbid_link(rv_smoke_tests)

# The executables under test are built with the tests.
add_dependencies(rv_smoke_tests retroverse)
target_compile_definitions(rv_smoke_tests PRIVATE
    RV_SMOKE_RETROVERSE="$<TARGET_FILE:retroverse>"
    RV_SMOKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RV_SMOKE_WORK_DIR="${CMAKE_BINARY_DIR}/smoke"
    RV_SMOKE_DEVELOPER_BUILD=$<BOOL:${RV_DEV_UI}>
    RV_SMOKE_GL_DEBUG=$<BOOL:${RV_GL_DEBUG}>
    RV_SMOKE_DEV_ASSET_ROOT=$<BOOL:${RV_DEV_ASSET_ROOT}>)
if(TARGET retroverse-asset-preview)
    add_dependencies(rv_smoke_tests retroverse-asset-preview)
    target_compile_definitions(rv_smoke_tests PRIVATE
        RV_SMOKE_ASSET_PREVIEW="$<TARGET_FILE:retroverse-asset-preview>")
endif()

# main.cpp turns Catch2's "every test case skipped" (4) into ctest's skip code; SKIP_IS_FAILURE keeps
# catch_discover_tests from registering 4 as the skip code instead.
catch_discover_tests(rv_smoke_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "gpu."
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    SKIP_IS_FAILURE
    PROPERTIES
        LABELS gpu
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        RUN_SERIAL TRUE)
