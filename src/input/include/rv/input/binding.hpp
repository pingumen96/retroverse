#pragma once

#include <variant>

#include <rv/input/codes.hpp>

namespace rv::input {

/// Default dead zone of gamepad axis bindings, as a fraction of the axis range.
inline constexpr float kDefaultDeadzone = 0.15f;
/// Default response exponent of gamepad axis bindings (1 = linear).
inline constexpr float kDefaultExponent = 1.0f;

// One device input feeding an action. InputSystem sums the contributions of every binding of an action; a
// negative scale reverses the direction (S bound to MoveForward with scale -1 walks backwards).

/// Keyboard key: contributes `scale` while held.
struct KeyBinding {
    Key key = Key::A;
    float scale = 1.0f;

    bool operator==(const KeyBinding&) const = default;
};

/// Mouse button: contributes `scale` while held.
struct MouseButtonBinding {
    MouseButton button = MouseButton::Left;
    float scale = 1.0f;

    bool operator==(const MouseButtonBinding&) const = default;
};

/// Mouse motion or wheel: contributes `value * scale` (pixels or notches this frame).
struct MouseAxisBinding {
    MouseAxis axis = MouseAxis::DeltaX;
    float scale = 1.0f;

    bool operator==(const MouseAxisBinding&) const = default;
};

/// Gamepad button: contributes `scale` while held.
struct GamepadButtonBinding {
    GamepadButton button = GamepadButton::South;
    float scale = 1.0f;

    bool operator==(const GamepadButtonBinding&) const = default;
};

/// Gamepad stick axis or trigger: contributes `shape(value) * scale`, where shape() first removes the dead zone
/// and rescales the rest of the range, sign(v) * clamp((|v| - deadzone) / (1 - deadzone), 0, 1), then applies the
/// response curve |x|^exponent keeping the sign. Values beyond [-1, 1] saturate.
struct GamepadAxisBinding {
    GamepadAxis axis = GamepadAxis::LeftX;
    float scale = 1.0f;
    float deadzone = kDefaultDeadzone; ///< in [0, 1)
    float exponent = kDefaultExponent; ///< > 0: 1 is linear, 2 gives finer control near the center

    bool operator==(const GamepadAxisBinding&) const = default;
};

using Binding =
    std::variant<KeyBinding, MouseButtonBinding, MouseAxisBinding, GamepadButtonBinding, GamepadAxisBinding>;

/// True when the code is a real code (not Count or beyond), the scale is finite and, for gamepad axes, the dead
/// zone is in [0, 1) and the exponent is finite and > 0. ActionMap stores valid bindings only.
[[nodiscard]] bool isValid(const Binding& binding) noexcept;

} // namespace rv::input
