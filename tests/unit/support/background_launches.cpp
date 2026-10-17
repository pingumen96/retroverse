// rv_tests starts its programs in the background (tests/support/background_launches.hpp): RV_LAUNCH_BACKGROUND is set
// before main(), so that also a run of rv_tests.exe outside ctest (a filter typed by hand, an agent's run) never opens a
// console window or hands the foreground to the fake emulator. test_background_launches.cpp checks that it holds.

#include "support/background_launches.hpp"

namespace {

const bool kBackgroundLaunches = rv::test::ensureBackgroundLaunches();

} // namespace
