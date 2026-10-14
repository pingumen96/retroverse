#pragma once

#include <array>
#include <cstdint>

#include <rv/input/action.hpp>

namespace rv::input {

/// Snapshot of every action for one frame. Produced by the input system, consumed by controllers and UI.
/// Header-only and trivially copyable so it can be built by hand in tests.
class ActionState {
public:
    /// Analog value: axes report their value, buttons report 1 while held and 0 otherwise.
    [[nodiscard]] float axis(Action action) const noexcept { return m_axes[toIndex(action)]; }
    /// Button currently down (or axis magnitude above the press threshold).
    [[nodiscard]] bool held(Action action) const noexcept { return (m_flags[toIndex(action)] & kHeld) != 0; }
    /// Went down this frame.
    [[nodiscard]] bool pressed(Action action) const noexcept { return (m_flags[toIndex(action)] & kPressed) != 0; }
    /// Went up this frame.
    [[nodiscard]] bool released(Action action) const noexcept { return (m_flags[toIndex(action)] & kReleased) != 0; }

    [[nodiscard]] DeviceKind activeDevice() const noexcept { return m_activeDevice; }

    void setAxis(Action action, float value) noexcept { m_axes[toIndex(action)] = value; }

    /// Sets the digital state and overwrites the axis with 1 (held) or 0. For actions that are both digital
    /// and analog (e.g. a trigger bound to Sprint), call setAxis() afterwards with the analog value.
    void setButton(Action action, bool isHeld, bool wasPressed, bool wasReleased) noexcept {
        const unsigned flags = (isHeld ? kHeld : 0u) | (wasPressed ? kPressed : 0u) | (wasReleased ? kReleased : 0u);
        m_flags[toIndex(action)] = static_cast<std::uint8_t>(flags);
        m_axes[toIndex(action)] = isHeld ? 1.0f : 0.0f;
    }

    void setActiveDevice(DeviceKind device) noexcept { m_activeDevice = device; }

    void clear() noexcept {
        m_axes.fill(0.0f);
        m_flags.fill(0);
    }

private:
    static constexpr std::uint8_t kHeld = 1u << 0;
    static constexpr std::uint8_t kPressed = 1u << 1;
    static constexpr std::uint8_t kReleased = 1u << 2;

    std::array<float, kActionCount> m_axes{};
    std::array<std::uint8_t, kActionCount> m_flags{};
    DeviceKind m_activeDevice = DeviceKind::KeyboardMouse;
};

} // namespace rv::input
