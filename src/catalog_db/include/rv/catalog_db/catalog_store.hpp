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
// waiting. A refused write leaves it open and working, as it was before the write: a write of several statements
// (putEntry(), removeEntry()) runs in a savepoint, undone whole when SQLite refuses one of them. When SQLite rolls the
// whole transaction back on its own (a full disk, an I/O error, an interrupt), it ends there and every later call fails
// (InvalidArgument), so that nothing is ever written outside it. Commits run a WAL checkpoint (ADR-0008 §5: a
// checkpoint after each import) that waits briefly for readers and is otherwise left to the next one; a commit that
// wrote only reconciliation's state rows (quick hashes, history, review rows: the quick-hash pass's batches) is light,
// with neither the checkpoint (SQLite's automatic one and the next import's do it) nor rv_meta's versions. Writes are
// upserts (INSERT ... ON CONFLICT DO UPDATE), never REPLACE, so that triggers on the index tables (the search index, I2
// step 9) see updates as updates.
//
// Besides the port's rules, the transaction refuses: in putIdentity(), an identity without a GameId, system or key, of
// an unknown library (InvalidArgument), or with the relative path of another identity of its system (Conflict); in
// putEntry(), an entry of another system than its identity's, an absent media kind with an extension, a count or file
// size beyond 2^63 - 1, a release date or last played time that is none, raw values that are not UTF-8
// (InvalidArgument), a rating beyond 10 or players below 1 (ValidationFailed, the schema's checks); in putSystem(), an
// unknown library (InvalidArgument); in putDiscGroup(), a head or member without an identity, a member count that
// differs from the group's keys (InvalidArgument). removeEntry() removes the entry's idx_games and idx_media rows and
// the disc group it heads, with its members and references (a group it is a member of is its head's and stays as it
// was read); for an id without index rows it does nothing. putAlias() also removes the alias's place among the members
// of other groups (the import writes those groups again), inserts the rv_identity_aliases row and deletes the identity
// (a FOREIGN KEY error when a state row still names it).
//
// rv_meta's rows: schema_created_at and schema_created_by (the migration that made the schema); lookup_key_rule and
// casing_table_version, the lookup key version of every identity's key, recorded by each commit ("mixed" while the
// identities hold keys of several versions, a re-keying not finished; the versions of the system that made the schema
// until an identity exists); sort_key_version, the version of idx_games' sort keys (catalog::kSortKeyVersion), recorded
// by each commit too (but the light ones, which write no key).
//
// The import's writes (I2 step 11a): disc groups in idx_disc_groups, idx_disc_members (by GameId) and
// idx_disc_references (by relative path: what the head says, files that are not there included), read back joined to
// the identities of their heads and members; the import issues (idx_import_issues); the import records (rv_imports,
// the per-system figures as JSON, of the systems the import recorded; the newest kKeptImportRecords per library are
// kept, recordImport() deleting older ones). lockImport() takes the import lock apart from a transaction (byte 2 of
// the database's lock file, as beginImport() does), so that an import holds it while it reads the library and the
// store and begins its write transaction (BEGIN IMMEDIATE) only for the writes. prepareWrites() suspends the search
// index (search_index.hpp) for a bulk write, as StoreOptions::searchIndex says, and commit() rebuilds it.
//
// Reconciliation (I2 step 11b): an identity is read and written with its quick hash (rv_game_identities' quick_hash,
// quick_hash_size, quick_hash_time; putQuickHash() writes those alone); the history (rv_identity_history), the review
// rows (rv_identity_reviews, read with their identity's system: a review is of its identity's library and system) and
// the aliases (rv_identity_aliases) are written and read. putAlias() first resolves the holder's open review rows
// (Aliased), then moves every state row that names the holder to the identity it stands for: the columns of the state
// tables (rv_*, never idx_*) with a single-column foreign key into rv_game_identities are found in the schema
// (pragma_foreign_key_list over sqlite_schema, once per store), so that the tables later steps add (play sessions,
// launch choices) are covered without this store knowing them; for each, an UPDATE OR IGNORE then a DELETE of what is
// left: a table that holds one row per game keeps the row the moved identity has and drops the holder's (the moved
// identity's choice wins), every other row follows. Aliases that stood for the holder thus stand for the moved identity
// (aliases never chain).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db {

