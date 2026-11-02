#include <catch2/catch_test_macros.hpp>

#include <bitset>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <vector>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/binding.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/ui_command.hpp>

using rv::input::Action;
using rv::input::ActionState;
using rv::input::Key;
using rv::ui::DirectionRepeater;
using rv::ui::NavDirection;
using rv::ui::NavInput;
using rv::ui::NavSettings;
using rv::ui::StickDirection;
using rv::ui::UiCommand;
using rv::ui::UiCommandList;

namespace {

/// A frame step that binary floating point represents exactly, so frame times add up without rounding.
constexpr double kDt = 1.0 / 64.0;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// Steps of a repeater holding `direction` on frames [first, last] (time = frame * kDt), the press on frame 0.
int countSteps(DirectionRepeater& repeater, NavDirection direction, int first, int last) {
    int steps = 0;
    for (int frame = first; frame <= last; ++frame) {
        if (repeater.update(direction, frame == 0 ? 0.0 : kDt)) {
            ++steps;
        }
    }
    return steps;
}

// The action of a command as InputSystem resolves it: went down this frame (and still held), held, tapped within the
// frame (down and up again: pressed and released while not held).
void press(ActionState& actions, UiCommand command) {
    actions.setButton(rv::ui::actionFor(command), true, true, false);
}

void hold(ActionState& actions, UiCommand command) {
    actions.setButton(rv::ui::actionFor(command), true, false, false);
}

void tap(ActionState& actions, UiCommand command) {
    actions.setButton(rv::ui::actionFor(command), false, true, true);
}

/// The UI's stick (UiNavX, UiNavY) as the bindings give it (the default ones: the stick's travel itself).
ActionState stick(float x, float y) {
    ActionState actions;
    actions.setAxis(Action::UiNavX, x);
    actions.setAxis(Action::UiNavY, y);
    return actions;
}

std::vector<UiCommand> commandsOf(NavInput& nav, const ActionState& actions, double deltaSeconds = kDt) {
    UiCommandList list;
    nav.update(actions, deltaSeconds, list);
    return {list.commands().begin(), list.commands().end()};
}

std::vector<UiCommand> just(std::initializer_list<UiCommand> commands) {
    return commands;
}

} // namespace

TEST_CASE("ui/nav_input: a held direction steps at once, after the delay, then once per interval", "[ui][nav_input]") {
    DirectionRepeater repeater;
    CHECK(repeater.update(NavDirection::Down, 0.0) == NavDirection::Down);
    // Nothing before the 0.4 s delay (frame 25 is at 0.390625 s)...
    CHECK(countSteps(repeater, NavDirection::Down, 1, 25) == 0);
    // ...then one step by 0.40625 s, and one per 0.1 s: 0.5, 0.6, 0.7, 0.8, 0.9 by frame 60 (0.9375 s).
    CHECK(countSteps(repeater, NavDirection::Down, 26, 26) == 1);
    CHECK(countSteps(repeater, NavDirection::Down, 27, 60) == 5);
    CHECK(repeater.held() == NavDirection::Down);
}

TEST_CASE("ui/nav_input: 24 frames at 60 Hz reach the 0.4 s delay despite rounding", "[ui][nav_input]") {
    DirectionRepeater repeater;
    REQUIRE(repeater.update(NavDirection::Down, 0.0) == NavDirection::Down);
    for (int frame = 1; frame < 24; ++frame) {
        CHECK_FALSE(repeater.update(NavDirection::Down, 1.0 / 60.0).has_value());
    }
    // 24 x (1/60) is 0.39999999999999997 in binary floating point.
    CHECK(repeater.update(NavDirection::Down, 1.0 / 60.0) == NavDirection::Down);
    for (int frame = 25; frame < 30; ++frame) {
        CHECK_FALSE(repeater.update(NavDirection::Down, 1.0 / 60.0).has_value());
    }
    CHECK(repeater.update(NavDirection::Down, 1.0 / 60.0) == NavDirection::Down); // 30 frames: 0.5 s
}

TEST_CASE("ui/nav_input: a direction held past 1.5 s repeats twice as fast", "[ui][nav_input]") {
    DirectionRepeater repeater;
    const int slow = countSteps(repeater, NavDirection::Up, 0, 128);    // 0 .. 2.0 s
    const int fast = countSteps(repeater, NavDirection::Up, 129, 192); // 2.0 .. 3.0 s: one step per 0.05 s
    CHECK(slow >= 16);
    CHECK(fast >= 19);
    CHECK(fast <= 21);
}

