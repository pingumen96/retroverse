#pragma once

// Reconciliation of moved and renamed files (ADR-0008 §4; I2 step 11b), over the ports alone:
//  - IdentityPairing, the import's pairing pass (IIdentityPairing, import_service.hpp): within one library and system
//  it
//    pairs the identities whose file vanished with the new files, by the rules below, and returns review rows for what
//    no rule pairs safely; it reads the quick hash of the new files it needs, never of others.
//  - runQuickHashPass(), the progressive quick-hash pass: the present identities whose file has no current quick hash
//    (fingerprint.hpp) are hashed in a fixed order and their hashes committed in batches, under the import lock, so
//    that a later import can pair them by hash once their file moves. Resumable (what is committed stays, and the next
//    run starts with what is left), cancellable, bounded (a limit of files); a file that cannot be read is an issue.
//  - resolveReview(), a review row resolved by the user: paired with an identity (the move applied as the import
//    applies a pairing, the other identity recorded as its alias), or kept separate.
//
// The pairing rules, as implemented (per system; vanished identities in the stored order, key then path; new files in
// entry order, which within a system is key order):
//  1. The quick hash: a vanished identity whose stored quick hash is current (isCurrent() against its last file facts:
//     a hash that is stale says nothing of the content that vanished) pairs with a new file of the same digest and
//     size. Only the new files whose size equals one of those hashes' sizes are read (folders never), each once; a
//     cancel flag set meanwhile ends the pass (Cancelled). The identities and files of one digest form a group, paired
//     by ties: the score of a pair is the number of trailing path elements their keys share (1 or more: the same file
//     name; more: the same folders above it too), and a pair whose identity's best file and whose file's best identity
//     are each other alone is paired (QuickHash), repeatedly; what is left with every score equal and as many files as
//     identities is a group of identical copies that moved together, paired in key order (IdenticalCopies); anything
//     else is left to review. A group of more than 4,194,304 (identity, file) pairs is not compared: all to review.
//     The quick hash reads a file's two ends only: two files over 128 KiB of one size that share their first and last
//     64 KiB count as identical (ADR-0008's evidence).
//  2. Size, time and name, for the vanished identities without a current hash: the new files (not paired, whose quick
//     hash, if read, matched no stored hash) of equal size, last write time and file name (the last element of the key:
//     names folded as the file systems compare them), and of the same kind (file or folder). An identity pairs only
//     with a unique such file that no other such identity has (SizeTimeName); no ties are broken here.
//  3. Review rows (never a silent merge) for the vanished identities left unpaired that have candidates: the files of
//     their quick hash that ties could not tell apart, the new files of their hash's size that could not be read, the
//     files of equal size, time and name that were not unique, and the new files of the same file name (other content,
//     size or time). The candidates are in key order, at most kMaxReviewCandidates. A vanished identity without any
//     candidate is left vanished.
// A new file with a holder (NewFile::heldBy, a file moved back) pairs like any other; the import records the holder as
// the moved identity's alias (MoveReason::MovedBack). Statistics say what was read.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/change_set.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// A file could not be read for its quick hash (denied, locked, written meanwhile): it is not paired by its hash, or
/// not hashed by the pass. Warning.
inline constexpr std::string_view kQuickHashIssueUnreadable = "quick-hash-unreadable";
/// The quick-hash pass could not reach a system's files (its drive went while it read): the rest of the system is left
/// for a later pass. Warning.
inline constexpr std::string_view kQuickHashIssueUnavailable = "quick-hash-unavailable";

/// The most candidates a review row lists, in key order (its reason says how many there were).
inline constexpr std::size_t kMaxReviewCandidates = 100;

/// The quick hash of `identity` when it is current for its last file facts; nothing when it has none, when it is stale
/// or when the identity has no file facts (or a folder's).
[[nodiscard]] std::optional<QuickHash> currentQuickHash(const StoredIdentity& identity) noexcept;

/// The import's pairing pass of ADR-0008 §4 (the rules above). Kept for many imports; used from the import's thread.
class IdentityPairing final : public IIdentityPairing {
public:
    /// What the pass did, summed over its calls.
    struct Statistics {
        std::size_t requests = 0;        ///< systems it was asked about
        std::size_t filesHashed = 0;     ///< new files whose quick hash it read
        std::size_t filesUnreadable = 0; ///< new files it could not read (issues)
        std::uint64_t bytesRead = 0;     ///< quickHashBytes() of the files hashed
        std::size_t pairs = 0;
        std::size_t reviews = 0;

        bool operator==(const Statistics&) const = default;
    };

    /// `reader` reads the quick hashes of new files (readQuickHashBelow() when empty).
    explicit IdentityPairing(QuickHashReader reader = {});

    [[nodiscard]] Expected<IdentityPairs> pair(const PairingRequest& request) override;

    [[nodiscard]] const Statistics& statistics() const noexcept { return m_statistics; }

private:
    QuickHashReader m_reader;
    Statistics m_statistics;
};

/// Where the quick-hash pass is.
struct QuickHashProgress {
    std::size_t done = 0;  ///< files looked at (read or not)
    std::size_t total = 0; ///< files it will look at (the candidates, within the limit)
    std::size_t hashed = 0;
    std::uint64_t bytesRead = 0;
};

