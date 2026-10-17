#pragma once

// What the import tests (I2 step 11a) share: a catalog database opened with its store, the index and state tables
// dumped as text (what two imports compare, a rebuild against a refresh), a library source and a media resolver that
// give what a test scripts, and catalog values built by hand.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>

namespace rv::test {

/// A catalog database opened (made and migrated) with its store, as an import runs on it.
struct ImportCatalog {
    std::unique_ptr<catalog_db::Database> database;
    std::unique_ptr<catalog_db::SqliteCatalogStore> store;

    /// openCatalogDatabase() and SqliteCatalogStore::open(), REQUIREd.
    [[nodiscard]] static ImportCatalog open(const std::filesystem::path& file,
                                            const catalog_db::StoreOptions& options = {});
    /// Closes the store, then the database.
    void close();
};

/// Tables of the catalog as text: per table, its rows in a fixed order, each its columns joined (a NULL apart from an
/// empty text). The index: idx_systems, idx_games, idx_media, idx_disc_groups, idx_disc_members, idx_disc_references,
/// idx_import_issues (without its row ids) and the search index's terms with their rows (fts5vocab).
using TableDump = std::map<std::string, std::vector<std::string>>;
[[nodiscard]] TableDump dumpIndex(catalog_db::Connection& connection);
/// The state: rv_meta, rv_libraries, rv_game_identities and the reconciliation tables (rv_imports left out: every
/// import adds a row).
[[nodiscard]] TableDump dumpState(catalog_db::Connection& connection);
/// Per table, its row count and the SHA-256 of its rows (large dumps compared in a message).
[[nodiscard]] std::map<std::string, std::string> digestOf(const TableDump& dump);
/// CHECKs that two dumps hold the same tables with the same rows, table by table, naming the first row that differs.
void checkSameDump(const TableDump& expected, const TableDump& actual);

/// Sets the last write time of `root` and of every folder below it (the deepest first) to one fixed time. Windows lists
/// a folder's time from its parent folder's index, a copy of the folder's own that a freshly made tree brings up to date
/// later, on its own (seconds after the folder's last change, or when the parent changes): two imports a moment apart
/// may then see two times for one folder (a Folder entry's file facts). A time set through a handle is written to both
/// at once, so that the fixture tests compare imports exactly.
void settleFolderTimes(const std::filesystem::path& root);

/// A library source that gives the snapshot a test scripts.
class ScriptedSource final : public catalog::ILibrarySource {
public:
    catalog::LibrarySnapshot snapshot;
    /// The cache the last load was given (LibraryLoadOptions::discGroups).
    std::vector<catalog::DiscGroup> lastCache;
    /// Called at each load, before the snapshot is given (to cancel, say).
    std::function<void()> onLoad;
    std::size_t loads = 0;

    Expected<catalog::LibrarySnapshot> load(const catalog::LibraryProfile& profile,
                                            const catalog::LibraryLoadOptions& options) override;
};

/// A media resolver with the media a test scripts, by "system/relative path"; Unavailable for every entry while
/// `unavailable`, or for the systems listed in `unavailableSystems`.
class ScriptedMedia final : public catalog::IMediaResolver {
public:
    std::map<std::string, catalog::EntryMedia> media;
    bool unavailable = false;
    std::vector<std::string> unavailableSystems;
    /// Called at each presence() (to cancel, say).
    std::function<void()> onPresence;
    std::size_t asked = 0;
    /// refresh() calls (the import refreshes the resolver at the start of every run).
    std::size_t refreshes = 0;

    Expected<catalog::EntryMedia> presence(const catalog::SystemName& system, const catalog::LookupKey& key,
                                           bool directory) override;
    Expected<std::filesystem::path> path(const catalog::SystemName& system, const catalog::LookupKey& key,
                                         bool directory, catalog::MediaKind kind,
                                         const catalog::MediaPresence& media) const override;
    void refresh() noexcept override { ++refreshes; }
};

/// A library whose folders are invented (nothing is read: the scripted source gives the snapshot).
[[nodiscard]] catalog::LibraryProfile scriptedLibrary(std::string_view id = "esde");
/// A system with a ROM folder under an invented root, Available, its gamelist read.
[[nodiscard]] catalog::SystemInfo scriptedSystem(std::string_view name);
/// SystemName::parse() REQUIREd.
[[nodiscard]] catalog::SystemName systemName(std::string_view name);
/// makeLookupKey() REQUIREd.
[[nodiscard]] catalog::LookupKey lookupKey(std::string_view relativePath);
/// A game on disk: its file facts (`size`, a fixed time), titled from its name, no gamelist entry.
[[nodiscard]] catalog::CatalogEntry diskGame(std::string_view system, std::string_view relativePath,
                                             std::uint64_t size = 1024);
/// A gamelist entry whose file is gone: MissingFile, Missing, no file facts, titled `title`.
[[nodiscard]] catalog::CatalogEntry missingGame(std::string_view system, std::string_view relativePath,
                                                std::string_view title);
/// What the gamelist alone tells of an entry under a folder that cannot be listed: Game, SourceUnavailable.
[[nodiscard]] catalog::CatalogEntry unlistedGame(std::string_view system, std::string_view relativePath,
                                                 catalog::SourceUnavailableReason reason);
/// The snapshot's entries sorted in entryOrder().
void sortEntries(catalog::LibrarySnapshot& snapshot);

/// ImportService::run() on the catalog with `source` and `media`, REQUIREd to succeed.
catalog::ImportResult importOk(ImportCatalog& catalog, catalog::ILibrarySource& source,
                               catalog::IMediaResolver& media, const catalog::LibraryProfile& profile,
                               const catalog::ImportOptions& options = {},
                               catalog::IIdentityPairing* pairing = nullptr);

/// The figures of `system` in a change set, REQUIREd to be there.
[[nodiscard]] const catalog::SystemChanges& changesOf(const catalog::ChangeSet& changes, std::string_view system);

} // namespace rv::test
