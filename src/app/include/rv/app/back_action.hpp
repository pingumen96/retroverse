#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>

namespace rv::app {

/// What the Back action (Esc, gamepad B) does: it leaves one level at a time (specification §12.1).
enum class BackAction : std::uint8_t {
    Ignore,       ///< the developer overlay has the keyboard: Esc there only leaves the field being edited
    ReleaseMouse, ///< the mouse was captured for first-person look: free it
    CloseOverlay, ///< hide the developer overlay (which gives a capture it took back to first-person look)
    Quit,         ///< nothing left to leave
};

/// Back pressed while the mouse is `mouseCaptured`, with the developer overlay `overlayVisible`, and
/// `overlayHasKeyboard` when a text field or a dragged widget of the overlay has the keyboard: Ignore while the overlay
/// has the keyboard (the pad's B included, so that leaving a field never quits), else ReleaseMouse while the mouse is
/// captured, else CloseOverlay while the overlay is visible, else Quit. Builds without the overlay pass false for both.
[[nodiscard]] BackAction backAction(bool mouseCaptured, bool overlayVisible, bool overlayHasKeyboard) noexcept;

} // namespace rv::app
