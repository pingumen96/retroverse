#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include <rv/input/action_map.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>

using Catch::Approx;
using rv::input::Action;
using rv::input::ActionMap;
using rv::input::ActionState;
using rv::input::Binding;
using rv::input::DeviceKind;
using rv::input::GamepadAxis;
using rv::input::GamepadAxisBinding;
using rv::input::GamepadButton;
using rv::input::InputSettings;
using rv::input::InputSystem;
using rv::input::Key;
using rv::input::KeyBinding;
using rv::input::MouseAxis;
using rv::input::MouseAxisBinding;
using rv::input::MouseButton;
using rv::input::RawInputFrame;
using rv::input::toIndex;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

/// Keyboard frame with `down` held.
RawInputFrame keys(std::initializer_list<Key> down) {
    RawInputFrame frame;
    for (const Key key : down) {
        frame.keysDown.set(toIndex(key));
    }
    return frame;
}

/// Connected gamepad with one axis deflected.
RawInputFrame padAxis(GamepadAxis axis, float value) {
    RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.lastActiveDevice = DeviceKind::Gamepad;
    frame.gamepadAxes[toIndex(axis)] = value;
    return frame;
}

ActionMap singleBinding(Action action, Binding binding) {
    ActionMap map;
    map.bind(action, binding);
    return map;
}

/// Actions reported as pressed, in Action order.
std::vector<Action> pressedActions(const ActionState& state) {
    std::vector<Action> pressed;
    for (std::size_t i = 0; i < rv::input::kActionCount; ++i) {
        if (state.pressed(static_cast<Action>(i))) {
            pressed.push_back(static_cast<Action>(i));
        }
    }
    return pressed;
}

} // namespace

TEST_CASE("input/input_system: opposite keys cancel and single keys give full deflection", "[input][input_system]") {
    InputSystem input;
    CHECK(input.map() == ActionMap::defaults());

    CHECK(input.resolve(keys({Key::W})).axis(Action::MoveForward) == 1.0f);
    CHECK(input.resolve(keys({Key::S})).axis(Action::MoveForward) == -1.0f);
    CHECK(input.resolve(keys({Key::Up})).axis(Action::MoveForward) == 1.0f);
    CHECK(input.resolve(keys({Key::A})).axis(Action::MoveRight) == -1.0f);
    CHECK(input.resolve(keys({Key::Right})).axis(Action::MoveRight) == 1.0f);

    const ActionState both = input.resolve(keys({Key::W, Key::S}));
    CHECK(both.axis(Action::MoveForward) == 0.0f);
    CHECK(both.held(Action::MoveForward)); // bound keys are held even when they cancel out

    // Two keys in the same direction saturate instead of doubling the speed.
    CHECK(input.resolve(keys({Key::W, Key::Up})).axis(Action::MoveForward) == 1.0f);

    // The movement vector is not normalized by the input system.
    const ActionState diagonal = input.resolve(keys({Key::W, Key::D}));
    CHECK(diagonal.axis(Action::MoveForward) == 1.0f);
    CHECK(diagonal.axis(Action::MoveRight) == 1.0f);

    const ActionState idle = input.resolve(RawInputFrame{});
    CHECK(idle.axis(Action::MoveForward) == 0.0f);
    CHECK(idle.axis(Action::MoveRight) == 0.0f);
    CHECK_FALSE(idle.held(Action::MoveForward));
}

