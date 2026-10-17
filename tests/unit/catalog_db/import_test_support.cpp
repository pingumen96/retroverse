#include "import_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog_db/migrator.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::test {

namespace fs = std::filesystem;
using catalog_db::ColumnType;
using catalog_db::Connection;

ImportCatalog ImportCatalog::open(const fs::path& file, const catalog_db::StoreOptions& options) {
    auto database = catalog_db::openCatalogDatabase(file);
    INFO((database ? std::string("opened") : database.error().toString()));
    REQUIRE(database.has_value());
    ImportCatalog catalog;
    catalog.database = std::make_unique<catalog_db::Database>(std::move(*database));
    auto store = catalog_db::SqliteCatalogStore::open(*catalog.database, options);
    REQUIRE(store.has_value());
    catalog.store = std::move(*store);
    return catalog;
}

void ImportCatalog::close() {
    store.reset();
    database.reset();
}

namespace {

/// The rows `sql` returns, each its columns as text joined by U+001F, a NULL written "\x01" (an empty text stays empty).
std::vector<std::string> rowsOf(Connection& connection, std::string_view sql) {
    auto statement = connection.prepare(sql);
    INFO(sql);
    REQUIRE(statement.has_value());
    std::vector<std::string> rows;
    while (true) {
        const auto next = statement->step();
        REQUIRE(next.has_value());
        if (!*next) {
            break;
        }
        std::string row;
        for (int column = 0; column < statement->columnCount(); ++column) {
            if (column > 0) {
                row += '\x1f';
            }
            if (statement->columnType(column) == ColumnType::Null) {
                row += '\x01';
            } else {
                row += statement->columnText(column);
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

TableDump dumpIndex(Connection& connection) {
    const std::array<std::pair<std::string_view, std::string_view>, 7> kTables{{
        {"idx_systems", "SELECT * FROM idx_systems ORDER BY library, name"},
        {"idx_games", "SELECT * FROM idx_games ORDER BY id"},
        {"idx_media", "SELECT * FROM idx_media ORDER BY game, kind"},
        {"idx_disc_groups", "SELECT * FROM idx_disc_groups ORDER BY head"},
        {"idx_disc_members", "SELECT * FROM idx_disc_members ORDER BY head, position"},
        {"idx_disc_references", "SELECT * FROM idx_disc_references ORDER BY head, position"},
        {"idx_import_issues",
         "SELECT library, severity, code, system, file, reason FROM idx_import_issues ORDER BY library, id"},
    }};
    TableDump dump;
    for (const auto& [table, sql] : kTables) {
        dump[std::string(table)] = rowsOf(connection, sql);
    }
    REQUIRE(connection.execute("CREATE VIRTUAL TABLE IF NOT EXISTS temp.import_test_terms USING fts5vocab(main, "
                               "idx_game_search, instance)")
                .has_value());
    dump["idx_game_search"] = rowsOf(connection, "SELECT term, doc FROM temp.import_test_terms ORDER BY term, doc");
    REQUIRE(connection.execute("DROP TABLE temp.import_test_terms").has_value());
    return dump;
}

TableDump dumpState(Connection& connection) {
    const std::array<std::pair<std::string_view, std::string_view>, 6> kTables{{
        {"rv_meta", "SELECT * FROM rv_meta ORDER BY key"},
        {"rv_libraries", "SELECT * FROM rv_libraries ORDER BY id"},
        {"rv_game_identities", "SELECT * FROM rv_game_identities ORDER BY id"},
        {"rv_identity_history", "SELECT * FROM rv_identity_history ORDER BY id"},
        {"rv_identity_aliases", "SELECT * FROM rv_identity_aliases ORDER BY id"},
        {"rv_identity_reviews", "SELECT * FROM rv_identity_reviews ORDER BY id"},
    }};
    TableDump dump;
    for (const auto& [table, sql] : kTables) {
        dump[std::string(table)] = rowsOf(connection, sql);
    }
    return dump;
}

std::map<std::string, std::string> digestOf(const TableDump& dump) {
    std::map<std::string, std::string> digests;
    for (const auto& [table, rows] : dump) {
        core::Sha256 hash;
        for (const std::string& row : rows) {
            hash.update(row);
            hash.update(std::string_view("\n", 1));
        }
        digests[table] = fmt::format("{} rows, {}", rows.size(), core::toHex(hash.finish()));
    }
    return digests;
}

void checkSameDump(const TableDump& expected, const TableDump& actual) {
    std::vector<std::string> expectedTables;
    std::vector<std::string> actualTables;
    for (const auto& [table, rows] : expected) {
        expectedTables.push_back(table);
    }
    for (const auto& [table, rows] : actual) {
        actualTables.push_back(table);
    }
    REQUIRE(expectedTables == actualTables);
    for (const auto& [table, rows] : expected) {
        const std::vector<std::string>& other = actual.at(table);
        INFO(table);
        CHECK(rows.size() == other.size());
        const auto [mine, theirs] = std::mismatch(rows.begin(), rows.end(), other.begin(), other.end());
        const std::string expectedRow = mine == rows.end() ? std::string("(none)") : *mine;
        const std::string actualRow = theirs == other.end() ? std::string("(none)") : *theirs;
        INFO("first difference: expected " << expectedRow << " | actual " << actualRow);
        CHECK(rows == other);
    }
}

Expected<catalog::LibrarySnapshot> ScriptedSource::load(const catalog::LibraryProfile& profile,
                                                       const catalog::LibraryLoadOptions& options) {
    ++loads;
    lastCache.assign(options.discGroups.begin(), options.discGroups.end());
    if (onLoad) {
        onLoad();
    }
    if (options.cancel != nullptr && options.cancel->load()) {
        return makeError(ErrorCode::Cancelled, "the load was cancelled");
    }
    catalog::LibrarySnapshot copy = snapshot;
    copy.profile = profile;
    return copy;
}

Expected<catalog::EntryMedia> ScriptedMedia::presence(const catalog::SystemName& system, const catalog::LookupKey& key,
                                                      bool) {
    ++asked;
    if (onPresence) {
        onPresence();
    }
    if (unavailable || std::find(unavailableSystems.begin(), unavailableSystems.end(), system.str()) !=
                           unavailableSystems.end()) {
        return makeError(ErrorCode::Unavailable, "the media root cannot be reached", system.str());
    }
    const auto found = media.find(system.str() + "/" + key.relativePath());
    return found == media.end() ? catalog::EntryMedia{} : found->second;
}

Expected<fs::path> ScriptedMedia::path(const catalog::SystemName& system, const catalog::LookupKey& key,
                                       bool directory, catalog::MediaKind kind,
                                       const catalog::MediaPresence& presence) const {
    if (!presence.present) {
        return makeError(ErrorCode::NotFound, "no such media", key.relativePath());
    }
    return fs::path(L"M:\\media") / core::pathFromUtf8(system.str()) / core::pathFromUtf8(catalog::toString(kind)) /
           core::pathFromUtf8(catalog::mediaStem(key.relativePath(), directory) + presence.extension);
}

catalog::LibraryProfile scriptedLibrary(std::string_view id) {
    catalog::LibraryProfile profile;
    profile.id = catalog::LibraryId::parse(id).value();
    profile.esdeHome = fs::path(L"C:\\Users\\player\\ES-DE");
    profile.installFolder = fs::path(L"C:\\Programs\\ES-DE");
    return profile;
}

catalog::SystemInfo scriptedSystem(std::string_view name) {
    catalog::SystemInfo system;
    system.name = systemName(name);
    system.fullName = fmt::format("System {}", name);
    system.platforms = {std::string(name)};
    system.theme = std::string(name);
    system.romFolderConfigured = fmt::format("%ROMPATH%\\{}", name);
    system.romFolder = fs::path(L"R:\\roms") / core::pathFromUtf8(name);
    system.gamelist = catalog::GamelistState::Read;
    return system;
}

catalog::SystemName systemName(std::string_view name) {
    auto parsed = catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

catalog::LookupKey lookupKey(std::string_view relativePath) {
    auto key = catalog::makeLookupKey(relativePath);
    INFO(relativePath);
    REQUIRE(key.has_value());
    return std::move(*key);
}

catalog::CatalogEntry diskGame(std::string_view system, std::string_view relativePath, std::uint64_t size) {
    catalog::CatalogEntry entry;
    entry.system = systemName(system);
    entry.key = lookupKey(relativePath);
    entry.state = catalog::EntryState::Game;
    entry.file = catalog::FileFacts{false, size, 126'000'000'000'000'000 + static_cast<std::int64_t>(size), false};
    entry.title = catalog::titleFromFileStem(relativePath);
    return entry;
}

catalog::CatalogEntry missingGame(std::string_view system, std::string_view relativePath, std::string_view title) {
    catalog::CatalogEntry entry;
    entry.system = systemName(system);
    entry.key = lookupKey(relativePath);
    entry.state = catalog::EntryState::MissingFile;
    entry.availability = catalog::Availability::Missing;
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = std::string(title);
    entry.metadata.name = std::string(title);
    return entry;
}

catalog::CatalogEntry unlistedGame(std::string_view system, std::string_view relativePath,
                                   catalog::SourceUnavailableReason reason) {
    catalog::CatalogEntry entry;
    entry.system = systemName(system);
    entry.key = lookupKey(relativePath);
    entry.state = catalog::EntryState::Game;
    entry.availability = catalog::Availability::SourceUnavailable;
    entry.unavailableReason = reason;
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = catalog::titleFromFileStem(relativePath);
    entry.metadata.name = entry.title;
    return entry;
}

void sortEntries(catalog::LibrarySnapshot& snapshot) {
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), catalog::entryOrder);
}

catalog::ImportResult importOk(ImportCatalog& catalog, catalog::ILibrarySource& source, catalog::IMediaResolver& media,
                               const catalog::LibraryProfile& profile, const catalog::ImportOptions& options,
                               catalog::IIdentityPairing* pairing) {
    catalog::ImportService service(source, media, *catalog.store, pairing);
    auto result = service.run(profile, options);
    INFO((result ? std::string("imported") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

const catalog::SystemChanges& changesOf(const catalog::ChangeSet& changes, std::string_view system) {
    const catalog::SystemChanges* found = changes.findSystem(systemName(system));
    INFO(system);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace rv::test
