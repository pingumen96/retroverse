#pragma once

// Private to rv_cli_core (I2 step 9): what `systems`, `games`, `search` and `show` share: the catalog database opened
// for reading, the delivery of results, and catalog values as text and as JSON. Defined in cmd_games.cpp, which also
// runs `games` and `search` (runGameList()).

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/expected.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::cli {

using CatalogJson = nlohmann::ordered_json;

/// The catalog database of the user data directory, opened for reading, and its queries on a reader connection.
struct CatalogReader {
    std::unique_ptr<catalog_db::Database> database;
    std::unique_ptr<catalog_db::SqliteCatalogQuery> query;
};

/// Opens <user data>/database/retroverse.sqlite for reading: never made, migrated or switched to WAL (nothing is
/// written into it), its queries on a read-only connection. Errors: NotFound (no database yet: an import makes it),
/// Conflict (a schema version that is not the program's), and those of Database::open().
[[nodiscard]] Expected<CatalogReader> openCatalogReader(const core::Paths& paths);

/// Reports on stderr that the catalog could not be read ("retroverse-cli <command>: ..."), with what to do for a
/// missing or older database, and returns ExitCode::RuntimeFailure.
[[nodiscard]] ExitCode catalogFailure(CommandContext& context, const Error& error);

/// Writes `text` to the --output file (`file`), or to the run's stdout when there is none; a file that cannot be
/// written fails the run (ExitCode::RuntimeFailure). Returns `code` otherwise.
[[nodiscard]] ExitCode writeResults(CommandContext& context, std::optional<Output>& file, const std::string& text,
                                    ExitCode code);

/// Reports a usage error on stderr: "retroverse-cli <command>: <message>" and the help hint; ExitCode::UsageError.
[[nodiscard]] ExitCode usageError(CommandContext& context, std::string_view message);

/// JSON as text: two spaces of indentation, text that is not UTF-8 replaced (never thrown), a final line feed.
[[nodiscard]] std::string jsonText(const CatalogJson& json);

/// "YYYY-MM-DD".
[[nodiscard]] std::string dateText(const catalog::ReleaseDate& date);

/// A list row as JSON: id, system, systemFullName, title, state, availability, releaseDate ("YYYY-MM-DD" or null),
/// year (or null), developer, publisher, genre, players ("1-4", "8+" or null), rating (0 to 1 in tenths, or null),
/// favorite, description.
[[nodiscard]] CatalogJson rowJson(const catalog::CatalogRow& row);

/// `games` and `search` (`search` true: a search text, the first argument, is required).
[[nodiscard]] ExitCode runGameList(CommandContext& context, bool search);

} // namespace rv::cli
