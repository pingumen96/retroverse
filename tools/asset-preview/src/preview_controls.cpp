#include <rv/preview/preview_controls.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>

namespace rv::preview {
namespace {

/// A stick with a radial deadzone, rescaled so that the output starts at 0 at its edge and reaches 1 at full tilt.
glm::vec2 stick(const input::RawInputFrame& frame, input::GamepadAxis x, input::GamepadAxis y) noexcept {
    const glm::vec2 value(frame.gamepadAxes[input::toIndex(x)], frame.gamepadAxes[input::toIndex(y)]);
    const float length = glm::length(value);
    if (!std::isfinite(length) || length <= kStickDeadzone) {
        return glm::vec2(0.0f);
    }
    const float scaled = std::min((length - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f);
    return value * (scaled / length);
}

float axis(const input::RawInputFrame& frame, input::GamepadAxis which) noexcept {
    const float value = frame.gamepadAxes[input::toIndex(which)];
    return std::isfinite(value) ? value : 0.0f;
}

bool held(const input::RawInputFrame& frame, input::MouseButton button) noexcept {
    return frame.mouseButtonsDown[input::toIndex(button)];
}

bool pressed(const input::RawInputFrame& frame, input::Key key) noexcept {
    return frame.keysPressed[input::toIndex(key)];
}

} // namespace

scene::OrbitInput orbitInputFrom(const input::RawInputFrame& frame) noexcept {
    scene::OrbitInput orbit;
    if (held(frame, input::MouseButton::Left)) {
        orbit.orbitPixels = frame.mouseDelta;
    } else if (held(frame, input::MouseButton::Middle) || held(frame, input::MouseButton::Right)) {
        orbit.panPixels = frame.mouseDelta;
    }
    orbit.zoomNotches = frame.mouseWheel;
    if (frame.gamepadConnected) {
        orbit.orbitRate = stick(frame, input::GamepadAxis::RightX, input::GamepadAxis::RightY);
        orbit.panRate = stick(frame, input::GamepadAxis::LeftX, input::GamepadAxis::LeftY);
        orbit.zoomRate = axis(frame, input::GamepadAxis::RightTrigger) - axis(frame, input::GamepadAxis::LeftTrigger);
    }
    return orbit;
}

bool movesCamera(const scene::OrbitInput& input) noexcept {
    const glm::vec2 zero(0.0f);
    return input.orbitPixels != zero || input.panPixels != zero || input.orbitRate != zero || input.panRate != zero ||
           input.zoomNotches != 0.0f || input.zoomRate != 0.0f;
}

PreviewShortcuts previewShortcutsFrom(const input::RawInputFrame& frame) noexcept {
    PreviewShortcuts shortcuts;
    shortcuts.frameSubject = pressed(frame, input::Key::F);
    shortcuts.toggleAutoRotate = pressed(frame, input::Key::R);
    constexpr std::array<input::Key, 5> kModeKeys{input::Key::Digit1, input::Key::Digit2, input::Key::Digit3,
                                                  input::Key::Digit4, input::Key::Digit5};
    for (std::size_t index = 0; index < kModeKeys.size(); ++index) {
        if (pressed(frame, kModeKeys[index])) {
            shortcuts.viewMode = kViewModes[index];
        }
    }
    return shortcuts;
}

} // namespace rv::preview
