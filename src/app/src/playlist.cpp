#include <rv/app/playlist.hpp>

#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace rv::app {
namespace {

/// The games a playlist may play: a playable clip, not excluded, each id once, in the room's order.
std::vector<const ScreenGame*> candidates(std::span<const ScreenGame> games,
                                          const std::set<std::string, std::less<>>& excluded) {
    std::vector<const ScreenGame*> list;
    std::set<std::string_view> seen;
    for (const ScreenGame& game : games) {
        if (game.id.empty() || !isPlayableVideoFile(game.media.video) || excluded.contains(game.id) ||
            !seen.insert(game.id).second) {
            continue;
        }
        list.push_back(&game);
    }
    return list;
}

std::vector<std::string> idsOf(const std::vector<const ScreenGame*>& games) {
    std::vector<std::string> ids;
    ids.reserve(games.size());
    for (const ScreenGame* game : games) {
        ids.push_back(game->id);
    }
    return ids;
}

/// `ids` shuffled with the seed of (room, screen, mode, cycle); never starting with `previousLast` when it has another
/// game (that game swapped with the last).
std::vector<std::string> shuffled(std::vector<std::string> ids, const PlaylistOptions& options, PlaylistMode mode,
                                  std::uint64_t cycle, std::optional<std::string_view> previousLast) {
    core::Random random(core::deriveSeed(options.roomId, options.screenId, toString(mode), cycle));
    random.shuffle(std::span<std::string>(ids));
    if (previousLast && ids.size() > 1 && ids.front() == *previousLast) {
        std::swap(ids.front(), ids.back());
    }
    return ids;
}

std::vector<const ScreenGame*> favourites(const std::vector<const ScreenGame*>& games) {
    std::vector<const ScreenGame*> list;
    std::copy_if(games.begin(), games.end(), std::back_inserter(list), [](const ScreenGame* game) {
        return game->favorite;
    });
    return list;
}

std::vector<const ScreenGame*> recent(const std::vector<const ScreenGame*>& games) {
    std::vector<const ScreenGame*> list;
    std::copy_if(games.begin(), games.end(), std::back_inserter(list), [](const ScreenGame* game) {
        return game->lastPlayed.has_value();
    });
    std::stable_sort(list.begin(), list.end(), [](const ScreenGame* a, const ScreenGame* b) {
        return *a->lastPlayed > *b->lastPlayed;
    });
    if (list.size() > kRecentPlaylistGames) {
        list.resize(kRecentPlaylistGames);
    }
    return list;
}

struct ExplicitGames {
    std::vector<std::string> ids;
    std::vector<std::string> unknown;
    std::vector<std::string> unplayable;
};

ExplicitGames explicitGames(std::span<const ScreenGame> games, const std::vector<const ScreenGame*>& playable,
                            const PlaylistOptions& options) {
    ExplicitGames result;
    std::set<std::string_view> seen;
    for (const std::string& id : options.explicitGames) {
        if (!seen.insert(id).second) {
            continue; // listed twice: once in a cycle
        }
        const bool known =
            std::any_of(games.begin(), games.end(), [&id](const ScreenGame& game) { return game.id == id; });
        if (!known) {
            result.unknown.push_back(id);
            continue;
        }
        const bool canPlay =
            std::any_of(playable.begin(), playable.end(), [&id](const ScreenGame* game) { return game->id == id; });
        if (!canPlay) {
            result.unplayable.push_back(id);
            continue;
        }
        result.ids.push_back(id);
    }
    return result;
}

} // namespace

std::string_view toString(PlaylistMode mode) noexcept {
    switch (mode) {
    case PlaylistMode::Shuffle:
        return "shuffle";
    case PlaylistMode::Sequential:
        return "sequential";
    case PlaylistMode::Favorites:
        return "favorites";
    case PlaylistMode::Recent:
        return "recent";
    case PlaylistMode::Explicit:
        return "explicit";
    }
    return "unknown";
}

