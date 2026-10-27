#pragma once

// Snapshots, transactions and undo of the world's explicit actions (ADR-0015 §6; I4b step 1). An explicit action (an
// editor save, a regeneration, a CLI edit, an undo) changes the plan and the overrides together, under the world lock
// (world_store.hpp):
//   1. a snapshot of the world's files as they are, backups/snapshots/<YYYYMMDDTHHMMSS.mmmZ>-<action>[-<n>]/: the
//      plan, its meta sidecar and overrides.json (those that are there) and snapshot.json (below), made in a folder
//      "<name>.partial" renamed whole once written;
//   2. the journal, transaction.json in the world's folder, naming the snapshot;
//   3. overrides.json, then the plan (savePlan(): its backup and its sidecar), each written atomically;
//   4. the journal removed; then the snapshots beyond the newest kSnapshotsKept removed.
// A lock that finds a journal (an action interrupted between 2 and 4: a crash, a power loss) restores its snapshot's
// files byte for byte, removes the journal and the snapshot (the action never happened), and logs it; a
// "<name>.partial" folder (interrupted at 1) is removed. So the plan and the overrides never disagree.
//
// Undo restores the newest snapshot of kind `action` not yet undone, byte for byte, as an action of its own: the state
// it replaces becomes a snapshot of kind `undo` (naming the snapshot it undid), which undo never offers, and the
// snapshot restored is marked undone. Undo walks back through the kept snapshots; there is no redo. Updates are not
// explicit actions: they save the plan alone (world_store.hpp's backups) and take no snapshot.
//
// snapshot.json (formatted JSON, keys sorted; every key required):
//   {"schemaVersion": 1, "sequence": 7, "kind": "action" | "undo", "action": "regenerate", "scope": "world",
//    "writer": "retroverse-cli worldplan generate --replace (...)", "takenAt": "2026-10-27T10:00:00.000Z",
//    "planDigest": "<sha-256>" | "", "metaDigest": ..., "overridesDigest": ..., "undone": false, "undoes": "" | <name>}
// A digest is the SHA-256 of the file's bytes as kept in the snapshot, empty when the file was not there (restoring
// removes it then). `sequence` orders the snapshots (one more than the highest kept; never the clock, which may be set
// back); `undoes` names the snapshot an undo snapshot undid.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_store.hpp>

