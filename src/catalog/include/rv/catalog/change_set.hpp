#pragma once

// What an import changed (ADR-0008 §4): games added and removed, metadata changed, files moved with the reason their
// identity followed them, availability changes, the pairings left for the user to review and the issues met, and per
// system what the import did with it (SystemChanges: its index rows added, changed, removed, unchanged, kept as source
// unavailable or kept as they were, its identities' changes, the mass-disappearance guard's figures). Reported by the
// import (ImportService, I2 step 11a), shown by `retroverse-cli import` and kept for the user (the store's import
// records: ICatalogStore, ImportRecord).

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

/// What an import did with one system (ADR-0008 §4, §7).
enum class SystemOutcome : std::uint8_t {
    /// The snapshot's view of the system is in the index: entries added, changed and removed; the rows under a folder
    /// below the system folder that could not be listed kept, marked source unavailable.
    Applied,
    /// Its ROM folder is missing or could not be listed: its rows kept, marked source unavailable with the reason, and
    /// the entries its gamelist alone names added; nothing removed, no identity taken as vanished (SC-06).
    SourceUnavailable,
    /// Its gamelist exists but could not be read, even after a retry: its previous rows kept as they were (ADR-0008 §7).
    GamelistFailed,
    /// Its gamelist could not be read and the index held no row of it: nothing imported (rather than unscraped) until
    /// an import reads the gamelist; its system row says "failed".
    MetadataPending,
    /// A mass disappearance: more than half of the files present at the last import are gone. Nothing changed for the
    /// system until an import is told to accept the removals (ImportOptions::acceptRemovals).
    KeptByGuard,
    /// The configuration no longer defines it: its rows and its system row removed (its identities stay). Under the
    /// guard while files of it were present.
    NotConfigured,
};

/// "applied", "source-unavailable", "gamelist-failed", "metadata-pending", "kept-by-guard", "not-configured".
[[nodiscard]] constexpr std::string_view toString(SystemOutcome outcome) noexcept {
    switch (outcome) {
    case SystemOutcome::Applied:
        return "applied";
    case SystemOutcome::SourceUnavailable:
        return "source-unavailable";
    case SystemOutcome::GamelistFailed:
        return "gamelist-failed";
    case SystemOutcome::MetadataPending:
        return "metadata-pending";
    case SystemOutcome::KeptByGuard:
        return "kept-by-guard";
    case SystemOutcome::NotConfigured:
        return "not-configured";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SystemOutcome> parseSystemOutcome(std::string_view text) noexcept {
    return detail::enumFromString(text, SystemOutcome::NotConfigured);
}

/// What one import did with one system. The rows the index holds for the system after the import are partitioned into
/// added, changed, unchanged, unavailable and kept; `removed` counts those that left it.
struct SystemChanges {
    SystemName system;
    SystemOutcome outcome = SystemOutcome::Applied;
    /// Index rows written for entries the index did not hold.
    std::size_t added = 0;
    /// Index rows written again: the entry, its media or its sort key differ from the row (a changed availability
    /// included).
    std::size_t changed = 0;
    /// Index rows removed: their entry is no longer in the snapshot (their identities stay).
    std::size_t removed = 0;
    /// Index rows equal to the snapshot's entry: not written.
    std::size_t unchanged = 0;
    /// Index rows under a folder that could not be listed (the system folder, or one below it): the rows the index
    /// held kept as they were, marked source unavailable with the reason, and the entries the gamelist alone names.
    std::size_t unavailable = 0;
    /// Index rows kept as they were because the system was not applied (GamelistFailed, KeptByGuard).
    std::size_t kept = 0;
    /// GameIds given out to keys seen for the first time.
    std::size_t newIdentities = 0;
    /// Identities whose file was present at the last import and is gone now, from a folder that was listed (gamelist
    /// entry or not): reconciliation candidates (ADR-0008 §4), which keep their rows.
    std::size_t vanished = 0;
    /// Identities whose file is there again (not present at the last import).
    std::size_t returned = 0;
    /// Identities whose stored key was of another key rule or casing table: computed again from the relative path.
    std::size_t rekeyed = 0;
    /// Identities that followed their file to a new path (the pairing pass, IIdentityPairing).
    std::size_t moved = 0;
    /// The guard's figures: the identities present at the last import outside the folders that cannot be listed now,
    /// and those of them whose file is gone (and was not paired with a new file).
    std::size_t presentBefore = 0;
    std::size_t gone = 0;
    /// The issues that name the system, the snapshot's and the import's.
    std::size_t issues = 0;

    /// The rows the index holds for the system after the import.
    [[nodiscard]] std::size_t rows() const noexcept { return added + changed + unchanged + unavailable + kept; }

    bool operator==(const SystemChanges&) const = default;
};

/// What one import changed.
struct ChangeSet {
    /// Entries newly in the index (a key seen for the first time, or an identity's entry back in the index).
    std::vector<ChangedGame> added;
    /// Entries that left the index (their identities stay, with their last path and history).
    std::vector<ChangedGame> removed;
    /// Entries whose index row changed: metadata, state, file facts, media or sort key.
    std::vector<ChangedGame> metadataChanged;
    std::vector<MovedGame> moved;
    /// Entries whose availability changed (among those of metadataChanged, or added and removed).
    std::vector<AvailabilityChange> availabilityChanged;
    std::vector<ReviewRow> reviews;
    /// The import's issues: the snapshot's (ILibrarySource), then those of the import itself.
    std::vector<Issue> issues;
    /// Per system, by name: every system the import looked at (the snapshot's and the store's).
    std::vector<SystemChanges> systems;
    /// The library's index held no row before the import.
    bool firstImport = false;

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
    /// The systems' figures summed (`system` empty; `outcome` Applied).
    [[nodiscard]] SystemChanges total() const noexcept {
        SystemChanges sum;
        for (const SystemChanges& system : systems) {
            sum.added += system.added;
            sum.changed += system.changed;
            sum.removed += system.removed;
            sum.unchanged += system.unchanged;
            sum.unavailable += system.unavailable;
            sum.kept += system.kept;
            sum.newIdentities += system.newIdentities;
            sum.vanished += system.vanished;
            sum.returned += system.returned;
            sum.rekeyed += system.rekeyed;
            sum.moved += system.moved;
            sum.presentBefore += system.presentBefore;
            sum.gone += system.gone;
            sum.issues += system.issues;
        }
        return sum;
    }
    /// The figures of `system`, or nullptr.
    [[nodiscard]] const SystemChanges* findSystem(const SystemName& system) const noexcept {
        for (const SystemChanges& changes : systems) {
            if (changes.system == system) {
                return &changes;
            }
        }
        return nullptr;
    }

    bool operator==(const ChangeSet&) const = default;
};

} // namespace rv::catalog
