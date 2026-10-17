// The input contexts of retroverse.exe (I2 step A1): the world's actions while free roam has the input, the UI actions
// while the quick catalog or its panel is open, through the default bindings the two share devices with.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include <rv/app/input_context.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>

using rv::app::InputContext;
using rv::input::Action;
using rv::input::ActionState;
using rv::input::GamepadButton;
using rv::input::Key;

namespace {

/// The actions of one frame in which `key` went down, through the default bindings.
ActionState keyPress(Key key) {
    rv::input::InputSystem system(rv::input::ActionMap::defaults());
    rv::input::RawInputFrame frame;
    frame.keysDown.set(rv::input::toIndex(key));
    frame.keysPressed.set(rv::input::toIndex(key));
    return system.resolve(frame);
}

/// The actions of one frame in which the pad's `button` went down, through the default bindings.
ActionState padPress(GamepadButton button) {
    rv::input::InputSystem system(rv::input::ActionMap::defaults());
    rv::input::RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.gamepadButtonsDown.set(rv::input::toIndex(button));
    frame.gamepadButtonsPressed.set(rv::input::toIndex(button));
    frame.lastActiveDevice = rv::input::DeviceKind::Gamepad;
    return system.resolve(frame);
}

} // namespace

TEST_CASE("app/input_context: the catalog or its panel give the input to the UI", "[app][input_context]") {
    CHECK(rv::app::inputContextFor(false, false) == InputContext::World);
    CHECK(rv::app::inputContextFor(true, false) == InputContext::Ui);
    CHECK(rv::app::inputContextFor(false, true) == InputContext::Ui);
    CHECK(rv::app::inputContextFor(true, true) == InputContext::Ui);
    CHECK(rv::app::toString(InputContext::World) == "world");
    CHECK(rv::app::toString(InputContext::Ui) == "ui");
}

TEST_CASE("app/input_context: each context reads its own actions and the global ones", "[app][input_context]") {
    for (std::size_t index = 0; index < rv::input::kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        CAPTURE(rv::input::actionName(action));
        const bool global = rv::app::isGlobalAction(action);
        const bool ui = rv::input::isUiAction(action);
        CHECK(rv::app::readsAction(InputContext::World, action) == (global || !ui));
        CHECK(rv::app::readsAction(InputContext::Ui, action) == (global || ui || action == Action::QuickCatalog));
    }
    CHECK(rv::app::isGlobalAction(Action::ToggleDevUI));
    CHECK(rv::app::isGlobalAction(Action::ReloadShaders));
    CHECK(rv::app::isGlobalAction(Action::Screenshot));
    CHECK_FALSE(rv::app::isGlobalAction(Action::Back));
}

TEST_CASE("app/input_context: a device shared by both contexts acts in the one that has the input",
          "[app][input_context]") {
    // Enter: Confirm in the world, UiActivate in the catalog.
    const ActionState enter = keyPress(Key::Enter);
    REQUIRE(enter.pressed(Action::Confirm));
    REQUIRE(enter.pressed(Action::UiActivate));
    const ActionState enterWorld = rv::app::actionsFor(InputContext::World, enter);
    CHECK(enterWorld.pressed(Action::Confirm));
    CHECK_FALSE(enterWorld.pressed(Action::UiActivate));
    CHECK_FALSE(enterWorld.held(Action::UiActivate));
    const ActionState enterUi = rv::app::actionsFor(InputContext::Ui, enter);
    CHECK(enterUi.pressed(Action::UiActivate));
    CHECK_FALSE(enterUi.pressed(Action::Confirm));

    // The down arrow walks backwards in the world and moves down in the catalog: under the catalog the walker stands.
    const ActionState down = keyPress(Key::Down);
    REQUIRE(down.axis(Action::MoveForward) < 0.0f);
    REQUIRE(down.pressed(Action::UiDown));
    CHECK(rv::app::actionsFor(InputContext::Ui, down).axis(Action::MoveForward) == 0.0f);
    CHECK(rv::app::actionsFor(InputContext::Ui, down).pressed(Action::UiDown));
    CHECK(rv::app::actionsFor(InputContext::World, down).axis(Action::MoveForward) < 0.0f);
    CHECK_FALSE(rv::app::actionsFor(InputContext::World, down).pressed(Action::UiDown));

    // B: Back in the world, UiBack in the catalog (the application turns both into its Back levels).
    const ActionState east = padPress(GamepadButton::East);
    CHECK(rv::app::actionsFor(InputContext::World, east).pressed(Action::Back));
    CHECK_FALSE(rv::app::actionsFor(InputContext::World, east).pressed(Action::UiBack));
    CHECK(rv::app::actionsFor(InputContext::Ui, east).pressed(Action::UiBack));
    CHECK_FALSE(rv::app::actionsFor(InputContext::Ui, east).pressed(Action::Back));
    // The device stays the one the player uses (prompts follow it).
    CHECK(rv::app::actionsFor(InputContext::Ui, east).activeDevice() == rv::input::DeviceKind::Gamepad);

    // F1 and F12 work everywhere.
    CHECK(rv::app::actionsFor(InputContext::Ui, keyPress(Key::F1)).pressed(Action::ToggleDevUI));
    CHECK(rv::app::actionsFor(InputContext::World, keyPress(Key::F12)).pressed(Action::Screenshot));
}

TEST_CASE("app/input_context: QuickCatalog opens the catalog, and closes it unless the press is a UI command",
          "[app][input_context]") {
    // Tab: QuickCatalog alone, so it opens and closes the catalog.
    const ActionState tab = keyPress(Key::Tab);
    REQUIRE(tab.pressed(Action::QuickCatalog));
    CHECK_FALSE(rv::app::anyUiActionPressed(tab));
    CHECK(rv::app::catalogToggleRequested(InputContext::World, tab));
    CHECK(rv::app::catalogToggleRequested(InputContext::Ui, tab));

    // The pad's Y: QuickCatalog in the world (opens it), "Vedi media" in the catalog (does not close it; B does).
    const ActionState north = padPress(GamepadButton::North);
    REQUIRE(north.pressed(Action::QuickCatalog));
    REQUIRE(north.pressed(Action::UiMedia));
    CHECK(rv::app::catalogToggleRequested(InputContext::World, north));
    CHECK_FALSE(rv::app::catalogToggleRequested(InputContext::Ui, north));

    // Nothing pressed, nothing toggled; a held QuickCatalog is no new press.
    CHECK_FALSE(rv::app::catalogToggleRequested(InputContext::World, ActionState{}));
    ActionState held;
    held.setButton(Action::QuickCatalog, true, false, false);
    CHECK_FALSE(rv::app::catalogToggleRequested(InputContext::World, held));

    // An input script that presses quick_catalog alone toggles in both contexts.
    ActionState scripted;
    scripted.setButton(Action::QuickCatalog, true, true, false);
    CHECK(rv::app::catalogToggleRequested(InputContext::Ui, scripted));
}
