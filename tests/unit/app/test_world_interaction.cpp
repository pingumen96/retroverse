// I3 step 7: app::WorldInteraction (ADR-0012 §1-§4, §6, §8): each state's input context, the Back levels, every
// transition and its failure paths, the hover's dwell and pull-out, the walker's eases, the lift and the inspection's
// rotation and zoom, reduceMotion; on hand-built shelves through the app's loop (world_interaction_harness.hpp).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <set>

#include <rv/app/back_action.hpp>
#include <rv/app/input_context.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/core/math.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>

#include "world_interaction_harness.hpp"

using Catch::Approx;
using rv::app::BackAction;
using rv::app::BackState;
using rv::app::InteractionNotice;
using rv::app::WorldBack;
using rv::app::WorldContext;
using rv::app::WorldInteraction;
using rv::app::WorldState;
using rv::input::Action;
using rv::input::ActionState;
using rv::test::facing;
using rv::test::InteractionHarness;
using rv::test::kFrameSeconds;
using rv::test::pressing;
using rv::world::kNoPick;
using rv::world::PickId;

namespace {

/// Through the default bindings: one frame in which `key` went down.
ActionState keyPress(rv::input::Key key) {
    rv::input::InputSystem system(rv::input::ActionMap::defaults());
    rv::input::RawInputFrame frame;
    frame.keysDown.set(rv::input::toIndex(key));
    frame.keysPressed.set(rv::input::toIndex(key));
    return system.resolve(frame);
}

/// Through the default bindings: one frame in which the pad's `button` went down.
ActionState padPress(rv::input::GamepadButton button) {
    rv::input::InputSystem system(rv::input::ActionMap::defaults());
    rv::input::RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.gamepadButtonsDown.set(rv::input::toIndex(button));
    frame.gamepadButtonsPressed.set(rv::input::toIndex(button));
    frame.lastActiveDevice = rv::input::DeviceKind::Gamepad;
    return system.resolve(frame);
}

/// Through the default bindings: a frame with the pad's `axis` at `value`.
ActionState padAxis(rv::input::GamepadAxis axis, float value) {
    rv::input::InputSystem system(rv::input::ActionMap::defaults());
    rv::input::RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.gamepadAxes[rv::input::toIndex(axis)] = value;
    frame.lastActiveDevice = rv::input::DeviceKind::Gamepad;
    return system.resolve(frame);
}

/// A harness standing 1.2 m in front of module 0's level 4, slot 16, its hover acquired.
InteractionHarness hovering(rv::app::InteractionSettings settings = {}) {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    InteractionHarness harness(facing(probe.shelves().item(probe.at(0, 4, 16)), 1.2f), settings);
    harness.idle(3);
    REQUIRE(harness.interaction.hovered() == harness.room.at(0, 4, 16));
    return harness;
}

/// Into Focus of module 0's level 4, slot 16, the walker's ease finished.
InteractionHarness focused(rv::app::InteractionSettings settings = {}) {
    InteractionHarness harness = hovering(settings);
    harness.frame(pressing({Action::Interact}));
    REQUIRE(harness.interaction.state() == WorldState::Focus);
    harness.idle(30);
    REQUIRE_FALSE(harness.interaction.walkerEasing());
    return harness;
}

/// Into Inspect of module 0's level 4, slot 16, the lift finished.
InteractionHarness inspecting(rv::app::InteractionSettings settings = {}) {
    InteractionHarness harness = focused(settings);
    harness.frame(pressing({Action::UiActivate}));
    REQUIRE(harness.interaction.state() == WorldState::Inspect);
    harness.idle(30);
    REQUIRE(harness.interaction.inspectView().lift == 1.0f);
    return harness;
}

/// Mouse pixels that turn the walker round (the default 2.2 mrad per pixel): the synthetic shelves have no walls, so it
/// then faces nothing.
constexpr float kTurnRound = rv::core::kPi / 0.0022f;

bool sameWalker(const rv::scene::WalkerPose& a, const rv::scene::WalkerPose& b) {
    return rv::scene::samePose(a, b);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Input contexts
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("app/world_interaction: each state has its input context", "[app][world_interaction][context]") {
    CHECK(rv::app::worldContextFor(WorldState::FreeRoam) == WorldContext::FreeRoam);
    CHECK(rv::app::worldContextFor(WorldState::Focus) == WorldContext::Focus);
    CHECK(rv::app::worldContextFor(WorldState::Inspect) == WorldContext::Inspect);
    CHECK(rv::app::worldContextFor(WorldState::Launching) == WorldContext::Suspended);
    CHECK(rv::app::worldContextFor(WorldState::Returning) == WorldContext::Suspended);
    CHECK(rv::app::worldContextFor(WorldState::AnimatingToConsole) == WorldContext::Suspended);

    const std::set<Action> look{Action::LookX, Action::LookY, Action::LookDeltaX, Action::LookDeltaY};
    const std::set<Action> arrows{Action::UiUp, Action::UiDown, Action::UiLeft, Action::UiRight};
    const auto in = [](const std::set<Action>& set, Action action) { return set.count(action) == 1; };
    for (std::size_t index = 0; index < rv::input::kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        CAPTURE(rv::input::actionName(action));
        const bool global = rv::app::isGlobalAction(action);
        // FreeRoam: the World context of A1, unchanged.
        CHECK(rv::app::readsAction(WorldContext::FreeRoam, action) ==
              rv::app::readsAction(rv::app::InputContext::World, action));
        CHECK(rv::app::readsAction(WorldContext::Focus, action) ==
              (global || in(look, action) || in(arrows, action) || action == Action::Interact ||
               action == Action::UiActivate || action == Action::Back || action == Action::QuickCatalog));
        CHECK(rv::app::readsAction(WorldContext::Inspect, action) ==
              (global || in(look, action) || in(arrows, action) || action == Action::UiPageUp ||
               action == Action::UiPageDown || action == Action::UiActivate || action == Action::Confirm ||
               action == Action::UiMedia || action == Action::UiInfo || action == Action::UiBack ||
               action == Action::Interact));
        CHECK(rv::app::readsAction(WorldContext::Suspended, action) == global);
    }
    CHECK(rv::app::toString(WorldContext::FreeRoam) == "free roam");
    CHECK(rv::app::toString(WorldContext::Focus) == "focus");
    CHECK(rv::app::toString(WorldContext::Inspect) == "inspect");
    CHECK(rv::app::toString(WorldContext::Suspended) == "suspended");
}

TEST_CASE("app/world_interaction: the north button is Vedi media in Inspect only", "[app][world_interaction][context]") {
    const ActionState north = padPress(rv::input::GamepadButton::North);
    REQUIRE(north.pressed(Action::QuickCatalog));
    REQUIRE(north.pressed(Action::UiMedia));
    for (const WorldContext context : {WorldContext::FreeRoam, WorldContext::Focus}) {
        const ActionState read = rv::app::actionsFor(context, north);
        CHECK(read.pressed(Action::QuickCatalog));
        CHECK_FALSE(read.pressed(Action::UiMedia));
    }
    const ActionState inInspect = rv::app::actionsFor(WorldContext::Inspect, north);
    CHECK_FALSE(inInspect.pressed(Action::QuickCatalog));
    CHECK(inInspect.pressed(Action::UiMedia));
    CHECK_FALSE(rv::app::actionsFor(WorldContext::Suspended, north).pressed(Action::QuickCatalog));

    // Through the interaction: Y opens the catalog from FreeRoam and Focus, and is Vedi media in Inspect.
    InteractionHarness harness = hovering();
    CHECK(harness.frame(north).openCatalog);
    CHECK_FALSE(harness.last.showMedia);
    InteractionHarness inFocus = focused();
    CHECK(inFocus.frame(north).openCatalog);
    InteractionHarness inInspectHarness = inspecting();
    const rv::app::InteractionResult result = inInspectHarness.frame(north);
    CHECK_FALSE(result.openCatalog);
    CHECK(result.showMedia);
    CHECK(inInspectHarness.interaction.state() == WorldState::Inspect);
    // Tab, QuickCatalog alone, opens nothing in Inspect either.
    CHECK_FALSE(inInspectHarness.frame(keyPress(rv::input::Key::Tab)).openCatalog);
}

TEST_CASE("app/world_interaction: the triggers zoom in Inspect only", "[app][world_interaction][context]") {
    const ActionState right = padAxis(rv::input::GamepadAxis::RightTrigger, 1.0f);
    const ActionState left = padAxis(rv::input::GamepadAxis::LeftTrigger, 1.0f);
    REQUIRE(right.axis(Action::UiPageDown) == 1.0f);
    REQUIRE(left.axis(Action::UiPageUp) == 1.0f);
    for (const WorldContext context : {WorldContext::FreeRoam, WorldContext::Focus, WorldContext::Suspended}) {
        CHECK(rv::app::actionsFor(context, right).axis(Action::UiPageDown) == 0.0f);
        CHECK(rv::app::actionsFor(context, left).axis(Action::UiPageUp) == 0.0f);
    }
    CHECK(rv::app::actionsFor(WorldContext::Inspect, right).axis(Action::UiPageDown) == 1.0f);

    // In Focus a held trigger moves nothing; in Inspect it zooms.
    InteractionHarness inFocus = focused();
    const rv::app::InspectView before = inFocus.interaction.inspectView();
    for (int frame = 0; frame < 30; ++frame) {
        inFocus.frame(right);
    }
    CHECK(inFocus.interaction.inspectView() == before);
    InteractionHarness inInspect = inspecting();
    for (int frame = 0; frame < 12; ++frame) {
        inInspect.frame(right);
    }
    // 0.5 m/s for 12 frames from 0.45 m.
    CHECK(inInspect.interaction.inspectView().distance == Approx(0.45f - 0.5f * 12.0f * kFrameSeconds).margin(1e-4));
    for (int frame = 0; frame < 200; ++frame) {
        inInspect.frame(right);
    }
    CHECK(inInspect.interaction.inspectView().distance == 0.3f);
    for (int frame = 0; frame < 200; ++frame) {
        inInspect.frame(left);
    }
    CHECK(inInspect.interaction.inspectView().distance == 1.0f);
    // The wheel: a notch away from the user 5 cm closer.
    inInspect.frame(ActionState{}, kFrameSeconds, 2.0f);
    CHECK(inInspect.interaction.inspectView().distance == Approx(0.9f));
    // Not in Focus.
    inFocus.frame(ActionState{}, kFrameSeconds, 2.0f);
    CHECK(inFocus.interaction.inspectView().distance == before.distance);
}

TEST_CASE("app/world_interaction: keyboard, mouse and pad reach the same transitions", "[app][world_interaction][context]") {
    // E, the left button and the pad's south button are interact; Enter and the south button are ui_activate.
    CHECK(keyPress(rv::input::Key::E).pressed(Action::Interact));
    CHECK(padPress(rv::input::GamepadButton::South).pressed(Action::Interact));
    // FreeRoam: the arrows walk and step nothing; in Focus they step and walk nothing.
    const ActionState up = keyPress(rv::input::Key::Up);
    CHECK(rv::app::actionsFor(WorldContext::FreeRoam, up).axis(Action::MoveForward) == 1.0f);
    CHECK_FALSE(rv::app::actionsFor(WorldContext::FreeRoam, up).pressed(Action::UiUp));
    CHECK(rv::app::actionsFor(WorldContext::Focus, up).pressed(Action::UiUp));
    CHECK(rv::app::actionsFor(WorldContext::Focus, up).axis(Action::MoveForward) == 0.0f);
    // Esc: Back in FreeRoam and Focus, ui_back in Inspect.
    const ActionState escape = keyPress(rv::input::Key::Escape);
    CHECK(rv::app::actionsFor(WorldContext::Focus, escape).pressed(Action::Back));
    CHECK_FALSE(rv::app::actionsFor(WorldContext::Inspect, escape).pressed(Action::Back));
    CHECK(rv::app::actionsFor(WorldContext::Inspect, escape).pressed(Action::UiBack));

    // Enter inspects in Focus; in Inspect it presses the card's focused button (I3 step 14a: the card's entry, Avvia
    // when the card has it focused), so the interaction alone launches nothing.
    InteractionHarness byKeyboard = focused();
    byKeyboard.frame(keyPress(rv::input::Key::Enter));
    CHECK(byKeyboard.interaction.state() == WorldState::Inspect);
    byKeyboard.idle(30);
    CHECK_FALSE(byKeyboard.frame(keyPress(rv::input::Key::Enter)).launch.has_value());
    CHECK(byKeyboard.press(rv::app::CardAction::Launch, keyPress(rv::input::Key::Enter)).launch.has_value());
    // The pad's south button inspects, Start launches.
    InteractionHarness byPad = focused();
    byPad.frame(padPress(rv::input::GamepadButton::South));
    CHECK(byPad.interaction.state() == WorldState::Inspect);
    byPad.idle(30);
    CHECK(byPad.frame(padPress(rv::input::GamepadButton::Start)).launch.has_value());
    // X is Informazioni.
    InteractionHarness info = inspecting();
    CHECK(info.frame(padPress(rv::input::GamepadButton::West)).showInfo);
}

TEST_CASE("app/world_interaction: the walker gets the actions of its state", "[app][world_interaction][context]") {
    ActionState actions;
    actions.setAxis(Action::MoveForward, 1.0f);
    actions.setAxis(Action::LookDeltaX, 12.0f);
    actions.setButton(Action::Sprint, true, true, false);
    const ActionState roam = rv::app::walkerActionsFor(WorldContext::FreeRoam, actions);
    CHECK(roam.axis(Action::MoveForward) == 1.0f);
    CHECK(roam.axis(Action::LookDeltaX) == 12.0f);
    CHECK(roam.held(Action::Sprint));
    const ActionState focus = rv::app::walkerActionsFor(WorldContext::Focus, actions);
    CHECK(focus.axis(Action::MoveForward) == 0.0f);
    CHECK_FALSE(focus.held(Action::Sprint));
    CHECK(focus.axis(Action::LookDeltaX) == 12.0f);
    for (const WorldContext context : {WorldContext::Inspect, WorldContext::Suspended}) {
        const ActionState none = rv::app::walkerActionsFor(context, actions);
        CHECK(none.axis(Action::MoveForward) == 0.0f);
        CHECK(none.axis(Action::LookDeltaX) == 0.0f);
    }
}

TEST_CASE("app/world_interaction: the pick ray's source and the left-click rule", "[app][world_interaction][context]") {
    using rv::app::LeftClick;
    using rv::app::PickRaySource;
    using rv::input::DeviceKind;
    CHECK(rv::app::pickRaySource(true, DeviceKind::KeyboardMouse) == PickRaySource::Reticle);
    CHECK(rv::app::pickRaySource(true, DeviceKind::Gamepad) == PickRaySource::Reticle);
    CHECK(rv::app::pickRaySource(false, DeviceKind::Gamepad) == PickRaySource::Reticle);
    CHECK(rv::app::pickRaySource(false, DeviceKind::KeyboardMouse) == PickRaySource::Cursor);
    CHECK(rv::app::leftClickMeaning(false) == LeftClick::CaptureMouse);
    CHECK(rv::app::leftClickMeaning(true) == LeftClick::Interact);
}

// ---------------------------------------------------------------------------------------------------------------
// Back levels
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("app/world_interaction: the world's Back levels sit between the shell's", "[app][world_interaction][back]") {
    using rv::app::WorldBackDecision;
    for (const WorldBack world : {WorldBack::Shell, WorldBack::LeaveInspect, WorldBack::SkipLowering,
                                  WorldBack::LeaveFocus}) {
        CAPTURE(rv::app::toString(world));
        // The overlay's field and popup, the panel and the catalog come first.
        CHECK(rv::app::worldBackAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true}, world) ==
              WorldBackDecision{WorldBack::Shell, BackAction::Ignore});
        CHECK(rv::app::worldBackAction(BackState{.overlayVisible = true, .overlayPopupOpen = true}, world) ==
              WorldBackDecision{WorldBack::Shell, BackAction::ClosePopup});
        CHECK(rv::app::worldBackAction(BackState{.panelOpen = true}, world) ==
              WorldBackDecision{WorldBack::Shell, BackAction::PanelBack});
        CHECK(rv::app::worldBackAction(BackState{.catalogOpen = true}, world) ==
              WorldBackDecision{WorldBack::Shell, BackAction::CatalogBack});
        // Then the world's, before the captured mouse, the overlay and quitting.
        CHECK(rv::app::worldBackAction(BackState{.mouseCaptured = true}, world) ==
              WorldBackDecision{world, BackAction::ReleaseMouse});
        CHECK(rv::app::worldBackAction(BackState{.overlayVisible = true}, world) ==
              WorldBackDecision{world, BackAction::CloseOverlay});
        CHECK(rv::app::worldBackAction(BackState{}, world) == WorldBackDecision{world, BackAction::Quit});
    }
    CHECK(rv::app::toString(WorldBack::Shell) == "shell");
    CHECK(rv::app::toString(WorldBack::LeaveInspect) == "leave inspect");
    CHECK(rv::app::toString(WorldBack::SkipLowering) == "skip lowering");
    CHECK(rv::app::toString(WorldBack::LeaveFocus) == "leave focus");
}

