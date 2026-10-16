#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

#include <rv/input/codes.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/ui_command.hpp>

using rv::input::GamepadAxis;
using rv::input::GamepadButton;
using rv::input::Key;
using rv::input::RawInputFrame;
using rv::input::toIndex;
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

/// A connected pad with nothing pressed.
RawInputFrame pad() {
    RawInputFrame frame;
    frame.gamepadConnected = true;
    return frame;
}

void press(RawInputFrame& frame, GamepadButton button) {
    frame.gamepadButtonsPressed.set(toIndex(button));
    frame.gamepadButtonsDown.set(toIndex(button));
}

void hold(RawInputFrame& frame, GamepadButton button) {
    frame.gamepadButtonsDown.set(toIndex(button));
}

void tap(RawInputFrame& frame, GamepadButton button) {
    frame.gamepadButtonsPressed.set(toIndex(button));
    frame.gamepadButtonsReleased.set(toIndex(button));
}

void pressKey(RawInputFrame& frame, Key key) {
    frame.keysPressed.set(toIndex(key));
    frame.keysDown.set(toIndex(key));
}

RawInputFrame stick(float x, float y) {
    RawInputFrame frame = pad();
    frame.gamepadAxes[toIndex(GamepadAxis::LeftX)] = x;
    frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] = y;
    return frame;
}

std::vector<UiCommand> commandsOf(NavInput& nav, const RawInputFrame& frame, double deltaSeconds = kDt) {
    UiCommandList list;
    nav.update(frame, deltaSeconds, list);
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

TEST_CASE("ui/nav_input: D-pad, keys and buttons become UI commands", "[ui][nav_input]") {
    NavInput nav;
    RawInputFrame frame = pad();
    tap(frame, GamepadButton::DpadDown); // pressed and released within the frame
    CHECK(commandsOf(nav, frame) == just({UiCommand::Down}));

    frame = pad();
    press(frame, GamepadButton::South);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Activate}));
    frame = pad();
    press(frame, GamepadButton::East);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Back}));
    frame = pad();
    press(frame, GamepadButton::North);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Media}));
    frame = pad();
    press(frame, GamepadButton::West);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Info}));

    // The keyboard: arrows, Enter and Esc; letters stay free for text fields.
    RawInputFrame keys;
    pressKey(keys, Key::Up);
    CHECK(commandsOf(nav, keys) == just({UiCommand::Up}));
    keys = {};
    pressKey(keys, Key::Enter);
    CHECK(commandsOf(nav, keys) == just({UiCommand::Activate}));
    keys = {};
    pressKey(keys, Key::Escape);
    CHECK(commandsOf(nav, keys) == just({UiCommand::Back}));
    keys = {};
    pressKey(keys, Key::A);
    pressKey(keys, Key::Space);
    CHECK(commandsOf(nav, keys).empty());
}

TEST_CASE("ui/nav_input: a step comes before the buttons of the same frame", "[ui][nav_input]") {
    NavInput nav;
    RawInputFrame frame = pad();
    press(frame, GamepadButton::DpadRight);
    press(frame, GamepadButton::South);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Right, UiCommand::Activate}));
}

TEST_CASE("ui/nav_input: a disconnected pad gives nothing", "[ui][nav_input]") {
    NavInput nav;
    RawInputFrame frame = pad();
    press(frame, GamepadButton::DpadDown);
    press(frame, GamepadButton::South);
    frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] = -1.0f;
    frame.gamepadConnected = false;
    CHECK(commandsOf(nav, frame).empty());
}

