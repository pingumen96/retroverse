#pragma once

// Port ICatalogStore (ADR-0008 §4, §5): what the import (I2 step 11) needs from the database, implemented by
// rv_catalog_db (SqliteCatalogStore, step 4). The store holds state tables (rv_*: libraries, identities and what only
// Retroverse knows) and index tables (idx_*: a function of the snapshot and the identities, rebuildable at any time).
//
// At I2 step 1: the identity lookups, the GameIds already taken, the index rows of a system for the import's
// comparison, and one write transaction per import for libraries, identities, systems and entries. Step 11a (the
// import, ImportService) adds: the systems and disc groups the index holds (IndexedDiscGroup: the cache of the next
// load, LibraryLoadOptions::discGroups, with the GameIds of the head and the members), the systems of the identities,
// the import lock held apart from the transaction (lockImport(): an import holds it from before it reads the library
// to its end), and in the transaction: the bulk write hint (prepareWrites()), the index cleared (`db rebuild-index`),
// systems and disc groups written and removed, identities turned into aliases (putAlias(), the step where step 11b's
// moved-back pairings free a path), the import's issues, and the import records kept for the user (ImportRecord).
// Step 11b (reconciliation, reconciliation.hpp) adds: the quick hash of an identity's file, read with the identity
// (StoredIdentity::quickHash) and written with it, or alone (IImportTransaction::putQuickHash(), the quick-hash pass);
// the identities' history (IdentityEvent: moves with their reason, aliases, keys computed again, reviews resolved), the
// aliases kept (StoredAlias) and the review rows (StoredReview), read and written; putAlias() moving every state row
// that names the holder to the identity it stands for. Play sessions have their own store (rv_catalog_db's session
// store, step 12).
//
// Reads and the import transaction: an instance has one connection. Its reads made while one of its import
// transactions is open see that transaction's writes; other connections and processes see them only once committed.
// Whatever writes identities, aliases or index rows holds the import lock (the import; step 11b's quick-hash pass and
// review resolution take it too, and write through import transactions of their own), so the import reads the
// identities, the taken GameIds and the index rows it compares with the snapshot under the import lock alone, before
// its write transaction: nothing can change them between its reads and its writes, and the other writers of the
// database (play sessions, launch choices) wait only for its writes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/change_set.hpp>
#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// An identity as the store keeps it (a state table: never removed by an import, whatever ES-DE says).
struct StoredIdentity {
    GameId id;
    LibraryId library;
    SystemName system;
    /// The relative path its file had when last seen, and its key. Lookups match the exact relative path first, then
    /// the key (ADR-0008 §4).
    LookupKey key;
    /// What the key was computed with: when it differs from currentLookupKeyVersion(), the import computes the key
    /// again from the relative path and stores it in place.
    LookupKeyVersion keyVersion;
    /// Its file was present at the last import: once gone, the identity is a reconciliation candidate.
    bool present = false;
    /// Size and time of its file at the last import that saw it.
    std::optional<FileFacts> file;
    /// The quick hash of its file (fingerprint.hpp) with the size and time of the file it was computed from: current
    /// while they are those of `file` (isCurrent()), stale once the file changed. Nothing until a pass hashed it.
    std::optional<QuickHash> quickHash;

    bool operator==(const StoredIdentity&) const = default;
};

/// What the index tables hold for one entry: its identity's GameId, the entry as the snapshot gave it, its media and
/// its sort key. Written and read back unchanged (the import compares them to find what changed).
struct IndexedEntry {
    GameId id;
    CatalogEntry entry;
    EntryMedia media;
    /// sortKeyOf(entry), version kSortKeyVersion.
    std::string sortKey;

    bool operator==(const IndexedEntry&) const = default;
};

/// A disc group as the index holds it (ADR-0008 §3): the group (its head, kind, members and references by key, with the
/// size and last write time of the head its references were read from), which the next load takes back as its cache
/// (LibraryLoadOptions::discGroups), and the GameIds of the head and of the members. The references are kept by
/// relative path, since they include files that are not there (and have no identity).
struct IndexedDiscGroup {
    /// The head's GameId.
    GameId head;
    DiscGroup group;
    /// The members' GameIds, in the order of group.members.
    std::vector<GameId> members;

    bool operator==(const IndexedDiscGroup&) const = default;
};