TEST_CASE("app/world_interaction: Back leaves Inspect, then Focus, then the shell's levels",
          "[app][world_interaction][back]") {
    InteractionHarness harness = inspecting();
    const PickId item = harness.room.at(0, 4, 16);
    CHECK(harness.interaction.backLevel() == WorldBack::LeaveInspect);
    // Esc in Inspect is the card's ui_back.
    rv::app::InteractionResult result = harness.frame(keyPress(rv::input::Key::Escape));
    CHECK_FALSE(result.shellBack.has_value());
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.focused() == item);
    // The lowering (0.2 s) is under way: Back skips it, staying in Focus.
    CHECK(harness.interaction.backLevel() == WorldBack::SkipLowering);
    CHECK(harness.interaction.inspectView().lift > 0.0f);
    result = harness.frame(keyPress(rv::input::Key::Escape));
    CHECK_FALSE(result.shellBack.has_value());
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.inspectView().lift == 0.0f);
    CHECK(harness.interaction.pullOf(item) == 1.0f); // at its pulled-out pose
    // Back from Focus: FreeRoam, the item put back.
    CHECK(harness.interaction.backLevel() == WorldBack::LeaveFocus);
    result = harness.frame(keyPress(rv::input::Key::Escape));
    CHECK_FALSE(result.shellBack.has_value());
    CHECK(harness.interaction.state() == WorldState::FreeRoam);
    CHECK(harness.interaction.focused() == kNoPick);
    CHECK(harness.interaction.pullOf(item) < 1.0f);
    harness.idle(30);
    CHECK(harness.interaction.pullOf(item) == 0.0f);
    CHECK(harness.interaction.pulledItems().empty());
    // Put back, it stays in its slot although the hover rests on it.
    CHECK(harness.interaction.hovered() == item);
    harness.idle(60);
    CHECK(harness.interaction.pulledItems().empty());
    // In FreeRoam Back is the shell's: the captured mouse, then quitting.
    CHECK(harness.interaction.backLevel() == WorldBack::Shell);
    result = harness.frame(padPress(rv::input::GamepadButton::East));
    CHECK(result.shellBack == BackAction::ReleaseMouse);
    harness.back.mouseCaptured = false;
    result = harness.frame(keyPress(rv::input::Key::Escape));
    CHECK(result.shellBack == BackAction::Quit);
    CHECK(harness.interaction.state() == WorldState::FreeRoam);
}