TEST_CASE("input/input_system: gamepad dead zone rescales the remaining range", "[input][input_system]") {
    InputSystem input; // left_y: dead zone 0.15, linear
    const auto forward = [&input](float raw) {
        return input.resolve(padAxis(GamepadAxis::LeftY, raw)).axis(Action::MoveForward);
    };

    CHECK(forward(0.0f) == 0.0f);
    CHECK(forward(0.1f) == 0.0f);
    CHECK(forward(0.15f) == 0.0f);
    CHECK(forward(-0.15f) == 0.0f);
    CHECK(forward(0.15f + 0.85f * 0.25f) == Approx(0.25f));
    CHECK(forward(0.575f) == Approx(0.5f));
    CHECK(forward(-0.575f) == Approx(-0.5f));
    CHECK(forward(1.0f) == 1.0f);
    CHECK(forward(-1.0f) == -1.0f);
    // Out-of-range and broken driver values are tamed.
    CHECK(forward(1.5f) == 1.0f);
    CHECK(forward(-7.0f) == -1.0f);
    CHECK(forward(kNaN) == 0.0f);
    CHECK(forward(kInfinity) == 0.0f);

    // Without a dead zone the axis passes through unchanged.
    InputSystem raw(singleBinding(Action::MoveForward, GamepadAxisBinding{GamepadAxis::LeftY, 1.0f, 0.0f, 1.0f}));
    CHECK(raw.resolve(padAxis(GamepadAxis::LeftY, 0.3f)).axis(Action::MoveForward) == Approx(0.3f));
    CHECK(raw.resolve(padAxis(GamepadAxis::LeftY, -0.05f)).axis(Action::MoveForward) == Approx(-0.05f));
}

TEST_CASE("input/input_system: response exponent shapes the curve and keeps the sign", "[input][input_system]") {
    InputSystem squared(singleBinding(Action::MoveRight, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.0f, 2.0f}));
    const auto right = [&squared](float raw) {
        return squared.resolve(padAxis(GamepadAxis::LeftX, raw)).axis(Action::MoveRight);
    };
    CHECK(right(0.5f) == Approx(0.25f));
    CHECK(right(-0.5f) == Approx(-0.25f));
    CHECK(right(0.1f) == Approx(0.01f));
    CHECK(right(1.0f) == 1.0f);
    CHECK(right(0.0f) == 0.0f);

    // The dead zone is removed first, then the curve is applied to the rescaled value.
    InputSystem shaped(singleBinding(Action::MoveRight, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.2f, 2.0f}));
    CHECK(shaped.resolve(padAxis(GamepadAxis::LeftX, 0.6f)).axis(Action::MoveRight) == Approx(0.25f));
    CHECK(shaped.resolve(padAxis(GamepadAxis::LeftX, -0.6f)).axis(Action::MoveRight) == Approx(-0.25f));
    CHECK(shaped.resolve(padAxis(GamepadAxis::LeftX, 0.15f)).axis(Action::MoveRight) == 0.0f);

    // Exponents below 1 boost small deflections.
    InputSystem boosted(singleBinding(Action::MoveRight, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.0f, 0.5f}));
    CHECK(boosted.resolve(padAxis(GamepadAxis::LeftX, 0.25f)).axis(Action::MoveRight) == Approx(0.5f));

    // Scale applies after the curve.
    InputSystem scaled(singleBinding(Action::MoveRight, GamepadAxisBinding{GamepadAxis::LeftX, -0.5f, 0.0f, 2.0f}));
    CHECK(scaled.resolve(padAxis(GamepadAxis::LeftX, 0.5f)).axis(Action::MoveRight) == Approx(-0.125f));
}

