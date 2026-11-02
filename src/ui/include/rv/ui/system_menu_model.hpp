#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The menu (ADR-0018 §1; assets/ui/system_menu.rml through ui::SystemMenuScreen): Riprendi, Impostazioni, Emulatori, Esci,
// with one confirmation before Esci. It holds what is not about the building (the directory stays the building's panel).
// Each later step adds its section's button (Statistiche, Copertura media, Libreria): a section is a screen of its own,
// whose Back returns here.
//
// | View    | Up, Down           | Activate             | Back                    |
// |---------|--------------------|----------------------|-------------------------|
// | Main    | the focused button | its button           | closes                  |
// | Confirm | the focused button | Esci ends, Annulla   | back to Main, on Esci   |
//                                  back to Main

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace system_menu {
inline constexpr std::string_view kDocumentPath = "ui/system_menu.rml";
inline constexpr std::string_view kDataModel = "system_menu";
inline constexpr std::string_view kButtonEvent = "button";
inline constexpr std::string_view kPanelId = "system-menu";
} // namespace system_menu

enum class SystemMenuButton : std::uint8_t { Resume, Settings, Emulators, Quit, ConfirmQuit, Stay };
/// "resume", "settings", "emulators", "quit", "confirm-quit", "stay" (the button event's argument).
[[nodiscard]] std::string_view toString(SystemMenuButton button) noexcept;
[[nodiscard]] std::optional<SystemMenuButton> systemMenuButtonFromName(std::string_view name) noexcept;
/// The button's element id ("sm-resume" ...).
[[nodiscard]] std::string_view elementIdOf(SystemMenuButton button) noexcept;

enum class SystemMenuView : std::uint8_t { Main, Confirm };
/// "main", "confirm".
[[nodiscard]] std::string_view toString(SystemMenuView view) noexcept;

/// What the menu asks of the app.
enum class SystemMenuCommand : std::uint8_t {
    Close,        ///< it closed itself: back to the world
    OpenSettings, ///< the settings screen opens over it
    OpenEmulators, ///< the emulators screen opens over it
    Quit,         ///< Esci, confirmed
};
/// "close", "open-settings", "open-emulators", "quit".
[[nodiscard]] std::string_view toString(SystemMenuCommand command) noexcept;

/// The menu's state: open, its view, its focused button. Main thread; no rendering. Every change of what the panel shows
/// increments revision().
class SystemMenuModel {
public:
    /// Opens on Main, Riprendi focused.
    void open();
    void close() noexcept;

    /// A UI command. The command for the app, if any.
    [[nodiscard]] std::optional<SystemMenuCommand> apply(UiCommand command);
    /// A click on `button` (shown): it is focused and pressed.
    [[nodiscard]] std::optional<SystemMenuCommand> click(SystemMenuButton button);

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] SystemMenuView view() const noexcept { return m_view; }
    /// The buttons shown, in focus order.
    [[nodiscard]] std::vector<SystemMenuButton> buttons() const;
    [[nodiscard]] SystemMenuButton focused() const noexcept { return m_focused; }
    /// The focused button's element id.
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void focus(SystemMenuButton button) noexcept;
    void show(SystemMenuView view, SystemMenuButton button) noexcept;
    [[nodiscard]] std::optional<SystemMenuCommand> press(SystemMenuButton button);

    bool m_open = false;
    SystemMenuView m_view = SystemMenuView::Main;
    SystemMenuButton m_focused = SystemMenuButton::Resume;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
