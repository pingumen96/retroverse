// rv_smoke_tests: the GPU smoke tests (label "gpu", tests/smoke/CMakeLists.txt). Catch2 with its own main, so that a
// test case skipped because the machine has no OpenGL 4.5 core profile context reports ctest's skip code, and so that
// every program the tests start runs in the background (RV_LAUNCH_BACKGROUND, tests/support/background_launches.hpp;
// the desktop case's registration sets it to "0"), and no failure dialog of this process appears.

#include <catch2/catch_session.hpp>

#include "support/background_launches.hpp"

namespace {

/// What Catch2 returns when every test case it ran was skipped (SKIP).
constexpr int kCatchAllSkipped = 4;
/// What ctest reads as a skipped test (SKIP_RETURN_CODE of the gpu tests).
constexpr int kCtestSkipped = 77;

} // namespace

int main(int argc, char* argv[]) {
    rv::test::prepareAutomatedRun();
    const int result = Catch::Session().run(argc, argv);
    return result == kCatchAllSkipped ? kCtestSkipped : result;
}
