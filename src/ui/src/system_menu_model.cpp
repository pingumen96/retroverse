#include <rv/ui/system_menu_model.hpp>

#include <algorithm>
#include <array>

namespace rv::ui {
namespace {

constexpr std::array<SystemMenuButton, 5> kButtons = {SystemMenuButton::Resume, SystemMenuButton::Settings,
                                                      SystemMenuButton::Quit, SystemMenuButton::ConfirmQuit,
                                                      SystemMenuButton::Stay};

} // namespace

std::string_view toString(SystemMenuButton button) noexcept {
    switch (button) {
    case SystemMenuButton::Resume:
        return "resume";
    case SystemMenuButton::Settings:
        return "settings";
    case SystemMenuButton::Quit:
        return "quit";
    case SystemMenuButton::ConfirmQuit:
        return "confirm-quit";
    case SystemMenuButton::Stay:
        return "stay";
    }
    return "unknown";
}

std::optional<SystemMenuButton> systemMenuButtonFromName(std::string_view name) noexcept {
    for (const SystemMenuButton button : kButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(SystemMenuButton button) noexcept {
    switch (button) {
    case SystemMenuButton::Resume:
        return "sm-resume";
    case SystemMenuButton::Settings:
        return "sm-settings";
    case SystemMenuButton::Quit:
        return "sm-quit";
    case SystemMenuButton::ConfirmQuit:
        return "sm-confirm-quit";
    case SystemMenuButton::Stay:
        return "sm-stay";
    }
    return {};
}

std::string_view toString(SystemMenuView view) noexcept {
    return view == SystemMenuView::Confirm ? "confirm" : "main";
}

std::string_view toString(SystemMenuCommand command) noexcept {
    switch (command) {
    case SystemMenuCommand::Close:
        return "close";
    case SystemMenuCommand::OpenSettings:
        return "open-settings";
    case SystemMenuCommand::Quit:
        return "quit";
    }
    return "unknown";
}

void SystemMenuModel::open() {
    m_open = true;
    show(SystemMenuView::Main, SystemMenuButton::Resume);
}

void SystemMenuModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

std::vector<SystemMenuButton> SystemMenuModel::buttons() const {
    if (m_view == SystemMenuView::Confirm) {
        return {SystemMenuButton::Stay, SystemMenuButton::ConfirmQuit};
    }
    return {SystemMenuButton::Resume, SystemMenuButton::Settings, SystemMenuButton::Quit};
}

std::string SystemMenuModel::focusedElement() const {
    return std::string(elementIdOf(m_focused));
}

void SystemMenuModel::focus(SystemMenuButton button) noexcept {
    if (m_focused != button) {
        m_focused = button;
        ++m_revision;
    }
}

void SystemMenuModel::show(SystemMenuView view, SystemMenuButton button) noexcept {
    m_view = view;
    m_focused = button;
    ++m_revision;
}

std::optional<SystemMenuCommand> SystemMenuModel::press(SystemMenuButton button) {
    switch (button) {
    case SystemMenuButton::Resume:
        close();
        return SystemMenuCommand::Close;
    case SystemMenuButton::Settings:
        return SystemMenuCommand::OpenSettings;
    case SystemMenuButton::Quit:
        // Annulla focused: an A pressed twice by mistake leaves nothing.
        show(SystemMenuView::Confirm, SystemMenuButton::Stay);
        return std::nullopt;
    case SystemMenuButton::ConfirmQuit:
        return SystemMenuCommand::Quit;
    case SystemMenuButton::Stay:
        show(SystemMenuView::Main, SystemMenuButton::Quit);
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<SystemMenuCommand> SystemMenuModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Up:
    case UiCommand::Down: {
        const std::vector<SystemMenuButton> shown = buttons();
        const auto at = std::find(shown.begin(), shown.end(), m_focused);
        if (at == shown.end()) {
            focus(shown.front());
        } else if (command == UiCommand::Up && at != shown.begin()) {
            focus(*(at - 1));
        } else if (command == UiCommand::Down && at + 1 != shown.end()) {
            focus(*(at + 1));
        }
        return std::nullopt;
    }
    case UiCommand::Activate:
        return press(m_focused);
    case UiCommand::Back:
        if (m_view == SystemMenuView::Confirm) {
            show(SystemMenuView::Main, SystemMenuButton::Quit);
            return std::nullopt;
        }
        close();
        return SystemMenuCommand::Close;
    default:
        return std::nullopt;
    }
}

std::optional<SystemMenuCommand> SystemMenuModel::click(SystemMenuButton button) {
    if (!m_open) {
        return std::nullopt;
    }
    const std::vector<SystemMenuButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), button) == shown.end()) {
        return std::nullopt;
    }
    focus(button);
    return press(button);
}

} // namespace rv::ui