TEST_CASE("ui/nav_input: held D-pad and stick repeat alike", "[ui][nav_input]") {
    const auto countHeld = [](const RawInputFrame& first, const RawInputFrame& held, int frames) {
        NavInput nav;
        int downs = static_cast<int>(commandsOf(nav, first, 0.0).size());
        for (int frame = 1; frame <= frames; ++frame) {
            for (const UiCommand command : commandsOf(nav, held)) {
                downs += command == UiCommand::Down ? 1 : 0;
            }
        }
        return downs;
    };
    RawInputFrame firstPad = pad();
    press(firstPad, GamepadButton::DpadDown);
    RawInputFrame heldPad = pad();
    hold(heldPad, GamepadButton::DpadDown);
    const RawInputFrame heldStick = stick(0.0f, -0.9f);
    // 0, 0.4, 0.5, ..., 0.9 s: 7 steps within 60 frames.
    CHECK(countHeld(firstPad, heldPad, 60) == 7);
    CHECK(countHeld(heldStick, heldStick, 60) == 7);
}

TEST_CASE("ui/nav_input: digital directions win over the stick, the last pressed over the others", "[ui][nav_input]") {
    NavInput nav;
    CHECK(commandsOf(nav, stick(0.0f, -0.9f)) == just({UiCommand::Down}));
    RawInputFrame frame = stick(0.0f, -0.9f);
    press(frame, GamepadButton::DpadUp);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Up})); // the D-pad wins over the held stick

    NavInput keys;
    RawInputFrame down;
    pressKey(down, Key::Down);
    CHECK(commandsOf(keys, down) == just({UiCommand::Down}));
    RawInputFrame both;
    both.keysDown.set(toIndex(Key::Down));
    pressKey(both, Key::Right);
    CHECK(commandsOf(keys, both) == just({UiCommand::Right})); // pressed last
    both.keysPressed.reset();
    CHECK(commandsOf(keys, both).empty()); // still Right, within its delay
}

TEST_CASE("ui/nav_input: a direction pressed again within a frame steps again", "[ui][nav_input]") {
    NavInput nav;
    RawInputFrame frame = pad();
    press(frame, GamepadButton::DpadDown);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Down}));
    RawInputFrame again = pad();
    press(again, GamepadButton::DpadDown);
    again.gamepadButtonsReleased.set(toIndex(GamepadButton::DpadDown)); // released and pressed between two frames
    CHECK(commandsOf(nav, again) == just({UiCommand::Down}));
}

TEST_CASE("ui/nav_input: after a reset a held direction waits for its release", "[ui][nav_input]") {
    NavInput nav;
    RawInputFrame first = pad();
    press(first, GamepadButton::DpadDown);
    CHECK(commandsOf(nav, first) == just({UiCommand::Down}));
    nav.reset(); // e.g. the screen opened while the pad was held

    RawInputFrame held = pad();
    hold(held, GamepadButton::DpadDown);
    for (int frame = 0; frame < 120; ++frame) {
        CHECK(commandsOf(nav, held).empty());
    }
    const RawInputFrame stickHeld = stick(0.0f, -0.9f);
    CHECK(commandsOf(nav, stickHeld).empty()); // the stick too, until everything is released
    CHECK(commandsOf(nav, pad()).empty());
    RawInputFrame pressedAgain = pad();
    press(pressedAgain, GamepadButton::DpadDown);
    CHECK(commandsOf(nav, pressedAgain) == just({UiCommand::Down}));
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

TEST_CASE("ui/nav_input: custom bindings and settings are honored", "[ui][nav_input]") {
    rv::ui::NavBindings bindings = rv::ui::NavBindings::defaults();
    bindings.buttons[rv::ui::toIndex(UiCommand::Activate)] = {GamepadButton::Start};
    bindings.leftStickNavigates = false;
    NavSettings settings;
    settings.repeatDelay = 0.2;
    NavInput nav(settings, bindings);
    RawInputFrame frame = pad();
    press(frame, GamepadButton::South);
    CHECK(commandsOf(nav, frame).empty());
    frame = pad();
    press(frame, GamepadButton::Start);
    CHECK(commandsOf(nav, frame) == just({UiCommand::Activate}));
    CHECK(commandsOf(nav, stick(0.0f, -1.0f)).empty());
    CHECK(nav.settings().repeatDelay == 0.2);
}
