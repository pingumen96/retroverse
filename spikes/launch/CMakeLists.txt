# spikes/launch (I1 spike f, ADR-0006): emulator launch and return to the borderless fullscreen window. The stand-in
# emulator it launches, rv_fake_emulator, lives in tests/fake_emulator (built with the tests, or for the spikes alone).
#   spike_launch_core   GPU-free part of the spike: frame pacing and return measurements, the command lines of the
#                       targets (the isolated RetroArch configuration)
#   spike_launch        the measurement executable: a Retroverse-like borderless fullscreen window rendering an
#                       animated scene, launches through rv_launcher, rv_platform_win and rv_platform, timings of the
#                       return, a JSON report
#   spike_launch_tests  unit tests of spike_launch_core (label "unit")
# ctest cases labeled "desktop" run spike_launch against the fake emulator: they show fullscreen windows and move the
# foreground, so only the msvc-x64-debug-desktop test preset runs them, with RV_LAUNCH_BACKGROUND=0 (the fake emulator
# then shows its windows and takes the foreground as asked).

add_library(spike_launch_core STATIC
    core/src/measurements.cpp
    core/src/targets.cpp
)
rv_set_target_defaults(spike_launch_core)
target_include_directories(spike_launch_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
target_link_libraries(spike_launch_core PUBLIC rv_core)
rv_forbid_link(spike_launch_core)

add_executable(spike_launch
    src/harness.cpp
    src/main.cpp
    src/spike_scene.cpp
)
rv_set_target_defaults(spike_launch)
target_link_libraries(spike_launch PRIVATE
    spike_launch_core rv_core rv_input rv_launcher rv_platform_win rv_platform rv_render)
# spike_launch starts the fake emulator from its own directory.
add_dependencies(spike_launch rv_fake_emulator)

if(RV_BUILD_TESTS)
    add_executable(spike_launch_tests
        tests/test_measurements.cpp
        tests/test_targets.cpp
    )
    rv_set_target_defaults(spike_launch_tests)
    target_link_libraries(spike_launch_tests PRIVATE spike_launch_core Catch2::Catch2WithMain)
    rv_forbid_link(spike_launch_tests)
endif()

# Test registration. The desktop cases: "<name>|<spike_launch arguments separated by spaces>".
set(RV_LAUNCH_SPIKE_DESKTOP_CASES
    "fake_direct_minimize|--strategy direct --policy minimize --repeat 3"
    "fake_direct_keep|--strategy direct --policy keep --repeat 3"
    "fake_tree_minimize|--strategy tree --policy minimize --repeat 2"
    "fake_tree_keep|--strategy tree --policy keep --repeat 2"
    "fake_confirm_keep|--strategy confirm --policy keep --repeat 2"
    "fake_crash_minimize|--fake-crash --expect crashed --policy minimize --repeat 2"
    "fake_exclusive_minimize|--fake-exclusive --policy minimize --repeat 2"
    "fake_exclusive_keep|--fake-exclusive --policy keep --repeat 2"
    "fake_steal_focus_keep|--fake-steal-focus --policy keep --repeat 2"
    "fake_other_window_keep|--fake-foreground-window 5000 --fake-input --policy keep --repeat 2"
    CACHE INTERNAL "spike_launch cases of the desktop tests")

function(_rv_launch_spike_register_tests)
    if(NOT RV_BUILD_TESTS OR NOT TARGET spike_launch_tests)
        return()
    endif()
    catch_discover_tests(spike_launch_tests
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "unit."
        WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        PROPERTIES LABELS unit)
    foreach(case IN LISTS RV_LAUNCH_SPIKE_DESKTOP_CASES)
        string(FIND "${case}" "|" bar)
        string(SUBSTRING "${case}" 0 ${bar} name)
        math(EXPR rest_start "${bar} + 1")
        string(SUBSTRING "${case}" ${rest_start} -1 rest)
        string(REPLACE " " ";" parts "${rest}")
        set(data_dir "${CMAKE_BINARY_DIR}/spike-launch/${name}")
        add_test(NAME "desktop.launch.${name}"
            COMMAND spike_launch --target fake ${parts} --data-dir "${data_dir}" --warmup-ms 1000 --settle-ms 1200
            WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        set_tests_properties("desktop.launch.${name}" PROPERTIES LABELS desktop TIMEOUT 300 RUN_SERIAL TRUE
            ENVIRONMENT "RV_LAUNCH_BACKGROUND=0")
    endforeach()
endfunction()
_rv_launch_spike_register_tests()
