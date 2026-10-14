#include <rv/input/input_system.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>

namespace rv::input {
namespace {

[[nodiscard]] float finiteOrZero(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

template <std::size_t N>
[[nodiscard]] bool isSet(const std::bitset<N>& bits, std::size_t index) noexcept {
    return index < N && bits[index];
}

/// Dead zone with rescaling, then response curve, keeping the sign. Expects the ranges guaranteed by isValid():
/// deadzone in [0, 1), exponent > 0.
[[nodiscard]] float shapeGamepadAxis(float value, float deadzone, float exponent) noexcept {
    const float raw = finiteOrZero(value);
    const float magnitude = std::min(std::abs(raw), 1.0f);
    float shaped = std::clamp((magnitude - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
    if (exponent != 1.0f) {
        shaped = std::pow(shaped, exponent);
    }
    return raw < 0.0f ? -shaped : shaped;
}

[[nodiscard]] float mouseAxisValue(const RawInputFrame& frame, MouseAxis axis) noexcept {
    switch (axis) {
    case MouseAxis::DeltaX:
        return finiteOrZero(frame.mouseDelta.x);
    case MouseAxis::DeltaY:
        return finiteOrZero(frame.mouseDelta.y);
    case MouseAxis::Wheel:
        return finiteOrZero(frame.mouseWheel);
    case MouseAxis::Count:
        break;
    }
    return 0.0f;
}

/// What the bindings of one action contribute in one frame.
struct Contribution {
    float sum = 0.0f;
    bool buttonHeld = false;    ///< a bound key or button is down at the end of the frame
    bool buttonPressed = false; ///< a bound key or button went down during the frame

    void addButton(bool down, bool pressedThisFrame, float scale) noexcept {
        if (down) {
            sum += scale;
            buttonHeld = true;
        }
        buttonPressed = buttonPressed || pressedThisFrame;
    }
};

void accumulate(const KeyBinding& binding, const RawInputFrame& frame, Contribution& out) noexcept {
    const std::size_t index = toIndex(binding.key);
    out.addButton(isSet(frame.keysDown, index), isSet(frame.keysPressed, index), binding.scale);
}

void accumulate(const MouseButtonBinding& binding, const RawInputFrame& frame, Contribution& out) noexcept {
    const std::size_t index = toIndex(binding.button);
    out.addButton(isSet(frame.mouseButtonsDown, index), isSet(frame.mouseButtonsPressed, index), binding.scale);
}

void accumulate(const MouseAxisBinding& binding, const RawInputFrame& frame, Contribution& out) noexcept {
    out.sum += mouseAxisValue(frame, binding.axis) * binding.scale;
}

void accumulate(const GamepadButtonBinding& binding, const RawInputFrame& frame, Contribution& out) noexcept {
    if (!frame.gamepadConnected) {
        return;
    }
    const std::size_t index = toIndex(binding.button);
    out.addButton(isSet(frame.gamepadButtonsDown, index), isSet(frame.gamepadButtonsPressed, index), binding.scale);
}

void accumulate(const GamepadAxisBinding& binding, const RawInputFrame& frame, Contribution& out) noexcept {
    const std::size_t index = toIndex(binding.axis);
    if (!frame.gamepadConnected || index >= kGamepadAxisCount) {
        return;
    }
    out.sum += shapeGamepadAxis(frame.gamepadAxes[index], binding.deadzone, binding.exponent) * binding.scale;
}

[[nodiscard]] Contribution collect(std::span<const Binding> bindings, const RawInputFrame& frame) {
    Contribution contribution;
    for (const Binding& binding : bindings) {
        std::visit([&](const auto& alternative) { accumulate(alternative, frame, contribution); }, binding);
    }
    return contribution;
}

[[nodiscard]] bool isLookAction(Action action) noexcept {
    return action == Action::LookX || action == Action::LookY || action == Action::LookDeltaX ||
           action == Action::LookDeltaY;
}

[[nodiscard]] float finalizeValue(Action action, float sum, const InputSettings& settings) noexcept {
    float value = 0.0f;
    switch (action) {
    case Action::LookDeltaX:
    case Action::LookDeltaY:
        value = sum * settings.mouseSensitivityScale;
        break;
    case Action::LookX:
    case Action::LookY:
        // Scaled before the clamp: these stay rates in [-1, 1] (action.hpp) whatever the sensitivity.
        value = std::clamp(sum * settings.stickSensitivityScale, -1.0f, 1.0f);
        break;
    default:
        value = std::clamp(sum, -1.0f, 1.0f);
        break;
    }
    if (settings.invertLookY && (action == Action::LookY || action == Action::LookDeltaY)) {
        value = -value;
    }
    // NaN survives std::clamp; an overflowing unclamped product would reach the camera as infinity.
    return finiteOrZero(value);
}

[[nodiscard]] float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

InputSystem::InputSystem(ActionMap map) : m_map(std::move(map)) {}

void InputSystem::setMap(ActionMap map) {
    m_map = std::move(map);
}

void InputSystem::setSettings(const InputSettings& settings) {
    const InputSettings defaults;
    m_settings.pressThreshold =
        std::clamp(finiteOr(settings.pressThreshold, defaults.pressThreshold), kMinPressThreshold, 1.0f);
    m_settings.invertLookY = settings.invertLookY;
    m_settings.mouseSensitivityScale = finiteOr(settings.mouseSensitivityScale, defaults.mouseSensitivityScale);
    m_settings.stickSensitivityScale = finiteOr(settings.stickSensitivityScale, defaults.stickSensitivityScale);
}

ActionState InputSystem::resolve(const RawInputFrame& frame) {
    ActionState state;
    state.setActiveDevice(frame.lastActiveDevice);
    for (std::size_t index = 0; index < kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        const Contribution contribution = collect(m_map.bindings(action), frame);
        const float value = finalizeValue(action, contribution.sum, m_settings);

        const bool analogHeld = !isLookAction(action) && std::abs(value) >= m_settings.pressThreshold;
        const bool held = contribution.buttonHeld || analogHeld;
        const bool wasHeld = m_previousHeld[index];
        // A bound button that went down and up again within this frame still counts as a full click.
        const bool tapped = !wasHeld && !held && contribution.buttonPressed;

        state.setButton(action, held, (held && !wasHeld) || tapped, (!held && wasHeld) || tapped);
        state.setAxis(action, value);
        m_previousHeld[index] = held;
    }
    return state;
}

void InputSystem::reset() noexcept {
    m_previousHeld.reset();
}

} // namespace rv::input
