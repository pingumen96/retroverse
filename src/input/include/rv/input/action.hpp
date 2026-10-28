#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rv::input {

/// Logical actions the application reacts to (specification §12.1). Bindings map devices to actions.
///
/// Axis conventions:
///  - MoveForward / MoveRight: analog in [-1, 1] (keyboard keys produce -1 or +1).
///  - LookX / LookY: *rate* in [-1, 1] from a gamepad stick; consumers multiply by a turn speed and dt.
///  - LookDeltaX / LookDeltaY: *delta* in device units for this frame (mouse pixels); consumers multiply by a
///    sensitivity and never by dt.
///  Positive LookX/LookDeltaX turns right; positive LookY/LookDeltaY looks up.
///  - UiNavX / UiNavY: analog in [-1, 1], +X right, +Y up: the product UI's stick, the focus moves ui::NavInput makes
///    of it with hysteresis. The default bindings read the stick whole (no dead zone): the hysteresis is its dead zone,
///    so its thresholds are fractions of the stick's travel (ADR-0005).
///
/// The Ui* actions drive the product UI (ADR-0005): ui::NavInput turns them into ui::UiCommands for the screen that has
/// the focus, so that a user's bindings.json and --input-script reach the UI like every other action. They share
/// devices with the actions of the 3D world (Enter is Confirm and UiActivate, B is Back and UiBack): the application
/// reads the ones of the context that has the input.
enum class Action : std::uint16_t {
    MoveForward,
    MoveRight,
    LookX,
    LookY,
    LookDeltaX,
    LookDeltaY,
    Sprint,
    Interact,         ///< E / click / A: interact, inspect
    Back,             ///< Esc / B: back, leave a mode
    QuickCatalog,     ///< Tab / Y: quick library overlay
    Confirm,          ///< Enter / Start: play now from the catalog
    Map,              ///< M / View: world map
    Menu,             ///< Esc menu / Start: settings
    ToggleDevUI,      ///< F1: developer overlay
    ReloadShaders,    ///< F5
    Screenshot,       ///< F12
    UiUp,             ///< arrow up / D-pad up: focus up (repeats while held)
    UiDown,           ///< arrow down / D-pad down
    UiLeft,           ///< arrow left / D-pad left
    UiRight,          ///< arrow right / D-pad right
    UiNavX,           ///< left stick X: focus left and right, with hysteresis (see the axis conventions)
    UiNavY,           ///< left stick Y: focus up and down
    UiPageUp,         ///< Page Up / left trigger: a page up in a list (repeats while held)
    UiPageDown,       ///< Page Down / right trigger: a page down
    UiHome,           ///< Home / left stick click: the first entry of a list
    UiEnd,            ///< End / right stick click: the last entry of a list
    UiActivate,       ///< Enter / A: press the focused element ("Avvia" on a game)
    UiBack,           ///< Esc / B: back one level ("Indietro")
    UiMedia,          ///< Y: "Vedi media"
    UiInfo,           ///< X: "Informazioni"
    UiFilterPrevious, ///< left shoulder: the previous filter
    UiFilterNext,     ///< right shoulder: the next filter
    ToggleMute,       ///< F8: mute the clips' sound, or sound them again (I5 step 5; no pad button: I7 remaps it)
    EditPin,          ///< P / X: the editor pins or unpins the game under the reticle (I4b step 3, ADR-0015 §4)
    EditRotateLeft,   ///< Q / left shoulder: the editor turns a held fixture left (I4b step 4, ADR-0015 §4)
    EditRotateRight,  ///< R / right shoulder: the editor turns a held fixture right
    Count
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

[[nodiscard]] constexpr std::size_t toIndex(Action action) noexcept {
    return static_cast<std::size_t>(action);
}

/// LookX, LookY, LookDeltaX and LookDeltaY: camera rates and deltas, which analog motion never makes "held" (turning
/// the camera presses nothing).
[[nodiscard]] constexpr bool isLookAction(Action action) noexcept {
    return action == Action::LookX || action == Action::LookY || action == Action::LookDeltaX ||
           action == Action::LookDeltaY;
}

/// The actions of the product UI, UiUp to UiFilterNext.
[[nodiscard]] constexpr bool isUiAction(Action action) noexcept {
    return toIndex(action) >= toIndex(Action::UiUp) && toIndex(action) <= toIndex(Action::UiFilterNext);
}

/// Stable lowercase identifiers used in JSON binding files: "move_forward", "look_delta_x", ...
[[nodiscard]] std::string_view actionName(Action action) noexcept;
[[nodiscard]] std::optional<Action> actionFromName(std::string_view name) noexcept;

/// Which family of devices produced the most recent input.
enum class DeviceKind : std::uint8_t { KeyboardMouse, Gamepad };

} // namespace rv::input