TEST_CASE("app/world_interaction: an overlay popup takes Back before the world", "[app][world_interaction][back]") {
    InteractionHarness harness = inspecting();
    harness.back = BackState{.mouseCaptured = true, .overlayVisible = true, .overlayPopupOpen = true};
    const rv::app::InteractionResult result = harness.frame(keyPress(rv::input::Key::Escape));
    CHECK(result.shellBack == BackAction::ClosePopup);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    // A field of the overlay has the keyboard: Esc is ignored, the world stays.
    InteractionHarness inFocus = focused();
    inFocus.back = BackState{.overlayVisible = true, .overlayHasKeyboard = true};
    CHECK(inFocus.frame(keyPress(rv::input::Key::Escape)).shellBack == BackAction::Ignore);
    CHECK(inFocus.interaction.state() == WorldState::Focus);
}

// ---------------------------------------------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("app/world_interaction: the hover pulls a spine out after 150 ms and back when it leaves",
          "[app][world_interaction][transitions]") {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    const PickId item = probe.at(0, 4, 16);
    InteractionHarness harness(facing(probe.shelves().item(item), 1.2f));
    CHECK(harness.interaction.hovered() == kNoPick);
    // Three frames on the item: hovered and outlined.
    harness.idle(2);
    CHECK(harness.interaction.hovered() == kNoPick);
    harness.idle(1);
    CHECK(harness.interaction.hovered() == item);
    CHECK(harness.interaction.highlighted() == item);
    // The dwell: 150 ms counted from the frame the hover came (that frame the first of 9 at 60 fps); then 250 ms out,
    // 15 frames counting the first.
    harness.idle(7);
    CHECK(harness.interaction.pullOf(item) == 0.0f);
    harness.idle(1);
    CHECK(harness.interaction.pullOf(item) > 0.0f);
    harness.idle(13);
    CHECK(harness.interaction.pullOf(item) < 1.0f);
    harness.idle(1);
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    REQUIRE(harness.interaction.pulledItems().size() == 1);
    CHECK(harness.interaction.pulledItems()[0] == rv::app::PulledItem{item, 1.0f});
    // A still view keeps it out.
    harness.idle(60);
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    CHECK(harness.interaction.state() == WorldState::FreeRoam);

    // The hover leaves (the walker turns round to face nothing): after 3 frames it slides back in 200 ms, 12 frames.
    ActionState turn;
    turn.setAxis(Action::LookDeltaX, kTurnRound);
    harness.frame(turn);
    harness.idle(2);
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    harness.idle(1);
    CHECK(harness.interaction.hovered() != item);
    CHECK(harness.interaction.pullOf(item) < 1.0f);
    harness.idle(11);
    CHECK(harness.interaction.pullOf(item) == 0.0f);
    CHECK(harness.interaction.pulledItems().empty());
}

