#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// A screen's playlist (I5 step 1, ADR-0016 §3): the order in which a screen plays its room's games when nothing is
// selected, cycle after cycle. Only games with a playable clip (isPlayableVideoFile) and not excluded (a clip that
// failed is skipped for the rest of the run) enter it. Its order is deterministic: the shuffled modes draw from
// core::Random seeded by core::deriveSeed(room id, screen id, mode, cycle), never from the clock.
//
//  - shuffle (RoomShuffle, the default): a permutation of the games; no game repeats until all have played; a new
//    cycle reshuffles, and never starts with the previous cycle's last game (that game swapped with the cycle's last);
//  - sequential: the room's sort order (each game's sort key, then its id);
//  - favorites: the room's favourites in shuffle order (their own seed); shuffle when there are none;
//  - recent: the kRecentPlaylistGames games played most recently, the newest first (equal times keep the room's
//    order); shuffle when none was played;
//  - explicit: the GameIds listed for the room and screen (video.json), in their order, each once; an id the room does
//    not have, or whose game has no playable clip, is skipped (unknownIds(), unplayableIds()); shuffle when none is
//    left.
//
// The cycles of sequential, recent and explicit repeat one order.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/screen_game.hpp>

namespace rv::app {

enum class PlaylistMode : std::uint8_t {
    Shuffle,
    Sequential,
    Favorites,
    Recent,
    Explicit,
};

/// "shuffle", "sequential", "favorites", "recent", "explicit" (video.json's names).
[[nodiscard]] std::string_view toString(PlaylistMode mode) noexcept;
[[nodiscard]] std::optional<PlaylistMode> parsePlaylistMode(std::string_view text) noexcept;

/// The games of the recent mode, at most.
inline constexpr std::size_t kRecentPlaylistGames = 10;
/// The hero screen's id in the playlist's seeds (one hero screen per room; secondary TVs get ids of their own).
inline constexpr std::string_view kHeroScreenId = "hero";

struct PlaylistOptions {
    PlaylistMode mode = PlaylistMode::Shuffle;
    /// The room's id ("room:snes"; the test room's scene name) and the screen's (kHeroScreenId): the seeds' parts.
    std::string roomId;
    std::string screenId = std::string(kHeroScreenId);
    /// The explicit mode's GameIds, in order.
    std::vector<std::string> explicitGames;

    bool operator==(const PlaylistOptions&) const = default;
};

/// The order of cycle `cycle` of `games` (in the room's order: the featured one) under `options`, without the games
/// of `excluded`; `previousLast`: the game the previous cycle ended with (shuffle and favorites never start with it).
[[nodiscard]] std::vector<std::string> playlistCycle(std::span<const ScreenGame> games, const PlaylistOptions& options,
                                                     std::uint64_t cycle,
                                                     std::optional<std::string_view> previousLast = std::nullopt,
                                                     const std::set<std::string, std::less<>>& excluded = {});

/// The mode a playlist plays in: `options.mode`, or shuffle when its games are none (favorites, recent, explicit).
[[nodiscard]] PlaylistMode effectivePlaylistMode(std::span<const ScreenGame> games, const PlaylistOptions& options,
                                                 const std::set<std::string, std::less<>>& excluded = {});

/// A playlist being played: its cycles one after the other (see the file comment).
class Playlist {
public:
    Playlist() = default;
    Playlist(std::vector<ScreenGame> games, PlaylistOptions options, std::set<std::string, std::less<>> excluded = {});

    [[nodiscard]] const PlaylistOptions& options() const noexcept { return m_options; }
    /// The mode it plays in (effectivePlaylistMode()).
    [[nodiscard]] PlaylistMode mode() const noexcept { return m_mode; }
    /// No game to play.
    [[nodiscard]] bool empty() const noexcept { return m_order.empty(); }
    /// The games of a cycle (the current one's, those excluded since it began included).
    [[nodiscard]] std::size_t size() const noexcept { return m_order.size(); }
    /// The current cycle's order, and its number (0 first).
    [[nodiscard]] const std::vector<std::string>& order() const noexcept { return m_order; }
    [[nodiscard]] std::uint64_t cycle() const noexcept { return m_cycle; }

    /// The next game (the next cycle's first after the last), skipping the excluded ones; nothing when none is left.
    std::optional<std::string> next();
    /// The game next() returned last (nothing before the first).
    [[nodiscard]] const std::optional<std::string>& current() const noexcept { return m_current; }

    /// `id`'s clip failed: skipped for the rest of the run (the cycle going on, and the cycles after it).
    void exclude(std::string_view id);
    [[nodiscard]] const std::set<std::string, std::less<>>& excluded() const noexcept { return m_excluded; }

    /// The explicit mode's ids the room does not have, and those whose game has no playable clip, in their order.
    [[nodiscard]] const std::vector<std::string>& unknownIds() const noexcept { return m_unknown; }
    [[nodiscard]] const std::vector<std::string>& unplayableIds() const noexcept { return m_unplayable; }

private:
    void startCycle(std::uint64_t cycle, std::optional<std::string_view> previousLast);

    std::vector<ScreenGame> m_games;
    PlaylistOptions m_options;
    PlaylistMode m_mode = PlaylistMode::Shuffle;
    std::set<std::string, std::less<>> m_excluded;
    std::vector<std::string> m_order;
    std::uint64_t m_cycle = 0;
    std::size_t m_index = 0;
    std::optional<std::string> m_current;
    std::vector<std::string> m_unknown;
    std::vector<std::string> m_unplayable;
};

} // namespace rv::app