namespace rv::world {

inline constexpr std::string_view kSnapshotFileName = "snapshot.json";
/// The snapshots kept in backups/snapshots/, the newest (ADR-0015 §6).
inline constexpr std::size_t kSnapshotsKept = 20;
inline constexpr std::int64_t kSnapshotSchemaVersion = 1;
inline constexpr std::int64_t kTransactionSchemaVersion = 1;

/// What a snapshot keeps: the state before an explicit action, or the state an undo replaced.
enum class SnapshotKind : std::uint8_t {
    Action, ///< before an explicit action: undo offers it
    Undo,   ///< the state an undo replaced: never offered (no redo)
};

[[nodiscard]] constexpr std::string_view toString(SnapshotKind kind) noexcept {
    switch (kind) {
    case SnapshotKind::Action:
        return "action";
    case SnapshotKind::Undo:
        return "undo";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SnapshotKind> parseSnapshotKind(std::string_view text) noexcept {
    return detail::enumFromString(text, SnapshotKind::Undo);
}

/// An explicit action, as its snapshot records it.
struct WorldAction {
    /// What it is: 1 to 32 characters among a-z, 0-9 and '-', the first a letter ("edit", "regenerate"); it names the
    /// snapshot's folder. "undo" is undo's own.
    std::string action;
    /// What it changes ("world", a floor id, a room id): informative.
    std::string scope;
};

/// An action's name as WorldAction::action allows it.
[[nodiscard]] bool isValidActionName(std::string_view text) noexcept;

/// A snapshot in backups/snapshots/.
struct WorldSnapshot {
    /// Its folder's name.
    std::string name;
    std::int64_t schemaVersion = kSnapshotSchemaVersion;
    std::uint64_t sequence = 0;
    SnapshotKind kind = SnapshotKind::Action;
    std::string action;
    std::string scope;
    /// The program that took it (SaveOptions::writer).
    std::string writer;
    /// When (UTC, core::formatUtcIso8601()).
    std::string takenAt;
    /// The SHA-256 of each file kept; empty when the file was not there.
    std::string planDigest;
    std::string metaDigest;
    std::string overridesDigest;
    /// Restored by an undo (kind Action only).
    bool undone = false;
    /// Kind Undo: the snapshot whose undo replaced this state.
    std::string undoes;

    bool operator==(const WorldSnapshot&) const = default;
};

/// snapshot.json's text (the name is the folder's, not written): formatted JSON, keys sorted, a final line feed.
[[nodiscard]] std::string writeSnapshotRecord(const WorldSnapshot& snapshot);
/// Reads snapshot.json back (the name left empty). Errors (context and line): ParseError, ValidationFailed (a key
/// missing, unknown or of another type, an action name or a digest not as written), Unsupported (a newer schema).
[[nodiscard]] Expected<WorldSnapshot> readSnapshotRecord(std::string_view text, std::string_view context);

/// The world's snapshots, newest first (by sequence); a folder whose snapshot.json cannot be read is left out and
/// logged. None when there is no backups/snapshots/.
[[nodiscard]] std::vector<WorldSnapshot> listSnapshots(const WorldFiles& files);
/// The snapshot undo restores next among `snapshots` (listSnapshots()'s): the newest of kind Action not undone.
[[nodiscard]] std::optional<WorldSnapshot> nextUndo(const std::vector<WorldSnapshot>& snapshots);

/// What commitWorld() did.
struct CommitReport {
    /// False when the folder held this plan and these overrides already: no snapshot, nothing written.
    bool written = false;
    /// The snapshot taken before the writes.
    std::optional<WorldSnapshot> snapshot;
    /// savePlan()'s and saveOverrides()'s reports.
    SaveReport plan;
    OverridesSaveReport overrides;
    /// The snapshots removed beyond kSnapshotsKept, by name.
    std::vector<std::string> snapshotsRemoved;
};

/// Saves `result`'s plan and `overrides` together as the explicit action `action`, under `lock`: a snapshot, the
/// journal, overrides.json, the plan (savePlan(), with `options`), the journal removed, the old snapshots rotated. A
/// failed write restores the snapshot at once (and a crash leaves the journal to the next lock): the two files change
/// together or not at all. Nothing is done when the plan's text and the overrides' are the folder's already. Errors:
/// InvalidArgument for a lock moved from, an action name isValidActionName() refuses, overrides writeOverrides()
/// refuses, and a RequiresExplicitRegeneration result; ValidationFailed for a plan that breaks a hard rule (nothing
/// written, no snapshot); those of the snapshot and of the writes.
[[nodiscard]] Expected<CommitReport> commitWorld(const WorldLock& lock, const WorldAction& action,
                                                 const GenerationResult& result, const WorldOverrides& overrides,
                                                 const SaveOptions& options = {});

struct UndoOptions {
    /// The program undoing (the undo snapshot's writer).
    std::string writer;
    /// The undo snapshot's time and name; empty: the system clock.
    std::function<std::chrono::system_clock::time_point()> clock;
    /// Only find what would be restored: nothing written.
    bool dryRun = false;
};

/// What undoLastAction() did.
struct UndoReport {
    /// The snapshot restored (marked undone, unless a dry run).
    WorldSnapshot restored;
    /// The state it replaced, kept as a snapshot of kind Undo; none for a dry run.
    std::optional<WorldSnapshot> kept;
    std::vector<std::string> snapshotsRemoved;
};

/// Undoes the last explicit action not undone yet, under `lock`: the newest snapshot of kind Action not undone
/// restored byte for byte (the plan, its sidecar and overrides.json; a file it did not hold removed) as a transaction
/// of its own, behind a snapshot of kind Undo of the state it replaces; the snapshot restored marked undone. Errors:
/// InvalidArgument for a lock moved from; NotFound when there is nothing to undo (no snapshot, or only undo snapshots
/// and undone ones: an undo is never undone); ValidationFailed when the snapshot's files are not those its digests
/// name (damaged: nothing written); those of reading the snapshot and of the writes.
[[nodiscard]] Expected<UndoReport> undoLastAction(const WorldLock& lock, const UndoOptions& options = {});

namespace detail {

/// Where commitWorld() or undoLastAction() stops, as a process ended at that point would (tests of the rollback).
enum class TransactionStop : std::uint8_t {
    None,              ///< the whole action
    AfterSnapshot,     ///< the snapshot made, no journal
    AfterJournal,      ///< the journal written, nothing else
    BetweenWrites,     ///< overrides.json written, not the plan
    BeforeJournalEnds, ///< both written, the journal still there
};

/// commitWorld() stopped at `stop`: what it wrote is left as it is and Cancelled returned (tests only).
[[nodiscard]] Expected<CommitReport> commitWorldStoppingAt(const WorldLock& lock, const WorldAction& action,
                                                           const GenerationResult& result,
                                                           const WorldOverrides& overrides, const SaveOptions& options,
                                                           TransactionStop stop);
/// undoLastAction() stopped at `stop` (tests only).
[[nodiscard]] Expected<UndoReport> undoLastActionStoppingAt(const WorldLock& lock, const UndoOptions& options,
                                                            TransactionStop stop);

/// What the lock does when taken (WorldLock::acquire()): removes the "<name>.partial" snapshot folders and the
/// temporary files in the snapshots, and rolls back the action a journal names. Returns the snapshot restored, if any.
/// Errors: those of reading the journal or the snapshot and of restoring it (the journal then stays).
[[nodiscard]] Expected<std::optional<std::string>> recoverInterruptedAction(const WorldFiles& files);

} // namespace detail

} // namespace rv::world
