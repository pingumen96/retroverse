#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <string_view>

#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>

namespace rv::app {

/// Who reads the actions of a frame. The 3D world's actions and the product UI's share devices (Enter is Confirm and
/// UiActivate, B is Back and UiBack, the left stick MoveForward and UiNavY, its click Sprint and UiHome: the default
/// bindings, assets/config/default_bindings.json), so each frame one context reads its own and the other sees nothing.
enum class InputContext : std::uint8_t {
    World, ///< free roam: walking, looking, Back, QuickCatalog (opens the catalog); the UI actions are not read
    Ui,    ///< a product UI screen has the input (the quick catalog, its game-session panel, the directory): the UI
           ///< actions, QuickCatalog (closes the catalog) and Map (closes the directory, I4 step 7b); the world's
           ///< actions are not read, so the walker stands still
};

/// "world", "ui".
[[nodiscard]] std::string_view toString(InputContext context) noexcept;

/// The context of a frame: Ui while the catalog (or the directory, I4 step 7b) or the game-session panel is open, World
/// otherwise.
[[nodiscard]] constexpr InputContext inputContextFor(bool catalogOpen, bool panelOpen) noexcept {
    return catalogOpen || panelOpen ? InputContext::Ui : InputContext::World;
}

/// ToggleDevUI, ReloadShaders, Screenshot and ToggleMute (I5 step 5): developer and host functions every context reads.
[[nodiscard]] constexpr bool isGlobalAction(input::Action action) noexcept {
    return action == input::Action::ToggleDevUI || action == input::Action::ReloadShaders ||
           action == input::Action::Screenshot || action == input::Action::ToggleMute;
}

/// True when `context` reads `action`: the global actions everywhere; in World every action but the UI ones; in Ui the
/// UI actions (input::isUiAction), QuickCatalog and Map.
[[nodiscard]] bool readsAction(InputContext context, input::Action action) noexcept;

/// `actions` as `context` reads them: every action it does not read is released (value 0, neither held, pressed nor
/// released), the active device kept. The walker under an open catalog stands still, the catalog never scrolls while
/// the player walks.
[[nodiscard]] input::ActionState actionsFor(InputContext context, const input::ActionState& actions) noexcept;

/// True when one of the UI actions was pressed this frame.
[[nodiscard]] bool anyUiActionPressed(const input::ActionState& actions) noexcept;

/// QuickCatalog pressed this frame (`actions` as resolved, before actionsFor): in World it opens the catalog; in Ui it
/// closes it, unless a UI action was pressed in the same frame: the default bindings give the pad's north button to
/// both QuickCatalog and UiMedia, so the button that opened the catalog is "Vedi media" inside it (B and Esc leave the
/// catalog through Back), while Tab, which no UI action uses, opens and closes it.
[[nodiscard]] bool catalogToggleRequested(InputContext context, const input::ActionState& actions) noexcept;

} // namespace rv::app
