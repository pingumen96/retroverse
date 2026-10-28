#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("input/action: the product UI's actions", "[input][action]") {
    // Named as binding files and input scripts write them (ADR-0005, I2 step 6).
    const std::vector<std::pair<Action, std::string_view>> names = {
        {Action::UiUp, "ui_up"},
        {Action::UiDown, "ui_down"},
        {Action::UiLeft, "ui_left"},
        {Action::UiRight, "ui_right"},
        {Action::UiNavX, "ui_nav_x"},
        {Action::UiNavY, "ui_nav_y"},
        {Action::UiPageUp, "ui_page_up"},
        {Action::UiPageDown, "ui_page_down"},
        {Action::UiHome, "ui_home"},
        {Action::UiEnd, "ui_end"},
        {Action::UiActivate, "ui_activate"},
        {Action::UiBack, "ui_back"},
        {Action::UiMedia, "ui_media"},
        {Action::UiInfo, "ui_info"},
        {Action::UiFilterPrevious, "ui_filter_previous"},
        {Action::UiFilterNext, "ui_filter_next"},
    };
    std::size_t uiActions = 0;
    for (std::size_t i = 0; i < rv::input::kActionCount; ++i) {
        uiActions += rv::input::isUiAction(static_cast<Action>(i)) ? 1 : 0;
    }
    CHECK(uiActions == names.size());
    for (const auto& [action, name] : names) {
        INFO(name);
        CHECK(rv::input::actionName(action) == name);
        CHECK(rv::input::actionFromName(name) == action);
        CHECK(rv::input::isUiAction(action));
        CHECK_FALSE(rv::input::isLookAction(action));
    }
    // The actions of the 3D world keep their places and names.
    CHECK_FALSE(rv::input::isUiAction(Action::Back));
    CHECK_FALSE(rv::input::isUiAction(Action::Screenshot));
    CHECK(rv::input::toIndex(Action::Screenshot) == 15);
    // I5 step 5's action, then I4b step 3's and step 4's, come after them all (the action table's order is kept).
    CHECK(rv::input::toIndex(Action::ToggleMute) == rv::input::kActionCount - 4);
    CHECK(rv::input::toIndex(Action::EditPin) == rv::input::kActionCount - 3);
    CHECK(rv::input::toIndex(Action::EditRotateLeft) == rv::input::kActionCount - 2);
    CHECK(rv::input::toIndex(Action::EditRotateRight) == rv::input::kActionCount - 1);
    CHECK(rv::input::actionName(Action::EditRotateLeft) == "edit_rotate_left");
    CHECK(rv::input::actionFromName("edit_rotate_right") == Action::EditRotateRight);
    CHECK_FALSE(rv::input::isUiAction(Action::EditRotateLeft));
    CHECK(rv::input::actionName(Action::ToggleMute) == "toggle_mute");
    CHECK(rv::input::actionFromName("toggle_mute") == Action::ToggleMute);
    CHECK_FALSE(rv::input::isUiAction(Action::ToggleMute));
    CHECK(rv::input::actionName(Action::Confirm) == "confirm");
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
