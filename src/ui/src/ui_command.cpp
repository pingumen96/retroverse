#include <rv/ui/ui_command.hpp>

#include <algorithm>

namespace rv::ui {
namespace {

constexpr std::array<std::string_view, kUiCommandCount> kCommandNames{"up",     "down", "left",  "right",
                                                                     "activate", "back", "media", "info"};

} // namespace

std::string_view toString(UiCommand command) noexcept {
    const std::size_t index = toIndex(command);
    return index < kCommandNames.size() ? kCommandNames[index] : std::string_view("unknown");
}

std::string_view toString(NavDirection direction) noexcept {
    switch (direction) {
    case NavDirection::Up:
        return "up";
    case NavDirection::Down:
        return "down";
    case NavDirection::Left:
        return "left";
    case NavDirection::Right:
        return "right";
    }
    return "unknown";
}

std::optional<UiCommand> uiCommandFromName(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kCommandNames.size(); ++index) {
        if (kCommandNames[index] == name) {
            return static_cast<UiCommand>(index);
        }
    }
    return std::nullopt;
}

std::optional<NavDirection> directionOf(UiCommand command) noexcept {
    switch (command) {
    case UiCommand::Up:
        return NavDirection::Up;
    case UiCommand::Down:
        return NavDirection::Down;
    case UiCommand::Left:
        return NavDirection::Left;
    case UiCommand::Right:
        return NavDirection::Right;
    default:
        return std::nullopt;
    }
}

UiCommand commandFor(NavDirection direction) noexcept {
    switch (direction) {
    case NavDirection::Up:
        return UiCommand::Up;
    case NavDirection::Down:
        return UiCommand::Down;
    case NavDirection::Left:
        return UiCommand::Left;
    case NavDirection::Right:
        return UiCommand::Right;
    }
    return UiCommand::Up;
}

bool UiCommandList::push(UiCommand command) noexcept {
    if (m_size == m_commands.size()) {
        ++m_dropped;
        return false;
    }
    m_commands[m_size++] = command;
    return true;
}

void UiCommandList::clear() noexcept {
    m_size = 0;
    m_dropped = 0;
}

bool UiCommandList::contains(UiCommand command) const noexcept {
    const std::span<const UiCommand> list = commands();
    return std::find(list.begin(), list.end(), command) != list.end();
}

} // namespace rv::ui
