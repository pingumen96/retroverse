#pragma once

// GPU-free (target rv_devui_model): no SDL, OpenGL or Dear ImGui header is included.

#include <compare>
#include <cstdint>

namespace rv::devui {

/// Input events as the overlay's routing tells them apart (DevUi classifies SDL events into these kinds).
enum class InputEventKind : std::uint8_t {
    MouseMotion,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    KeyDown, ///< key repeats included
    KeyUp,
    TextInput, ///< characters typed while a text field has the keyboard
    Other,     ///< window, display, gamepad and quit events
};

/// What decides where an event goes.
struct OverlayInputState {
    bool visible = false;
    /// Dear ImGui wants the mouse (io.WantCaptureMouse after the last overlay frame): the pointer is over a panel, a
    /// widget is being dragged or a popup is open.
    bool wantsMouse = false;
    /// Dear ImGui wants the keyboard (io.WantCaptureKeyboard): a text field or a dragged widget is active.
    bool wantsKeyboard = false;
    /// The game holds the mouse in relative mode (first-person look): there is no cursor for the panels.
    bool mouseCaptured = false;
};

/// Who receives one event.
struct EventRoute {
    bool toOverlay = false;
    bool toGame = true;

    friend auto operator<=>(const EventRoute&, const EventRoute&) = default;
};

/// Routing of one event between the developer overlay (Dear ImGui) and the game input (mouse capture policy and input
/// collector):
///  - hidden overlay: the game gets everything, the overlay nothing;
///  - mouse events while the game has the mouse captured: the game only (relative look; the overlay has no cursor);
///  - other mouse events: the overlay always (a click outside the panels also ends a text edit there), the game unless
///    the overlay wants the mouse; button releases reach the game in any case;
///  - key presses: the overlay always, the game unless the overlay wants the keyboard (Esc in a text field only leaves
///    the field, it never quits); key releases reach both, so a key held when the overlay took the keyboard is
///    released in the game too;
///  - text input: the overlay only;
///  - every other event (focus, window, gamepad, quit): both.
/// Releases are harmless where nothing was held, so delivering them everywhere never leaves a key or button stuck.
[[nodiscard]] EventRoute routeOverlayEvent(InputEventKind kind, const OverlayInputState& state) noexcept;

/// Hand-over of the mouse between first-person look and the overlay: opening the overlay releases a captured mouse so
/// that the cursor can reach the panels, and closing it captures the mouse again when the overlay released it. A
/// capture taken while the overlay is open (a click into the scene) is kept when it closes.
class CaptureHandover {
public:
    /// The overlay opens while the mouse is `captured`; true when the caller must release it.
    [[nodiscard]] bool open(bool captured) noexcept;
    /// The overlay closes while the mouse is `captured`; true when the caller must capture it again.
    [[nodiscard]] bool close(bool captured) noexcept;

private:
    /// The overlay released the capture when it opened.
    bool m_restore = false;
};

} // namespace rv::devui