TEST_CASE("input/input_system: analog values are clamped to [-1, 1]", "[input][input_system]") {
    InputSystem input;
    RawInputFrame frame = padAxis(GamepadAxis::LeftY, 1.0f);
    frame.keysDown.set(toIndex(Key::W));
    CHECK(input.resolve(frame).axis(Action::MoveForward) == 1.0f);

    frame = padAxis(GamepadAxis::LeftY, -1.0f);
    frame.keysDown.set(toIndex(Key::S));
    frame.keysDown.set(toIndex(Key::Down));
    CHECK(input.resolve(frame).axis(Action::MoveForward) == -1.0f);

    InputSystem tripled(singleBinding(Action::Interact, KeyBinding{Key::E, 3.0f}));
    CHECK(tripled.resolve(keys({Key::E})).axis(Action::Interact) == 1.0f);

    // Mouse axes bound to non-look actions are clamped too (the wheel here).
    InputSystem wheel(singleBinding(Action::MoveForward, MouseAxisBinding{MouseAxis::Wheel, 0.5f}));
    RawInputFrame scroll;
    scroll.mouseWheel = 1.0f;
    CHECK(wheel.resolve(scroll).axis(Action::MoveForward) == 0.5f);
    scroll.mouseWheel = -6.0f;
    CHECK(wheel.resolve(scroll).axis(Action::MoveForward) == -1.0f);

    // Look rates are clamped first, then scaled by the stick sensitivity.
    InputSystem look(singleBinding(Action::LookX, GamepadAxisBinding{GamepadAxis::RightX, 2.0f, 0.0f, 1.0f}));
    look.setSettings(InputSettings{.stickSensitivityScale = 1.5f});
    CHECK(look.resolve(padAxis(GamepadAxis::RightX, 1.0f)).axis(Action::LookX) == Approx(1.5f));
    CHECK(look.resolve(padAxis(GamepadAxis::RightX, 0.25f)).axis(Action::LookX) == Approx(0.75f));
    CHECK(look.resolve(padAxis(GamepadAxis::RightX, -1.0f)).axis(Action::LookX) == Approx(-1.5f));
}

TEST_CASE("input/input_system: mouse deltas are unclamped and scaled by the mouse sensitivity",
          "[input][input_system]") {
    InputSystem input;
    RawInputFrame frame;
    frame.mouseDelta = {25.0f, -40.0f}; // right and up on screen

    ActionState state = input.resolve(frame);
    CHECK(state.axis(Action::LookDeltaX) == 25.0f);
    CHECK(state.axis(Action::LookDeltaY) == 40.0f); // moving the mouse up looks up
    CHECK_FALSE(state.held(Action::LookDeltaX));
    CHECK_FALSE(state.pressed(Action::LookDeltaY));
    CHECK(state.axis(Action::LookX) == 0.0f);

    input.setSettings(InputSettings{.mouseSensitivityScale = 0.5f});
    state = input.resolve(frame);
    CHECK(state.axis(Action::LookDeltaX) == 12.5f);
    CHECK(state.axis(Action::LookDeltaY) == 20.0f);

    frame.mouseDelta = {kNaN, kInfinity};
    state = input.resolve(frame);
    CHECK(state.axis(Action::LookDeltaX) == 0.0f);
    CHECK(state.axis(Action::LookDeltaY) == 0.0f);
}

TEST_CASE("input/input_system: invertLookY negates only the vertical look actions", "[input][input_system]") {
    InputSystem input;
    RawInputFrame frame = padAxis(GamepadAxis::RightY, 1.0f);
    frame.gamepadAxes[toIndex(GamepadAxis::RightX)] = 1.0f;
    frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] = 1.0f;
    frame.mouseDelta = {10.0f, -10.0f};

    const ActionState normal = input.resolve(frame);
    CHECK(normal.axis(Action::LookY) == 1.0f);
    CHECK(normal.axis(Action::LookDeltaY) == 10.0f);

    input.setSettings(InputSettings{.invertLookY = true});
    CHECK(input.settings().invertLookY);
    const ActionState inverted = input.resolve(frame);
    CHECK(inverted.axis(Action::LookY) == -1.0f);
    CHECK(inverted.axis(Action::LookDeltaY) == -10.0f);
    CHECK(inverted.axis(Action::LookX) == 1.0f);
    CHECK(inverted.axis(Action::LookDeltaX) == 10.0f);
    CHECK(inverted.axis(Action::MoveForward) == 1.0f);
}

