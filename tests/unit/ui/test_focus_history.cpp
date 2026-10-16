#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

#include <rv/ui/focus_history.hpp>

using rv::ui::FocusHistory;
using rv::ui::FocusTarget;

namespace {

FocusTarget target(std::string region, std::string element) {
    return FocusTarget{std::move(region), std::move(element)};
}

} // namespace

TEST_CASE("ui/focus_history: Back returns to the element the focus left for another region", "[ui][focus_history]") {
    FocusHistory history;
    history.onFocusMoved(std::nullopt, "list"); // the screen's first focus records nothing
    CHECK(history.empty());
    history.onFocusMoved(target("list", "game-g01"), "list"); // moves within a region record nothing
    history.onFocusMoved(target("list", "game-g11"), "actions");
    REQUIRE(history.depth() == 1);
    REQUIRE(history.peek() != nullptr);
    CHECK(*history.peek() == target("list", "game-g11"));

    const std::optional<FocusTarget> back = history.back();
    REQUIRE(back.has_value());
    CHECK(*back == target("list", "game-g11"));
    // The move Back causes is not a new step: a second Back leaves the screen.
    history.onFocusMoved(target("actions", "act-play"), "list");
    CHECK(history.empty());
    CHECK_FALSE(history.back().has_value());
}

TEST_CASE("ui/focus_history: moving into a region already on the path cuts the path back to it",
          "[ui][focus_history]") {
    FocusHistory history;
    history.onFocusMoved(target("list", "game-g03"), "chips");
    history.onFocusMoved(target("chips", "chip-all"), "search");
    REQUIRE(history.depth() == 2);
    // Down from the search field into the chips again: the step out of the chips is undone.
    history.onFocusMoved(target("search", "search"), "chips");
    REQUIRE(history.depth() == 1);
    CHECK(*history.peek() == target("list", "game-g03"));
    // Back into the list by navigation: the path is empty again, Back would leave the screen.
    history.onFocusMoved(target("chips", "chip-all"), "list");
    CHECK(history.empty());
}

TEST_CASE("ui/focus_history: a Back whose element is gone does not swallow the next move", "[ui][focus_history]") {
    FocusHistory history;
    history.onFocusMoved(target("list", "game-g07"), "actions");
    REQUIRE(history.back().has_value());
    // The game was filtered out: the screen focused the chips instead of the list.
    history.onFocusMoved(target("actions", "act-play"), "chips");
    REQUIRE(history.depth() == 1);
    CHECK(*history.peek() == target("actions", "act-play"));
}

TEST_CASE("ui/focus_history: the path is bounded and can be cleared", "[ui][focus_history]") {
    FocusHistory history;
    for (std::size_t step = 0; step < FocusHistory::kMaxDepth + 5; ++step) {
        history.onFocusMoved(target("r" + std::to_string(step), "e"), "r" + std::to_string(step + 1));
    }
    CHECK(history.depth() == FocusHistory::kMaxDepth);
    CHECK(history.peek()->region == "r" + std::to_string(FocusHistory::kMaxDepth + 4));
    history.clear();
    CHECK(history.empty());
    CHECK(history.peek() == nullptr);
}