TEST_CASE("app/world_interaction: a pull-out interrupted reverses from where it is", "[app][world_interaction][transitions]") {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    const PickId item = probe.at(0, 4, 16);
    InteractionHarness harness(facing(probe.shelves().item(item), 1.2f));
    harness.idle(3 + 7 + 6); // hovered, the rest of the dwell, then 6 of the 15 frames out
    const float pulled = harness.interaction.pullOf(item);
    CHECK(pulled > 0.0f);
    CHECK(pulled < 1.0f);
    ActionState turn;
    turn.setAxis(Action::LookDeltaX, kTurnRound);
    harness.frame(turn);
    harness.idle(2);
    // Still going out until the hover has left (3 frames on nothing).
    const float atLeave = harness.interaction.pullOf(item);
    CHECK(atLeave > pulled);
    CHECK(atLeave < 1.0f);
    harness.idle(1);
    CHECK(harness.interaction.hovered() == kNoPick);
    const float back = harness.interaction.pullOf(item);
    CHECK(back < atLeave);
    // It slides back from there: no jump to the end and back.
    CHECK(atLeave - back < 0.25f);
    harness.idle(12);
    CHECK(harness.interaction.pullOf(item) == 0.0f);
}

TEST_CASE("app/world_interaction: interact on a spine-out item eases the walker into Focus",
          "[app][world_interaction][transitions]") {
    InteractionHarness harness = hovering();
    const PickId item = harness.room.at(0, 4, 16);
    const rv::scene::WalkerPose target = harness.room.focusPose(item);
    // Nothing hovered or no interact: nothing happens.
    harness.idle(1);
    CHECK(harness.interaction.state() == WorldState::FreeRoam);
    // The press starts the ease: 350 ms, 21 frames, each placing the walker.
    rv::app::InteractionResult result = harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.focused() == item);
    CHECK(harness.interaction.highlighted() == item);
    CHECK(harness.interaction.context() == WorldContext::Focus);
    REQUIRE(result.walker.has_value());
    int driven = 1;
    while (harness.idle(1).walker) {
        ++driven;
    }
    CHECK(driven == 21);
    CHECK(sameWalker(harness.pose(), target));
    // Walking is off in Focus.
    ActionState walk;
    walk.setAxis(Action::MoveForward, 1.0f);
    for (int frame = 0; frame < 30; ++frame) {
        harness.frame(walk);
    }
    CHECK(harness.pose().feet == target.feet);
    // The item stays pulled out; the hover follows the reticle onto it.
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    CHECK(harness.interaction.hovered() == item);
}

TEST_CASE("app/world_interaction: the button that started an ease skips it", "[app][world_interaction][transitions]") {
    InteractionHarness harness = hovering();
    const PickId item = harness.room.at(0, 4, 16);
    harness.frame(pressing({Action::Interact}));
    harness.idle(4);
    REQUIRE(harness.interaction.walkerEasing());
    // Pressed again mid-ease: the walker arrives this frame, still in Focus.
    const rv::app::InteractionResult skipped = harness.frame(pressing({Action::Interact}));
    REQUIRE(skipped.walker.has_value());
    CHECK(sameWalker(*skipped.walker, harness.room.focusPose(item)));
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    CHECK_FALSE(harness.idle(1).walker.has_value());
    // The next press inspects; pressed again during the lift (300 ms, 18 frames) it skips the lift, not Avvia.
    harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.state() == WorldState::Inspect);
    harness.idle(5);
    CHECK(harness.interaction.inspectView().lift > 0.0f);
    CHECK(harness.interaction.inspectView().lift < 1.0f);
    const rv::app::InteractionResult lifted = harness.frame(pressing({Action::UiActivate}));
    CHECK_FALSE(lifted.launch.has_value());
    CHECK(harness.interaction.inspectView().lift == 1.0f);
    // Vedi media and Informazioni wait for the item in the hand.
    InteractionHarness waiting = focused();
    waiting.frame(pressing({Action::UiActivate}));
    CHECK_FALSE(waiting.frame(pressing({Action::UiMedia})).showMedia);
    CHECK_FALSE(waiting.frame(pressing({Action::UiInfo})).showInfo);
    waiting.idle(30);
    CHECK(waiting.frame(pressing({Action::UiMedia})).showMedia);
    CHECK(waiting.frame(pressing({Action::UiInfo})).showInfo);
}

TEST_CASE("app/world_interaction: after a D-pad step interact inspects at once", "[app][world_interaction][transitions]") {
    InteractionHarness harness = focused();
    const PickId next = harness.room.at(0, 4, 17);
    harness.frame(pressing({Action::UiRight}));
    harness.idle(2);
    REQUIRE(harness.interaction.walkerEasing());
    // The step started this ease, not interact: the press inspects, and the walker arrives at once (it does not move
    // while the item is in the hand).
    const rv::app::InteractionResult result = harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == next);
    REQUIRE(result.walker.has_value());
    CHECK(sameWalker(*result.walker, harness.room.focusPose(next)));
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK_FALSE(harness.idle(1).walker.has_value());
}

TEST_CASE("app/world_interaction: Inspect lifts, rotates and zooms the item", "[app][world_interaction][transitions]") {
    InteractionHarness harness = focused();
    const rv::scene::WalkerPose walker = harness.pose();
    harness.frame(pressing({Action::UiActivate}));
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.highlighted() == kNoPick);
    int lifting = 1;
    while (harness.interaction.inspectView().lift < 1.0f) {
        harness.idle(1);
        ++lifting;
    }
    CHECK(lifting == 18);
    // The right stick turns the item, not the walker: 180 degrees per second at full deflection.
    ActionState stick;
    stick.setAxis(Action::LookX, 1.0f);
    for (int frame = 0; frame < 30; ++frame) {
        harness.frame(stick);
    }
    CHECK(harness.interaction.inspectView().yaw == Approx(rv::core::degreesToRadians(180.0f * 30.0f * kFrameSeconds)));
    CHECK(sameWalker(harness.pose(), walker));
    // Yaw is unlimited (wrapped), pitch stops at 80 degrees.
    for (int frame = 0; frame < 120; ++frame) {
        harness.frame(stick);
    }
    CHECK(std::fabs(harness.interaction.inspectView().yaw) <= rv::core::kPi);
    ActionState mouse;
    mouse.setAxis(Action::LookDeltaY, 1000.0f);
    harness.frame(mouse);
    CHECK(harness.interaction.inspectView().pitch == Approx(rv::core::degreesToRadians(80.0f)));
    mouse.setAxis(Action::LookDeltaY, -5000.0f);
    harness.frame(mouse);
    CHECK(harness.interaction.inspectView().pitch == Approx(rv::core::degreesToRadians(-80.0f)));
    // Walking does nothing either.
    ActionState walk;
    walk.setAxis(Action::MoveForward, 1.0f);
    harness.frame(walk);
    CHECK(sameWalker(harness.pose(), walker));
    // Back to Focus and in again: the inspection starts facing the front at 0.45 m.
    harness.frame(pressing({Action::UiBack}));
    harness.idle(20);
    harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.inspectView().yaw == 0.0f);
    CHECK(harness.interaction.inspectView().pitch == 0.0f);
    CHECK(harness.interaction.inspectView().distance == 0.45f);
}

TEST_CASE("app/world_interaction: a face-out item is inspected straight from FreeRoam",
          "[app][world_interaction][transitions]") {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    const PickId band = probe.at(2, 4, 2); // module 2's level 4 is face-out
    InteractionHarness harness(facing(probe.shelves().item(band), 1.0f));
    harness.idle(3);
    REQUIRE(harness.interaction.hovered() == band);
    harness.idle(30); // the dwell tilts it towards the viewer
    CHECK(harness.interaction.pullOf(band) == 1.0f);
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InteractionResult result = harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == band);
    CHECK_FALSE(result.walker.has_value()); // the walker does not move
    harness.idle(30);
    CHECK(sameWalker(harness.pose(), walker));
    // Back: Focus of the same item, at its pulled-out (tilted) pose; the walker stays.
    harness.frame(pressing({Action::UiBack}));
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.focused() == band);
    harness.idle(30);
    CHECK(harness.interaction.pullOf(band) == 1.0f);
    CHECK(sameWalker(harness.pose(), walker));
}

