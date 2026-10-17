#pragma once

// Private to rv_cli_core (I2 step 11a): the import of the library <user data>/config/library.json records into the
// catalog database, which `import` and `db rebuild-index` share (catalog::ImportService over esde::EsdeLibrarySource,
// esde::EsdeMediaResolver and catalog_db::SqliteCatalogStore), and its summary as text or JSON. Defined in
// cmd_import.cpp.

#include <optional>
#include <string>
#include <string_view>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>

namespace rv::cli {

/// What to import and how to report it.
struct CatalogImportRequest {
    /// The command, for messages ("retroverse-cli import", "retroverse-cli db rebuild-index").
    std::string_view name;
    /// `db rebuild-index`: the library's index tables cleared and written again (catalog::ImportOptions::rebuildIndex).
    bool rebuildIndex = false;
    /// --system: only that system (a partial import).
    std::optional<std::string> system;
    /// --accept-removals.
    bool acceptRemovals = false;
    /// --json.
    bool json = false;
    /// The --output file; nothing: the context's stdout.
    std::optional<Output>* file = nullptr;
};

/// Imports the library library.json records into <user data>/database/retroverse.sqlite (made and migrated when
/// needed) and prints the ChangeSet per system. Exit codes: 0; 1 when an error issue was met or the mass-disappearance
/// guard kept a system as it was; 2 without a library.json, for a system the configuration does not define, and when
/// library.json names another library than the catalog's (with the re-association to run); 3 when the library or the
/// database cannot be read or written, another import runs (in this or another process), or the results cannot be
/// written.
[[nodiscard]] ExitCode runCatalogImport(CommandContext& context, const CatalogImportRequest& request);

} // namespace rv::cli