/// What started an import.
enum class ImportKind : std::uint8_t {
    Import,       ///< `retroverse-cli import`, the app's refresh: what changed is written
    RebuildIndex, ///< `db rebuild-index`: the library's index tables cleared and every row written again
};

/// "import", "rebuild-index".
[[nodiscard]] constexpr std::string_view toString(ImportKind kind) noexcept {
    switch (kind) {
    case ImportKind::Import:
        return "import";
    case ImportKind::RebuildIndex:
        return "rebuild-index";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<ImportKind> parseImportKind(std::string_view text) noexcept {
    return detail::enumFromString(text, ImportKind::RebuildIndex);
}

/// An import as the store keeps it for the user (a state table: rebuilding the index keeps it): when it ran, how, and
/// what it did per system.
struct ImportRecord {
    /// The store's id (AUTOINCREMENT); 0 until stored.
    std::int64_t id = 0;
    LibraryId library;
    ImportKind kind = ImportKind::Import;
    /// UTC ISO-8601 (core::formatUtcIso8601): when the import began, and when it committed.
    std::string startedAt;
    std::string finishedAt;
    /// It was told to accept the removals the mass-disappearance guard holds back.
    bool acceptRemovals = false;
    /// Only these systems were imported (a partial import); empty: every system.
    std::vector<SystemName> systemsAsked;
    /// ChangeSet::systems.
    std::vector<SystemChanges> systems;

    bool operator==(const ImportRecord&) const = default;
};

/// An identity that becomes an alias of another (ADR-0008 §4): a file moved back onto the path or key of an identity
/// that held it meanwhile keeps the identity it had, with its history, and the other is recorded as its alias (the
/// pairing pass of step 11b, MoveReason::MovedBack); resolving a review does the same.
struct IdentityAlias {
    /// The identity that stops being one, as stored: its GameId stands for `game` from now on and is never given out
    /// again (ICatalogStore::takenGameIds()); its key and relative path are recorded with the alias.
    StoredIdentity alias;
    /// The identity it stands for (of the same library and system).
    GameId game;
    MoveReason reason = MoveReason::MovedBack;

    bool operator==(const IdentityAlias&) const = default;
};

/// What happened to an identity (rv_identity_history, ADR-0008 §5: history).
enum class IdentityEventKind : std::uint8_t {
    Moved,          ///< it followed its file to a new path (`reason` says why; `from` and `to` the paths)
    Alias,          ///< another identity became its alias (`alias`; `from` the alias's last path; `reason` why)
    Rekeyed,        ///< its key was computed again with another key rule or casing table (`detail`: the versions)
    ReviewResolved, ///< a review row of it was resolved (`review`; `detail`: how)
};

/// "moved", "alias", "rekeyed", "review-resolved".
[[nodiscard]] constexpr std::string_view toString(IdentityEventKind kind) noexcept {
    switch (kind) {
    case IdentityEventKind::Moved:
        return "moved";
    case IdentityEventKind::Alias:
        return "alias";
    case IdentityEventKind::Rekeyed:
        return "rekeyed";
    case IdentityEventKind::ReviewResolved:
        return "review-resolved";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<IdentityEventKind> parseIdentityEventKind(std::string_view text) noexcept {
    return detail::enumFromString(text, IdentityEventKind::ReviewResolved);
}

/// One row of an identity's history, as written and read back.
struct IdentityEvent {
    /// The store's id (AUTOINCREMENT: the order events happened in); 0 until stored.
    std::int64_t id = 0;
    /// The identity it happened to.
    GameId game;
    /// UTC ISO-8601 (core::formatUtcIso8601); written empty, the store gives it the transaction's time.
    std::string at;
    IdentityEventKind kind = IdentityEventKind::Moved;
    /// Why it moved, or why the alias was recorded; nothing for the other kinds.
    std::optional<MoveReason> reason;
    /// The key and relative path before and after (empty: none). Moved: the old path and the new one. Alias: the
    /// alias's last path. Rekeyed: the path and the key computed now (the key before is not kept: the store computes
    /// keys again from their paths when it reads them).
    std::string fromKey;
    std::string fromPath;
    std::string toKey;
    std::string toPath;
    /// The other identity of an Alias event (the GameId that became an alias of `game`).
    std::optional<GameId> alias;
    /// The review row of a ReviewResolved event (and of the Moved and Alias events its pairing wrote).
    std::optional<std::int64_t> review;
    /// For people: what a move rested on ("quick hash"), the key versions of a re-keying, how a review was resolved.
    std::string detail;

    bool operator==(const IdentityEvent&) const = default;
};

/// A GameId that stands for another identity (rv_identity_aliases): never given out again.
struct StoredAlias {
    GameId alias;
    /// The identity it stands for now (when that one became an alias in turn, the alias was moved to the identity it
    /// stands for: aliases never chain).
    GameId game;
    /// The key and relative path it had when it became an alias.
    LookupKey key;
    /// UTC ISO-8601.
    std::string at;
    MoveReason reason = MoveReason::MovedBack;

    bool operator==(const StoredAlias&) const = default;
};

/// How a review row was resolved.
enum class ReviewResolution : std::uint8_t {
    Paired,       ///< the user paired the vanished identity with another one, which became its alias
    KeptSeparate, ///< the user kept them apart: nothing changed
    Returned,     ///< the vanished identity's file came back (an import saw it at its path or key again)
    Aliased,      ///< the vanished identity became an alias of another (a file moved back onto its path or key)
};

/// "paired", "kept-separate", "returned", "aliased".
[[nodiscard]] constexpr std::string_view toString(ReviewResolution resolution) noexcept {
    switch (resolution) {
    case ReviewResolution::Paired:
        return "paired";
    case ReviewResolution::KeptSeparate:
        return "kept-separate";
    case ReviewResolution::Returned:
        return "returned";
    case ReviewResolution::Aliased:
        return "aliased";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<ReviewResolution> parseReviewResolution(std::string_view text) noexcept {
    return detail::enumFromString(text, ReviewResolution::Aliased);
}

/// A review row as the store keeps it (rv_identity_reviews).
struct StoredReview {
    /// Its id (the store's), the vanished identity (`game`: when that identity became an alias, the identity it stands
    /// for), its system, its last path, the candidates in key order and why no pairing was safe.
    ReviewRow review;
    /// UTC ISO-8601: when the import made it, and when it was resolved (nothing while it is open).
    std::string createdAt;
    std::optional<std::string> resolvedAt;
    std::optional<ReviewResolution> resolution;
    /// Paired: the identity that became the vanished one's alias; Aliased: the identity it became an alias of.
    std::optional<GameId> resolvedWith;

    [[nodiscard]] bool open() const noexcept { return !resolvedAt.has_value(); }

    bool operator==(const StoredReview&) const = default;
};

/// The import lock, held apart from an import transaction (ICatalogStore::lockImport()): released when the object goes.
class IImportLock {
public:
    virtual ~IImportLock() = default;
};

/// The writes of one import, applied together (ADR-0008 §5): other connections and processes (the app's read-only
/// connection, another retroverse-cli) see none of them before commit(), the store's own reads see them at once (see
/// above), and a transaction destroyed without commit() leaves the store as it was. It holds the store's import lock,
/// which keeps two imports (also from two processes) from overlapping. Used from the thread that began it.
class IImportTransaction {
public:
    /// Rolls back unless committed, and releases the import lock.
    virtual ~IImportTransaction() = default;

    /// Adds the library, or updates its adapter and folders; its id never changes (ADR-0008 §4).
    [[nodiscard]] virtual Status putLibrary(const LibraryProfile& profile) = 0;
    /// Adds the identity, or replaces the one with the same GameId (its library must exist).
    [[nodiscard]] virtual Status putIdentity(const StoredIdentity& identity) = 0;
    /// Adds the system to the library's index, or replaces it.
    [[nodiscard]] virtual Status putSystem(const LibraryId& library, const SystemInfo& system) = 0;
    /// Adds the entry's index rows, or replaces those of the same GameId. Its identity must exist and its key must not
    /// be empty (an entry outside its system folder is never indexed): InvalidArgument otherwise.
    [[nodiscard]] virtual Status putEntry(const IndexedEntry& entry) = 0;
    /// Removes the index rows of `id` (its entry, its media, the disc group it heads); its identity stays (and comes
    /// back with its id when the file does).
    [[nodiscard]] virtual Status removeEntry(GameId id) = 0;

    /// Says how many index rows (entries put and removed) the import is about to write, before it writes them, so that
    /// the store can prepare for a bulk write: rv_catalog_db suspends its search index for a first import (the index
    /// holds no entry) or when the rows are a large share of the index, and rebuilds it in commit(). Returns whether it
    /// did. Optional: an import that never calls it is written as it comes.
    [[nodiscard]] virtual Expected<bool> prepareWrites(std::size_t rows) = 0;
    /// Removes every index row of the library (systems, entries, media, disc groups, issues) before an import writes
    /// them all again (`db rebuild-index`); its state rows (the library, the identities, the import records) stay.
    [[nodiscard]] virtual Status clearIndex(const LibraryId& library) = 0;
    /// Removes the system's row from the library's index (a system the configuration no longer defines; its entries
    /// are removed with removeEntry()).
    [[nodiscard]] virtual Status removeSystem(const LibraryId& library, const SystemName& system) = 0;
    /// Adds the disc group headed by `group.head`, or replaces it (its kind, head size and time, members and
    /// references). The head's and the members' identities must exist (InvalidArgument otherwise), and `members` must
    /// match the group's member keys one to one.
    [[nodiscard]] virtual Status putDiscGroup(const IndexedDiscGroup& group) = 0;
    /// Removes the disc group headed by `head` (nothing when there is none).
    [[nodiscard]] virtual Status removeDiscGroup(GameId head) = 0;
    /// Turns the identity `alias.alias` into an alias of `alias.game`: its open review rows are resolved (Aliased);
    /// every state row that names it is moved to `alias.game` (its history, its review rows, the aliases that stand for
    /// it, play sessions and launch choices: whatever state table names an identity, rv_catalog_db finds them in the
    /// schema), a row of `alias.game` taking precedence in a table that holds one per game; the index rows that name it
    /// (its entry and media, the disc group it heads, its place among the members of other groups, which the import
    /// writes again) are removed; its GameId is recorded among the aliases with its key and relative path; and its
    /// identity row is removed, which frees its relative path for the identity that moves back there. The import calls
    /// it before it writes any identity. Errors: InvalidArgument (an alias of itself, either identity not in the store,
    /// the two of different libraries or systems), and the store's.
    [[nodiscard]] virtual Status putAlias(const IdentityAlias& alias) = 0;
    /// Writes the quick hash of the identity `id`'s file, and nothing else of it (the quick-hash pass). Errors:
    /// InvalidArgument (no identity has that GameId; a size beyond 2^63 - 1), the store's.
    [[nodiscard]] virtual Status putQuickHash(GameId id, const QuickHash& hash) = 0;
    /// Adds an event to its identity's history (`event.id` is ignored: the store gives the next; an empty `event.at`
    /// takes the transaction's time). Errors: InvalidArgument (no identity has `event.game`), the store's.
    [[nodiscard]] virtual Status putHistory(const IdentityEvent& event) = 0;
    /// Adds a review row (`review.id` is ignored) and returns its id, which the user names to resolve it. Errors:
    /// InvalidArgument (no identity has `review.game`, a candidate without a path), the store's.
    [[nodiscard]] virtual Expected<std::int64_t> putReview(const ReviewRow& review) = 0;
    /// Resolves the open review row `id`: when, how, and the identity it was resolved with. Errors: NotFound (no such
    /// row), Conflict (resolved already), the store's.
    [[nodiscard]] virtual Status resolveReview(std::int64_t id, ReviewResolution resolution,
                                               std::optional<GameId> with) = 0;
    /// Resolves every open review row of the identity `game` (an identity whose file came back: Returned); nothing when
    /// it has none.
    [[nodiscard]] virtual Status resolveReviewsOf(GameId game, ReviewResolution resolution) = 0;
    /// Replaces the library's import issues (the issues of its last import, in order) with `issues`; when `systems` is
    /// not empty (a partial import), only the issues of those systems are replaced, those of other systems and of the
    /// library as a whole stay.
    [[nodiscard]] virtual Status putImportIssues(const LibraryId& library, std::span<const Issue> issues,
                                                 std::span<const SystemName> systems) = 0;
    /// Keeps the record of this import (ImportRecord: its id is the store's).
    [[nodiscard]] virtual Status recordImport(const ImportRecord& record) = 0;

    /// Makes every write visible to other connections at once, releases the import lock (unless lockImport() holds it)
    /// and ends the transaction (later calls fail with InvalidArgument).
    [[nodiscard]] virtual Status commit() = 0;
};

/// The store. An instance owns the process's one writer connection and is used from one thread at a time (the catalog
/// worker in the app, the main thread of retroverse-cli).
class ICatalogStore {
public:
    virtual ~ICatalogStore() = default;

    /// Every library the store knows, by id.
    [[nodiscard]] virtual Expected<std::vector<LibraryProfile>> libraries() = 0;

    /// The identities of `system` in `library`, in (key, relative path) order: what the import matches the snapshot's
    /// entries against.
    [[nodiscard]] virtual Expected<std::vector<StoredIdentity>> identities(const LibraryId& library,
                                                                           const SystemName& system) = 0;
    /// The identity with the GameId `id`, of any library; nothing when there is none.
    [[nodiscard]] virtual Expected<std::optional<StoredIdentity>> identity(GameId id) = 0;
    /// Every GameId already given out, by identities and aliases of every library, ascending: the taken set of
    /// assignGameIds(). A GameId is never given out twice, even once its identity is an alias.
    [[nodiscard]] virtual Expected<std::vector<GameId>> takenGameIds() = 0;

    /// The index rows of `system` in `library`, in the snapshot's entry order (entryOrder()).
    [[nodiscard]] virtual Expected<std::vector<IndexedEntry>> indexedEntries(const LibraryId& library,
                                                                             const SystemName& system) = 0;

    /// The systems of `library`'s index, by name (ordinal).
    [[nodiscard]] virtual Expected<std::vector<SystemInfo>> systems(const LibraryId& library) = 0;
    /// The systems that `library`'s identities belong to, by name (ordinal): also those whose index holds nothing now.
    [[nodiscard]] virtual Expected<std::vector<SystemName>> identitySystems(const LibraryId& library) = 0;
    /// The disc groups of `library`'s index, by system then head (ordinal): what the next load takes as its cache.
    [[nodiscard]] virtual Expected<std::vector<IndexedDiscGroup>> discGroups(const LibraryId& library) = 0;
    /// The record of `library`'s last import, nothing when none was recorded.
    [[nodiscard]] virtual Expected<std::optional<ImportRecord>> lastImport(const LibraryId& library) = 0;

    /// The history of the identity `game`, oldest first (the history of the identities that became its aliases
    /// included: putAlias() moves it).
    [[nodiscard]] virtual Expected<std::vector<IdentityEvent>> history(GameId game) = 0;
    /// The aliases that stand for the identity `game`, by alias GameId.
    [[nodiscard]] virtual Expected<std::vector<StoredAlias>> aliasesOf(GameId game) = 0;
    /// The alias `alias`; nothing when that GameId is no alias.
    [[nodiscard]] virtual Expected<std::optional<StoredAlias>> alias(GameId alias) = 0;
    /// The review rows of `library` (those of its identities), by id; only the open ones when `openOnly`.
    [[nodiscard]] virtual Expected<std::vector<StoredReview>> reviews(const LibraryId& library, bool openOnly) = 0;
    /// The review row `id`, of any library; nothing when there is none.
    [[nodiscard]] virtual Expected<std::optional<StoredReview>> review(std::int64_t id) = 0;

    /// Takes the import lock without waiting and holds it until the returned object goes, apart from any transaction:
    /// an import holds it while it reads the library (seconds, minutes on a cold drive) and the store, and begins its
    /// write transaction only once it knows what to write, so that other writers (play sessions) wait only for its
    /// writes. beginImport() of this instance then uses it. Errors: Conflict when another import holds it (this instance's own
    /// lock or transaction included).
    [[nodiscard]] virtual Expected<std::unique_ptr<IImportLock>> lockImport(const LibraryId& library) = 0;
    /// Begins the import of `library`, taking the import lock unless this instance's lockImport() holds it. Errors:
    /// Conflict when another import holds it (this instance's own open transaction included), IoError or Unavailable
    /// when the database cannot be written.
    [[nodiscard]] virtual Expected<std::unique_ptr<IImportTransaction>> beginImport(const LibraryId& library) = 0;
};

} // namespace rv::catalog
