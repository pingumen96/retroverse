// The process priority's choice (I5 step 2, ADR-0016 §2): above normal only while the view renders, normal while
// minimized and during a game.

#include <rv/app/process_priority.hpp>

#include <catch2/catch_test_macros.hpp>

using rv::app::PriorityChoice;

TEST_CASE("app/process_priority: above normal while rendering, normal while minimized or during a game",
          "[app][process_priority]") {
    CHECK(rv::app::processPriorityFor(false, false) == PriorityChoice{true, "rendering"});
    CHECK(rv::app::processPriorityFor(true, false) == PriorityChoice{false, "minimized"});
    CHECK(rv::app::processPriorityFor(false, true) == PriorityChoice{false, "launch"});
    CHECK(rv::app::processPriorityFor(true, true) == PriorityChoice{false, "launch"});
}
