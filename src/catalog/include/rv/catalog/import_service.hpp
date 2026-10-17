#pragma once

// The import (ADR-0008 §4, §5, §7; I2 step 11a): a library's snapshot into the catalog store, through the ports alone
// (ILibrarySource, IMediaResolver, ICatalogStore). One run:
//  1. takes the import lock (ICatalogStore::lockImport(): one import at a time, also across processes; Conflict when
//     another holds it) and keeps it to the end;
//  2. checks the library against the store, the authority on its id (a library.json naming another library is refused
//     until it names the store's again: Conflict), under the lock, so that two imports never both see an empty store;
//  3. loads the snapshot (ILibrarySource::load()), handing it the disc groups the index holds (their references, with
//     the facts of the heads they were read from: a sheet read before is not read again; `db rebuild-index` reads every
//     sheet that can be read, LibraryLoadOptions::readEverySheet), then refreshes the media resolver
//     (IMediaResolver::refresh(): a resolver kept between runs sees the media as they are now) and asks presence() for
//     every entry with a key; neither reads a ROM byte (cue sheets and playlists aside);
//  4. reads, per system, the stored identities and index rows, and computes what changes (ChangeSet) against them,
//     system by system (below), the pairing pass included: under the import lock alone, since whatever writes
//     identities, aliases or index rows holds it (ICatalogStore);
//  5. begins the write transaction and writes it: identities that become aliases first (IImportTransaction::putAlias(),
//     which frees their paths), identities in ascending GameId order (those that exist before the new ones), index rows
//     in ascending GameId order with the unchanged ones skipped (an unchanged upsert would still run the search index's
//     trigger and rewrite the order index), removals, systems, disc groups (with their references by relative path),
//     the import's issues and its record; commits (the store checkpoints the WAL after it). The store suspends its
//     search index for a bulk write (prepareWrites()). The write transaction thus lasts as long as the writes alone.
// A cancellation (ImportOptions::cancel) is honoured between pieces of work: the transaction is dropped and the store
// stays as it was.
//
// Identities (ADR-0008 §4): every entry with a key has one. A stored identity is matched on the exact relative path
// first, then on the key (a file renamed in case only keeps its id; two names that fold to one key match in order, the
// next one gets a new id, and the import reports it); a key seen for the first time gets the GameId its tuple derives
// (assignGameIds(): in (system, key, relative path) order, a counter after a collision with an id given out or taken).
// A matched identity follows its entry: seen on disk, it takes the entry's relative path, file facts and present flag;
// a MissingFile entry (the file gone, the gamelist entry left) keeps the identity's last path and facts and clears
// present; an identity no entry names, in a folder that was listed, clears present (vanished, gamelist or not); under a
// folder that could not be listed it stays as it was (never taken as vanished, out of any reconciliation). Identities
// whose key was computed with another key rule or casing table are written again with the key computed now, in place
// (StoredIdentity::keyVersion). The import never removes an identity (only a pairing turns one into an alias, below): a
// file that comes back at its path, or its key, gets its id back. A new ROM root or drive letter changes no relative
// path, so no id.
//
// What the index holds, per system (SystemOutcome):
//  - Applied: every entry of the snapshot with a key, as the snapshot gives it, with its media and sort key. Rows the
//    index held whose entry is gone are removed (a vanished file without a gamelist entry leaves the index; with one,
//    its MissingFile entry stays). Rows under a folder below the system folder that could not be listed are kept as
//    they were, their availability SourceUnavailable with the folder's reason; the entries the gamelist alone names
//    there and the index did not hold are added as the snapshot gives them.
//  - SourceUnavailable: the system folder is missing or cannot be listed (an unplugged drive, the ROM root missing or
//    emptied): every row kept as it was, marked source unavailable with the system's reason; entries its gamelist alone
//    names and the index did not hold added. Nothing removed.
//  - GamelistFailed: the gamelist exists but failed (GamelistState::Failed): the system's rows, identities and disc
//    groups kept as they were; its system row is written with the gamelist state "failed" and the alternative emulator
//    it held.
//  - MetadataPending: the gamelist failed and the index held no row of the system: nothing imported (the system is not
//    imported unscraped); its system row says "failed" until an import reads the gamelist.
//  - KeptByGuard: the mass-disappearance guard (ADR-0008 §4): when more than half of the identities present at the
//    last import (outside folders that cannot be listed now) are gone, and not paired with a new file, nothing changes
//    for the system (its system row included) and the import reports it (kImportIssueMassDisappearance), unless
//    ImportOptions::acceptRemovals. A system folder that is missing is no disappearance: it is SourceUnavailable.
//  - NotConfigured: a system the store holds that the configuration no longer defines: its rows and system row
//    removed, its identities kept (not present); under the guard while it had present files. Once that is done, a
//    system of identities alone (no row, no system row, no file present) is left out of the ChangeSet and reported no
//    more.
// Disc groups follow their system: a group whose head lies below a folder that cannot be listed (or in a system folder
// that cannot be) is kept as the index held it, as the head's row is.
// Media are stored as presence and extension per kind (paths are derived again with IMediaResolver::path()); when
// presence() is Unavailable (a media root out of reach, a media folder that cannot be listed), the entry keeps the
// media its row held.
//
// The seam for reconciliation (I2 step 11b, IIdentityPairing): per applied system, once the stored identities are
// matched and before any GameId is given out, the import hands the pairing pass the vanished identities (present at
// the last import, their file gone from a folder that was listed, with or without a MissingFile entry still naming
// them) and the new files: the entries seen on disk whose path and key match no identity, and those matched to an
// identity that was not present at the last import (NewFile::heldBy: a file that comes back, or one that lands on the
// path or key a MissingFile entry kept since a move). What it pairs, the import applies as a move: the new file's entry
// takes the vanished identity's GameId and the identity takes the new file's path, facts and present flag (MoveReason
// in ChangeSet::moved); a MissingFile entry that named the moved identity at its old path then has no identity and gets
// a new GameId (a counter: the tuple's own value is the moved identity's); an identity that held the new file
// (NewFile::heldBy) becomes an alias of the moved one (ADR-0008 §4's moved-back rule: MovedGame::alias,
// IImportTransaction::putAlias(), written before any identity so that its path is free; its index rows leave). The
// guard counts what stays gone after the pairing. Identities that exist are written before new ones, so that the old
// path of a moved identity is free when the new identity of that path is written. Without a pairing pass (11a), a moved
// file is a vanished identity and a new one. The pass runs under the import lock, outside the write transaction: what it
// reads of the store (quick hashes) cannot change meanwhile, and hashing new files keeps no other writer waiting.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/change_set.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// The codes of the issues the import reports besides the snapshot's (Issue::code).
/// More than half of a system's present files are gone: nothing changed for the system (SystemOutcome::KeptByGuard).
/// Warning.
inline constexpr std::string_view kImportIssueMassDisappearance = "import-mass-disappearance";
/// Every system folder that held files at the last import is missing or cannot be listed at once (the ROM root
/// unplugged, renamed or emptied): their rows are kept as source unavailable. Warning.
inline constexpr std::string_view kImportIssueRomRootUnavailable = "import-rom-root-unavailable";
/// Two relative paths of a system fold to one lookup key: each keeps an identity of its own (ADR-0008 §4). Info.
inline constexpr std::string_view kImportIssueKeyCollision = "import-key-collision";
/// A system's gamelist failed: its previous rows are kept (or, on its first import, nothing is imported: metadata
/// pending). Warning.
inline constexpr std::string_view kImportIssueGamelistKept = "import-gamelist-failed";
/// The configuration no longer defines a system the catalog holds: its rows are removed (or kept by the guard).
/// Warning.
inline constexpr std::string_view kImportIssueSystemNotConfigured = "import-system-not-configured";

