#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>

namespace rv::app {

/// What the Back action (Esc, gamepad B) does: it leaves one level at a time (specification §12.1).
enum class BackAction : std::uint8_t {
    Ignore,       ///< the developer overlay has the keyboard: Esc there only leaves the field being edited
    ClosePopup,   ///< close the topmost popup of the developer overlay (a combo's list, a color picker, a tab menu)
    ReleaseMouse, ///< the mouse was captured for first-person look: free it
    CloseOverlay, ///< hide the developer overlay (which gives a capture it took back to first-person look)
    Quit,         ///< nothing left to leave
};

/// What Back can leave when it is pressed. Builds without the developer overlay leave its fields false.
struct BackState {
    /// First-person look holds the mouse.
    bool mouseCaptured = false;
    bool overlayVisible = false;
    /// A text field or a dragged widget of the visible overlay has the keyboard.
    bool overlayHasKeyboard = false;
    /// A popup of the visible overlay is open.
    bool overlayPopupOpen = false;
};

/// Back pressed in `state`: Ignore while the overlay has the keyboard (the pad's B included, so that leaving a field
/// never quits), else ClosePopup while one of its popups is open, else ReleaseMouse while the mouse is captured, else
/// CloseOverlay while the overlay is visible, else Quit. A hidden overlay has neither the keyboard nor popups.
[[nodiscard]] BackAction backAction(const BackState& state) noexcept;

} // namespace rv::app