TEST_CASE("app/world_interaction: D-pad steps move the focus exactly, across modules", "[app][world_interaction][transitions]") {
    InteractionHarness harness = focused();
    const PickId start = harness.room.at(0, 4, 16);
    // Right: the next spine; the old one slides back, the new one comes out, the walker eases to face it.
    harness.frame(pressing({Action::UiRight}));
    const PickId next = harness.room.at(0, 4, 17);
    CHECK(harness.interaction.focused() == next);
    CHECK(harness.interaction.walkerEasing());
    harness.idle(30);
    CHECK(sameWalker(harness.pose(), harness.room.focusPose(next)));
    CHECK(harness.interaction.pullOf(start) == 0.0f);
    CHECK(harness.interaction.pullOf(next) == 1.0f);
    CHECK(harness.interaction.hovered() == next);
    // Up and down: the levels above and below.
    harness.frame(keyPress(rv::input::Key::Up));
    CHECK(harness.interaction.focused() == harness.room.at(0, 5, 17));
    harness.frame(padPress(rv::input::GamepadButton::DpadDown));
    harness.frame(padPress(rv::input::GamepadButton::DpadDown));
    CHECK(harness.interaction.focused() == harness.room.at(0, 3, 17));
    harness.idle(30);
    // To the level's end, then into the next module: the focus never strays onto what the ease sweeps past.
    for (int slot = 18; slot <= 32; ++slot) {
        harness.frame(padPress(rv::input::GamepadButton::DpadRight));
        CHECK(harness.interaction.focused() == harness.room.at(0, 3, slot));
    }
    harness.frame(padPress(rv::input::GamepadButton::DpadRight));
    CHECK(harness.interaction.focused() == harness.room.at(1, 3, 0));
    harness.idle(60);
    CHECK(harness.interaction.focused() == harness.room.at(1, 3, 0));
    CHECK(sameWalker(harness.pose(), harness.room.focusPose(harness.room.at(1, 3, 0))));
    // A step that finds nothing (left of the wall's first item) leaves the focus where it is.
    InteractionHarness atEnd = focused();
    for (int slot = 15; slot >= 0; --slot) {
        atEnd.frame(pressing({Action::UiLeft}));
    }
    CHECK(atEnd.interaction.focused() == atEnd.room.at(0, 4, 0));
    atEnd.frame(pressing({Action::UiLeft}));
    CHECK(atEnd.interaction.focused() == atEnd.room.at(0, 4, 0));
}

TEST_CASE("app/world_interaction: looking moves the focus within its shelf section", "[app][world_interaction][transitions]") {
    InteractionHarness harness = focused();
    // About two spines to the right at 0.9 m (2.2 mrad per pixel, 26.7 mrad per spine).
    ActionState look;
    look.setAxis(Action::LookDeltaX, 24.0f);
    harness.frame(look);
    harness.idle(3);
    CHECK(harness.interaction.focused() == harness.room.at(0, 4, 18));
    CHECK(harness.interaction.pullOf(harness.room.at(0, 4, 18)) > 0.0f);
    // A still view held 60 frames never changes it.
    for (int frame = 0; frame < 60; ++frame) {
        harness.idle(1);
        REQUIRE(harness.interaction.focused() == harness.room.at(0, 4, 18));
    }
    // Looking onto the next module's items (another section) does not move the focus there.
    InteractionHarness edge = focused();
    for (int slot = 17; slot <= 32; ++slot) {
        edge.frame(pressing({Action::UiRight}));
    }
    edge.idle(30);
    REQUIRE(edge.interaction.focused() == edge.room.at(0, 4, 32));
    // 0.31 rad: 29 cm to the right at 0.9 m, past the gap between the modules onto module 1's slot 2.
    look.setAxis(Action::LookDeltaX, 140.0f);
    edge.frame(look);
    edge.idle(5);
    REQUIRE(edge.interaction.hovered() != kNoPick);
    CHECK(edge.room.shelves().item(edge.interaction.hovered()).module == 1);
    CHECK(edge.interaction.focused() == edge.room.at(0, 4, 32));
}

TEST_CASE("app/world_interaction: Avvia launches and the return comes back to the same inspection",
          "[app][world_interaction][transitions]") {
    InteractionHarness harness = inspecting();
    const PickId item = harness.room.at(0, 4, 16);
    ActionState turn;
    turn.setAxis(Action::LookX, 0.5f);
    harness.frame(turn);
    const rv::app::InspectView view = harness.interaction.inspectView();
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InteractionResult result = harness.press(rv::app::CardAction::Launch);
    REQUIRE(result.launch.has_value());
    CHECK(result.launch->item == item);
    CHECK(result.launch->game == rv::test::ShelfRoom::gameOf(item));
    CHECK(harness.interaction.state() == WorldState::Launching);
    CHECK(harness.interaction.context() == WorldContext::Suspended);
    REQUIRE(harness.interaction.launchRecord().has_value());
    CHECK(harness.interaction.launchRecord()->game == rv::test::ShelfRoom::gameOf(item));
    CHECK(harness.interaction.launchRecord()->inspect == view);
    CHECK(sameWalker(harness.interaction.launchRecord()->walker, walker));
    // While the game runs nothing is read (the panel has the input), Back included.
    CHECK_FALSE(harness.frame(pressing({Action::UiActivate})).launch.has_value());
    CHECK_FALSE(harness.frame(keyPress(rv::input::Key::Escape)).shellBack.has_value());
    CHECK(harness.interaction.backLevel() == WorldBack::Shell);
    // The walker moved meanwhile (a window handed over and back): the return restores it.
    harness.place(facing(harness.room.shelves().item(harness.room.at(1, 2, 3)), 2.0f));
    harness.interaction.launchEnded(rv::app::LaunchMessage{std::string(rv::app::launch_outcome::kReturned), "", ""});
    CHECK(harness.interaction.state() == WorldState::Returning);
    const rv::app::InteractionResult back = harness.idle(1);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == item);
    REQUIRE(back.walker.has_value());
    CHECK(sameWalker(*back.walker, walker));
    CHECK(sameWalker(harness.pose(), walker));
    CHECK(harness.interaction.inspectView() == view);
    CHECK(harness.interaction.notice() == InteractionNotice::None);
    // launchEnded outside Launching changes nothing.
    harness.interaction.launchEnded(rv::app::LaunchMessage{"crashed", "", ""});
    CHECK(harness.interaction.state() == WorldState::Inspect);
}

TEST_CASE("app/world_interaction: a launch that fails stays in Inspect and says why", "[app][world_interaction][failures]") {
    InteractionHarness harness = inspecting();
    REQUIRE(harness.frame(pressing({Action::Confirm})).launch.has_value());
    const rv::app::LaunchMessage refused{std::string(rv::app::launch_outcome::kNotReady), "rom-missing", "game.sfc"};
    harness.interaction.launchEnded(refused);
    harness.idle(1);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.notice() == InteractionNotice::LaunchOutcome);
    CHECK(harness.interaction.noticeMessage() == refused);
    // The state is usable: Back leaves it, and the notice goes with the next transition.
    harness.frame(pressing({Action::UiBack}));
    CHECK(harness.interaction.state() == WorldState::Focus);
    harness.idle(20);
    harness.frame(pressing({Action::Interact}));
    CHECK(harness.interaction.notice() == InteractionNotice::None);
}

