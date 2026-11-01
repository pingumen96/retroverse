#include <rv/platform/sdl_input_collector.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>

#include <rv/platform/sdl_input_codes.hpp>

namespace rv::platform {
namespace {

/// Records a button going down or up in down/pressed/released bitsets; repeated downs and ups are ignored.
template <std::size_t N>
void setButton(std::bitset<N>& down, std::bitset<N>& pressed, std::bitset<N>& released, std::size_t index,
               bool isDown) {
    if (index >= N || down[index] == isDown) {
        return;
    }
    down[index] = isDown;
    (isDown ? pressed : released).set(index);
}

/// Releases every held button, reporting each release.
template <std::size_t N>
void releaseButtons(std::bitset<N>& down, std::bitset<N>& released) {
    released |= down;
    down.reset();
}

} // namespace

void SdlInputCollector::handleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        handleKey(event.key);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        handleMouseButton(event.button);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (m_mouseCaptured) {
            m_frame.mouseDelta += glm::vec2(event.motion.xrel, event.motion.yrel);
            m_lastDevice = input::DeviceKind::KeyboardMouse;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        m_frame.mouseWheel += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        m_lastDevice = input::DeviceKind::KeyboardMouse;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        m_gamepads.handleEvent(event);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        m_gamepads.handleEvent(event);
        std::erase_if(m_pads, [id = event.gdevice.which](const PadState& pad) { return pad.id == id; });
        if (m_gamepads.count() == 0) {
            m_lastDevice = input::DeviceKind::KeyboardMouse; // no hint may name a pad that is gone
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        m_gamepads.handleEvent(event);
        handleGamepadButton(event.gbutton);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        m_gamepads.handleEvent(event);
        handleGamepadAxis(event.gaxis);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        releaseAll();
        break;
    default:
        break;
    }
}

input::RawInputFrame SdlInputCollector::endFrame() {
    input::RawInputFrame frame = m_frame;
    frame.lastActiveDevice = m_lastDevice;
    if (const auto active = m_gamepads.activeId()) {
        frame.gamepadConnected = true;
        if (const PadState* pad = padState(*active)) {
            frame.gamepadButtonsDown = pad->down;
            frame.gamepadButtonsPressed = pad->pressed;
            frame.gamepadButtonsReleased = pad->released;
            frame.gamepadAxes = pad->axes;
        }
    }

    m_frame.keysPressed.reset();
    m_frame.keysReleased.reset();
    m_frame.mouseButtonsPressed.reset();
    m_frame.mouseButtonsReleased.reset();
    m_frame.mouseDelta = glm::vec2(0.0f);
    m_frame.mouseWheel = 0.0f;
    for (PadState& pad : m_pads) {
        pad.pressed.reset();
        pad.released.reset();
    }
    return frame;
}

void SdlInputCollector::releaseAll() noexcept {
    releaseButtons(m_frame.keysDown, m_frame.keysReleased);
    m_scancodesDown.reset();
    m_keyHolders.fill(0);
    releaseButtons(m_frame.mouseButtonsDown, m_frame.mouseButtonsReleased);
    for (PadState& pad : m_pads) {
        releaseButtons(pad.down, pad.released);
        pad.axes.fill(0.0f);
    }
}

void SdlInputCollector::handleKey(const SDL_KeyboardEvent& event) {
    const int code = static_cast<int>(event.scancode);
    if (event.repeat || code < 0 || code >= SDL_SCANCODE_COUNT) {
        return;
    }
    const auto key = keyFromScancode(event.scancode);
    const auto scancode = static_cast<std::size_t>(code);
    if (!key || m_scancodesDown[scancode] == event.down) {
        return; // unmapped key, or an up without a down (the key was held before focus was gained)
    }
    m_scancodesDown[scancode] = event.down;
    const std::size_t index = input::toIndex(*key);
    std::uint8_t& holders = m_keyHolders[index];
    if (event.down) {
        ++holders;
        m_lastDevice = input::DeviceKind::KeyboardMouse;
    } else if (holders > 0) {
        --holders;
    }
    setButton(m_frame.keysDown, m_frame.keysPressed, m_frame.keysReleased, index, holders > 0);
}

void SdlInputCollector::handleMouseButton(const SDL_MouseButtonEvent& event) {
    const auto button = mouseButtonFromSdl(event.button);
    if (!button) {
        return;
    }
    if (event.down) {
        m_lastDevice = input::DeviceKind::KeyboardMouse;
    }
    setButton(m_frame.mouseButtonsDown, m_frame.mouseButtonsPressed, m_frame.mouseButtonsReleased,
              input::toIndex(*button), event.down);
}

void SdlInputCollector::handleGamepadButton(const SDL_GamepadButtonEvent& event) {
    PadState* pad = padState(event.which);
    const auto button = gamepadButtonFromSdl(static_cast<SDL_GamepadButton>(event.button));
    if (pad == nullptr || !button) {
        return;
    }
    if (event.down) {
        m_lastDevice = input::DeviceKind::Gamepad;
    }
    setButton(pad->down, pad->pressed, pad->released, input::toIndex(*button), event.down);
}

void SdlInputCollector::handleGamepadAxis(const SDL_GamepadAxisEvent& event) {
    PadState* pad = padState(event.which);
    const auto axis = gamepadAxisFromSdl(static_cast<SDL_GamepadAxis>(event.axis));
    if (pad == nullptr || !axis) {
        return;
    }
    const float value = gamepadAxisValue(*axis, event.value);
    pad->axes[input::toIndex(*axis)] = value;
    if (std::abs(value) >= GamepadManager::kActivityThreshold) {
        m_lastDevice = input::DeviceKind::Gamepad;
    }
}

SdlInputCollector::PadState* SdlInputCollector::padState(SDL_JoystickID id) {
    const auto it = std::find_if(m_pads.begin(), m_pads.end(), [id](const PadState& pad) { return pad.id == id; });
    if (it != m_pads.end()) {
        return &*it;
    }
    // Only pads the manager opened report input (SDL sends gamepad events for open pads only).
    if (!m_gamepads.isOpen(id)) {
        return nullptr;
    }
    PadState& pad = m_pads.emplace_back();
    pad.id = id;
    return &pad;
}

} // namespace rv::platform
