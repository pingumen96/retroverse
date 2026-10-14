#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rv::input {

/// Device-neutral input codes. rv_platform translates SDL events into these codes; binding files refer to them
/// by stable lowercase names (keyName(), keyFromName(), ...). Parsing is exact and case-sensitive.
///
/// Value conventions of the matching raw values (see RawInputFrame):
///  - Gamepad sticks: [-1, 1], +X right, +Y UP (stick pushed away from the player). SDL reports +Y down; the SDL
///    collector in rv_platform negates the Y axes.
///  - Gamepad triggers: [0, 1], 0 at rest.
///  - Mouse delta: pixels moved this frame, +X right, +Y DOWN (screen convention, as SDL reports it).
///  - Mouse wheel: notches this frame, +1 per notch scrolled away from the user (SDL convention).

/// Physical key positions named after the US QWERTY layout (SDL scancode semantics): Key::W is the key in the W
/// position whatever the active layout prints on it, so WASD keeps its shape on AZERTY or QWERTZ keyboards.
/// Names: "a".."z", "0".."9" (top row digits), "f1".."f12", "escape", "enter", "tab", "space", "backspace", "up",
/// "down", "left", "right", "left_shift", "right_shift", "left_ctrl", "right_ctrl", "left_alt", "right_alt",
/// "page_up", "page_down", "home", "end", "insert", "delete", "grave" (backquote, left of "1"), "minus", "equals".
enum class Key : std::uint8_t {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Enter, Tab, Space, Backspace,
    Up, Down, Left, Right,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    PageUp, PageDown, Home, End, Insert, Delete,
    Grave, Minus, Equals,
    Count
};

/// Names: "left", "right", "middle", "x1", "x2" (x1/x2: side buttons, usually back/forward).
enum class MouseButton : std::uint8_t { Left, Right, Middle, X1, X2, Count };

/// Names: "delta_x", "delta_y" (pixels, +Y down), "wheel" (notches, + away from the user).
enum class MouseAxis : std::uint8_t { DeltaX, DeltaY, Wheel, Count };

/// Positional gamepad buttons (same order as SDL_GamepadButton up to DPAD_RIGHT).
/// Names: "south" (A / Cross), "east" (B / Circle), "west" (X / Square), "north" (Y / Triangle),
/// "back" (View / Select), "guide", "start" (Menu / Start), "left_stick", "right_stick", "left_shoulder",
/// "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right".
enum class GamepadButton : std::uint8_t {
    South, East, West, North,
    Back, Guide, Start,
    LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};

/// Names: "left_x", "left_y", "right_x", "right_y" (sticks, [-1, 1], +Y up), "left_trigger", "right_trigger"
/// ([0, 1]). Same order as SDL_GamepadAxis.
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
inline constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(MouseButton::Count);
inline constexpr std::size_t kMouseAxisCount = static_cast<std::size_t>(MouseAxis::Count);
inline constexpr std::size_t kGamepadButtonCount = static_cast<std::size_t>(GamepadButton::Count);
inline constexpr std::size_t kGamepadAxisCount = static_cast<std::size_t>(GamepadAxis::Count);

/// Dense index of a code, for bitsets and arrays sized by the k*Count constants above.
[[nodiscard]] constexpr std::size_t toIndex(Key code) noexcept { return static_cast<std::size_t>(code); }
[[nodiscard]] constexpr std::size_t toIndex(MouseButton code) noexcept { return static_cast<std::size_t>(code); }
[[nodiscard]] constexpr std::size_t toIndex(MouseAxis code) noexcept { return static_cast<std::size_t>(code); }
[[nodiscard]] constexpr std::size_t toIndex(GamepadButton code) noexcept { return static_cast<std::size_t>(code); }
[[nodiscard]] constexpr std::size_t toIndex(GamepadAxis code) noexcept { return static_cast<std::size_t>(code); }

/// Stable lowercase names used in binding files. Out-of-range values (including Count) format as "unknown",
/// which no parser accepts.
[[nodiscard]] std::string_view keyName(Key key) noexcept;
[[nodiscard]] std::optional<Key> keyFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view mouseButtonName(MouseButton button) noexcept;
[[nodiscard]] std::optional<MouseButton> mouseButtonFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view mouseAxisName(MouseAxis axis) noexcept;
[[nodiscard]] std::optional<MouseAxis> mouseAxisFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepadButtonName(GamepadButton button) noexcept;
[[nodiscard]] std::optional<GamepadButton> gamepadButtonFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepadAxisName(GamepadAxis axis) noexcept;
[[nodiscard]] std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name) noexcept;

} // namespace rv::input
