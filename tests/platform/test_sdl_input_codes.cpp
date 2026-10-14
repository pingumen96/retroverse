#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <rv/input/codes.hpp>
#include <rv/platform/sdl_input_codes.hpp>

using rv::input::GamepadAxis;
using rv::input::GamepadButton;
using rv::input::Key;
using rv::input::MouseButton;
using rv::platform::gamepadAxisValue;

TEST_CASE("platform/sdl_input_codes: every key has a scancode, by physical position", "[platform][codes]") {
    std::map<Key, std::set<int>> scancodesByKey;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        if (const auto key = rv::platform::keyFromScancode(static_cast<SDL_Scancode>(code))) {
            scancodesByKey[*key].insert(code);
        }
    }
    CHECK(scancodesByKey.size() == rv::input::kKeyCount);
    for (const auto& [key, codes] : scancodesByKey) {
        INFO(rv::input::keyName(key));
        // One physical key per rv::input::Key, except Enter (main and keypad).
        CHECK(codes.size() == (key == Key::Enter ? 2u : 1u));
    }

    // Spot checks across every run of the table, against the US QWERTY names of the SDL scancodes.
    const std::map<SDL_Scancode, Key> expected{
        {SDL_SCANCODE_A, Key::A},
        {SDL_SCANCODE_W, Key::W},
        {SDL_SCANCODE_Z, Key::Z},
        {SDL_SCANCODE_1, Key::Digit1},
        {SDL_SCANCODE_9, Key::Digit9},
        {SDL_SCANCODE_0, Key::Digit0},
        {SDL_SCANCODE_F1, Key::F1},
        {SDL_SCANCODE_F5, Key::F5},
        {SDL_SCANCODE_F12, Key::F12},
        {SDL_SCANCODE_ESCAPE, Key::Escape},
        {SDL_SCANCODE_RETURN, Key::Enter},
        {SDL_SCANCODE_KP_ENTER, Key::Enter},
        {SDL_SCANCODE_TAB, Key::Tab},
        {SDL_SCANCODE_SPACE, Key::Space},
        {SDL_SCANCODE_BACKSPACE, Key::Backspace},
        {SDL_SCANCODE_UP, Key::Up},
        {SDL_SCANCODE_DOWN, Key::Down},
        {SDL_SCANCODE_LEFT, Key::Left},
        {SDL_SCANCODE_RIGHT, Key::Right},
        {SDL_SCANCODE_LSHIFT, Key::LeftShift},
        {SDL_SCANCODE_RSHIFT, Key::RightShift},
        {SDL_SCANCODE_LCTRL, Key::LeftCtrl},
        {SDL_SCANCODE_RCTRL, Key::RightCtrl},
        {SDL_SCANCODE_LALT, Key::LeftAlt},
        {SDL_SCANCODE_RALT, Key::RightAlt},
        {SDL_SCANCODE_PAGEUP, Key::PageUp},
        {SDL_SCANCODE_PAGEDOWN, Key::PageDown},
        {SDL_SCANCODE_HOME, Key::Home},
        {SDL_SCANCODE_END, Key::End},
        {SDL_SCANCODE_INSERT, Key::Insert},
        {SDL_SCANCODE_DELETE, Key::Delete},
        {SDL_SCANCODE_GRAVE, Key::Grave},
        {SDL_SCANCODE_MINUS, Key::Minus},
        {SDL_SCANCODE_EQUALS, Key::Equals},
    };
    for (const auto& [scancode, key] : expected) {
        INFO(SDL_GetScancodeName(scancode));
        CHECK(rv::platform::keyFromScancode(scancode) == key);
    }
    // Keys without an rv::input code.
    for (const SDL_Scancode scancode : {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_KP_5,
                                        SDL_SCANCODE_LGUI, SDL_SCANCODE_F13, SDL_SCANCODE_SEMICOLON}) {
        INFO(static_cast<int>(scancode));
        CHECK_FALSE(rv::platform::keyFromScancode(scancode).has_value());
    }
}