std::optional<PlaylistMode> parsePlaylistMode(std::string_view text) noexcept {
    for (const PlaylistMode mode : {PlaylistMode::Shuffle, PlaylistMode::Sequential, PlaylistMode::Favorites,
                                    PlaylistMode::Recent, PlaylistMode::Explicit}) {
        if (toString(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

PlaylistMode effectivePlaylistMode(std::span<const ScreenGame> games, const PlaylistOptions& options,
                                   const std::set<std::string, std::less<>>& excluded) {
    const std::vector<const ScreenGame*> playable = candidates(games, excluded);
    switch (options.mode) {
    case PlaylistMode::Shuffle:
    case PlaylistMode::Sequential:
        return options.mode;
    case PlaylistMode::Favorites:
        return favourites(playable).empty() ? PlaylistMode::Shuffle : options.mode;
    case PlaylistMode::Recent:
        return recent(playable).empty() ? PlaylistMode::Shuffle : options.mode;
    case PlaylistMode::Explicit:
        return explicitGames(games, playable, options).ids.empty() ? PlaylistMode::Shuffle : options.mode;
    }
    return PlaylistMode::Shuffle;
}

std::vector<std::string> playlistCycle(std::span<const ScreenGame> games, const PlaylistOptions& options,
                                       std::uint64_t cycle, std::optional<std::string_view> previousLast,
                                       const std::set<std::string, std::less<>>& excluded) {
    const std::vector<const ScreenGame*> playable = candidates(games, excluded);
    switch (effectivePlaylistMode(games, options, excluded)) {
    case PlaylistMode::Shuffle:
        return shuffled(idsOf(playable), options, PlaylistMode::Shuffle, cycle, previousLast);
    case PlaylistMode::Sequential: {
        std::vector<const ScreenGame*> sorted = playable;
        std::stable_sort(sorted.begin(), sorted.end(), [](const ScreenGame* a, const ScreenGame* b) {
            return a->sortKey != b->sortKey ? a->sortKey < b->sortKey : a->id < b->id;
        });
        return idsOf(sorted);
    }
    case PlaylistMode::Favorites:
        return shuffled(idsOf(favourites(playable)), options, PlaylistMode::Favorites, cycle, previousLast);
    case PlaylistMode::Recent:
        return idsOf(recent(playable));
    case PlaylistMode::Explicit:
        return explicitGames(games, playable, options).ids;
    }
    return {};
}

Playlist::Playlist(std::vector<ScreenGame> games, PlaylistOptions options, std::set<std::string, std::less<>> excluded)
    : m_games(std::move(games)), m_options(std::move(options)), m_excluded(std::move(excluded)) {
    m_mode = effectivePlaylistMode(m_games, m_options, m_excluded);
    if (m_options.mode == PlaylistMode::Explicit) {
        ExplicitGames listed = explicitGames(m_games, candidates(m_games, m_excluded), m_options);
        m_unknown = std::move(listed.unknown);
        m_unplayable = std::move(listed.unplayable);
    }
    startCycle(0, std::nullopt);
}

void Playlist::startCycle(std::uint64_t cycle, std::optional<std::string_view> previousLast) {
    m_cycle = cycle;
    m_index = 0;
    m_mode = effectivePlaylistMode(m_games, m_options, m_excluded);
    m_order = playlistCycle(m_games, m_options, cycle, previousLast, m_excluded);
}

std::optional<std::string> Playlist::next() {
    // One pass over the rest of this cycle and one over the next: a cycle of excluded games only gives nothing.
    for (int pass = 0; pass < 2; ++pass) {
        while (m_index < m_order.size()) {
            const std::string& id = m_order[m_index++];
            if (!m_excluded.contains(id)) {
                m_current = id;
                return m_current;
            }
        }
        const std::optional<std::string> last = m_current;
        startCycle(m_cycle + 1, last ? std::optional<std::string_view>(*last) : std::nullopt);
        if (m_order.empty()) {
            break;
        }
    }
    return std::nullopt;
}

void Playlist::exclude(std::string_view id) {
    m_excluded.emplace(id);
}

} // namespace rv::app
