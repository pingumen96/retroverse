#pragma once

// Helpers of the rv_catalog_db tests: catalog values with every field set, an entry's identity and GameId, test
// migrations after the program's, and the header of a database file read from its bytes, without opening it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/migrator.hpp>

#include "support/temp_dir.hpp"

namespace rv::test {

/// "<dir>/data/retroverse.sqlite": a database file in a folder that does not exist yet.
[[nodiscard]] std::filesystem::path databaseFileIn(const TempDir& dir);

/// A library with every folder set but the ROM root (paths outside ASCII among them).
[[nodiscard]] catalog::LibraryProfile testLibrary(std::string_view id = "esde");

/// A system of the library, its ROM folder `romFolder`.
[[nodiscard]] catalog::SystemInfo testSystem(std::string_view name, const std::filesystem::path& romFolder);

/// A scraped game with every field set: file facts, metadata, raw values, alternative emulator, ES-DE flags.
[[nodiscard]] catalog::CatalogEntry fullEntry(std::string_view system, std::string_view relativePath,
                                              std::string_view title);

/// A file without metadata (an unscraped game): its title from its name, its file facts.
[[nodiscard]] catalog::CatalogEntry bareEntry(std::string_view system, std::string_view relativePath);

/// The entry as the index holds it under `id`: its sort key and, when `withMedia`, a cover (.png) and a video (.mp4).
[[nodiscard]] catalog::IndexedEntry indexedEntry(catalog::GameId id, catalog::CatalogEntry entry,
                                                 bool withMedia = true);

/// The identity of `entry` in `library`: present when it has file facts, the current lookup key version.
[[nodiscard]] catalog::StoredIdentity identityOf(catalog::GameId id, const catalog::LibraryId& library,
                                                 const catalog::CatalogEntry& entry);

/// The GameId the entry's tuple gives with `counter`.
[[nodiscard]] catalog::GameId gameIdOf(const catalog::LibraryId& library, const catalog::CatalogEntry& entry,
                                       std::uint32_t counter = 0);

/// The program's migrations, then test migrations up to `last`: version n creates the table test_v<n>.
[[nodiscard]] std::vector<catalog_db::Migration> migrationsUpTo(int last);

/// What a database file's header says.
struct DatabaseHeader {
    std::uint32_t userVersion = 0;
    std::int32_t applicationId = 0;
    /// The file format's write version: 1 a rollback journal, 2 WAL.
    int writeVersion = 0;
};

/// The header of the database file `file`, read from its first 100 bytes (any path length) without opening it as a
/// database; nothing when it cannot be read or is no SQLite file. In WAL mode the file lags behind the WAL until a
/// checkpoint: read databases that are closed, and backups.
[[nodiscard]] std::optional<DatabaseHeader> readDatabaseHeader(const std::filesystem::path& file);

/// True when `path` exists (any path length).
[[nodiscard]] bool existsAnyLength(const std::filesystem::path& path);

} // namespace rv::test