TEST_CASE("platform/sdl_input_codes: mouse and gamepad codes", "[platform][codes]") {
    CHECK(rv::platform::mouseButtonFromSdl(SDL_BUTTON_LEFT) == MouseButton::Left);
    CHECK(rv::platform::mouseButtonFromSdl(SDL_BUTTON_MIDDLE) == MouseButton::Middle);
    CHECK(rv::platform::mouseButtonFromSdl(SDL_BUTTON_RIGHT) == MouseButton::Right);
    CHECK(rv::platform::mouseButtonFromSdl(SDL_BUTTON_X1) == MouseButton::X1);
    CHECK(rv::platform::mouseButtonFromSdl(SDL_BUTTON_X2) == MouseButton::X2);
    CHECK_FALSE(rv::platform::mouseButtonFromSdl(0).has_value());
    CHECK_FALSE(rv::platform::mouseButtonFromSdl(6).has_value());

    // The positional buttons translate one to one, by name; the extra SDL buttons have no rv::input code.
    std::set<GamepadButton> buttons;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        const auto sdlButton = static_cast<SDL_GamepadButton>(button);
        const auto translated = rv::platform::gamepadButtonFromSdl(sdlButton);
        INFO(SDL_GetGamepadStringForButton(sdlButton));
        CHECK(translated.has_value() == (button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
        if (translated) {
            buttons.insert(*translated);
        }
    }
    CHECK(buttons.size() == rv::input::kGamepadButtonCount);
    CHECK(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_SOUTH) == GamepadButton::South);
    CHECK(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_EAST) == GamepadButton::East);
    CHECK(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_START) == GamepadButton::Start);
    CHECK(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_LEFT_STICK) == GamepadButton::LeftStick);
    CHECK(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) == GamepadButton::DpadRight);
    CHECK_FALSE(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_INVALID).has_value());
    CHECK_FALSE(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_MISC1).has_value());
    CHECK_FALSE(rv::platform::gamepadButtonFromSdl(SDL_GAMEPAD_BUTTON_TOUCHPAD).has_value());

    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_LEFTX) == GamepadAxis::LeftX);
    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_LEFTY) == GamepadAxis::LeftY);
    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_RIGHTX) == GamepadAxis::RightX);
    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_RIGHTY) == GamepadAxis::RightY);
    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == GamepadAxis::LeftTrigger);
    CHECK(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == GamepadAxis::RightTrigger);
    CHECK_FALSE(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_INVALID).has_value());
    CHECK_FALSE(rv::platform::gamepadAxisFromSdl(SDL_GAMEPAD_AXIS_COUNT).has_value());
}

TEST_CASE("platform/sdl_input_codes: axis values map SDL ranges to [-1, 1] with +Y up and triggers to [0, 1]",
          "[platform][codes]") {
    // Sticks: both extremes exactly +-1, the center exactly 0, SDL's direction kept on X.
    CHECK(rv::platform::stickValueFromSdl(SDL_JOYSTICK_AXIS_MIN) == -1.0f);
    CHECK(rv::platform::stickValueFromSdl(SDL_JOYSTICK_AXIS_MAX) == 1.0f);
    CHECK(rv::platform::stickValueFromSdl(0) == 0.0f);
    CHECK(rv::platform::stickValueFromSdl(16384) == 16384.0f / 32767.0f);
    CHECK(rv::platform::stickValueFromSdl(-16384) == -0.5f);
    CHECK(gamepadAxisValue(GamepadAxis::LeftX, SDL_JOYSTICK_AXIS_MAX) == 1.0f);
    CHECK(gamepadAxisValue(GamepadAxis::RightX, SDL_JOYSTICK_AXIS_MIN) == -1.0f);

    // Y: SDL reports +Y towards the player (down); rv::input reports +Y away from the player (up).
    CHECK(gamepadAxisValue(GamepadAxis::LeftY, SDL_JOYSTICK_AXIS_MIN) == 1.0f);  // pushed forward: walk forward
    CHECK(gamepadAxisValue(GamepadAxis::LeftY, SDL_JOYSTICK_AXIS_MAX) == -1.0f); // pulled back
    CHECK(gamepadAxisValue(GamepadAxis::RightY, -16384) == 0.5f);
    CHECK(gamepadAxisValue(GamepadAxis::RightY, 0) == 0.0f);

    // Triggers: rest at 0, fully pressed at 1, nothing negative.
    CHECK(rv::platform::triggerValueFromSdl(0) == 0.0f);
    CHECK(rv::platform::triggerValueFromSdl(SDL_JOYSTICK_AXIS_MAX) == 1.0f);
    CHECK(rv::platform::triggerValueFromSdl(-100) == 0.0f);
    CHECK(gamepadAxisValue(GamepadAxis::LeftTrigger, SDL_JOYSTICK_AXIS_MAX) == 1.0f);
    CHECK(gamepadAxisValue(GamepadAxis::RightTrigger, SDL_JOYSTICK_AXIS_MIN) == 0.0f);
    CHECK(gamepadAxisValue(GamepadAxis::RightTrigger, 8192) == 8192.0f / 32767.0f);
    CHECK(gamepadAxisValue(GamepadAxis::Count, SDL_JOYSTICK_AXIS_MAX) == 0.0f);

    // Every SDL value stays in range and the conversion is monotonic.
    bool inRange = true;
    bool monotonic = true;
    float previous = -2.0f;
    for (int raw = SDL_JOYSTICK_AXIS_MIN; raw <= SDL_JOYSTICK_AXIS_MAX; ++raw) {
        const auto value = static_cast<Sint16>(raw);
        const float stick = rv::platform::stickValueFromSdl(value);
        const float trigger = rv::platform::triggerValueFromSdl(value);
        inRange = inRange && stick >= -1.0f && stick <= 1.0f && trigger >= 0.0f && trigger <= 1.0f;
        monotonic = monotonic && stick > previous;
        previous = stick;
    }
    CHECK(inRange);
    CHECK(monotonic);
}
