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
enum class Action : std::uint16_t {
    MoveForward,
    MoveRight,
    LookX,
    LookY,
    LookDeltaX,
    LookDeltaY,
    Sprint,
    Interact,     ///< E / click / A: interact, inspect
    Back,         ///< Esc / B: back, leave a mode
    QuickCatalog, ///< Tab / Y: quick library overlay
    Confirm,      ///< Enter / Start: play now from the catalog
    Map,          ///< M / View: world map
    Menu,         ///< Esc menu / Start: settings
    ToggleDevUI,  ///< F1: developer overlay
    ReloadShaders,///< F5
    Screenshot,   ///< F12
    Count
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

[[nodiscard]] constexpr std::size_t toIndex(Action action) noexcept {
    return static_cast<std::size_t>(action);
}

/// Stable lowercase identifiers used in JSON binding files: "move_forward", "look_delta_x", ...
[[nodiscard]] std::string_view actionName(Action action) noexcept;
[[nodiscard]] std::optional<Action> actionFromName(std::string_view name) noexcept;

/// Which family of devices produced the most recent input.
enum class DeviceKind : std::uint8_t { KeyboardMouse, Gamepad };

} // namespace rv::input
