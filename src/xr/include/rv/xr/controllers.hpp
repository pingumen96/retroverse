#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/core/math.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/xr/pose.hpp>

namespace rv::xr {

/// A hand of the user (OpenXR's /user/hand/left and /user/hand/right).
enum class Hand : std::uint8_t { Left, Right };

inline constexpr std::size_t kHandCount = 2;

[[nodiscard]] constexpr std::size_t toIndex(Hand hand) noexcept {
    return static_cast<std::size_t>(hand);
}

/// "left" or "right".
[[nodiscard]] std::string_view toString(Hand hand) noexcept;

/// The buttons and sticks of the controllers as one gamepad (xr_bindings.json maps each controller's inputs onto a
/// gamepad's), so that the action map's gamepad bindings drive the VR mode as they drive a pad (ADR-0017 §5).
struct PadState {
    std::bitset<input::kGamepadButtonCount> buttons;
    /// Indexed with input::toIndex(GamepadAxis): sticks in [-1, 1] with +Y up, triggers in [0, 1].
    std::array<float, input::kGamepadAxisCount> axes{};

    bool operator==(const PadState&) const = default;
};

/// A controller held in a hand, as the runtime located it at the frame's display time (in the reference space).
struct HandState {
    /// The controller is there and its poses are tracked.
    bool tracked = false;
    /// Where the hand holds it (OpenXR's grip pose: the palm's centre, -Z along the closed fist), and where it points
    /// (the aim pose: -Z along the pointing ray).
    std::optional<Pose> grip;
    std::optional<Pose> aim;
    /// The trigger, [0, 1]: select with this hand's ray.
    float select = 0.0f;

    bool operator==(const HandState&) const = default;
};

/// What the controllers gave a frame (Runtime::syncControllers).
struct ControllerState {
    PadState pad;
    std::array<HandState, kHandCount> hands{};

    bool operator==(const ControllerState&) const = default;
};

/// The controllers' gamepad merged into a frame's device input (ADR-0017 §5: the Touch controllers drive the same
/// actions as the pad). Each update() takes the frame's PadState; mergeInto() adds its buttons (held, pressed and
/// released against the previous update's) and its sticks and triggers (where they are farther from rest than the
/// physical pad's) to the frame, which then reports a connected gamepad, the last device used when a button went down
/// or an axis left its dead zone. GPU-free.
class VirtualPad {
public:
    /// Motion below this (sticks, triggers) leaves the last device used as it was.
    static constexpr float kActivity = 0.2f;

    void update(const PadState& state) noexcept;
    /// No controller this frame: every button released.
    void release() noexcept { update(PadState{}); }
    void mergeInto(input::RawInputFrame& raw) const noexcept;
    [[nodiscard]] const PadState& state() const noexcept { return m_state; }

private:
    PadState m_state;
    PadState m_previous;
};

/// Which hand points, and where (ADR-0017 §5: the ray feeds the pick). The hand whose trigger went down last points
/// (the right hand at first); a hand that loses tracking passes the pointing to the other. Each update() takes the
/// frame's hands in the tracking space and where the space sits in the world. GPU-free.
class Pointer {
public:
    /// The trigger counts as pressed past this and released below kRelease.
    static constexpr float kPress = 0.6f;
    static constexpr float kRelease = 0.4f;

    void update(const std::array<HandState, kHandCount>& hands, const TrackingAnchor& anchor) noexcept;
    /// No controller: nothing points.
    void clear() noexcept;

    /// The pointing hand, while one is tracked with its aim pose.
    [[nodiscard]] std::optional<Hand> hand() const noexcept {
        return m_ray ? std::optional<Hand>(m_hand) : std::nullopt;
    }
    /// Its ray in the world (the aim pose's position along its -Z).
    [[nodiscard]] const std::optional<core::Ray>& ray() const noexcept { return m_ray; }
    /// The pointing hand's trigger: held, gone down, gone up this update.
    [[nodiscard]] bool selectHeld() const noexcept { return m_held[toIndex(m_hand)]; }
    [[nodiscard]] bool selectPressed() const noexcept { return m_pressed; }
    [[nodiscard]] bool selectReleased() const noexcept { return m_released; }
    /// Each hand's grip and aim poses in the world (none while untracked).
    [[nodiscard]] const std::optional<Pose>& grip(Hand hand) const noexcept { return m_grips[toIndex(hand)]; }
    [[nodiscard]] const std::optional<Pose>& aim(Hand hand) const noexcept { return m_aims[toIndex(hand)]; }

private:
    Hand m_hand = Hand::Right;
    std::array<bool, kHandCount> m_held{};
    bool m_pressed = false;
    bool m_released = false;
    std::optional<core::Ray> m_ray;
    std::array<std::optional<Pose>, kHandCount> m_grips;
    std::array<std::optional<Pose>, kHandCount> m_aims;
};

} // namespace rv::xr
