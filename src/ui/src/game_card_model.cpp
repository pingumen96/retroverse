#include <rv/ui/game_card_model.hpp>

namespace rv::ui {

std::string_view toString(CardButton button) noexcept {
    switch (button) {
    case CardButton::Play:
        return "play";
    case CardButton::Media:
        return "media";
    case CardButton::Info:
        return "info";
    case CardButton::Back:
        return "back";
    }
    return "unknown";
}

std::optional<CardButton> cardButtonFromName(std::string_view name) noexcept {
    for (const CardButton button : kCardButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(CardButton button) noexcept {
    switch (button) {
    case CardButton::Play:
        return game_card::kPlayId;
    case CardButton::Media:
        return game_card::kMediaId;
    case CardButton::Info:
        return game_card::kInfoId;
    case CardButton::Back:
        return game_card::kBackId;
    }
    return game_card::kPlayId;
}

CardCoverBox cardCoverBox(std::uint32_t shapeWidth, std::uint32_t shapeHeight) noexcept {
    CardCoverBox box;
    // The shape against the frame's, cross-multiplied: exact for the frame's own shape.
    const std::uint64_t across = static_cast<std::uint64_t>(shapeWidth) * game_card::kCoverFrameHeight;
    const std::uint64_t down = static_cast<std::uint64_t>(shapeHeight) * game_card::kCoverFrameWidth;
    if (shapeWidth == 0 || shapeHeight == 0 || across == down) {
        return box;
    }
    if (across < down) {
        // Narrower than the frame: its whole height, bands of the frame left and right.
        box.width = static_cast<double>(across) / static_cast<double>(down);
        box.left = (1.0 - box.width) / 2.0;
    } else {
        // Wider: its whole width, bands above and below.
        box.height = static_cast<double>(down) / static_cast<double>(across);
        box.top = (1.0 - box.height) / 2.0;
    }
    return box;
}

CardButton stepFocus(CardButton from, NavDirection direction) noexcept {
    // The grid, row-major: Play (0, 0), Media (1, 0), Info (0, 1), Back (1, 1).
    const auto index = static_cast<int>(from);
    int column = index % 2;
    int row = index / 2;
    switch (direction) {
    case NavDirection::Left:
        column = 0;
        break;
    case NavDirection::Right:
        column = 1;
        break;
    case NavDirection::Up:
        row = 0;
        break;
    case NavDirection::Down:
        row = 1;
        break;
    }
    return kCardButtons[static_cast<std::size_t>(row * 2 + column)];
}

void GameCardModel::show(std::string_view gameId) {
    if (!m_shown || gameId != m_gameId) {
        m_focused = CardButton::Play;
    }
    m_shown = true;
    m_gameId = std::string(gameId);
}

void GameCardModel::hide() noexcept {
    m_shown = false;
    m_focused = CardButton::Play;
}

std::optional<CardButton> GameCardModel::apply(UiCommand command) noexcept {
    if (const std::optional<NavDirection> direction = directionOf(command)) {
        m_focused = stepFocus(m_focused, *direction);
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Activate:
        return m_focused;
    case UiCommand::Back:
        return CardButton::Back;
    case UiCommand::Media:
        return CardButton::Media;
    case UiCommand::Info:
        return CardButton::Info;
    default:
        return std::nullopt;
    }
}

CardButton GameCardModel::click(CardButton button) noexcept {
    m_focused = button;
    return button;
}

} // namespace rv::ui