/// How an import runs.
struct ImportOptions {
    /// Read between pieces of work when not null: once it holds true, the import ends with Cancelled and the store is
    /// left as it was.
    const std::atomic<bool>* cancel = nullptr;
    /// Apply the removals the mass-disappearance guard would hold back (`retroverse-cli import --accept-removals`).
    bool acceptRemovals = false;
    /// Only these systems (a partial import: `retroverse-cli import --system`): the systems the store holds that are
    /// not listed are left as they are. The source must give each listed system (an ILibrarySource made for those
    /// systems, or for every one); empty: every system.
    std::vector<SystemName> systems;
    /// `db rebuild-index`: the library's index tables are cleared and every row is written again, from a fresh snapshot
    /// (every cue sheet and playlist that can be read is read again: LibraryLoadOptions::readEverySheet), the identities
    /// and, where the snapshot cannot tell (folders that cannot be listed, failed gamelists, sheets that cannot be read,
    /// media out of reach, systems kept by the guard), the rows the index held before; the result equals what an import
    /// gives, and a disc group stored wrong is read again.
    bool rebuildIndex = false;
};

/// What an import did, for people and tests (counts of rows written, times).
struct ImportStatistics {
    std::chrono::nanoseconds loadTime{};    ///< ILibrarySource::load()
    std::chrono::nanoseconds mediaTime{};   ///< IMediaResolver::presence() of every entry
    std::chrono::nanoseconds compareTime{}; ///< the store's reads and the comparison (the pairing pass included)
    std::chrono::nanoseconds writeTime{};   ///< the write transaction: the writes and the commit
    std::chrono::nanoseconds totalTime{};
    std::size_t entries = 0;           ///< the snapshot's entries (outside the system folders included)
    std::size_t mediaUnknown = 0;      ///< entries whose media presence() could not know (their stored media kept)
    std::size_t identitiesWritten = 0; ///< identities written (new, changed, re-keyed, moved)
    std::size_t aliasesWritten = 0;    ///< identities turned into aliases (IImportTransaction::putAlias())
    std::size_t entriesWritten = 0;    ///< index rows written
    std::size_t entriesRemoved = 0;    ///< index rows removed
    std::size_t discGroupsWritten = 0;
    std::size_t discGroupsRemoved = 0;
    std::size_t systemsWritten = 0;
    /// The store suspended its search index for the writes and rebuilt it before the commit (prepareWrites()).
    bool searchIndexRebuilt = false;