class Database;

/// When an import transaction suspends the search index for its writes (search_index.hpp).
enum class SearchIndexPolicy : std::uint8_t {
    /// For a first import (the index holds no entry, of any library), or when the rows the import writes are at least
    /// a fifth of the index (kBulkWriteShare): FTS5 flushes a segment per row written through the triggers (about 75
    /// µs a row), its rebuild tokenizes the whole catalog once (1.1 to 1.7 s per 100,000 entries, docs/i2/step-9.md).
    Automatic,
    /// Whenever the import writes rows (tests).
    Suspend,
    /// Never: the triggers index the rows one by one (tests, the comparison of both ways).
    FollowWrites,
};

/// SearchIndexPolicy::Automatic suspends the search index when rows written * kBulkWriteShare >= the index's entries.
inline constexpr std::size_t kBulkWriteShare = 5;

/// The import records (rv_imports) kept per library: recordImport() deletes the older ones, so that a catalog imported
/// at every start of the app keeps a bounded history.
inline constexpr std::size_t kKeptImportRecords = 100;

/// How a store writes.
struct StoreOptions {
    SearchIndexPolicy searchIndex = SearchIndexPolicy::Automatic;
};

class SqliteCatalogStore final : public catalog::ICatalogStore {
public:
    /// The store on `database`'s writer connection. `database` must outlive the store; an import transaction, and an
    /// import lock, must not outlive its store (a transaction still open when the store goes is rolled back, and fails
    /// every later call). Errors: Conflict when the database's schema version is not programSchemaVersion() (older:
    /// migrate() it first; newer: written by a newer Retroverse), and SQLite's.
    [[nodiscard]] static Expected<std::unique_ptr<SqliteCatalogStore>> open(Database& database,
                                                                          const StoreOptions& options = {});

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
    [[nodiscard]] Expected<std::vector<catalog::SystemInfo>> systems(const catalog::LibraryId& library) override;
    [[nodiscard]] Expected<std::vector<catalog::SystemName>>
    identitySystems(const catalog::LibraryId& library) override;
    [[nodiscard]] Expected<std::vector<catalog::IndexedDiscGroup>>
    discGroups(const catalog::LibraryId& library) override;
    [[nodiscard]] Expected<std::optional<catalog::ImportRecord>> lastImport(const catalog::LibraryId& library) override;
    [[nodiscard]] Expected<std::vector<catalog::IdentityEvent>> history(catalog::GameId game) override;
    [[nodiscard]] Expected<std::vector<catalog::StoredAlias>> aliasesOf(catalog::GameId game) override;
    [[nodiscard]] Expected<std::optional<catalog::StoredAlias>> alias(catalog::GameId alias) override;
    [[nodiscard]] Expected<std::vector<catalog::StoredReview>> reviews(const catalog::LibraryId& library,
                                                                       bool openOnly) override;
    [[nodiscard]] Expected<std::optional<catalog::StoredReview>> review(std::int64_t id) override;
    [[nodiscard]] Expected<std::vector<catalog::StoredReview>> reviewsOf(catalog::GameId game) override;
    /// Errors: Conflict (this store's lock or transaction is held; another holder of the database's lock imports).
    [[nodiscard]] Expected<std::unique_ptr<catalog::IImportLock>> lockImport(const catalog::LibraryId& library) override;
    /// Errors: Conflict (this store's transaction is open; another holder of the database's lock imports; another
    /// connection writes past the writer's busy timeout), SQLite's others.
    [[nodiscard]] Expected<std::unique_ptr<catalog::IImportTransaction>>
    beginImport(const catalog::LibraryId& library) override;

private:
    struct State;
    class ImportTransaction;
    class ImportLock;

    explicit SqliteCatalogStore(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace rv::catalog_db
