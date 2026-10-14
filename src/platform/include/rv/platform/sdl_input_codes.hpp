#pragma once

#include <optional>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <rv/input/codes.hpp>

namespace rv::platform {

// SDL3 -> rv::input translation tables (rv_input stays SDL-free). Pure functions: no SDL call, no initialization.

/// Keys are translated by SCANCODE, the physical key position (named after US QWERTY), never by key code, so WASD
/// keeps its shape on AZERTY or QWERTZ layouts. Both Enter keys (main and keypad) give Key::Enter. Scancodes without an
/// rv::input::Key give nothing.
[[nodiscard]] std::optional<input::Key> keyFromScancode(SDL_Scancode scancode) noexcept;

/// SDL_BUTTON_LEFT, _MIDDLE, _RIGHT, _X1 and _X2; other indices give nothing.
[[nodiscard]] std::optional<input::MouseButton> mouseButtonFromSdl(Uint8 button) noexcept;

/// The positional buttons from SDL_GAMEPAD_BUTTON_SOUTH to SDL_GAMEPAD_BUTTON_DPAD_RIGHT; paddles, touchpad and misc
/// buttons give nothing.
[[nodiscard]] std::optional<input::GamepadButton> gamepadButtonFromSdl(SDL_GamepadButton button) noexcept;

/// Sticks and triggers; SDL_GAMEPAD_AXIS_INVALID and out-of-range values give nothing.
[[nodiscard]] std::optional<input::GamepadAxis> gamepadAxisFromSdl(SDL_GamepadAxis axis) noexcept;

/// SDL stick value (SDL_JOYSTICK_AXIS_MIN..MAX, -32768..32767) to [-1, 1] keeping SDL's direction (+X right, +Y DOWN):
/// both extremes map exactly to -1 and +1 and 0 maps to 0.
[[nodiscard]] float stickValueFromSdl(Sint16 value) noexcept;

/// SDL trigger value (0..32767) to [0, 1]; negative values, which SDL never reports for triggers, count as 0.
[[nodiscard]] float triggerValueFromSdl(Sint16 value) noexcept;

/// SDL gamepad axis value in the convention of rv::input::RawInputFrame::gamepadAxes: sticks in [-1, 1] with +Y UP
/// (SDL's Y axes point down and are negated), triggers in [0, 1].
[[nodiscard]] float gamepadAxisValue(input::GamepadAxis axis, Sint16 value) noexcept;

} // namespace rv::platform
