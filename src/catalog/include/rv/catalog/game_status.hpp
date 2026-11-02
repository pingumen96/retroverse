#pragma once

// A game's status (ADR-0018 §9): Da iniziare, In corso, Completato, Abbandonato in the product's words. What Retroverse's
// user set is a row of rv_game_status (SessionStore::setGameStatus); a game with no row derives its status, so that nobody
// must tag a library: ES-DE's <completed> gives Completed, a Returned session of Retroverse's (launch tests left out)
// InProgress, else NotStarted. ES-DE's <completed> is read, never written.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <rv/catalog/ids.hpp>

namespace rv::catalog {

enum class GameStatus : std::uint8_t { NotStarted, InProgress, Completed, Abandoned };

/// "not-started", "in-progress", "completed", "abandoned": what rv_game_status holds and the documents' keys.
[[nodiscard]] constexpr std::string_view toString(GameStatus status) noexcept {
    switch (status) {
    case GameStatus::NotStarted:
        return "not-started";
    case GameStatus::InProgress:
        return "in-progress";
    case GameStatus::Completed:
        return "completed";
    case GameStatus::Abandoned:
        return "abandoned";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<GameStatus> parseGameStatus(std::string_view text) noexcept {
    return detail::enumFromString(text, GameStatus::Abandoned);
}

/// How many statuses there are.
inline constexpr std::size_t kGameStatuses = 4;

/// The status of a game with no row.
[[nodiscard]] constexpr GameStatus derivedGameStatus(bool esdeCompleted, bool returnedSession) noexcept {
    return esdeCompleted ? GameStatus::Completed : returnedSession ? GameStatus::InProgress : GameStatus::NotStarted;
}

/// The status shown: what was set, else what it derives.
[[nodiscard]] constexpr GameStatus gameStatusOf(std::optional<GameStatus> stored, bool esdeCompleted,
                                                bool returnedSession) noexcept {
    return stored ? *stored : derivedGameStatus(esdeCompleted, returnedSession);
}

/// The status a step of Informazioni's row reaches, wrapping; `forward` false goes back.
[[nodiscard]] constexpr GameStatus steppedGameStatus(GameStatus status, bool forward = true) noexcept {
    const auto value = static_cast<std::size_t>(status);
    return static_cast<GameStatus>((value + (forward ? 1 : kGameStatuses - 1)) % kGameStatuses);
}

/// A game's status as Informazioni shows it: `stored`, it was set, else it derives.
struct GameStatusState {
    GameStatus status = GameStatus::NotStarted;
    bool stored = false;

    bool operator==(const GameStatusState&) const = default;
};

} // namespace rv::catalog
