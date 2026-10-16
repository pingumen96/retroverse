#pragma once

// Port ICatalogStore (ADR-0008 §4, §5): what the import (I2 step 11) needs from the database, implemented by
// rv_catalog_db (SqliteCatalogStore, step 4). The store holds state tables (rv_*: libraries, identities and what only
// Retroverse knows) and index tables (idx_*: a function of the snapshot and the identities, rebuildable at any time).
//
// Minimal at I2 step 1, as the import needs it first: the identity lookups, the GameIds already taken, the index rows
// of a system for the import's comparison, and one write transaction per import for libraries, identities, systems and
// entries. Step 11 adds what reconciliation needs (history, aliases, review rows, quick hashes, import records) and the
// disc groups of each import with their heads' sizes and times (CachedDiscGroup), which the next load takes back
// (LibraryLoadOptions::discGroups); play sessions have their own store (rv_catalog_db's session store, step 12).
//
// Reads and the import transaction: an instance has one connection. Its reads made while one of its import
// transactions is open see that transaction's writes; other connections and processes see them only once committed.
// The import therefore begins its transaction, which takes the import lock, before it reads the identities, the taken
// GameIds and the index rows it compares with the snapshot: nothing can change them between its reads and its writes.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
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
    /// Removes the index rows of `id`; its identity stays (and comes back with its id when the file does).
    [[nodiscard]] virtual Status removeEntry(GameId id) = 0;
    /// Makes every write visible to other connections at once, releases the import lock and ends the transaction
    /// (later calls fail with InvalidArgument).
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

    /// Begins the import of `library`, taking the import lock. Errors: Conflict when another import holds it (this
    /// instance's own open transaction included), IoError or Unavailable when the database cannot be written.
    [[nodiscard]] virtual Expected<std::unique_ptr<IImportTransaction>> beginImport(const LibraryId& library) = 0;
};

} // namespace rv::catalog