TEST_CASE("app/world_interaction: an item the room no longer holds falls back to FreeRoam", "[app][world_interaction][failures]") {
    // Removed while focused: FreeRoam, the notice, the walker where it stands.
    InteractionHarness inFocus = focused();
    const PickId item = inFocus.room.at(0, 4, 16);
    const rv::scene::WalkerPose walker = inFocus.pose();
    inFocus.room.remove(item);
    inFocus.interaction.roomChanged();
    CHECK(inFocus.interaction.state() == WorldState::FreeRoam);
    CHECK(inFocus.interaction.notice() == InteractionNotice::ItemUnavailable);
    CHECK(inFocus.interaction.focused() == kNoPick);
    CHECK(inFocus.interaction.pullOf(item) == 0.0f);
    CHECK(inFocus.interaction.hovered() == kNoPick);
    inFocus.idle(10);
    CHECK(sameWalker(inFocus.pose(), walker));
    // The stale index still hits it: never hovered again.
    CHECK(inFocus.interaction.hovered() == kNoPick);

    // Removed while inspected.
    InteractionHarness inInspect = inspecting();
    inInspect.room.remove(inInspect.room.at(0, 4, 16));
    inInspect.interaction.roomChanged();
    CHECK(inInspect.interaction.state() == WorldState::FreeRoam);
    CHECK(inInspect.interaction.inspectView().lift == 0.0f);

    // Removed while hovered, the room not yet told: interact says why and stays.
    InteractionHarness hovered = hovering();
    hovered.room.remove(hovered.room.at(0, 4, 16));
    hovered.frame(pressing({Action::Interact}));
    CHECK(hovered.interaction.state() == WorldState::FreeRoam);
    CHECK(hovered.interaction.notice() == InteractionNotice::ItemUnavailable);
    CHECK(hovered.interaction.hovered() == kNoPick);

    // Removed during a launch: the return restores the walker, then FreeRoam.
    InteractionHarness launching = inspecting();
    const rv::scene::WalkerPose atLaunch = launching.pose();
    REQUIRE(launching.press(rv::app::CardAction::Launch).launch.has_value());
    launching.room.remove(launching.room.at(0, 4, 16));
    launching.interaction.roomChanged();
    CHECK(launching.interaction.state() == WorldState::Launching);
    launching.interaction.launchEnded(rv::app::LaunchMessage{std::string(rv::app::launch_outcome::kReturned), "", ""});
    const rv::app::InteractionResult back = launching.idle(1);
    CHECK(launching.interaction.state() == WorldState::FreeRoam);
    CHECK(launching.interaction.notice() == InteractionNotice::ItemUnavailable);
    REQUIRE(back.walker.has_value());
    CHECK(sameWalker(*back.walker, atLaunch));

    // No room at all (the test room): nothing is hovered, Back is the shell's.
    InteractionHarness noRoom = hovering();
    noRoom.interaction.setRoom(nullptr);
    CHECK(noRoom.interaction.hovered() == kNoPick);
    CHECK(noRoom.interaction.pulledItems().empty());
    noRoom.frame(pressing({Action::Interact}));
    CHECK(noRoom.interaction.state() == WorldState::FreeRoam);
}

TEST_CASE("app/world_interaction: Mostra nella stanza eases to the item's Focus in 600 ms", "[app][world_interaction][transitions]") {
    InteractionHarness harness = hovering();
    const PickId item = harness.room.at(1, 2, 5);
    CHECK(harness.interaction.showInRoom(item, harness.pose()));
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.focused() == item);
    int driven = 0;
    while (harness.idle(1).walker) {
        ++driven;
    }
    CHECK(driven == 36);
    CHECK(sameWalker(harness.pose(), harness.room.focusPose(item)));
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    CHECK(harness.interaction.pullOf(harness.room.at(0, 4, 16)) == 0.0f);
    // Skippable with interact.
    CHECK(harness.interaction.showInRoom(harness.room.at(0, 1, 1), harness.pose()));
    harness.idle(3);
    const rv::app::InteractionResult skipped = harness.frame(pressing({Action::Interact}));
    REQUIRE(skipped.walker.has_value());
    CHECK(sameWalker(*skipped.walker, harness.room.focusPose(harness.room.at(0, 1, 1))));
    CHECK(harness.interaction.state() == WorldState::Focus);
    // An item the room does not hold: refused with the notice, nothing moves.
    CHECK_FALSE(harness.interaction.showInRoom(999999, harness.pose()));
    CHECK(harness.interaction.notice() == InteractionNotice::ItemUnavailable);
    CHECK(harness.interaction.focused() == harness.room.at(0, 1, 1));
    // Not from Inspect.
    InteractionHarness inInspect = inspecting();
    CHECK_FALSE(inInspect.interaction.showInRoom(inInspect.room.at(1, 2, 5), inInspect.pose()));
    CHECK(inInspect.interaction.state() == WorldState::Inspect);
}

TEST_CASE("app/world_interaction: while the product UI has the input only the animations move",
          "[app][world_interaction][transitions]") {
    InteractionHarness harness = hovering();
    const PickId item = harness.room.at(0, 4, 16);
    harness.frame(pressing({Action::Interact}));
    // The catalog opens over Focus: the frames carry no actions.
    for (int frame = 0; frame < 30; ++frame) {
        harness.frame(nullptr);
    }
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    CHECK(sameWalker(harness.pose(), harness.room.focusPose(item)));
}

TEST_CASE("app/world_interaction: reduceMotion makes every transition instant", "[app][world_interaction][transitions]") {
    rv::app::InteractionSettings settings;
    settings.reduceMotion = true;
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    const PickId item = probe.at(0, 4, 16);
    InteractionHarness harness(facing(probe.shelves().item(item), 1.2f), settings);
    harness.idle(3);
    REQUIRE(harness.interaction.hovered() == item);
    // The dwell still applies (scanning a shelf does not flash every spine); the pull-out itself is instant.
    harness.idle(7);
    CHECK(harness.interaction.pullOf(item) == 0.0f);
    harness.idle(1);
    CHECK(harness.interaction.pullOf(item) == 1.0f);
    // Focus: the walker placed in the press frame only.
    const rv::app::InteractionResult focus = harness.frame(pressing({Action::Interact}));
    REQUIRE(focus.walker.has_value());
    CHECK(sameWalker(*focus.walker, harness.room.focusPose(item)));
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK_FALSE(harness.idle(1).walker.has_value());
    // The lift and the lowering.
    harness.frame(pressing({Action::UiActivate}));
    CHECK(harness.interaction.inspectView().lift == 1.0f);
    harness.frame(pressing({Action::UiBack}));
    CHECK(harness.interaction.inspectView().lift == 0.0f);
    CHECK(harness.interaction.backLevel() == WorldBack::LeaveFocus);
    // A step, and the push-back.
    harness.frame(pressing({Action::UiRight}));
    CHECK(harness.interaction.pullOf(item) == 0.0f);
    CHECK(harness.interaction.pullOf(harness.room.at(0, 4, 17)) == 1.0f);
    // Turned on mid-ease: finished at once, the walker placed.
    InteractionHarness easing = hovering();
    easing.frame(pressing({Action::Interact}));
    easing.idle(3);
    easing.interaction.setReduceMotion(true);
    CHECK_FALSE(easing.interaction.walkerEasing());
    const rv::app::InteractionResult placed = easing.idle(1);
    REQUIRE(placed.walker.has_value());
    CHECK(sameWalker(*placed.walker, easing.room.focusPose(easing.room.at(0, 4, 16))));
    CHECK(easing.interaction.settings().reduceMotion);
}

