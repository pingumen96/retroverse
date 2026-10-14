#include <rv/platform/sdl_input_codes.hpp>

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_mouse.h>

namespace rv::platform {
namespace {

using input::GamepadAxis;
using input::GamepadButton;
using input::Key;
using input::MouseButton;

// Contiguous runs translated by offset; both enums must keep these runs in the same order.
static_assert(SDL_SCANCODE_Z - SDL_SCANCODE_A == 25 && input::toIndex(Key::Z) - input::toIndex(Key::A) == 25);
static_assert(SDL_SCANCODE_9 - SDL_SCANCODE_1 == 8 && input::toIndex(Key::Digit9) - input::toIndex(Key::Digit1) == 8);
static_assert(SDL_SCANCODE_F12 - SDL_SCANCODE_F1 == 11 && input::toIndex(Key::F12) - input::toIndex(Key::F1) == 11);

[[nodiscard]] Key keyAfter(Key first, int offset) noexcept {
    return static_cast<Key>(static_cast<int>(input::toIndex(first)) + offset);
}

} // namespace

std::optional<input::Key> keyFromScancode(SDL_Scancode scancode) noexcept {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return keyAfter(Key::A, scancode - SDL_SCANCODE_A);
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return keyAfter(Key::Digit1, scancode - SDL_SCANCODE_1);
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return keyAfter(Key::F1, scancode - SDL_SCANCODE_F1);
    }
    switch (scancode) {
    case SDL_SCANCODE_0:
        return Key::Digit0;
    case SDL_SCANCODE_ESCAPE:
        return Key::Escape;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return Key::Enter;
    case SDL_SCANCODE_TAB:
        return Key::Tab;
    case SDL_SCANCODE_SPACE:
        return Key::Space;
    case SDL_SCANCODE_BACKSPACE:
        return Key::Backspace;
    case SDL_SCANCODE_UP:
        return Key::Up;
    case SDL_SCANCODE_DOWN:
        return Key::Down;
    case SDL_SCANCODE_LEFT:
        return Key::Left;
    case SDL_SCANCODE_RIGHT:
        return Key::Right;
    case SDL_SCANCODE_LSHIFT:
        return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
        return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL:
        return Key::RightCtrl;
    case SDL_SCANCODE_LALT:
        return Key::LeftAlt;
    case SDL_SCANCODE_RALT:
        return Key::RightAlt;
    case SDL_SCANCODE_PAGEUP:
        return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return Key::PageDown;
    case SDL_SCANCODE_HOME:
        return Key::Home;
    case SDL_SCANCODE_END:
        return Key::End;
    case SDL_SCANCODE_INSERT:
        return Key::Insert;
    case SDL_SCANCODE_DELETE:
        return Key::Delete;
    case SDL_SCANCODE_GRAVE:
        return Key::Grave;
    case SDL_SCANCODE_MINUS:
        return Key::Minus;
    case SDL_SCANCODE_EQUALS:
        return Key::Equals;
    default:
        return std::nullopt;
    }
}

std::optional<input::MouseButton> mouseButtonFromSdl(Uint8 button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    case SDL_BUTTON_X1:
        return MouseButton::X1;
    case SDL_BUTTON_X2:
        return MouseButton::X2;
    default:
        return std::nullopt;
    }
}

std::optional<input::GamepadButton> gamepadButtonFromSdl(SDL_GamepadButton button) noexcept {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return GamepadButton::South;
    case SDL_GAMEPAD_BUTTON_EAST:
        return GamepadButton::East;
    case SDL_GAMEPAD_BUTTON_WEST:
        return GamepadButton::West;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return GamepadButton::North;
    case SDL_GAMEPAD_BUTTON_BACK:
        return GamepadButton::Back;
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return GamepadButton::Guide;
    case SDL_GAMEPAD_BUTTON_START:
        return GamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return GamepadButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return GamepadButton::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return GamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return GamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return GamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return GamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return GamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return GamepadButton::DpadRight;
    default:
        return std::nullopt;
    }
}

std::optional<input::GamepadAxis> gamepadAxisFromSdl(SDL_GamepadAxis axis) noexcept {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return GamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:
        return GamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return GamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return GamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return GamepadAxis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return GamepadAxis::RightTrigger;
    default:
        return std::nullopt;
    }
}

float stickValueFromSdl(Sint16 value) noexcept {
    // The range is asymmetric: dividing each side by its own extreme makes both full deflections exactly 1.
    return value < 0 ? static_cast<float>(value) / -static_cast<float>(SDL_JOYSTICK_AXIS_MIN)
                     : static_cast<float>(value) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
}

float triggerValueFromSdl(Sint16 value) noexcept {
    return value <= 0 ? 0.0f : static_cast<float>(value) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
}

float gamepadAxisValue(input::GamepadAxis axis, Sint16 value) noexcept {
    switch (axis) {
    case GamepadAxis::LeftX:
    case GamepadAxis::RightX:
        return stickValueFromSdl(value);
    case GamepadAxis::LeftY:
    case GamepadAxis::RightY:
        // SDL's +Y is the stick pulled towards the player; rv::input's +Y pushes it away (walks forward, looks up).
        return value == 0 ? 0.0f : -stickValueFromSdl(value);
    case GamepadAxis::LeftTrigger:
    case GamepadAxis::RightTrigger:
        return triggerValueFromSdl(value);
    case GamepadAxis::Count:
        break;
    }
    return 0.0f;
}

} // namespace rv::platform