TEST_CASE("ui/nav_input: changing or releasing the direction restarts the repeat", "[ui][nav_input]") {
    DirectionRepeater repeater;
    CHECK(repeater.update(NavDirection::Down, 0.0) == NavDirection::Down);
    CHECK_FALSE(repeater.update(NavDirection::Down, kDt).has_value());
    CHECK(repeater.update(NavDirection::Right, kDt) == NavDirection::Right); // a new direction steps at once
    CHECK_FALSE(repeater.update(NavDirection::Right, kDt).has_value());
    CHECK_FALSE(repeater.update(std::nullopt, kDt).has_value()); // released
    CHECK_FALSE(repeater.held().has_value());
    CHECK(repeater.update(NavDirection::Right, kDt) == NavDirection::Right); // pressed again: at once
}

TEST_CASE("ui/nav_input: a long frame steps once, without a backlog", "[ui][nav_input]") {
    DirectionRepeater repeater;
    CHECK(repeater.update(NavDirection::Down, 0.0) == NavDirection::Down);
    CHECK(repeater.update(NavDirection::Down, 5.0) == NavDirection::Down); // a 5 s hitch
    CHECK_FALSE(repeater.update(NavDirection::Down, kDt).has_value());    // no catching up
    CHECK(repeater.update(NavDirection::Down, 0.05) == NavDirection::Down); // the fast cadence resumes
    // Frame deltas that are not finite or negative do not advance time.
    CHECK_FALSE(repeater.update(NavDirection::Down, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(repeater.update(NavDirection::Down, -1.0).has_value());
}

TEST_CASE("ui/nav_input: the stick engages past 0.55, releases below 0.35 and keeps its state in between",
          "[ui][nav_input]") {
    StickDirection stickDirection;
    CHECK_FALSE(stickDirection.update(0.0f, -0.5f).has_value());           // below the engage threshold
    CHECK(stickDirection.update(0.0f, -0.6f) == NavDirection::Down);      // y up: pulled towards the player
    CHECK(stickDirection.update(0.0f, -0.4f) == NavDirection::Down);      // above the release threshold
    CHECK_FALSE(stickDirection.update(0.0f, -0.3f).has_value());           // released
    CHECK_FALSE(stickDirection.update(0.0f, -0.5f).has_value());           // not engaged again until 0.55
    CHECK(stickDirection.update(-0.7f, 0.0f) == NavDirection::Left);
    CHECK_FALSE(stickDirection.update(kNaN, kNaN).has_value());            // non-finite: centered
}

TEST_CASE("ui/nav_input: a stick resting near a threshold never toggles", "[ui][nav_input]") {
    StickDirection stickDirection;
    // Noise just under the engage threshold: never engaged.
    for (int sample = 0; sample < 200; ++sample) {
        CHECK_FALSE(stickDirection.update(0.02f, sample % 2 == 0 ? -0.45f : -0.54f).has_value());
    }
    // Engaged, then noise between the release threshold and full deflection: always engaged.
    REQUIRE(stickDirection.update(0.0f, -0.9f) == NavDirection::Down);
    for (int sample = 0; sample < 200; ++sample) {
        CHECK(stickDirection.update(0.03f, sample % 2 == 0 ? -0.36f : -0.6f) == NavDirection::Down);
    }
}

TEST_CASE("ui/nav_input: near a diagonal the stick keeps its axis until the other one clearly dominates",
          "[ui][nav_input]") {
    StickDirection stickDirection;
    CHECK(stickDirection.update(0.6f, -0.6f) == NavDirection::Down);   // a perfect diagonal goes vertical
    CHECK(stickDirection.update(0.65f, -0.6f) == NavDirection::Down);  // 0.65 < 1.25 x 0.6
    CHECK(stickDirection.update(0.6f, -0.62f) == NavDirection::Down);
    CHECK(stickDirection.update(0.8f, -0.6f) == NavDirection::Right);  // 0.8 > 0.75
    CHECK(stickDirection.update(0.6f, -0.7f) == NavDirection::Right);  // 0.7 < 1.25 x 0.6
    CHECK(stickDirection.update(0.5f, -0.7f) == NavDirection::Down);   // 0.7 > 0.625
    CHECK(stickDirection.update(0.0f, 0.9f) == NavDirection::Up);      // flicked to the other side
}

TEST_CASE("ui/nav_input: every command has an action of its own in the action map", "[ui][nav_input]") {
    std::set<Action> actions;
    for (std::size_t index = 0; index < rv::ui::kUiCommandCount; ++index) {
        const auto command = static_cast<UiCommand>(index);
        const Action action = rv::ui::actionFor(command);
        INFO(rv::ui::toString(command));
        // Confirm is the world's action too, read in the UI for the on-screen keyboard (I7 step 5).
        CHECK((rv::input::isUiAction(action) || command == UiCommand::Confirm));
        CHECK(actions.insert(action).second);
    }
    CHECK(rv::ui::actionFor(UiCommand::Confirm) == Action::Confirm);
    CHECK(rv::ui::actionFor(UiCommand::Down) == Action::UiDown);
    CHECK(rv::ui::actionFor(UiCommand::Activate) == Action::UiActivate);
    CHECK(rv::ui::actionFor(UiCommand::FilterNext) == Action::UiFilterNext);
}

TEST_CASE("ui/nav_input: the presses of the UI actions become commands", "[ui][nav_input]") {
    for (std::size_t index = 0; index < rv::ui::kUiCommandCount; ++index) {
        const auto command = static_cast<UiCommand>(index);
        INFO(rv::ui::toString(command));
        NavInput nav;
        ActionState actions;
        press(actions, command);
        CHECK(commandsOf(nav, actions) == just({command}));
    }
    // A tapped direction (down and up within one frame) steps once.
    NavInput nav;
    ActionState tapped;
    tap(tapped, UiCommand::Down);
    CHECK(commandsOf(nav, tapped) == just({UiCommand::Down}));
    // The world's actions are not the UI's: Back and Interact pressed give no command.
    ActionState world;
    world.setButton(Action::Back, true, true, false);
    world.setButton(Action::Interact, true, true, false);
    world.setAxis(Action::MoveForward, -1.0f);
    CHECK(commandsOf(nav, world).empty());
}

TEST_CASE("ui/nav_input: the steps come before the presses of the same frame, in a fixed order", "[ui][nav_input]") {
    NavInput nav;
    ActionState actions;
    press(actions, UiCommand::FilterNext);
    press(actions, UiCommand::Activate);
    press(actions, UiCommand::PageDown);
    press(actions, UiCommand::Right);
    press(actions, UiCommand::End);
    press(actions, UiCommand::Back);
    CHECK(commandsOf(nav, actions) == just({UiCommand::Right, UiCommand::PageDown, UiCommand::Activate, UiCommand::Back,
                                            UiCommand::End, UiCommand::FilterNext}));
}

TEST_CASE("ui/nav_input: held D-pad and stick repeat alike", "[ui][nav_input]") {
    const auto countHeld = [](const ActionState& first, const ActionState& held, int frames) {
        NavInput nav;
        int downs = static_cast<int>(commandsOf(nav, first, 0.0).size());
        for (int frame = 1; frame <= frames; ++frame) {
            for (const UiCommand command : commandsOf(nav, held)) {
                downs += command == UiCommand::Down ? 1 : 0;
            }
        }
        return downs;
    };
    ActionState firstPad;
    press(firstPad, UiCommand::Down);
    ActionState heldPad;
    hold(heldPad, UiCommand::Down);
    const ActionState heldStick = stick(0.0f, -0.9f);
    // 0, 0.4, 0.5, ..., 0.9 s: 7 steps within 60 frames.
    CHECK(countHeld(firstPad, heldPad, 60) == 7);
    CHECK(countHeld(heldStick, heldStick, 60) == 7);
}

TEST_CASE("ui/nav_input: digital directions win over the stick, the last pressed over the others", "[ui][nav_input]") {
    NavInput nav;
    CHECK(commandsOf(nav, stick(0.0f, -0.9f)) == just({UiCommand::Down}));
    ActionState both = stick(0.0f, -0.9f);
    press(both, UiCommand::Up);
    CHECK(commandsOf(nav, both) == just({UiCommand::Up})); // the D-pad wins over the held stick

    NavInput keys;
    ActionState down;
    press(down, UiCommand::Down);
    CHECK(commandsOf(keys, down) == just({UiCommand::Down}));
    ActionState two;
    hold(two, UiCommand::Down);
    press(two, UiCommand::Right);
    CHECK(commandsOf(keys, two) == just({UiCommand::Right})); // pressed last
    hold(two, UiCommand::Right);
    CHECK(commandsOf(keys, two).empty()); // still Right, within its delay
}

TEST_CASE("ui/nav_input: a direction released and pressed again steps at once", "[ui][nav_input]") {
    NavInput nav;
    ActionState down;
    press(down, UiCommand::Down);
    CHECK(commandsOf(nav, down) == just({UiCommand::Down}));
    ActionState held;
    hold(held, UiCommand::Down);
    CHECK(commandsOf(nav, held).empty());
    CHECK(commandsOf(nav, ActionState{}).empty()); // released
    CHECK(commandsOf(nav, down) == just({UiCommand::Down}));
    // The stick held down, then the D-pad down pressed: a fresh step, not the stick's repeat cadence.
    NavInput both;
    CHECK(commandsOf(both, stick(0.0f, -0.9f)) == just({UiCommand::Down}));
    ActionState stickAndPad = stick(0.0f, -0.9f);
    press(stickAndPad, UiCommand::Down);
    CHECK(commandsOf(both, stickAndPad) == just({UiCommand::Down}));
}

TEST_CASE("ui/nav_input: after a reset held directions and pages wait for their release", "[ui][nav_input]") {
    NavInput nav;
    ActionState first;
    press(first, UiCommand::Down);
    CHECK(commandsOf(nav, first) == just({UiCommand::Down}));
    nav.reset(); // e.g. the screen opened while the pad was held

    ActionState held;
    hold(held, UiCommand::Down);
    for (int frame = 0; frame < 120; ++frame) {
        CHECK(commandsOf(nav, held).empty());
    }
    CHECK(commandsOf(nav, stick(0.0f, -0.9f)).empty()); // the stick too, until everything is released
    CHECK(commandsOf(nav, ActionState{}).empty());
    CHECK(commandsOf(nav, first) == just({UiCommand::Down}));

    // A trigger held through the reset pages only once released and pulled again.
    NavInput pages;
    ActionState pull;
    press(pull, UiCommand::PageDown);
    CHECK(commandsOf(pages, pull) == just({UiCommand::PageDown}));
    pages.reset();
    ActionState holding;
    hold(holding, UiCommand::PageDown);
    for (int frame = 0; frame < 60; ++frame) {
        CHECK(commandsOf(pages, holding).empty());
    }
    CHECK(commandsOf(pages, ActionState{}).empty());
    CHECK(commandsOf(pages, pull) == just({UiCommand::PageDown}));
}

TEST_CASE("ui/nav_input: a stick resting around half deflection never moves the focus", "[ui][nav_input]") {
    NavInput nav;
    int steps = 0;
    for (int frame = 0; frame < 240; ++frame) {
        // A worn stick resting around 0.5: alternating samples that a single 0.5 threshold would turn into a burst.
        steps += static_cast<int>(commandsOf(nav, stick(0.01f, frame % 3 == 0 ? -0.52f : -0.47f)).size());
    }
    CHECK(steps == 0);
}

TEST_CASE("ui/nav_input: page moves repeat while held, the one pressed last winning", "[ui][nav_input]") {
    NavInput nav;
    ActionState pull;
    press(pull, UiCommand::PageDown);
    int pages = static_cast<int>(commandsOf(nav, pull, 0.0).size());
    ActionState holding;
    hold(holding, UiCommand::PageDown);
    for (int frame = 1; frame <= 60; ++frame) {
        for (const UiCommand command : commandsOf(nav, holding)) {
            pages += command == UiCommand::PageDown ? 1 : 0;
        }
    }
    CHECK(pages == 7); // as a held direction: 0, 0.4, 0.5, ..., 0.9 s

    // Both triggers: the one pulled last pages, and its repeat starts afresh.
    ActionState both;
    hold(both, UiCommand::PageDown);
    press(both, UiCommand::PageUp);
    CHECK(commandsOf(nav, both) == just({UiCommand::PageUp}));
    // A page and a focus step in one frame: both, the step first.
    NavInput mixed;
    ActionState stepAndPage;
    press(stepAndPage, UiCommand::PageUp);
    press(stepAndPage, UiCommand::Down);
    CHECK(commandsOf(mixed, stepAndPage) == just({UiCommand::Down, UiCommand::PageUp}));
}

TEST_CASE("ui/nav_input: a trigger resting at the press threshold pages at the held cadence, never per crossing",
          "[ui][nav_input]") {
    // The right trigger through the default bindings, chattering around the input system's press threshold (0.5 past
    // the binding's 0.15 dead zone: 0.575 of its travel): its held flag flickers every frame, the page stays engaged.
    const auto trigger = [](float value) {
        rv::input::RawInputFrame frame;
        frame.gamepadConnected = true;
        frame.gamepadAxes[rv::input::toIndex(rv::input::GamepadAxis::RightTrigger)] = value;
        return frame;
    };
    rv::input::InputSystem input;
    NavInput nav;
    int flickers = 0;
    int pages = 0;
    const auto run = [&](int first, int last, float high, float low) {
        for (int frame = first; frame <= last; ++frame) {
            const ActionState actions = input.resolve(trigger(frame % 2 == 0 ? high : low));
            flickers += actions.pressed(Action::UiPageDown) ? 1 : 0;
            for (const UiCommand command : commandsOf(nav, actions, frame == 0 ? 0.0 : kDt)) {
                pages += command == UiCommand::PageDown ? 1 : 0;
            }
        }
    };
    run(0, 19, 0.585f, 0.565f); // 0.30 s, under the repeat delay
    CHECK(flickers == 10);      // a press every other frame for the input system
    CHECK(pages == 1);
    run(20, 63, 0.585f, 0.565f); // on to 0.98 s: the held cadence, 0, 0.4, 0.5, ..., 0.9 s
    CHECK(pages == 7);
    // Under the release threshold (0.35 past the dead zone: 0.45 of the travel) the page lets go; the next pull pages
    // at once.
    run(64, 65, 0.44f, 0.44f);
    CHECK(pages == 7);
    run(66, 66, 0.6f, 0.6f);
    CHECK(pages == 8);
}

TEST_CASE("ui/nav_input: custom settings are honored", "[ui][nav_input]") {
    NavSettings settings;
    settings.repeatDelay = 0.2;
    settings.stickEngage = 0.8f;
    NavInput nav(settings);
    CHECK(commandsOf(nav, stick(0.0f, -0.7f)).empty()); // under the stricter threshold
    ActionState first;
    press(first, UiCommand::Down);
    CHECK(commandsOf(nav, first, 0.0) == just({UiCommand::Down}));
    ActionState held;
    hold(held, UiCommand::Down);
    int steps = 0;
    for (int frame = 1; frame <= 13; ++frame) { // 0.203 s
        steps += static_cast<int>(commandsOf(nav, held).size());
    }
    CHECK(steps == 1); // the repeat after 0.2 s
    CHECK(nav.settings().repeatDelay == 0.2);
}

TEST_CASE("ui/nav_input: the command keys are the keys bound to the UI's actions", "[ui][nav_input]") {
    using rv::input::toIndex;
    const std::bitset<rv::input::kKeyCount> defaults = rv::ui::commandKeys(rv::input::ActionMap::defaults());
    for (const Key key : {Key::Up, Key::Down, Key::Left, Key::Right, Key::Enter, Key::Escape, Key::PageUp,
                          Key::PageDown, Key::Home, Key::End, Key::F2, Key::F3, Key::F4, Key::F6}) {
        INFO(rv::input::keyName(key));
        CHECK(defaults.test(toIndex(key)));
    }
    // Letters, the space bar, Backspace and Tab stay with text fields and RmlUi; the world's keys are not the UI's.
    for (const Key key : {Key::A, Key::W, Key::E, Key::Space, Key::Backspace, Key::Delete, Key::Tab, Key::F1, Key::M}) {
        INFO(rv::input::keyName(key));
        CHECK_FALSE(defaults.test(toIndex(key)));
    }
    CHECK(defaults.count() == 14);

    // A user's bindings: a letter bound to a UI action becomes a command key, an unbound key stops being one.
    rv::input::ActionMap custom = rv::input::ActionMap::defaults();
    custom.bind(Action::UiDown, rv::input::KeyBinding{Key::J, 1.0f});
    custom.clear(Action::UiEnd);
    custom.bind(Action::UiEnd, rv::input::GamepadButtonBinding{rv::input::GamepadButton::RightStick, 1.0f});
    const std::bitset<rv::input::kKeyCount> keys = rv::ui::commandKeys(custom);
    CHECK(keys.test(toIndex(Key::J)));
    CHECK_FALSE(keys.test(toIndex(Key::End)));
    CHECK(rv::ui::commandKeys(rv::input::ActionMap{}).none());
}
