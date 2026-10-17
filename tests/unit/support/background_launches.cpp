// rv_tests starts its programs in the background (tests/support/background_launches.hpp): RV_LAUNCH_BACKGROUND is set
// before main(), so that also a run of rv_tests.exe outside ctest (a filter typed by hand, an agent's run) never opens a
// console window or hands the foreground to the fake emulator, and no failure dialog of rv_tests itself appears.
// tests/unit/platform_win/test_process_runner.cpp ("background launches never open a console window") checks that it
// holds.

#include "support/background_launches.hpp"

namespace {

const bool kBackgroundLaunches = rv::test::prepareAutomatedRun();

} // namespace