    bool operator==(const ImportStatistics&) const = default;
};

/// What ImportService::run() gives back.
struct ImportResult {
    ChangeSet changes;
    ImportStatistics statistics;
};

/// A vanished identity, as the pairing pass sees it (the seam for I2 step 11b).
struct VanishedIdentity {
    /// The identity as stored: its last path and file facts, `present` true.
    StoredIdentity identity;
    /// A MissingFile entry of the snapshot still names it (its gamelist entry left at the old path): moved, the identity
    /// leaves that entry to a new GameId.
    bool gamelistEntryKept = false;

    bool operator==(const VanishedIdentity&) const = default;
};

/// A new file, as the pairing pass sees it: an entry seen on disk whose path and key match no identity, or match one
/// that was not present at the last import.
struct NewFile {
    LookupKey key;
    FileFacts file;
    EntryState state = EntryState::Game;
    /// The identity its path or key matched, which was not present at the last import (its file comes back, or lands
    /// where a MissingFile entry kept that identity since a move); nothing for a path and key no identity has. Paired
    /// with a vanished identity (MoveReason::MovedBack), the file takes the vanished identity and this one becomes its
    /// alias; left unpaired, the file keeps this identity.
    std::optional<GameId> heldBy;

    bool operator==(const NewFile&) const = default;
};

/// What the pairing pass is asked, for one system.
struct PairingRequest {
    const LibraryId& library;
    /// The system as the snapshot gives it (its ROM folder, where the new files are).
    const SystemInfo& system;
    /// In the stored identities' order (key, then relative path).
    std::span<const VanishedIdentity> vanished;
    /// In entryOrder().
    std::span<const NewFile> newFiles;
    const std::atomic<bool>* cancel = nullptr;
};

/// A vanished identity paired with a new file.
struct IdentityPair {
    GameId identity;
    /// The new file's key (NewFile::key).
    LookupKey newFile;
    MoveReason reason = MoveReason::QuickHash;

    bool operator==(const IdentityPair&) const = default;
};

/// What the pairing pass decided for one system.
struct IdentityPairs {
    /// Each vanished identity and each new file at most once.
    std::vector<IdentityPair> pairs;
    /// Vanished identities no pairing was safe for (ChangeSet::reviews).
    std::vector<ReviewRow> reviews;
    std::vector<Issue> issues;
};

/// The seam where I2 step 11b pairs vanished identities with new files (ADR-0008 §4: quick hashes, size, time and
/// name, the moved-back rule, review rows). Called once per system that has both and whose gamelist was read, from the
/// import's thread, under the import lock and before the write transaction (nothing is written yet; whatever writes
/// identities holds the same lock). Errors end the import (Cancelled when `cancel` was set).
class IIdentityPairing {
public:
    virtual ~IIdentityPairing() = default;
    [[nodiscard]] virtual Expected<IdentityPairs> pair(const PairingRequest& request) = 0;
};

/// The import service. Used from one thread at a time (the catalog worker in the app, the main thread of
/// retroverse-cli); the ports must outlive it, and may be kept for many runs (each run refreshes the media resolver
/// and loads the library anew).
class ImportService {
public:
    /// `pairing` may be null: no reconciliation (I2 step 11a).
    ImportService(ILibrarySource& source, IMediaResolver& media, ICatalogStore& store,
                  IIdentityPairing* pairing = nullptr) noexcept
        : m_source(source), m_media(media), m_store(store), m_pairing(pairing) {}

    /// Imports the library `profile` describes, as above. Errors: Conflict (library.json names a library the store
    /// does not hold while it holds another: the message names both; another import holds the import lock),
    /// InvalidArgument (a system of ImportOptions::systems the snapshot does not give; a pairing that names what it was
    /// not given), Cancelled, the source's errors (the library cannot be read at all) and the store's.
    [[nodiscard]] Expected<ImportResult> run(const LibraryProfile& profile, const ImportOptions& options = {});

private:
    ILibrarySource& m_source;
    IMediaResolver& m_media;
    ICatalogStore& m_store;
    IIdentityPairing* m_pairing;
};

} // namespace rv::catalog