TEST_CASE("app/world_interaction: names for the log", "[app][world_interaction]") {
    CHECK(rv::app::toString(WorldState::FreeRoam) == "free roam");
    CHECK(rv::app::toString(WorldState::Focus) == "focus");
    CHECK(rv::app::toString(WorldState::Inspect) == "inspect");
    CHECK(rv::app::toString(WorldState::Launching) == "launching");
    CHECK(rv::app::toString(WorldState::Returning) == "returning");
    CHECK(rv::app::toString(WorldState::AnimatingToConsole) == "animating to console");
    CHECK(rv::app::toString(rv::app::InsertionEnd::Arrived) == "arrived");
    CHECK(rv::app::toString(rv::app::InsertionEnd::Skipped) == "skipped");
    CHECK(rv::app::toString(InteractionNotice::None) == "none");
    CHECK(rv::app::toString(InteractionNotice::ItemUnavailable) == "item unavailable");
    CHECK(rv::app::toString(InteractionNotice::LaunchOutcome) == "launch outcome");
}

TEST_CASE("app/world_interaction: the card's buttons are the entry of Avvia, Vedi media, Informazioni and Indietro",
          "[app][world_interaction][card]") {
    // I3 step 14a: the card's buttons have the focus, so ui_activate alone launches nothing; Enter (confirm and
    // ui_activate at once through the default bindings) presses the card's focused button instead; the pad's Start
    // (confirm alone) is Avvia.
    using rv::app::CardAction;
    InteractionHarness enter = inspecting();
    CHECK_FALSE(enter.frame(pressing({Action::UiActivate})).launch.has_value());
    const ActionState enterKey = keyPress(rv::input::Key::Enter);
    REQUIRE(enterKey.pressed(Action::Confirm));
    REQUIRE(enterKey.pressed(Action::UiActivate));
    CHECK_FALSE(enter.frame(enterKey).launch.has_value());
    CHECK(enter.interaction.state() == WorldState::Inspect);
    InteractionHarness start = inspecting();
    const rv::app::InteractionResult started = start.frame(pressing({Action::Confirm}));
    REQUIRE(started.launch.has_value());
    CHECK(start.interaction.state() == WorldState::Launching);

    InteractionHarness card = inspecting();
    const PickId item = card.interaction.focused();
    CHECK(card.press(CardAction::Media).showMedia);
    CHECK(card.press(CardAction::Info).showInfo);
    CHECK(card.interaction.state() == WorldState::Inspect);
    // Indietro is ui_back: back to Focus of the same item.
    const rv::app::InteractionResult back = card.press(CardAction::Back);
    CHECK_FALSE(back.shellBack.has_value());
    CHECK(card.interaction.state() == WorldState::Focus);
    CHECK(card.interaction.focused() == item);
    // Outside Inspect a card press means nothing.
    CHECK_FALSE(card.press(CardAction::Launch).launch.has_value());
    CHECK(card.interaction.state() == WorldState::Focus);
    // Avvia by the card.
    InteractionHarness launch = inspecting();
    const rv::app::InteractionResult launched = launch.press(CardAction::Launch);
    REQUIRE(launched.launch.has_value());
    CHECK(launched.launch->item == item);
    // Without the frame's actions (the product UI has the input) nothing is read, the card's press included.
    InteractionHarness noInput = inspecting();
    CHECK_FALSE(noInput.frame(nullptr, kFrameSeconds, 0.0f, CardAction::Launch).launch.has_value());
    CHECK(noInput.interaction.state() == WorldState::Inspect);
    CHECK(rv::app::toString(CardAction::Launch) == "launch");
    CHECK(rv::app::toString(CardAction::Back) == "back");
}

TEST_CASE("app/world_interaction: the card's presses wait for the item in the hand, but Indietro",
          "[app][world_interaction][card]") {
    using rv::app::CardAction;
    InteractionHarness lifting = focused();
    lifting.frame(pressing({Action::UiActivate}));
    REQUIRE(lifting.interaction.state() == WorldState::Inspect);
    lifting.idle(3);
    REQUIRE(lifting.interaction.inspectView().lift < 1.0f);
    CHECK_FALSE(lifting.press(CardAction::Launch).launch.has_value());
    CHECK_FALSE(lifting.press(CardAction::Media).showMedia);
    CHECK_FALSE(lifting.press(CardAction::Info).showInfo);
    CHECK(lifting.interaction.state() == WorldState::Inspect);
    lifting.press(CardAction::Back);
    CHECK(lifting.interaction.state() == WorldState::Focus);
}

// ---------------------------------------------------------------------------------------------------------------
// The insertion into the console (I5 step 9, ADR-0016 §11)
// ---------------------------------------------------------------------------------------------------------------

namespace {

/// Inspecting with the room's console 1.2 m to the walker's right and below the eye, its socket at `console`.
InteractionHarness inspectingWithConsole(rv::app::InteractionSettings settings = {}) {
    InteractionHarness harness = inspecting(settings);
    const rv::scene::WalkerPose pose = harness.pose();
    const glm::vec3 right(std::cos(pose.yaw), 0.0f, std::sin(pose.yaw));
    harness.room.console = pose.feet + 1.2f * right + glm::vec3(0.0f, 0.6f, 0.0f);
    return harness;
}

/// The insertion's frames from Avvia (its frame included) to the frame it ended; the last result.
int framesToEnd(InteractionHarness& harness, rv::app::InteractionResult& last, int limit = 100) {
    int frames = 1;
    while (!last.insertionEnded && frames < limit) {
        last = harness.idle(1);
        ++frames;
    }
    return frames;
}

} // namespace

TEST_CASE("app/world_interaction: Avvia in a room with a console eases the item into it in 450 ms, the launch held "
          "until then",
          "[app][world_interaction][insertion]") {
    InteractionHarness harness = inspectingWithConsole();
    const PickId item = harness.interaction.focused();
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InspectView view = harness.interaction.inspectView();
    const rv::scene::WalkerPose facingConsole = *harness.room.consolePose(walker);
    rv::app::InteractionResult last = harness.press(rv::app::CardAction::Launch);
    REQUIRE(last.launch.has_value());
    CHECK(last.launch->item == item);
    CHECK(last.launch->held);
    CHECK_FALSE(last.insertionEnded.has_value());
    CHECK(harness.interaction.state() == WorldState::AnimatingToConsole);
    CHECK(harness.interaction.context() == WorldContext::Suspended);
    CHECK(harness.interaction.focused() == item);
    CHECK(harness.interaction.highlighted() == kNoPick);
    CHECK(harness.interaction.backLevel() == WorldBack::Shell);
    // The first frame already moves it (its own frame time), the eye turning with it.
    CHECK(harness.interaction.insertion() > 0.0f);
    CHECK(harness.interaction.insertion() < 0.05f);
    REQUIRE(last.walker.has_value());
    REQUIRE(harness.interaction.launchRecord().has_value());
    CHECK(sameWalker(harness.interaction.launchRecord()->walker, walker));
    CHECK(harness.interaction.launchRecord()->inspect == view);
    // Neither the editor nor "Mostra nella stanza" during it.
    CHECK_FALSE(harness.interaction.enterEdit());
    CHECK_FALSE(harness.interaction.showInRoom(harness.room.at(1, 2, 3), harness.pose()));

    // 450 ms at 60 Hz: 27 frames, Avvia's included; the eye on the socket, the launch's hand-over released.
    const int frames = framesToEnd(harness, last);
    CHECK(frames == 27);
    REQUIRE(last.insertionEnded.has_value());
    CHECK(*last.insertionEnded == rv::app::InsertionEnd::Arrived);
    CHECK_FALSE(last.launch.has_value());
    CHECK(harness.interaction.state() == WorldState::Launching);
    CHECK(harness.interaction.insertion() == 1.0f);
    CHECK(sameWalker(harness.pose(), facingConsole));
    // It stays in the socket while the game runs.
    harness.idle(5);
    CHECK(harness.interaction.insertion() == 1.0f);
    CHECK_FALSE(harness.interaction.walkerEasing());

    // The return: Inspect of the same item, in the hand as before Avvia, the walker where Avvia found it.
    harness.interaction.launchEnded(rv::app::LaunchMessage{std::string(rv::app::launch_outcome::kReturned), "", ""});
    const rv::app::InteractionResult back = harness.idle(1);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == item);
    CHECK(harness.interaction.insertion() == 0.0f);
    CHECK(harness.interaction.inspectView() == view);
    REQUIRE(back.walker.has_value());
    CHECK(sameWalker(harness.pose(), walker));
    CHECK(harness.interaction.notice() == InteractionNotice::None);
}