TEST_CASE("input/input_system: pressed and released are edges between frames", "[input][input_system]") {
    InputSystem input;

    const ActionState first = input.resolve(keys({Key::E}));
    CHECK(first.held(Action::Interact));
    CHECK(first.pressed(Action::Interact));
    CHECK_FALSE(first.released(Action::Interact));
    CHECK(first.axis(Action::Interact) == 1.0f);

    const ActionState second = input.resolve(keys({Key::E}));
    CHECK(second.held(Action::Interact));
    CHECK_FALSE(second.pressed(Action::Interact));
    CHECK_FALSE(second.released(Action::Interact));

    const ActionState third = input.resolve(keys({}));
    CHECK_FALSE(third.held(Action::Interact));
    CHECK_FALSE(third.pressed(Action::Interact));
    CHECK(third.released(Action::Interact));
    CHECK(third.axis(Action::Interact) == 0.0f);

    const ActionState fourth = input.resolve(keys({}));
    CHECK_FALSE(fourth.released(Action::Interact));

    // Handing over from one bound input to another keeps the action held without new edges.
    CHECK(input.resolve(keys({Key::E})).pressed(Action::Interact));
    RawInputFrame click;
    click.mouseButtonsDown.set(toIndex(MouseButton::Left));
    const ActionState handover = input.resolve(click);
    CHECK(handover.held(Action::Interact));
    CHECK_FALSE(handover.pressed(Action::Interact));
    CHECK_FALSE(handover.released(Action::Interact));
    CHECK(input.resolve(RawInputFrame{}).released(Action::Interact));
}

TEST_CASE("input/input_system: a tap within one frame reports pressed and released", "[input][input_system]") {
    InputSystem input;

    RawInputFrame tap;
    tap.keysPressed.set(toIndex(Key::E));
    tap.keysReleased.set(toIndex(Key::E));
    const ActionState state = input.resolve(tap);
    CHECK(state.pressed(Action::Interact));
    CHECK(state.released(Action::Interact));
    CHECK_FALSE(state.held(Action::Interact));
    CHECK(state.axis(Action::Interact) == 0.0f);

    const ActionState next = input.resolve(RawInputFrame{});
    CHECK_FALSE(next.pressed(Action::Interact));
    CHECK_FALSE(next.released(Action::Interact));

    RawInputFrame click;
    click.mouseButtonsPressed.set(toIndex(MouseButton::Left));
    CHECK(input.resolve(click).pressed(Action::Interact));

    RawInputFrame padTap;
    padTap.gamepadConnected = true;
    padTap.gamepadButtonsPressed.set(toIndex(GamepadButton::East));
    const ActionState back = input.resolve(padTap);
    CHECK(back.pressed(Action::Back));
    CHECK(back.released(Action::Back));

    // A press of a key bound to an action that stays held is not a new edge.
    CHECK(input.resolve(keys({Key::E})).pressed(Action::Interact));
    RawInputFrame repeat = keys({Key::E});
    repeat.keysPressed.set(toIndex(Key::E));
    const ActionState held = input.resolve(repeat);
    CHECK(held.held(Action::Interact));
    CHECK_FALSE(held.pressed(Action::Interact));
    CHECK_FALSE(held.released(Action::Interact));
}

