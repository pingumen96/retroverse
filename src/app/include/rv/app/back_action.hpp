#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <string_view>

namespace rv::app {

/// What the Back action (Esc, gamepad B) does: it leaves one level at a time (specification §12.1).
enum class BackAction : std::uint8_t {
    Ignore,       ///< the developer overlay has the keyboard: Esc there only leaves the field being edited
    ClosePopup,   ///< close the topmost popup of the developer overlay (a combo's list, a color picker, a tab menu)
    PanelBack,    ///< the product UI's game-session panel is open: its Back (back to the game while it runs, or the
                  ///< outcome's message dismissed)
    CatalogBack,  ///< the quick catalog is open: its Back (along its focus history, then it closes)
    ReleaseMouse, ///< the mouse was captured for first-person look: free it
    CloseOverlay, ///< hide the developer overlay (which gives a capture it took back to first-person look)
    Quit,         ///< nothing left to leave
};

/// "ignore", "close popup", "panel back", "catalog back", "release mouse", "close overlay", "quit".
[[nodiscard]] std::string_view toString(BackAction action) noexcept;

/// What Back can leave when it is pressed. Builds without the developer overlay leave its fields false, runs without a
/// product UI (no catalog) the product UI's.
struct BackState {
    /// First-person look holds the mouse.
    bool mouseCaptured = false;
    bool overlayVisible = false;
    /// A text field or a dragged widget of the visible overlay has the keyboard.
    bool overlayHasKeyboard = false;
    /// A popup of the visible overlay is open.
    bool overlayPopupOpen = false;
    /// The product UI's game-session panel is shown (a game running, its outcome): a popup over the catalog.
    bool panelOpen = false;
    /// The quick catalog is open.
    bool catalogOpen = false;
};

/// Back pressed in `state`, the first level that applies:
///  1. Ignore while the overlay has the keyboard (the pad's B included, so that leaving a field never quits);
///  2. ClosePopup while one of its popups is open;
///  3. PanelBack while the product UI's panel is shown, then CatalogBack while the catalog is open: the product UI's
///     levels come before the mouse and the overlay, so that Back walks out of the catalog before it can quit;
///  4. ReleaseMouse while the mouse is captured;
///  5. CloseOverlay while the overlay is visible;
///  6. Quit.
/// A hidden overlay has neither the keyboard nor popups.
[[nodiscard]] BackAction backAction(const BackState& state) noexcept;

} // namespace rv::app
