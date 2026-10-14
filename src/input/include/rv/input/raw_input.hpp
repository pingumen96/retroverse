#pragma once

#include <array>
#include <bitset>

#include <glm/vec2.hpp>

#include <rv/input/action.hpp>
#include <rv/input/codes.hpp>

namespace rv::input {

/// Raw device state for one frame. Pure data: filled by rv_platform (SDL collector) once per frame, consumed by
/// InputSystem::resolve(), built by hand in tests. Value conventions are documented in codes.hpp.
///
/// Bitsets are indexed with toIndex(code). For keys, mouse buttons and gamepad buttons:
///  - `*Down`: held at the end of the frame;
///  - `*Pressed`: went down at least once during the frame (it may have been released again before its end);
///  - `*Released`: went up at least once during the frame.
struct RawInputFrame {
    std::bitset<kKeyCount> keysDown;
    std::bitset<kKeyCount> keysPressed;
    std::bitset<kKeyCount> keysReleased;

    std::bitset<kMouseButtonCount> mouseButtonsDown;
    std::bitset<kMouseButtonCount> mouseButtonsPressed;
    std::bitset<kMouseButtonCount> mouseButtonsReleased;
    /// Relative motion accumulated during the frame, in pixels: +X right, +Y down.
    glm::vec2 mouseDelta{0.0f};
    /// Wheel notches accumulated during the frame: + away from the user.
    float mouseWheel = 0.0f;

    /// False while no gamepad is open; InputSystem then ignores every gamepad field below. With several pads
    /// connected the collector reports the most recently used one.
    bool gamepadConnected = false;
    std::bitset<kGamepadButtonCount> gamepadButtonsDown;
    std::bitset<kGamepadButtonCount> gamepadButtonsPressed;
    std::bitset<kGamepadButtonCount> gamepadButtonsReleased;
    /// Indexed with toIndex(GamepadAxis): sticks in [-1, 1] with +Y up, triggers in [0, 1].
    std::array<float, kGamepadAxisCount> gamepadAxes{};

    /// Family of the device that produced the most recent input (drives on-screen button prompts).
    DeviceKind lastActiveDevice = DeviceKind::KeyboardMouse;
};

} // namespace rv::input
