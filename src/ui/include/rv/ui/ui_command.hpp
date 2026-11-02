#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace rv::ui {

/// A direction the focus moves in.
enum class NavDirection : std::uint8_t { Up, Down, Left, Right };

/// What the user asks of a product screen (specification §12.1, style §16.3: Avvia, Vedi media, Informazioni,
/// Indietro). Produced by NavInput from the UI actions of the action map (input::Action::UiUp ...), consumed by the
/// screen that has the focus.
enum class UiCommand : std::uint8_t {
    Up,             ///< move the focus up (D-pad, left stick, arrow key)
    Down,           ///< move the focus down
    Left,           ///< move the focus left
    Right,          ///< move the focus right
    Activate,       ///< press the focused element (A / Cross, Enter): "Avvia" on a game
    Back,           ///< leave one level (B / Circle, Esc): "Indietro"
    Media,          ///< secondary action "Vedi media" (Y / Triangle)
    Info,           ///< secondary action "Informazioni" (X / Square)
    PageUp,         ///< a page up in a list (left trigger, Page Up)
    PageDown,       ///< a page down in a list (right trigger, Page Down)
    Home,           ///< the first entry of a list (left stick click, Home)
    End,            ///< the last entry of a list (right stick click, End)
    FilterPrevious, ///< the previous filter (left shoulder)
    FilterNext,     ///< the next filter (right shoulder)
    Confirm,        ///< the text is done (Start, Enter): the on-screen keyboard's, the other screens ignore it
    Count
};

inline constexpr std::size_t kUiCommandCount = static_cast<std::size_t>(UiCommand::Count);

[[nodiscard]] constexpr std::size_t toIndex(UiCommand command) noexcept {
    return static_cast<std::size_t>(command);
}

/// Stable lowercase names, for logs and reports: "up", "down", "left", "right", "activate", "back", "media", "info",
/// "page_up", "page_down", "home", "end", "filter_previous", "filter_next", "confirm"; "unknown" for an out-of-range
/// value.
[[nodiscard]] std::string_view toString(UiCommand command) noexcept;
/// "up", "down", "left", "right"; "unknown" for an out-of-range value.
[[nodiscard]] std::string_view toString(NavDirection direction) noexcept;
/// Parses toString(UiCommand) exactly (case-sensitive).
[[nodiscard]] std::optional<UiCommand> uiCommandFromName(std::string_view name) noexcept;

/// The direction of a focus move command, nothing for the other commands.
[[nodiscard]] std::optional<NavDirection> directionOf(UiCommand command) noexcept;
/// The focus move command of a direction.
[[nodiscard]] UiCommand commandFor(NavDirection direction) noexcept;

/// The commands of one frame in the order they happened, with a fixed capacity so that collecting them never
/// allocates. A frame rarely holds more than two (a step and a button); commands beyond the capacity are dropped and
/// counted.
class UiCommandList {
public:
    static constexpr std::size_t kCapacity = 16;

    /// Appends `command`; false (and counted in dropped()) when the list is full.
    bool push(UiCommand command) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::span<const UiCommand> commands() const noexcept { return {m_commands.data(), m_size}; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    /// Commands refused by push() since the last clear().
    [[nodiscard]] std::size_t dropped() const noexcept { return m_dropped; }
    [[nodiscard]] bool contains(UiCommand command) const noexcept;

private:
    std::array<UiCommand, kCapacity> m_commands{};
    std::size_t m_size = 0;
    std::size_t m_dropped = 0;
};

} // namespace rv::ui
