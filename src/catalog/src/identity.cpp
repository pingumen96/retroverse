#include <rv/catalog/identity.hpp>

#include <string>
#include <tuple>
#include <unordered_set>

#include <rv/core/sha256.hpp>

namespace rv::catalog {

std::uint64_t gameIdCandidate(const LibraryId& library, const SystemName& system, std::string_view key,
                              std::uint32_t counter) {
    constexpr std::string_view kSeparator("\0", 1);
    core::Sha256 hash;
    hash.update(kGameIdTupleTag);
    hash.update(kSeparator);
    hash.update(library.str());
    hash.update(kSeparator);
    hash.update(system.str());
    hash.update(kSeparator);
    hash.update(key);
    if (counter != 0) {
        hash.update(kSeparator);
        hash.update(std::to_string(counter));
    }
    const core::Sha256Digest digest = hash.finish();
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | digest[i];
    }
    return value & GameId::kMaxValue;
}

std::optional<GameId> stableGameId(const LibraryId& library, const SystemName& system, std::string_view key,
                                   std::uint32_t counter) {
    return GameId::fromValue(gameIdCandidate(library, system, key, counter));
}

Expected<AssignedGameId> firstFreeGameId(const std::function<std::uint64_t(std::uint32_t)>& candidate,
                                         const GameIdTaken& isTaken, std::uint32_t maxCounter) {
    for (std::uint32_t counter = 0;; ++counter) {
        const auto id = GameId::fromValue(candidate(counter));
        if (id && !isTaken(*id)) {
            return AssignedGameId{*id, counter};
        }
        if (counter == maxCounter) {
            return makeError(ErrorCode::OutOfRange,
                             "no free GameId up to counter " + std::to_string(maxCounter) + ": every value is taken");
        }
    }
}

Expected<std::vector<AssignedGameId>> assignGameIds(const LibraryId& library, std::span<const GameIdRequest> requests,
                                                    const GameIdTaken& isTaken) {
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const GameIdRequest& request = requests[i];
        const std::string context = request.system.str() + "/" + request.key.relativePath();
        if (request.system.empty() || request.key.empty()) {
            return makeError(ErrorCode::InvalidArgument, "a GameId request needs a system and a key", context);
        }
        if (i == 0) {
            continue;
        }
        const GameIdRequest& previous = requests[i - 1];
        // (system, key, relative path) strictly increasing: in order, and no (system, relative path) twice (equal
        // relative paths have equal keys, so a repeated one is an equal neighbour in this order).
        if (!(std::tie(previous.system, previous.key) < std::tie(request.system, request.key))) {
            return makeError(ErrorCode::InvalidArgument,
                             previous.system == request.system && previous.key == request.key
                                 ? "a GameId request names the same file twice"
                                 : "GameId requests must come in (system, key, relative path) ordinal order",
                             context);
        }
    }

    std::vector<AssignedGameId> assigned;
    assigned.reserve(requests.size());
    std::unordered_set<GameId> given; // ids of this batch (looked up, never iterated)
    const GameIdTaken takenOrGiven = [&](GameId id) {
        return given.contains(id) || isTaken(id);
    };
    for (const GameIdRequest& request : requests) {
        auto id = firstFreeGameId(
            [&](std::uint32_t counter) { return gameIdCandidate(library, request.system, request.key.key(), counter); },
            takenOrGiven);
        if (!id) {
            return propagate(std::move(id.error()));
        }
        given.insert(id->id);
        assigned.push_back(*id);
    }
    return assigned;
}

} // namespace rv::catalog
