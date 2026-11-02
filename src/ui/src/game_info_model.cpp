#include <rv/ui/game_info_model.hpp>

#include <algorithm>

namespace rv::ui {

std::string_view toString(InfoStep step) noexcept {
    switch (step) {
    case InfoStep::None:
        return "none";
    case InfoStep::ScrollUp:
        return "scroll-up";
    case InfoStep::ScrollDown:
        return "scroll-down";
    case InfoStep::PageUp:
        return "page-up";
    case InfoStep::PageDown:
        return "page-down";
    case InfoStep::Top:
        return "top";
    case InfoStep::Bottom:
        return "bottom";
    case InfoStep::Close:
        return "close";
    case InfoStep::Media:
        return "media";
    }
    return "unknown";
}

std::vector<InfoRow> infoRowsOf(const GameInfoContent& content) {
    std::vector<InfoRow> rows;
    if (content.controls) {
        rows.push_back(InfoRow{InfoRow::Kind::Status, 0});
        rows.push_back(InfoRow{InfoRow::Kind::Emulator, 0});
    }
    for (std::size_t index = 0; index < std::min(content.versions.size(), game_info::kMaxVersions); ++index) {
        rows.push_back(InfoRow{InfoRow::Kind::Version, index});
    }
    return rows;
}

InfoStep infoStepOf(UiCommand command) noexcept {
    switch (command) {
    case UiCommand::Up:
        return InfoStep::ScrollUp;
    case UiCommand::Down:
        return InfoStep::ScrollDown;
    case UiCommand::PageUp:
    case UiCommand::FilterPrevious:
        return InfoStep::PageUp;
    case UiCommand::PageDown:
    case UiCommand::FilterNext:
        return InfoStep::PageDown;
    case UiCommand::Home:
        return InfoStep::Top;
    case UiCommand::End:
        return InfoStep::Bottom;
    case UiCommand::Back:
        return InfoStep::Close;
    case UiCommand::Media:
        return InfoStep::Media;
    default:
        return InfoStep::None;
    }
}

float scrolledTop(float top, InfoStep step, float visible, float content, float line) noexcept {
    const float bottom = std::max(0.0f, content - visible);
    const float page = std::max(line, visible - line);
    float next = top;
    switch (step) {
    case InfoStep::ScrollUp:
        next = top - line;
        break;
    case InfoStep::ScrollDown:
        next = top + line;
        break;
    case InfoStep::PageUp:
        next = top - page;
        break;
    case InfoStep::PageDown:
        next = top + page;
        break;
    case InfoStep::Top:
        next = 0.0f;
        break;
    case InfoStep::Bottom:
        next = bottom;
        break;
    default:
        break;
    }
    return std::clamp(next, 0.0f, bottom);
}

} // namespace rv::ui