/// How the quick-hash pass runs.
struct QuickHashPassOptions {
    /// Only this system's identities; nothing: every system of the library.
    std::optional<SystemName> system;
    /// At most this many files read (hashed or not); 0: no limit.
    std::size_t limit = 0;
    /// A batch is committed once it holds this many hashes, or once this long has passed since the last commit.
    std::size_t batchFiles = 256;
    std::chrono::milliseconds batchTime{2000};
    /// Read between files when not null: once it holds true, the pass commits what it hashed and ends (cancelled).
    const std::atomic<bool>* cancel = nullptr;
    /// Called after each file (and once at the end).
    std::function<void(const QuickHashProgress&)> progress;
    /// Reads the quick hashes (readQuickHashBelow() when empty).
    QuickHashReader reader;
};

/// What the quick-hash pass did. The library's identities (or the system's) are partitioned: not present, folders,
/// with a current hash, and candidates (present files without a current hash); the candidates into hashed, changed,
/// gone, unreadable, unavailable and remaining.
struct QuickHashPassResult {
    std::size_t identities = 0;
    std::size_t notPresent = 0; ///< their file was not present at the last import: nothing to read
    std::size_t folders = 0;    ///< folders have no quick hash
    std::size_t current = 0;    ///< a current quick hash already
    std::size_t candidates = 0;
    std::size_t hashed = 0;      ///< hashed and committed
    std::size_t changed = 0; ///< the file's size or time is no longer the last import's: hashed after the next import
    std::size_t gone = 0;        ///< the file is no longer where the last import saw it
    std::size_t unreadable = 0;  ///< could not be read (an issue each)
    std::size_t unavailable = 0; ///< not read: the system's folder was out of reach at the last import, or went
    std::size_t remaining = 0;   ///< not looked at: the limit, or a cancel
    std::size_t batches = 0;     ///< commits
    std::uint64_t bytesRead = 0;
    std::chrono::nanoseconds time{};
    bool cancelled = false;
    std::vector<Issue> issues;
};

/// The quick-hash pass over `library`'s identities (ADR-0008 §4): takes the import lock (ICatalogStore::lockImport()),
/// so that it never overlaps an import (which reads the identities before its writes trusting that nothing else
/// writes them), and holds it to the end; reads each system's identities (systems in ordinal order, identities in the
/// stored order) and the ROM folders the last import recorded (ICatalogStore::systems()); then reads, one after the
/// other, the present files without a current quick hash, at most 128 KiB each and never written, and commits their
/// hashes in batches (IImportTransaction::putQuickHash(), one import transaction per batch: other writers wait only for
/// a batch's writes). A file whose size or last write time are no longer its identity's is not hashed (the next import
/// updates its facts, the pass after it hashes it). A file that cannot be read is an issue, never a stop. Errors:
/// Conflict (an import or another pass holds the import lock), InvalidArgument (a system the library has no identity
/// of), the store's (what was committed before stays).
[[nodiscard]] Expected<QuickHashPassResult> runQuickHashPass(ICatalogStore& store, const LibraryId& library,
                                                             const QuickHashPassOptions& options = {});

/// What the user decided for a review row.
struct ReviewDecision {
    /// The review row's id (StoredReview::review.id).
    std::int64_t review = 0;
    /// Pair the vanished identity with this identity (of the same library and system, its file present); nothing:
    /// keep them separate.
    std::optional<GameId> pairWith;
};

/// What resolveReview() did.
struct ReviewResolutionResult {
    /// The row as resolved.
    StoredReview review;
    /// Paired: the move (MoveReason::ReviewResolved; `alias` the identity paired with, now an alias of `id`).
    std::optional<MovedGame> moved;
    /// Paired, when an index row (a gamelist entry left at the old path) still named the vanished identity: the new
    /// identity that row takes, as the import gives one.
    std::optional<GameId> leftAtOldPath;
};

/// Resolves a review row (ADR-0008 §4: "resolving a review does the same" as a file moved back), under the import lock,
/// in one import transaction. Paired: the vanished identity takes the chosen identity's file (its path, facts and quick
/// hash, present), the chosen identity becomes its alias (IImportTransaction::putAlias(): its state rows move to the
/// vanished identity, its index rows go), the index rows the chosen identity had are written again under the vanished
/// identity's GameId, a row still at the old path (a gamelist entry left there) gets a new identity as the import gives
/// one (assignGameIds()), the disc groups that named either are written again; the history records the move, the
/// alias and the resolution, and the row is resolved (Paired). Kept separate: the row is resolved (KeptSeparate) and
/// the history records it; nothing else changes. Either way the next import changes nothing. Errors: NotFound (no such
/// review row, no identity of that GameId), Conflict (the row is resolved already, the vanished identity's file is
/// present again, an import holds the import lock), InvalidArgument (a review of another library; an identity to pair
/// with that is the vanished one, an alias, of another library or system, or whose file is not present), the store's.
[[nodiscard]] Expected<ReviewResolutionResult> resolveReview(ICatalogStore& store, const LibraryId& library,
                                                             const ReviewDecision& decision);

} // namespace rv::catalog
