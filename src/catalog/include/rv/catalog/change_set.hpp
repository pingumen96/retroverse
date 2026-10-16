#pragma once

// What an import changed (ADR-0008 §4): games added and removed, metadata changed, files moved with the reason their
// identity followed them, availability changes, the pairings left for the user to review and the issues met. Reported
// by the import (step 11), shown by `retroverse-cli import` and kept for the user.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/snapshot.hpp>

namespace rv::catalog {

/// Why an identity followed its file to a new path (ADR-0008 §4: never a silent merge).
enum class MoveReason : std::uint8_t {
    QuickHash,       ///< the new file's quick hash equals the one stored for the vanished file
    SizeTimeName,    ///< no hash was stored: the only new file of the system with equal size, time and file name
    IdenticalCopies, ///< a group of identical copies moved together, paired in key order
    MovedBack,       ///< the file came back onto a key that had another identity meanwhile: that one became an alias
    ReviewResolved,  ///< the user resolved a review row
};

/// "quick-hash", "size-time-name", "identical-copies", "moved-back", "review-resolved".
[[nodiscard]] constexpr std::string_view toString(MoveReason reason) noexcept {
    switch (reason) {
    case MoveReason::QuickHash:
        return "quick-hash";
    case MoveReason::SizeTimeName:
        return "size-time-name";
    case MoveReason::IdenticalCopies:
        return "identical-copies";
    case MoveReason::MovedBack:
        return "moved-back";
    case MoveReason::ReviewResolved:
        return "review-resolved";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<MoveReason> parseMoveReason(std::string_view text) noexcept {
    return detail::enumFromString(text, MoveReason::ReviewResolved);
}

/// A game of a change set.
struct ChangedGame {
    GameId id;
    SystemName system;
    LookupKey key;

    bool operator==(const ChangedGame&) const = default;
};

/// An identity that followed its file.
struct MovedGame {
    GameId id;
    SystemName system;
    LookupKey from;
    LookupKey to;
    MoveReason reason = MoveReason::QuickHash;

    bool operator==(const MovedGame&) const = default;
};

/// A game whose availability changed (a file gone or back, a folder unreachable or reachable again).
struct AvailabilityChange {
    GameId id;
    SystemName system;
    LookupKey key;
    Availability before = Availability::Available;
    Availability after = Availability::Available;

    bool operator==(const AvailabilityChange&) const = default;
};

/// A vanished identity that could not be paired safely with a new file: the user decides (ADR-0008 §4).
struct ReviewRow {
    /// The store's id (AUTOINCREMENT, shown to the user); 0 until stored.
    std::int64_t id = 0;
    /// The vanished identity and its last path.
    GameId game;
    SystemName system;
    LookupKey vanished;
    /// The new files it could be, in key order.
    std::vector<LookupKey> candidates;
    /// Why no pairing was safe ("two new files of equal size, time and name").
    std::string reason;

    bool operator==(const ReviewRow&) const = default;
};

/// The sizes of a change set's lists.
struct ChangeSetCounts {
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t metadataChanged = 0;
    std::size_t moved = 0;
    std::size_t availabilityChanged = 0;
    std::size_t reviews = 0;
    std::size_t issues = 0;

    bool operator==(const ChangeSetCounts&) const = default;
};

/// What one import changed.
struct ChangeSet {
    /// New identities with their entries in the index.
    std::vector<ChangedGame> added;
    /// Entries that left the index (their identities stay, with their last path and history).
    std::vector<ChangedGame> removed;
    std::vector<ChangedGame> metadataChanged;
    std::vector<MovedGame> moved;
    std::vector<AvailabilityChange> availabilityChanged;
    std::vector<ReviewRow> reviews;
    std::vector<Issue> issues;

    [[nodiscard]] ChangeSetCounts counts() const noexcept {
        return ChangeSetCounts{
            added.size(),   removed.size(), metadataChanged.size(), moved.size(), availabilityChanged.size(),
            reviews.size(), issues.size()};
    }
    /// Nothing changed and nothing waits for review (issues aside).
    [[nodiscard]] bool empty() const noexcept {
        return added.empty() && removed.empty() && metadataChanged.empty() && moved.empty() &&
               availabilityChanged.empty() && reviews.empty();
    }

    bool operator==(const ChangeSet&) const = default;
};

} // namespace rv::catalog
