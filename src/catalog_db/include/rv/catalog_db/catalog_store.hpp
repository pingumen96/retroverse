#pragma once

// SqliteCatalogStore: rv_catalog's ICatalogStore (rv/catalog/catalog_store.hpp) on the catalog database (ADR-0008 §5).
//
// It reads and writes, through the database's one writer connection, the state tables rv_libraries and
// rv_game_identities (rv_identity_aliases for the GameIds taken) and the index tables idx_systems, idx_games and
// idx_media (schema.hpp, schema_identity.cpp, schema_index.cpp). Values are written and read back unchanged; a lookup
// key is stored with its exact relative path and computed again from it when read (catalog::makeLookupKey), and the
// order of a read is the port's, whatever the stored keys' casing table.
//
// The import transaction is one SQLite write transaction (BEGIN IMMEDIATE) on the writer: the store's own reads, on the
// same connection, see its writes at once; readers (Database::openReader()) and other processes see them only once it
// commits; a transaction dropped without commit() rolls back. It holds the import lock (DatabaseLock::lockImport()), so
// that a second beginImport(), on this store, another store or another process, gets Conflict at once instead of
// waiting. A refused write leaves it open and working; when SQLite rolls the whole transaction back on its own (a full
// disk, an I/O error, an interrupt), it ends there and every later call fails (InvalidArgument), so that nothing is
// ever written outside it. Commits run a WAL checkpoint (ADR-0008 §5: a checkpoint after each import) that waits
// briefly for readers and is otherwise left to the next one. Writes are upserts (INSERT ... ON CONFLICT DO UPDATE),
// never REPLACE, so that triggers on the index tables (the search index, I2 step 9) see updates as updates.
//
// Besides the port's rules, the transaction refuses: in putIdentity(), an identity without a GameId, system or key, of
// an unknown library (InvalidArgument), or with the key and relative path of another identity (Conflict); in
// putEntry(), an entry of another system than its identity's, an absent media kind with an extension, a count or file
// size beyond 2^63 - 1, a release date or last played time that is none, raw values that are not UTF-8
// (InvalidArgument), a rating beyond 10 or players below 1 (ValidationFailed, the schema's checks); in putSystem(), an
// unknown library (InvalidArgument). removeEntry() removes the entry's idx_games and idx_media rows and the disc group
// it heads (a group it is a member of is its head's and stays as it was read); for an id without index rows it does
// nothing.
//
// rv_meta's rows: schema_created_at and schema_created_by (the migration that made the schema); lookup_key_rule and
// casing_table_version, the lookup key version of every identity's key, recorded by each commit ("mixed" while the
// identities hold keys of several versions, a re-keying not finished; the versions of the system that made the schema
// until an identity exists); sort_key_version, the version of idx_games' sort keys (catalog::kSortKeyVersion).

#include <memory>
#include <optional>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db {

class Database;

class SqliteCatalogStore final : public catalog::ICatalogStore {
public:
    /// The store on `database`'s writer connection. `database` must outlive the store; an import transaction must not
    /// outlive its store (one still open when the store goes is rolled back, and fails every later call). Errors:
    /// Conflict when the database's schema version is not programSchemaVersion() (older: migrate() it first; newer:
    /// written by a newer Retroverse), and SQLite's.
    [[nodiscard]] static Expected<std::unique_ptr<SqliteCatalogStore>> open(Database& database);

    SqliteCatalogStore(const SqliteCatalogStore&) = delete;
    SqliteCatalogStore& operator=(const SqliteCatalogStore&) = delete;
    ~SqliteCatalogStore() override;

    /// By id.
    [[nodiscard]] Expected<std::vector<catalog::LibraryProfile>> libraries() override;
    [[nodiscard]] Expected<std::vector<catalog::StoredIdentity>> identities(const catalog::LibraryId& library,
                                                                            const catalog::SystemName& system) override;
    [[nodiscard]] Expected<std::optional<catalog::StoredIdentity>> identity(catalog::GameId id) override;
    [[nodiscard]] Expected<std::vector<catalog::GameId>> takenGameIds() override;
    [[nodiscard]] Expected<std::vector<catalog::IndexedEntry>>
    indexedEntries(const catalog::LibraryId& library, const catalog::SystemName& system) override;
    /// Errors: Conflict (this store's transaction is open; another holder of the database's lock imports; another
    /// connection writes past the writer's busy timeout), SQLite's others.
    [[nodiscard]] Expected<std::unique_ptr<catalog::IImportTransaction>>
    beginImport(const catalog::LibraryId& library) override;

private:
    struct State;
    class ImportTransaction;

    explicit SqliteCatalogStore(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace rv::catalog_db
