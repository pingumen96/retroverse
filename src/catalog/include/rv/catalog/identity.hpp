#pragma once

// GameId assignment (ADR-0008 §4). A key seen for the first time gets the GameId its tuple (library, system, key)
// derives with SHA-256; the store then keeps it, and it is never derived again for that identity: a move or a
// reconciliation keeps the id, and a key that comes back after its id went elsewhere gets a new one (the next counter).
// Independent oracle: scripts/game_id_oracle.py (Python's hashlib), whose values the unit tests hold.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// The first element of the hashed tuple, the version of the derivation below.
inline constexpr std::string_view kGameIdTupleTag = "rv-game/1";

/// The value a tuple gives: SHA-256 over the UTF-8 bytes of "rv-game/1", the library id, the ES-DE system name and the
/// lookup key, separated by NUL characters, followed for a counter n >= 1 by a fourth NUL and n in decimal ASCII
/// ("rv-game/1\0esde\0gb\0TINY TENNIS (EUROPE).GB\0" "2"); the first 8 bytes of the digest read big-endian, the top
/// bit cleared. It may be 0, which is no GameId.
[[nodiscard]] std::uint64_t gameIdCandidate(const LibraryId& library, const SystemName& system, std::string_view key,
                                            std::uint32_t counter = 0);

/// gameIdCandidate() as a GameId; nothing when it is 0 (which counts as a collision: the next counter is tried).
[[nodiscard]] std::optional<GameId> stableGameId(const LibraryId& library, const SystemName& system,
                                                 std::string_view key, std::uint32_t counter = 0);

/// A GameId given out, with the counter that gave it.
struct AssignedGameId {
    GameId id;
    /// 0: the tuple's own value. n >= 1: the values of counters 0 to n - 1 were taken (or 0): a collision with a stored
    /// identity or alias, or with an earlier key of the batch that folds to the same key.
    std::uint32_t counter = 0;

    bool operator==(const AssignedGameId&) const = default;
};

/// Whether a GameId is taken: held by a stored identity or alias (ICatalogStore::takenGameIds()).
using GameIdTaken = std::function<bool(GameId)>;

/// The last counter tried before giving up. Real collisions need counter 1 about once in 2^63 / (games in the store)
/// assignments; only names that fold to one key, one counter each, need more.
inline constexpr std::uint32_t kMaxGameIdCounter = 1'000'000;

/// The first value of `candidate(0)`, `candidate(1)`, ... (gameIdCandidate() for one tuple, or a test's stand-in) that
/// is neither 0 nor taken. Errors: OutOfRange when every counter up to `maxCounter` was 0 or taken.
[[nodiscard]] Expected<AssignedGameId> firstFreeGameId(const std::function<std::uint64_t(std::uint32_t)>& candidate,
                                                       const GameIdTaken& isTaken,
                                                       std::uint32_t maxCounter = kMaxGameIdCounter);

/// A key that needs a GameId: an entry of the library seen for the first time.
struct GameIdRequest {
    SystemName system;
    LookupKey key;

    bool operator==(const GameIdRequest&) const = default;
};

/// Assigns GameIds to the new keys of one import, deterministically. `requests` come in (system, key, relative path)
/// ordinal order, the order of the snapshot's entries; each gets firstFreeGameId() of its tuple (`library`, system,
/// key), where taken means `isTaken` or given to an earlier request of the batch. So two relative paths that fold to
/// one key (names differing in case only, in a case-sensitive folder or under an older casing table) get distinct ids:
/// the first in order its tuple's value, the next one counter 1 (the import reports it). The result holds one
/// AssignedGameId per request, in their order. Errors: InvalidArgument when the requests are out of order, name a
/// (system, relative path) twice or hold an empty system or key; OutOfRange as firstFreeGameId().
[[nodiscard]] Expected<std::vector<AssignedGameId>>
assignGameIds(const LibraryId& library, std::span<const GameIdRequest> requests, const GameIdTaken& isTaken);

} // namespace rv::catalog
