#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_scancode.h>

#include <rv/input/raw_input.hpp>
#include <rv/platform/gamepad_manager.hpp>

union SDL_Event;
struct SDL_KeyboardEvent;
struct SDL_MouseButtonEvent;
struct SDL_GamepadButtonEvent;
struct SDL_GamepadAxisEvent;

namespace rv::platform {

/// Turns SDL3 events into one rv::input::RawInputFrame per frame (main thread): feed every event to handleEvent(),
/// then call endFrame() once per frame. Value conventions are those of rv/input/codes.hpp.
///
///  - Keyboard: translated by scancode (keyFromScancode), key repeats ignored. A key is down while any scancode that
///    maps to it is held (the main and keypad Enter keys).
///  - Mouse: buttons; relative motion (xrel, yrel) accumulated only while the mouse is captured (setMouseCaptured,
///    see MouseCapture), so a free cursor never turns the camera; wheel notches with SDL_MOUSEWHEEL_FLIPPED undone.
///  - Gamepads: hot-plug through the owned GamepadManager; buttons and axes of every open pad are tracked and the frame
///    reports the active (most recently used) pad, with axes converted by gamepadAxisValue().
///  - Window focus loss releases everything held (keys, buttons) and centers the gamepad axes, so nothing stays stuck
///    while another application has focus; the next frame reports the releases.
///  - lastActiveDevice: KeyboardMouse after a key or mouse button press, captured mouse motion or a wheel notch;
///    Gamepad after a gamepad button press or an axis pushed past GamepadManager::kActivityThreshold.
/// Pressed and released bits report every edge during the frame (a tap within one frame sets both), as
/// RawInputFrame requires.
class SdlInputCollector {
public:
    SdlInputCollector() = default;
    SdlInputCollector(const SdlInputCollector&) = delete;
    SdlInputCollector& operator=(const SdlInputCollector&) = delete;

    /// Feeds one SDL event; events of other kinds are ignored.
    void handleEvent(const SDL_Event& event);
    /// Returns the input of the frame and starts the next one: edges, mouse motion and wheel are cleared, held keys and
    /// buttons and axis positions persist.
    [[nodiscard]] input::RawInputFrame endFrame();
    /// Releases every key and button and centers every gamepad axis, reporting the releases in the current frame.
    void releaseAll() noexcept;

    /// Mouse motion is accumulated only while captured.
    void setMouseCaptured(bool captured) noexcept { m_mouseCaptured = captured; }
    [[nodiscard]] bool mouseCaptured() const noexcept { return m_mouseCaptured; }

    [[nodiscard]] GamepadManager& gamepads() noexcept { return m_gamepads; }
    [[nodiscard]] const GamepadManager& gamepads() const noexcept { return m_gamepads; }

private:
    struct PadState {
        SDL_JoystickID id = 0;
        std::bitset<input::kGamepadButtonCount> down;
        std::bitset<input::kGamepadButtonCount> pressed;
        std::bitset<input::kGamepadButtonCount> released;
        std::array<float, input::kGamepadAxisCount> axes{};
    };

    void handleKey(const SDL_KeyboardEvent& event);
    void handleMouseButton(const SDL_MouseButtonEvent& event);
    void handleGamepadButton(const SDL_GamepadButtonEvent& event);
    void handleGamepadAxis(const SDL_GamepadAxisEvent& event);
    [[nodiscard]] PadState* padState(SDL_JoystickID id);

    /// Keyboard and mouse part of the frame being collected (its gamepad fields are filled by endFrame()).
    input::RawInputFrame m_frame;
    std::bitset<SDL_SCANCODE_COUNT> m_scancodesDown;
    /// Held scancodes per key.
    std::array<std::uint8_t, input::kKeyCount> m_keyHolders{};
    std::vector<PadState> m_pads;
    GamepadManager m_gamepads;
    input::DeviceKind m_lastDevice = input::DeviceKind::KeyboardMouse;
    bool m_mouseCaptured = false;
};

} // namespace rv::platform
