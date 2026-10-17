#pragma once

// Port ICatalogStore (ADR-0008 §4, §5): what the import (I2 step 11) needs from the database, implemented by
// rv_catalog_db (SqliteCatalogStore, step 4). The store holds state tables (rv_*: libraries, identities and what only
// Retroverse knows) and index tables (idx_*: a function of the snapshot and the identities, rebuildable at any time).
//
// At I2 step 1: the identity lookups, the GameIds already taken, the index rows of a system for the import's
// comparison, and one write transaction per import for libraries, identities, systems and entries. Step 11a (the
// import, ImportService) adds: the systems and disc groups the index holds (IndexedDiscGroup: the cache of the next
// load, LibraryLoadOptions::discGroups, with the GameIds of the head and the members), the systems of the identities,
// the import lock held apart from the transaction (lockImport(): an import holds it while it reads the library, before
// its write transaction), and in the transaction: the bulk write hint (prepareWrites()), the index cleared (`db
// rebuild-index`), systems and disc groups written and removed, the import's issues, and the import records kept for
// the user (ImportRecord). Step 11b adds what reconciliation needs (history, aliases, review rows, quick hashes); play
// sessions have their own store (rv_catalog_db's session store, step 12).
//
// Reads and the import transaction: an instance has one connection. Its reads made while one of its import
// transactions is open see that transaction's writes; other connections and processes see them only once committed.
// The import therefore begins its transaction, which takes the import lock (or uses the one lockImport() holds),
// before it reads the identities, the taken GameIds and the index rows it compares with the snapshot: nothing can
// change them between its reads and its writes.

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

/// A disc group as the index holds it (ADR-0008 §3): the group (its head, kind, members and references by key) with
/// the size and last write time its head had when it was read, which the next load takes back as its cache
/// (LibraryLoadOptions::discGroups), and the GameIds of the head and of the members. The references are kept by
/// relative path, since they include files that are not there (and have no identity).
struct IndexedDiscGroup {
    /// The head's GameId.
    GameId head;
    CachedDiscGroup cached;
    /// The members' GameIds, in the order of cached.group.members.
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

    /// Takes the import lock without waiting and holds it until the returned object goes, apart from any transaction:
    /// an import holds it while it reads the library (seconds, minutes on a cold drive), and begins its write
    /// transaction only once it has the snapshot, so that other writers (play sessions) wait only for its writes.
    /// beginImport() of this instance then uses it. Errors: Conflict when another import holds it (this instance's own
    /// lock or transaction included).
    [[nodiscard]] virtual Expected<std::unique_ptr<IImportLock>> lockImport(const LibraryId& library) = 0;
    /// Begins the import of `library`, taking the import lock unless this instance's lockImport() holds it. Errors:
    /// Conflict when another import holds it (this instance's own open transaction included), IoError or Unavailable
    /// when the database cannot be written.
    [[nodiscard]] virtual Expected<std::unique_ptr<IImportTransaction>> beginImport(const LibraryId& library) = 0;
};

} // namespace rv::catalog