TEST_CASE("input/input_system: analog actions are held at the press threshold", "[input][input_system]") {
    InputSystem input; // threshold 0.5, left_y dead zone 0.15

    const ActionState below = input.resolve(padAxis(GamepadAxis::LeftY, 0.55f)); // 0.47 after the dead zone
    CHECK(below.axis(Action::MoveForward) == Approx((0.55f - 0.15f) / 0.85f));
    CHECK_FALSE(below.held(Action::MoveForward));
    CHECK_FALSE(below.pressed(Action::MoveForward));

    const ActionState above = input.resolve(padAxis(GamepadAxis::LeftY, 0.6f)); // 0.53
    CHECK(above.held(Action::MoveForward));
    CHECK(above.pressed(Action::MoveForward));
    CHECK(above.axis(Action::MoveForward) == Approx((0.6f - 0.15f) / 0.85f)); // analog value, not 1

    const ActionState backwards = input.resolve(padAxis(GamepadAxis::LeftY, -0.9f));
    CHECK(backwards.held(Action::MoveForward));
    CHECK_FALSE(backwards.pressed(Action::MoveForward));
    CHECK(input.resolve(padAxis(GamepadAxis::LeftY, 0.0f)).released(Action::MoveForward));

    input.setSettings(InputSettings{.pressThreshold = 0.25f});
    CHECK(input.settings().pressThreshold == 0.25f);
    CHECK(input.resolve(padAxis(GamepadAxis::LeftY, 0.4f)).held(Action::MoveForward)); // 0.29

    // Look actions are never held from analog motion.
    const ActionState look = input.resolve(padAxis(GamepadAxis::RightX, 1.0f));
    CHECK(look.axis(Action::LookX) == 1.0f);
    CHECK_FALSE(look.held(Action::LookX));
    CHECK_FALSE(look.pressed(Action::LookX));
    RawInputFrame mouse;
    mouse.mouseDelta = {300.0f, -300.0f};
    const ActionState mouseLook = input.resolve(mouse);
    CHECK_FALSE(mouseLook.held(Action::LookDeltaX));
    CHECK_FALSE(mouseLook.held(Action::LookDeltaY));

    // ...but a button bound to a look action is.
    InputSystem keyLook(singleBinding(Action::LookX, KeyBinding{Key::L, 1.0f}));
    const ActionState turning = keyLook.resolve(keys({Key::L}));
    CHECK(turning.held(Action::LookX));
    CHECK(turning.pressed(Action::LookX));
    CHECK(turning.axis(Action::LookX) == 1.0f);

    // A trigger drives a digital action and keeps its analog value.
    InputSystem trigger(
        singleBinding(Action::Sprint, GamepadAxisBinding{GamepadAxis::RightTrigger, 1.0f, 0.1f, 1.0f}));
    const ActionState sprint = trigger.resolve(padAxis(GamepadAxis::RightTrigger, 0.82f));
    CHECK(sprint.held(Action::Sprint));
    CHECK(sprint.pressed(Action::Sprint));
    CHECK(sprint.axis(Action::Sprint) == Approx(0.8f));
}

TEST_CASE("input/input_system: reset clears the edge memory", "[input][input_system]") {
    InputSystem input;
    CHECK(input.resolve(keys({Key::E})).pressed(Action::Interact));
    CHECK_FALSE(input.resolve(keys({Key::E})).pressed(Action::Interact));

    input.reset();
    const ActionState again = input.resolve(keys({Key::E}));
    CHECK(again.held(Action::Interact));
    CHECK(again.pressed(Action::Interact));

    input.reset();
    const ActionState afterFocusLoss = input.resolve(RawInputFrame{});
    CHECK_FALSE(afterFocusLoss.held(Action::Interact));
    CHECK_FALSE(afterFocusLoss.released(Action::Interact));
}

TEST_CASE("input/input_system: gamepad state is ignored while disconnected", "[input][input_system]") {
    InputSystem input;
    RawInputFrame frame;
    frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] = 1.0f;
    frame.gamepadButtonsDown.set(toIndex(GamepadButton::South));
    frame.gamepadButtonsPressed.set(toIndex(GamepadButton::South));

    const ActionState disconnected = input.resolve(frame);
    CHECK(disconnected.axis(Action::MoveForward) == 0.0f);
    CHECK_FALSE(disconnected.held(Action::Interact));
    CHECK_FALSE(disconnected.pressed(Action::Interact));

    frame.gamepadConnected = true;
    const ActionState connected = input.resolve(frame);
    CHECK(connected.axis(Action::MoveForward) == 1.0f);
    CHECK(connected.held(Action::Interact));
    CHECK(connected.pressed(Action::Interact));
}

