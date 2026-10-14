#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>

using rv::input::Action;
using rv::input::ActionState;

TEST_CASE("input/action: names are unique and round-trip", "[input][action]") {
    std::set<std::string> seen;
    for (std::size_t i = 0; i < rv::input::kActionCount; ++i) {
        const auto action = static_cast<Action>(i);
        const auto name = rv::input::actionName(action);
        CHECK_FALSE(name.empty());
        CHECK(seen.insert(std::string(name)).second);
        CHECK(rv::input::actionFromName(name) == action);
    }
    CHECK_FALSE(rv::input::actionFromName("teleport_to_moon").has_value());
    CHECK(rv::input::actionName(Action::MoveForward) == "move_forward");
}

TEST_CASE("input/action_state: buttons and axes", "[input][action_state]") {
    ActionState state;
    CHECK(state.axis(Action::MoveForward) == 0.0f);
    CHECK_FALSE(state.held(Action::Interact));

    state.setButton(Action::Interact, true, true, false);
    CHECK(state.held(Action::Interact));
    CHECK(state.pressed(Action::Interact));
    CHECK_FALSE(state.released(Action::Interact));
    CHECK(state.axis(Action::Interact) == 1.0f);

    state.setButton(Action::Interact, false, false, true);
    CHECK_FALSE(state.held(Action::Interact));
    CHECK(state.released(Action::Interact));
    CHECK(state.axis(Action::Interact) == 0.0f);

    state.setButton(Action::Sprint, true, false, false);
    state.setAxis(Action::Sprint, 0.6f); // analog trigger
    CHECK(state.held(Action::Sprint));
    CHECK(state.axis(Action::Sprint) == 0.6f);

    state.setAxis(Action::LookDeltaX, -12.0f);
    state.setActiveDevice(rv::input::DeviceKind::Gamepad);
    CHECK(state.axis(Action::LookDeltaX) == -12.0f);
    CHECK(state.activeDevice() == rv::input::DeviceKind::Gamepad);

    state.clear();
    CHECK(state.axis(Action::LookDeltaX) == 0.0f);
    CHECK_FALSE(state.held(Action::Sprint));
}
