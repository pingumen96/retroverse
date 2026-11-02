#include <rv/app/statistics_data.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rv::app {
namespace {

/// A count as the document's integers hold it.
[[nodiscard]] int count(std::uint64_t value) noexcept {
    return static_cast<int>(std::min<std::uint64_t>(value, 2'000'000'000));
}

[[nodiscard]] std::string dayText(const std::optional<catalog::UtcTime>& time, const LocalDate& localDate) {
    if (!time || !localDate) {
        return {};
    }
    const std::optional<ui::DirectoryDate> day = localDate(*time);
    return day ? fmt::format("{:02}/{:02}/{:04}", day->day, day->month, day->year) : std::string();
}

[[nodiscard]] ui::StatisticsGame gameOf(const catalog::PlayedGame& game, const LocalDate& localDate) {
    ui::StatisticsGame shown;
    shown.title = game.title;
    shown.system = game.systemFullName.empty() ? game.system.str() : game.systemFullName;
    shown.hours = ui::estimatedHours(game.played.count());
    shown.sessions = count(game.sessions);
    shown.date = dayText(game.lastPlayed, localDate);
    shown.status = std::string(catalog::toString(game.status));
    return shown;
}

} // namespace

ui::StatisticsContent statisticsContentOf(const catalog::PlayStatistics& statistics, const LocalDate& localDate) {
    ui::StatisticsContent content;
    content.empty = statistics.empty();
    content.sessions = count(statistics.sessions);
    content.hours = ui::estimatedHours(statistics.played.count());
    content.gamesPlayed = count(statistics.gamesPlayed);
    content.games = count(statistics.games);
    content.crashed = count(statistics.crashed);
    content.leftRunning = count(statistics.leftRunning);
    for (const catalog::PlayedGame& game : statistics.mostPlayed) {
        content.mostPlayed.push_back(gameOf(game, localDate));
    }
    for (const catalog::PlayedGame& game : statistics.mostRecent) {
        content.recent.push_back(gameOf(game, localDate));
    }
    for (const catalog::SystemStatistics& system : statistics.systems) {
        content.systems.push_back(ui::StatisticsSystem{system.fullName.empty() ? system.system.str() : system.fullName,
                                                       ui::estimatedHours(system.played.count()), count(system.sessions),
                                                       count(system.attempts), count(system.games)});
    }
    for (const catalog::StatusStatistics& status : statistics.statuses) {
        content.statuses.push_back(ui::StatisticsStatus{std::string(catalog::toString(status.status)),
                                                        count(status.games), count(status.sessions),
                                                        ui::estimatedHours(status.played.count())});
    }
    return content;
}

} // namespace rv::app
