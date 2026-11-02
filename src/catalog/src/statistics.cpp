#include <rv/catalog/statistics.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace rv::catalog {
namespace {

[[nodiscard]] PlayedGame playedGameOf(const PlayStatisticsRow& row, std::optional<UtcTime> lastPlayed) {
    const GameStatus status = gameStatusOf(row.stored, row.esdeCompleted, row.sessions > 0);
    return PlayedGame{row.id, row.system, row.systemFullName, row.title, row.sessions, row.played, lastPlayed, status};
}

} // namespace

PlayStatistics buildPlayStatistics(const std::vector<PlayStatisticsRow>& rows, const PlayStatisticsQuery& query) {
    PlayStatistics statistics;
    for (std::size_t index = 0; index < statistics.statuses.size(); ++index) {
        statistics.statuses[index].status = static_cast<GameStatus>(index);
    }
    std::map<std::string, SystemStatistics> systems; // by name: an order that depends on nothing else
    std::vector<RecentGame> recent;
    std::unordered_map<std::uint64_t, const PlayStatisticsRow*> byId; // looked up, never iterated
    std::vector<const PlayStatisticsRow*> played;
    for (const PlayStatisticsRow& row : rows) {
        statistics.sessions += row.sessions;
        statistics.crashed += row.crashed;
        statistics.leftRunning += row.leftRunning;
        statistics.played += row.played;
        if (row.sessions > 0) {
            ++statistics.gamesPlayed;
            played.push_back(&row);
        }
        if (row.sessions > 0 || row.crashed > 0 || row.leftRunning > 0) {
            SystemStatistics& system = systems[row.system.str()];
            system.system = row.system;
            if (system.fullName.empty()) {
                system.fullName = row.systemFullName;
            }
            system.sessions += row.sessions;
            system.attempts += row.crashed + row.leftRunning;
            system.played += row.played;
            system.games += row.sessions > 0 ? 1 : 0;
        }
        if (!row.listed) {
            continue;
        }
        ++statistics.games;
        const GameStatus status = gameStatusOf(row.stored, row.esdeCompleted, row.sessions > 0);
        StatusStatistics& counted = statistics.statuses[static_cast<std::size_t>(status)];
        ++counted.games;
        counted.sessions += row.sessions;
        counted.played += row.played;
        if (row.lastSession || row.esdeLastPlayed) {
            // recent_games.hpp's order with no favourite: the latest first, the sort key, the GameId.
            RecentGame game;
            game.id = row.id;
            game.system = row.system;
            game.systemFullName = row.systemFullName;
            game.title = row.title;
            game.sortKey = row.sortKey;
            game.lastSession = row.lastSession;
            game.esdeLastPlayed = row.esdeLastPlayed;
            recent.push_back(std::move(game));
            byId.emplace(row.id.value(), &row);
        }
    }
    rankRecentGames(recent, query.localToUtc, query.limit);
    statistics.mostRecent.reserve(recent.size());
    for (const RecentGame& game : recent) {
        statistics.mostRecent.push_back(playedGameOf(*byId.at(game.id.value()), game.lastPlayed));
    }
    for (auto& [name, system] : systems) {
        statistics.systems.push_back(std::move(system));
    }
    std::stable_sort(statistics.systems.begin(), statistics.systems.end(),
                     [](const SystemStatistics& a, const SystemStatistics& b) {
                         return std::tuple(b.played, b.sessions) < std::tuple(a.played, a.sessions);
                     });
    const std::size_t kept = std::min(played.size(), query.limit);
    std::partial_sort(played.begin(), played.begin() + static_cast<std::ptrdiff_t>(kept), played.end(),
                      [](const PlayStatisticsRow* a, const PlayStatisticsRow* b) {
                          if (a->played != b->played) {
                              return a->played > b->played;
                          }
                          if (a->sessions != b->sessions) {
                              return a->sessions > b->sessions;
                          }
                          if (a->sortKey != b->sortKey) {
                              return a->sortKey < b->sortKey;
                          }
                          return a->id.value() < b->id.value();
                      });
    played.resize(kept);
    statistics.mostPlayed.reserve(kept);
    for (const PlayStatisticsRow* row : played) {
        statistics.mostPlayed.push_back(playedGameOf(*row, row->lastSession));
    }
    return statistics;
}

} // namespace rv::catalog
