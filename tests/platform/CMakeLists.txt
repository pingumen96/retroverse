# rv_platform_tests: unit tests of the SDL3 platform layer (label "unit"): SDL -> rv::input translation tables, the
# input collector fed with synthetic SDL events, gamepad hot-plug through SDL virtual joysticks, the mouse capture
# policy on a hidden window and the fullscreen shortcut. Links SDL through rv_platform but never OpenGL, which keeps
# rv_tests SDL-free; the hidden [.soak] case (test_thread_soak.cpp) makes a GL context through SDL and calls the two GL
# functions it needs through SDL's loader.
# Sources are globbed like rv_tests.

file(GLOB RV_PLATFORM_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(rv_platform_tests ${RV_PLATFORM_TEST_SOURCES})
rv_set_target_defaults(rv_platform_tests)
rv_enable_long_paths(rv_platform_tests)
target_link_libraries(rv_platform_tests PRIVATE rv_platform rv_ui_model Catch2::Catch2WithMain)

catch_discover_tests(rv_platform_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "unit."
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    PROPERTIES LABELS unit)
