#pragma once

#include <cstdint>
#include <string_view>

union SDL_Event;
struct SDL_Window;

namespace rv::platform {

/// Mouse capture for first-person look: SDL relative mouse mode on one window (the cursor is hidden and confined, and
/// motion arrives as relative deltas). Main thread only; the window must outlive this object.
///
/// Policy, applied by handleEvent():
///  - a click into the window while it has input focus and the mouse is free captures the mouse, and that click is
///    consumed (handleEvent returns true): it must not also act in the scene;
///  - losing focus releases the mouse (back in the window, a click captures it again);
///  - captureWhenFocused() captures at once when the window has focus, otherwise at its next focus gain (startup, the
///    developer overlay giving the mouse back);
///  - the application releases it explicitly (setCaptured(false), on Back).
/// Every change is logged. Destruction releases a captured mouse.
class MouseCapture {
public:
    explicit MouseCapture(SDL_Window* window) noexcept;
    MouseCapture(const MouseCapture&) = delete;
    MouseCapture& operator=(const MouseCapture&) = delete;
    ~MouseCapture();

    /// Applies the policy to one event. Returns true when the event was consumed (the click that captured).
    [[nodiscard]] bool handleEvent(const SDL_Event& event);
    /// Captures now if the window has input focus, otherwise when it next gains focus (cancelled by setCaptured()).
    /// `reason` completes the log line of an immediate capture ("Mouse captured (<reason>)"); a deferred one is logged
    /// as caused by the focus gain.
    void captureWhenFocused(std::string_view reason);
    /// Captures or releases the mouse and returns the resulting state (unchanged when SDL refuses, logged). `reason`
    /// completes the log line ("Mouse released (<reason>)").
    bool setCaptured(bool captured, std::string_view reason = "requested");

    [[nodiscard]] bool captured() const noexcept { return m_captured; }
    /// The window has input focus, as last reported by SDL.
    [[nodiscard]] bool focused() const noexcept { return m_focused; }

private:
    bool apply(bool captured, std::string_view reason);

    SDL_Window* m_window = nullptr;
    std::uint32_t m_windowId = 0;
    bool m_focused = false;
    bool m_captured = false;
    bool m_captureOnFocus = false;
};

} // namespace rv::platform
