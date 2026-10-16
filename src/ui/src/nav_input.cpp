#include <rv/ui/nav_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace rv::ui {
namespace {

constexpr std::array<NavDirection, 4> kDirections{NavDirection::Up, NavDirection::Down, NavDirection::Left,
                                                  NavDirection::Right};
/// Rounding slack of the repeat timing, seconds.
constexpr double kTimeSlack = 1.0e-6;
/// The commands that are not focus moves, in the order a frame reports them.
constexpr std::array<UiCommand, 4> kButtonCommands{UiCommand::Activate, UiCommand::Back, UiCommand::Media,
                                                   UiCommand::Info};

[[nodiscard]] std::size_t indexOf(NavDirection direction) noexcept {
    return static_cast<std::size_t>(direction);
}

[[nodiscard]] double sanitizedDelta(double deltaSeconds) noexcept {
    return std::isfinite(deltaSeconds) && deltaSeconds > 0.0 ? deltaSeconds : 0.0;
}

[[nodiscard]] float sanitizedAxis(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

[[nodiscard]] NavDirection verticalOf(float y) noexcept {
    return y > 0.0f ? NavDirection::Up : NavDirection::Down;
}

[[nodiscard]] NavDirection horizontalOf(float x) noexcept {
    return x > 0.0f ? NavDirection::Right : NavDirection::Left;
}

[[nodiscard]] bool isVertical(NavDirection direction) noexcept {
    return direction == NavDirection::Up || direction == NavDirection::Down;
}

} // namespace

DirectionRepeater::DirectionRepeater(const NavSettings& settings) noexcept : m_settings(settings) {}

std::optional<NavDirection> DirectionRepeater::update(std::optional<NavDirection> held, double deltaSeconds) noexcept {
    if (!held) {
        reset();
        return std::nullopt;
    }
    if (m_held != held) {
        // A new direction (or a change of direction) steps at once.
        m_held = held;
        m_heldSeconds = 0.0;
        m_nextStepAt = m_settings.repeatDelay;
        return held;
    }
    m_heldSeconds += sanitizedDelta(deltaSeconds);
    // A microsecond of slack: 24 frames of 1/60 s add up to a hair under 0.4 s in binary floating point, and must count
    // as the 0.4 s delay, not miss it by a frame.
    if (m_heldSeconds + kTimeSlack < m_nextStepAt) {
        return std::nullopt;
    }
    const double interval =
        m_heldSeconds + kTimeSlack >= m_settings.fastRepeatAfter ? m_settings.fastRepeatInterval
                                                                 : m_settings.repeatInterval;
    m_nextStepAt += std::max(interval, 1.0e-3);
    if (m_nextStepAt <= m_heldSeconds + kTimeSlack) {
        // Late by more than an interval (a long frame): one step now, the cadence restarts from here, no backlog.
        m_nextStepAt = m_heldSeconds + std::max(interval, 1.0e-3);
    }
    return held;
}

void DirectionRepeater::reset() noexcept {
    m_held.reset();
    m_heldSeconds = 0.0;
    m_nextStepAt = 0.0;
}

StickDirection::StickDirection(const NavSettings& settings) noexcept : m_settings(settings) {}

std::optional<NavDirection> StickDirection::update(float x, float y) noexcept {
    x = sanitizedAxis(x);
    y = sanitizedAxis(y);
    const float magnitude = std::sqrt(x * x + y * y);
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    if (!m_direction) {
        if (magnitude < m_settings.stickEngage) {
            return std::nullopt;
        }
        // The dominant axis; a perfect diagonal goes vertical (lists scroll vertically).
        m_direction = ay >= ax ? verticalOf(y) : horizontalOf(x);
        return m_direction;
    }
    if (magnitude < m_settings.stickRelease) {
        m_direction.reset();
        return std::nullopt;
    }
    // Engaged: the engaged axis keeps the direction unless the other axis exceeds it by the switch ratio. Along the
    // engaged axis the sign decides, so a stick flicked to the opposite side within one sample turns around.
    const float ratio = std::max(m_settings.axisSwitchRatio, 1.0f);
    if (isVertical(*m_direction)) {
        m_direction = ax > ay * ratio ? horizontalOf(x) : (y != 0.0f ? verticalOf(y) : *m_direction);
    } else {
        m_direction = ay > ax * ratio ? verticalOf(y) : (x != 0.0f ? horizontalOf(x) : *m_direction);
    }
    return m_direction;
}

NavBindings NavBindings::defaults() {
    using input::GamepadButton;
    using input::Key;
    NavBindings bindings;
    bindings.buttons[toIndex(UiCommand::Up)] = {GamepadButton::DpadUp};
    bindings.buttons[toIndex(UiCommand::Down)] = {GamepadButton::DpadDown};
    bindings.buttons[toIndex(UiCommand::Left)] = {GamepadButton::DpadLeft};
    bindings.buttons[toIndex(UiCommand::Right)] = {GamepadButton::DpadRight};
    bindings.buttons[toIndex(UiCommand::Activate)] = {GamepadButton::South};
    bindings.buttons[toIndex(UiCommand::Back)] = {GamepadButton::East};
    bindings.buttons[toIndex(UiCommand::Media)] = {GamepadButton::North};
    bindings.buttons[toIndex(UiCommand::Info)] = {GamepadButton::West};
    bindings.keys[toIndex(UiCommand::Up)] = {Key::Up};
    bindings.keys[toIndex(UiCommand::Down)] = {Key::Down};
    bindings.keys[toIndex(UiCommand::Left)] = {Key::Left};
    bindings.keys[toIndex(UiCommand::Right)] = {Key::Right};
    bindings.keys[toIndex(UiCommand::Activate)] = {Key::Enter};
    bindings.keys[toIndex(UiCommand::Back)] = {Key::Escape};
    bindings.leftStickNavigates = true;
    return bindings;
}

NavInput::NavInput(NavSettings settings, NavBindings bindings)
    : m_settings(settings), m_bindings(std::move(bindings)), m_repeater(m_settings), m_stick(m_settings) {}

bool NavInput::isDown(const input::RawInputFrame& frame, UiCommand command) const noexcept {
    if (frame.gamepadConnected) {
        for (const input::GamepadButton button : m_bindings.buttons[toIndex(command)]) {
            if (frame.gamepadButtonsDown[input::toIndex(button)]) {
                return true;
            }
        }
    }
    for (const input::Key key : m_bindings.keys[toIndex(command)]) {
        if (frame.keysDown[input::toIndex(key)]) {
            return true;
        }
    }
    return false;
}

bool NavInput::wasPressed(const input::RawInputFrame& frame, UiCommand command) const noexcept {
    if (frame.gamepadConnected) {
        for (const input::GamepadButton button : m_bindings.buttons[toIndex(command)]) {
            if (frame.gamepadButtonsPressed[input::toIndex(button)]) {
                return true;
            }
        }
    }
    for (const input::Key key : m_bindings.keys[toIndex(command)]) {
        if (frame.keysPressed[input::toIndex(key)]) {
            return true;
        }
    }
    return false;
}

std::optional<NavDirection> NavInput::heldDigital(const input::RawInputFrame& frame) const noexcept {
    std::optional<NavDirection> held;
    std::uint64_t heldOrder = 0;
    for (const NavDirection direction : kDirections) {
        if (!isDown(frame, commandFor(direction))) {
            continue;
        }
        const std::uint64_t order = m_pressOrder[indexOf(direction)];
        if (!held || order > heldOrder) {
            held = direction;
            heldOrder = order;
        }
    }
    return held;
}

std::optional<NavDirection> NavInput::tappedDigital(const input::RawInputFrame& frame) const noexcept {
    for (const NavDirection direction : kDirections) {
        const UiCommand command = commandFor(direction);
        if (wasPressed(frame, command) && !isDown(frame, command)) {
            return direction;
        }
    }
    return std::nullopt;
}

void NavInput::update(const input::RawInputFrame& frame, double deltaSeconds, UiCommandList& out) {
    bool anyPress = false;
    for (const NavDirection direction : kDirections) {
        if (wasPressed(frame, commandFor(direction))) {
            m_pressOrder[indexOf(direction)] = ++m_pressCounter;
            anyPress = true;
        }
    }
    std::optional<NavDirection> held = heldDigital(frame);
    // The stick keeps its hysteresis state current even while a digital direction wins over it.
    std::optional<NavDirection> stick;
    if (m_bindings.leftStickNavigates && frame.gamepadConnected) {
        stick = m_stick.update(frame.gamepadAxes[input::toIndex(input::GamepadAxis::LeftX)],
                               frame.gamepadAxes[input::toIndex(input::GamepadAxis::LeftY)]);
    } else {
        m_stick.reset();
    }
    if (m_suppressHeld) {
        // After reset(): a fresh press, or everything released, ends the suppression.
        if (anyPress || (!held && !stick)) {
            m_suppressHeld = false;
        } else {
            held.reset();
            stick.reset();
        }
    }
    // Pressed again within the frame (released and pressed between two frames): a fresh step.
    const bool freshPress = held && wasPressed(frame, commandFor(*held));
    if (!held) {
        held = stick;
    }
    if (freshPress) {
        m_repeater.reset();
    }
    std::optional<NavDirection> step = m_repeater.update(held, deltaSeconds);
    if (!held && !step) {
        step = tappedDigital(frame); // pressed and released within the frame: one step
    }
    if (step) {
        out.push(commandFor(*step));
    }
    for (const UiCommand command : kButtonCommands) {
        if (wasPressed(frame, command)) {
            out.push(command);
        }
    }
}

void NavInput::reset() noexcept {
    m_repeater.reset();
    m_stick.reset();
    m_pressOrder.fill(0);
    m_pressCounter = 0;
    m_suppressHeld = true;
}

} // namespace rv::ui
