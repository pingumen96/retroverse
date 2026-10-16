#include <rv/ui/nav_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <variant>

#include <rv/input/binding.hpp>

namespace rv::ui {
namespace {

constexpr std::array<NavDirection, 4> kDirections{NavDirection::Up, NavDirection::Down, NavDirection::Left,
                                                  NavDirection::Right};
/// Rounding slack of the repeat timing, seconds.
constexpr double kTimeSlack = 1.0e-6;
/// The commands of a press, in the order a frame reports them (after the focus and page steps).
constexpr std::array<UiCommand, 8> kButtonCommands{UiCommand::Activate,       UiCommand::Back,      UiCommand::Media,
                                                   UiCommand::Info,           UiCommand::Home,      UiCommand::End,
                                                   UiCommand::FilterPrevious, UiCommand::FilterNext};
/// The page commands, and the direction each runs the page repeater in.
constexpr std::array<UiCommand, 2> kPages{UiCommand::PageUp, UiCommand::PageDown};
constexpr std::array<NavDirection, 2> kPageDirections{NavDirection::Up, NavDirection::Down};

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

[[nodiscard]] UiCommand pageCommand(NavDirection direction) noexcept {
    return direction == NavDirection::Up ? UiCommand::PageUp : UiCommand::PageDown;
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

input::Action actionFor(UiCommand command) noexcept {
    using input::Action;
    switch (command) {
    case UiCommand::Up:
        return Action::UiUp;
    case UiCommand::Down:
        return Action::UiDown;
    case UiCommand::Left:
        return Action::UiLeft;
    case UiCommand::Right:
        return Action::UiRight;
    case UiCommand::Activate:
        return Action::UiActivate;
    case UiCommand::Back:
        return Action::UiBack;
    case UiCommand::Media:
        return Action::UiMedia;
    case UiCommand::Info:
        return Action::UiInfo;
    case UiCommand::PageUp:
        return Action::UiPageUp;
    case UiCommand::PageDown:
        return Action::UiPageDown;
    case UiCommand::Home:
        return Action::UiHome;
    case UiCommand::End:
        return Action::UiEnd;
    case UiCommand::FilterPrevious:
        return Action::UiFilterPrevious;
    case UiCommand::FilterNext:
        return Action::UiFilterNext;
    case UiCommand::Count:
        break;
    }
    return Action::UiActivate;
}

std::bitset<input::kKeyCount> commandKeys(const input::ActionMap& map) {
    std::bitset<input::kKeyCount> keys;
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        const auto action = static_cast<input::Action>(index);
        if (!input::isUiAction(action)) {
            continue;
        }
        for (const input::Binding& binding : map.bindings(action)) {
            if (const auto* key = std::get_if<input::KeyBinding>(&binding)) {
                keys.set(input::toIndex(key->key));
            }
        }
    }
    return keys;
}

NavInput::NavInput(NavSettings settings)
    : m_settings(settings), m_repeater(m_settings), m_pageRepeater(m_settings), m_stick(m_settings) {}

std::optional<NavDirection> NavInput::heldDigital(const input::ActionState& actions) const noexcept {
    std::optional<NavDirection> held;
    std::uint64_t heldOrder = 0;
    for (const NavDirection direction : kDirections) {
        if (!actions.held(actionFor(commandFor(direction)))) {
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

std::optional<NavDirection> NavInput::tappedDigital(const input::ActionState& actions) const noexcept {
    for (const NavDirection direction : kDirections) {
        const input::Action action = actionFor(commandFor(direction));
        if (actions.pressed(action) && !actions.held(action)) {
            return direction;
        }
    }
    return std::nullopt;
}

std::optional<NavDirection> NavInput::heldPage(const input::ActionState& actions) const noexcept {
    std::optional<NavDirection> held;
    std::uint64_t heldOrder = 0;
    for (std::size_t page = 0; page < kPages.size(); ++page) {
        if (!actions.held(actionFor(kPages[page]))) {
            continue;
        }
        if (!held || m_pagePressOrder[page] > heldOrder) {
            held = kPageDirections[page];
            heldOrder = m_pagePressOrder[page];
        }
    }
    return held;
}

std::optional<NavDirection> NavInput::tappedPage(const input::ActionState& actions) const noexcept {
    for (std::size_t page = 0; page < kPages.size(); ++page) {
        const input::Action action = actionFor(kPages[page]);
        if (actions.pressed(action) && !actions.held(action)) {
            return kPageDirections[page];
        }
    }
    return std::nullopt;
}

std::optional<NavDirection> NavInput::directionStep(const input::ActionState& actions, double deltaSeconds) {
    bool anyPress = false;
    for (const NavDirection direction : kDirections) {
        if (actions.pressed(actionFor(commandFor(direction)))) {
            m_pressOrder[indexOf(direction)] = ++m_pressCounter;
            anyPress = true;
        }
    }
    std::optional<NavDirection> held = heldDigital(actions);
    // The stick keeps its hysteresis state current even while a digital direction wins over it.
    std::optional<NavDirection> stick =
        m_stick.update(actions.axis(input::Action::UiNavX), actions.axis(input::Action::UiNavY));
    if (m_suppressHeld) {
        // After reset(): a fresh press, or everything released, ends the suppression.
        if (anyPress || (!held && !stick)) {
            m_suppressHeld = false;
        } else {
            held.reset();
            stick.reset();
        }
    }
    // The winning direction went down this frame (the stick may have held the same direction already): a fresh step.
    const bool freshPress = held && actions.pressed(actionFor(commandFor(*held)));
    if (!held) {
        held = stick;
    }
    if (freshPress) {
        m_repeater.reset();
    }
    std::optional<NavDirection> step = m_repeater.update(held, deltaSeconds);
    if (!held && !step) {
        step = tappedDigital(actions); // pressed and released within the frame: one step
    }
    return step;
}

std::optional<NavDirection> NavInput::pageStep(const input::ActionState& actions, double deltaSeconds) {
    bool anyPress = false;
    for (std::size_t page = 0; page < kPages.size(); ++page) {
        if (actions.pressed(actionFor(kPages[page]))) {
            m_pagePressOrder[page] = ++m_pressCounter;
            anyPress = true;
        }
    }
    std::optional<NavDirection> held = heldPage(actions);
    if (m_suppressPages) {
        if (anyPress || !held) {
            m_suppressPages = false;
        } else {
            held.reset();
        }
    }
    if (held && actions.pressed(actionFor(pageCommand(*held)))) {
        m_pageRepeater.reset();
    }
    std::optional<NavDirection> step = m_pageRepeater.update(held, deltaSeconds);
    if (!held && !step) {
        step = tappedPage(actions);
    }
    return step;
}

void NavInput::update(const input::ActionState& actions, double deltaSeconds, UiCommandList& out) {
    if (const std::optional<NavDirection> step = directionStep(actions, deltaSeconds)) {
        out.push(commandFor(*step));
    }
    if (const std::optional<NavDirection> page = pageStep(actions, deltaSeconds)) {
        out.push(pageCommand(*page));
    }
    for (const UiCommand command : kButtonCommands) {
        if (actions.pressed(actionFor(command))) {
            out.push(command);
        }
    }
}

void NavInput::reset() noexcept {
    m_repeater.reset();
    m_pageRepeater.reset();
    m_stick.reset();
    m_pressOrder.fill(0);
    m_pagePressOrder.fill(0);
    m_pressCounter = 0;
    m_suppressHeld = true;
    m_suppressPages = true;
}

} // namespace rv::ui