TEST_CASE("app/world_interaction: any button skips the insertion, the launch going on within the frame",
          "[app][world_interaction][insertion]") {
    // A look, a walk and the global actions (a screenshot, the mute) are no skip; any other press is, Back included.
    const auto skippedBy = [](std::initializer_list<Action> actions, float look) {
        InteractionHarness harness = inspectingWithConsole();
        const rv::scene::WalkerPose facingConsole = *harness.room.consolePose(harness.pose());
        REQUIRE(harness.press(rv::app::CardAction::Launch).launch->held);
        harness.idle(5);
        ActionState state = pressing(actions);
        state.setAxis(Action::LookDeltaX, look);
        const rv::app::InteractionResult result = harness.frame(state);
        const bool skipped = result.insertionEnded == rv::app::InsertionEnd::Skipped;
        if (skipped) {
            CHECK(harness.interaction.state() == WorldState::Launching);
            CHECK(harness.interaction.insertion() == 1.0f);
            REQUIRE(result.walker.has_value());
            CHECK(sameWalker(harness.pose(), facingConsole));
        } else {
            CHECK(harness.interaction.state() == WorldState::AnimatingToConsole);
        }
        return skipped;
    };
    CHECK(skippedBy({Action::Interact}, 0.0f));
    CHECK(skippedBy({Action::Confirm}, 0.0f));
    CHECK(skippedBy({Action::Back}, 0.0f));
    CHECK(skippedBy({Action::UiBack}, 0.0f));
    CHECK(skippedBy({Action::QuickCatalog}, 0.0f));
    CHECK_FALSE(skippedBy({}, 40.0f));
    CHECK_FALSE(skippedBy({Action::Screenshot, Action::ToggleMute}, 0.0f));
    // Through the default bindings: a key and the pad's buttons.
    CHECK(rv::app::anyButtonPressed(keyPress(rv::input::Key::E)));
    CHECK(rv::app::anyButtonPressed(keyPress(rv::input::Key::Escape)));
    CHECK(rv::app::anyButtonPressed(padPress(rv::input::GamepadButton::South)));
    CHECK(rv::app::anyButtonPressed(padPress(rv::input::GamepadButton::Start)));
    CHECK_FALSE(rv::app::anyButtonPressed(keyPress(rv::input::Key::F12)));
    CHECK_FALSE(rv::app::anyButtonPressed(ActionState{}));
    // Without the frame's actions (the product UI has the input) it runs on.
    InteractionHarness noInput = inspectingWithConsole();
    REQUIRE(noInput.press(rv::app::CardAction::Launch).launch->held);
    noInput.frame(nullptr);
    CHECK(noInput.interaction.state() == WorldState::AnimatingToConsole);
}

TEST_CASE("app/world_interaction: the insertion is instant under reduceMotion: the launch at once",
          "[app][world_interaction][insertion]") {
    rv::app::InteractionSettings settings;
    settings.reduceMotion = true;
    InteractionHarness harness = inspectingWithConsole(settings);
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InteractionResult result = harness.press(rv::app::CardAction::Launch);
    REQUIRE(result.launch.has_value());
    CHECK_FALSE(result.launch->held);
    CHECK_FALSE(result.insertionEnded.has_value());
    CHECK(harness.interaction.state() == WorldState::Launching);
    CHECK(harness.interaction.insertion() == 0.0f);
    CHECK(sameWalker(harness.pose(), walker));

    // Turned on during one: it arrives on the next update.
    InteractionHarness turned = inspectingWithConsole();
    REQUIRE(turned.press(rv::app::CardAction::Launch).launch->held);
    turned.idle(3);
    turned.interaction.setReduceMotion(true);
    const rv::app::InteractionResult next = turned.idle(1);
    CHECK(next.insertionEnded == rv::app::InsertionEnd::Arrived);
    CHECK(turned.interaction.state() == WorldState::Launching);
    CHECK(turned.interaction.insertion() == 1.0f);
}

TEST_CASE("app/world_interaction: a launch refused during the insertion comes back to Inspect with the item in the "
          "hand",
          "[app][world_interaction][insertion][failures]") {
    InteractionHarness harness = inspectingWithConsole();
    const PickId item = harness.interaction.focused();
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InspectView view = harness.interaction.inspectView();
    REQUIRE(harness.press(rv::app::CardAction::Launch).launch->held);
    harness.idle(8);
    REQUIRE(harness.interaction.state() == WorldState::AnimatingToConsole);
    REQUIRE(harness.interaction.insertion() > 0.0f);
    const rv::app::LaunchMessage refused{std::string(rv::app::launch_outcome::kNotReady), "rom-missing", "game.sfc"};
    harness.interaction.launchEnded(refused);
    CHECK(harness.interaction.state() == WorldState::Returning);
    const rv::app::InteractionResult back = harness.idle(1);
    CHECK_FALSE(back.insertionEnded.has_value());
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == item);
    CHECK(harness.interaction.insertion() == 0.0f);
    CHECK(harness.interaction.inspectView() == view);
    CHECK(harness.interaction.inspectView().lift == 1.0f);
    CHECK(sameWalker(harness.pose(), walker));
    CHECK(harness.interaction.notice() == InteractionNotice::LaunchOutcome);
    CHECK(harness.interaction.noticeMessage() == refused);
    // Usable: Avvia again inserts again.
    const rv::app::InteractionResult again = harness.press(rv::app::CardAction::Launch);
    REQUIRE(again.launch.has_value());
    CHECK(again.launch->held);
    CHECK(harness.interaction.state() == WorldState::AnimatingToConsole);
}

TEST_CASE("app/world_interaction: no insertion without a console", "[app][world_interaction][insertion]") {
    // The arcade's hero corner and a shared room's stand without a console (ADR-0013 §6): their rooms answer no
    // consolePose(), and Avvia launches at once as before.
    InteractionHarness harness = inspecting();
    REQUIRE_FALSE(harness.room.console.has_value());
    const rv::scene::WalkerPose walker = harness.pose();
    const rv::app::InteractionResult result = harness.press(rv::app::CardAction::Launch);
    REQUIRE(result.launch.has_value());
    CHECK_FALSE(result.launch->held);
    CHECK(harness.interaction.state() == WorldState::Launching);
    CHECK(harness.interaction.insertion() == 0.0f);
    harness.idle(30);
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK(sameWalker(harness.pose(), walker));
    // The insertion's length 0 is none either.
    rv::app::InteractionSettings instant;
    instant.timings.insertion = 0.0f;
    InteractionHarness zero = inspectingWithConsole(instant);
    const rv::app::InteractionResult launched = zero.press(rv::app::CardAction::Launch);
    REQUIRE(launched.launch.has_value());
    CHECK_FALSE(launched.launch->held);
    CHECK(zero.interaction.state() == WorldState::Launching);
}
