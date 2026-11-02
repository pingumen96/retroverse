#include <rv/ui/statistics_model.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {

std::string_view toString(StatisticsState state) noexcept {
    switch (state) {
    case StatisticsState::Loading:
        return "loading";
    case StatisticsState::Listed:
        return "listed";
    case StatisticsState::Empty:
        return "empty";
    case StatisticsState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view toString(StatisticsTab tab) noexcept {
    switch (tab) {
    case StatisticsTab::Summary:
        return "summary";
    case StatisticsTab::MostPlayed:
        return "most-played";
    case StatisticsTab::Recent:
        return "recent";
    case StatisticsTab::Systems:
        return "systems";
    case StatisticsTab::Statuses:
        return "statuses";
    }
    return "unknown";
}

std::optional<StatisticsTab> statisticsTabFromName(std::string_view name) noexcept {
    for (const StatisticsTab tab : kStatisticsTabs) {
        if (toString(tab) == name) {
            return tab;
        }
    }
    return std::nullopt;
}

void StatisticsModel::open() {
    m_open = true;
    m_state = StatisticsState::Loading;
    m_tab = StatisticsTab::Summary;
    m_request = StatisticsCommand::Load;
    ++m_revision;
}

void StatisticsModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

std::optional<StatisticsCommand> StatisticsModel::takeRequest() noexcept {
    return std::exchange(m_request, std::nullopt);
}

void StatisticsModel::step(int delta) {
    const int last = static_cast<int>(kStatisticsTabs.size()) - 1;
    const int next = std::clamp(static_cast<int>(m_tab) + delta, 0, last);
    if (next != static_cast<int>(m_tab)) {
        m_tab = static_cast<StatisticsTab>(next);
        ++m_revision;
    }
}

std::optional<StatisticsCommand> StatisticsModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Left:
    case UiCommand::FilterPrevious:
        step(-1);
        return std::nullopt;
    case UiCommand::Right:
    case UiCommand::FilterNext:
        step(1);
        return std::nullopt;
    case UiCommand::Back:
        close();
        return StatisticsCommand::Close;
    default:
        return std::nullopt;
    }
}

void StatisticsModel::click(StatisticsTab tab) {
    if (m_open && m_tab != tab) {
        m_tab = tab;
        ++m_revision;
    }
}

std::optional<StatisticsCommand> StatisticsModel::clickClose() {
    return apply(UiCommand::Back);
}

void StatisticsModel::setContent(StatisticsContent content) {
    m_state = content.empty ? StatisticsState::Empty : StatisticsState::Listed;
    m_content = std::move(content);
    ++m_revision;
}

void StatisticsModel::setUnavailable() {
    m_state = StatisticsState::Unavailable;
    m_content = {};
    ++m_revision;
}

std::string estimatedHours(std::int64_t milliseconds) {
    if (milliseconds <= 0) {
        return "0";
    }
    constexpr std::int64_t kTenthOfHour = 360'000;
    // Rounded to the nearest tenth, in integers: a sum of milliseconds, never a float's.
    const std::int64_t tenths = std::max<std::int64_t>((milliseconds + kTenthOfHour / 2) / kTenthOfHour, 1);
    return fmt::format("{},{}", tenths / 10, tenths % 10);
}

} // namespace rv::ui