TEST_CASE("input/input_system: default buttons trigger their actions", "[input][input_system]") {
    struct Case {
        RawInputFrame frame;
        std::vector<Action> pressed;
    };
    const auto pad = [](GamepadButton button) {
        RawInputFrame frame;
        frame.gamepadConnected = true;
        frame.lastActiveDevice = DeviceKind::Gamepad;
        frame.gamepadButtonsDown.set(toIndex(button));
        return frame;
    };
    RawInputFrame leftClick;
    leftClick.mouseButtonsDown.set(toIndex(MouseButton::Left));

    const std::vector<Case> cases = {
        {keys({Key::LeftShift}), {Action::Sprint}},
        {keys({Key::E}), {Action::Interact}},
        {leftClick, {Action::Interact}},
        {keys({Key::Escape}), {Action::Back}},
        {keys({Key::Tab}), {Action::QuickCatalog}},
        {keys({Key::Enter}), {Action::Confirm}},
        {keys({Key::M}), {Action::Map}},
        {keys({Key::F1}), {Action::ToggleDevUI}},
        {keys({Key::F5}), {Action::ReloadShaders}},
        {keys({Key::F12}), {Action::Screenshot}},
        {pad(GamepadButton::LeftStick), {Action::Sprint}},
        {pad(GamepadButton::South), {Action::Interact}},
        {pad(GamepadButton::East), {Action::Back}},
        {pad(GamepadButton::North), {Action::QuickCatalog}},
        {pad(GamepadButton::Start), {Action::Confirm, Action::Menu}},
        {pad(GamepadButton::Back), {Action::Map}},
        {pad(GamepadButton::Guide), {}},
        {keys({Key::Space}), {}},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        INFO("case " << i);
        InputSystem input;
        const ActionState state = input.resolve(cases[i].frame);
        CHECK(pressedActions(state) == cases[i].pressed);
        CHECK(state.activeDevice() == cases[i].frame.lastActiveDevice);
    }
}

TEST_CASE("input/input_system: settings are sanitized", "[input][input_system]") {
    InputSystem input;
    CHECK(input.settings().pressThreshold == 0.5f);

    input.setSettings(InputSettings{.pressThreshold = 0.0f});
    CHECK(input.settings().pressThreshold == rv::input::kMinPressThreshold);
    // A stick at rest must never hold an action, whatever the threshold.
    CHECK_FALSE(input.resolve(padAxis(GamepadAxis::LeftY, 0.0f)).held(Action::MoveForward));

    input.setSettings(InputSettings{.pressThreshold = 3.0f});
    CHECK(input.settings().pressThreshold == 1.0f);
    CHECK(input.resolve(padAxis(GamepadAxis::LeftY, 1.0f)).held(Action::MoveForward));

    input.setSettings(InputSettings{.pressThreshold = kNaN,
                                    .invertLookY = true,
                                    .mouseSensitivityScale = kInfinity,
                                    .stickSensitivityScale = kNaN});
    CHECK(input.settings().pressThreshold == 0.5f);
    CHECK(input.settings().invertLookY);
    CHECK(input.settings().mouseSensitivityScale == 1.0f);
    CHECK(input.settings().stickSensitivityScale == 1.0f);
}

TEST_CASE("input/input_system: setMap replaces the bindings and keeps the edge memory", "[input][input_system]") {
    InputSystem input;
    CHECK(input.resolve(keys({Key::E})).pressed(Action::Interact));

    const ActionMap remapped = singleBinding(Action::Interact, KeyBinding{Key::F, 1.0f});
    input.setMap(remapped);
    CHECK(input.map() == remapped);

    const ActionState unbound = input.resolve(keys({Key::E}));
    CHECK_FALSE(unbound.held(Action::Interact));
    CHECK(unbound.released(Action::Interact));
    CHECK(unbound.axis(Action::MoveForward) == 0.0f);

    CHECK(input.resolve(keys({Key::F})).pressed(Action::Interact));
    CHECK_FALSE(input.resolve(keys({Key::W})).held(Action::MoveForward));
}
