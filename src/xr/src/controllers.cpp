#include <rv/xr/controllers.hpp>

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

namespace rv::xr {
namespace {

float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

Hand other(Hand hand) noexcept {
    return hand == Hand::Left ? Hand::Right : Hand::Left;
}

} // namespace

std::string_view toString(Hand hand) noexcept {
    return hand == Hand::Left ? "left" : "right";
}

void VirtualPad::update(const PadState& state) noexcept {
    m_previous = m_state;
    m_state = state;
}

void VirtualPad::mergeInto(input::RawInputFrame& raw) const noexcept {
    bool active = false;
    for (std::size_t button = 0; button < input::kGamepadButtonCount; ++button) {
        const bool down = m_state.buttons[button];
        const bool was = m_previous.buttons[button];
        if (down) {
            raw.gamepadButtonsDown.set(button);
        }
        if (down && !was) {
            raw.gamepadButtonsPressed.set(button);
            active = true;
        }
        if (!down && was) {
            raw.gamepadButtonsReleased.set(button);
        }
    }
    for (std::size_t axis = 0; axis < input::kGamepadAxisCount; ++axis) {
        const float value = finiteOr(m_state.axes[axis], 0.0f);
        if (!raw.gamepadConnected || std::abs(value) > std::abs(raw.gamepadAxes[axis])) {
            raw.gamepadAxes[axis] = value;
        }
        active = active || std::abs(value) >= kActivity;
    }
    raw.gamepadConnected = true;
    if (active) {
        raw.lastActiveDevice = input::DeviceKind::Gamepad;
    }
}

void Pointer::update(const std::array<HandState, kHandCount>& hands, const TrackingAnchor& anchor) noexcept {
    m_pressed = false;
    m_released = false;
    std::optional<Hand> pressed;
    for (const Hand hand : {Hand::Left, Hand::Right}) {
        const std::size_t index = toIndex(hand);
        const HandState& state = hands[index];
        m_grips[index] = state.tracked && state.grip ? std::optional<Pose>(toWorld(anchor, *state.grip)) : std::nullopt;
        m_aims[index] = state.tracked && state.aim ? std::optional<Pose>(toWorld(anchor, *state.aim)) : std::nullopt;
        const bool was = m_held[index];
        const float value = finiteOr(state.select, 0.0f);
        const bool held = state.tracked && (was ? value > kRelease : value >= kPress);
        m_held[index] = held;
        if (held && !was) {
            pressed = hand;
        }
        if (!held && was && hand == m_hand) {
            m_released = true;
        }
    }
    if (pressed) {
        m_hand = *pressed;
        m_pressed = true;
    } else if (!m_aims[toIndex(m_hand)] && m_aims[toIndex(other(m_hand))]) {
        m_hand = other(m_hand);
    }
    m_ray.reset();
    if (const std::optional<Pose>& aim = m_aims[toIndex(m_hand)]) {
        m_ray = core::Ray{aim->position, glm::normalize(aim->orientation * glm::vec3(0.0f, 0.0f, -1.0f))};
    }
}

void Pointer::clear() noexcept {
    const bool wasHeld = m_held[toIndex(m_hand)];
    m_held = {};
    m_pressed = false;
    m_released = wasHeld;
    m_ray.reset();
    m_grips = {};
    m_aims = {};
}

} // namespace rv::xr
