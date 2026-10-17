// retroverse-cli db: maintains the catalog database (ADR-0008 §5), <user data>/database/retroverse.sqlite:
//   db migrate        creates the database or brings its schema to this program's version, backing it up first
//   db check          integrity, foreign keys, FTS5, schema version and definition; exit 1 when a check finds problems
//   db info           path, versions, sizes, row counts, rv_meta, backups
//   db rebuild-index  every index table of the library rebuilt from a fresh snapshot of it and the identities (I2 step
//                     11a): the import of `retroverse-cli import` (runCatalogImport(), import_text.hpp) with the library's
//                     index rows cleared and every row written again, every state row kept; the result equals what an
//                     import gives, the rows that only the index knows (folders that cannot be listed, gamelists that
//                     failed, media out of reach) taken from it before it is cleared
// Each takes --json (the results as one JSON object) and --output <file>. Opening the database takes its process-level
// lock: a migration waits for no other Retroverse process, and refuses while one has the database open. check and info
// write nothing into the file: they open it as they find it (no switch to WAL), and make none when it is missing.

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>

#include "commands.hpp"
#include "import_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] db <subcommand> [--json] [--output <file>]\n"
    "\n"
    "Maintains the catalog database, <user data>/database/retroverse.sqlite.\n"
    "\n"
    "Subcommands:\n"
    "  migrate        Create the database, or bring its schema to this program's version (a backup first).\n"
    "  check          Check its integrity, foreign keys, FTS5, schema version and schema; exit 1 when a check\n"
    "                 finds problems.\n"
    "  info           Show its path, schema version, sizes, row counts, metadata and backups.\n"
    "  rebuild-index  Rebuild the library's index tables from the library and the identities (the import of\n"
    "                 'retroverse-cli import', every index row written again; every state row kept).\n"
    "\n"
    "Options:\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 the check found problems (rebuild-index: an error issue, or a system kept by the\n"
    "mass-disappearance guard); 2 usage error (rebuild-index: no library.json, or one naming another library);\n"
    "3 the database could not be opened or written (rebuild-index: or the library could not be read).\n";

/// The arguments every subcommand takes.
struct SubcommandArgs {
    bool json = false;
    bool help = false;
    /// The --output file; nothing: the run's stdout.
    std::optional<Output> file;
};

/// Parses a subcommand's arguments. Writes the usage error to stderr and returns nothing when they are invalid.
std::optional<SubcommandArgs> parseArgs(CommandContext& context, std::string_view subcommand,
                                        std::span<const std::string> arguments) {
    core::ArgParser parser{fmt::format("retroverse-cli db {}", subcommand)};
    parser.flag("json", "Write the results as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(arguments);
    if (!args) {
        context.err.writeLine(fmt::format("retroverse-cli db {}: {}", subcommand, args.error().toString()));
        context.err.writeLine("Run 'retroverse-cli db --help' for the subcommands and their options.");
        return std::nullopt;
    }
    SubcommandArgs parsed;
    parsed.help = args->has("help");
    parsed.json = args->has("json");
    auto file = outputFileOption(*args);
    if (!file) {
        context.err.writeLine(fmt::format("retroverse-cli db {}: {}", subcommand, file.error().toString()));
        return std::nullopt;
    }
    parsed.file = std::move(*file);
    return parsed;
}

/// Writes the results to the --output file or to stdout; a file that cannot be written fails the run.
ExitCode deliver(CommandContext& context, std::string_view subcommand, SubcommandArgs& args, const std::string& text,
                 ExitCode code) {
    if (!args.file) {
        context.out.write(text);
        return code;
    }
    args.file->write(text);
    if (auto finished = args.file->finish(); !finished) {
        context.err.writeLine(
            fmt::format("retroverse-cli db {}: cannot write the results: {}", subcommand, finished.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    return code;
}

ExitCode failed(CommandContext& context, std::string_view subcommand, const Error& error) {
    context.err.writeLine(fmt::format("retroverse-cli db {}: {}", subcommand, error.toString()));
    return ExitCode::RuntimeFailure;
}

std::string pathText(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// JSON as text: two spaces of indentation, text that is not UTF-8 replaced (never thrown).
std::string jsonText(const nlohmann::json& json) {
    return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

// --- db migrate ------------------------------------------------------------------------------------------------------

ExitCode runMigrate(CommandContext& context, SubcommandArgs& args) {
    const std::filesystem::path file = catalog_db::databaseFile(context.paths);
    auto database = catalog_db::Database::open(file);
    if (!database) {
        return failed(context, "migrate", database.error());
    }
    const auto report = catalog_db::migrate(*database);
    if (!report) {
        return failed(context, "migrate", report.error());
    }
    std::string text;
    if (args.json) {
        nlohmann::json json{
            {"database", pathText(database->path())},
            {"fromVersion", report->fromVersion},
            {"toVersion", report->toVersion},
            {"migrated", report->migrated()},
            {"backup", report->backup ? nlohmann::json(pathText(*report->backup)) : nlohmann::json(nullptr)},
            {"removedBackups", nlohmann::json::array()},
        };
        for (const std::filesystem::path& removed : report->removedBackups) {
            json["removedBackups"].push_back(pathText(removed));
        }
        text = jsonText(json);
    } else {
        text += fmt::format("Database: {}\n", pathText(database->path()));
        if (report->migrated()) {
            text += fmt::format("Schema version: {} -> {}\n", report->fromVersion, report->toVersion);
            text += report->backup ? fmt::format("Backup: {}\n", pathText(*report->backup))
                                   : std::string("Backup: none (the database held nothing)\n");
            for (const std::filesystem::path& removed : report->removedBackups) {
                text += fmt::format("Removed old backup: {}\n", pathText(removed));
            }
        } else {
            text += fmt::format("Schema version: {} (up to date)\n", report->toVersion);
        }
    }
    return deliver(context, "migrate", args, text, ExitCode::Ok);
}

// --- db check --------------------------------------------------------------------------------------------------------

/// The report of `db check` as text or JSON. `schemaVersion` is nothing when the file could not be read at all.
std::string checkText(const SubcommandArgs& args, const std::filesystem::path& file,
                      const catalog_db::DatabaseCheckReport& report, std::optional<int> schemaVersion) {
    std::size_t problemCount = 0;
    for (const catalog_db::DatabaseCheck& check : report.checks) {
        problemCount += check.problems;
    }
    if (args.json) {
        // "problems" lists the problems the checks name (each lists its first 100), "problemCount" counts them all.
        nlohmann::json checks = nlohmann::json::array();
        nlohmann::json problems = nlohmann::json::array();
        for (const catalog_db::DatabaseCheck& check : report.checks) {
            checks.push_back({{"name", check.name},
                              {"passed", check.passed},
                              {"problems", check.problems},
                              {"details", check.details}});
            if (!check.passed) {
                for (const std::string& detail : check.details) {
                    problems.push_back(fmt::format("{}: {}", check.name, detail));
                }
            }
        }
        return jsonText(nlohmann::json{{"database", pathText(file)},
                                       {"exists", true},
                                       {"schemaVersion", schemaVersion ? nlohmann::json(*schemaVersion) : nullptr},
                                       {"programSchemaVersion", report.programSchemaVersion},
                                       {"passed", report.passed()},
                                       {"problemCount", problemCount},
                                       {"problems", problems},
                                       {"checks", checks}});
    }
    std::string text = fmt::format("Database: {}\n", pathText(file));
    for (const catalog_db::DatabaseCheck& check : report.checks) {
        text += fmt::format("{}: {}\n", check.name, check.passed ? "ok" : "PROBLEMS");
        for (const std::string& detail : check.details) {
            text += fmt::format("  {}\n", detail);
        }
        if (!check.passed && check.problems > check.details.size()) {
            text += fmt::format("  ... and {} more\n", check.problems - check.details.size());
        }
    }
    text += report.passed() ? std::string("No problems found.\n") : fmt::format("{} problem(s) found.\n", problemCount);
    return text;
}

/// The report of a file SQLite cannot read (damaged, or no SQLite database): what the integrity check finds.
catalog_db::DatabaseCheckReport damagedReport(const Error& error) {
    catalog_db::DatabaseCheckReport report;
    report.programSchemaVersion = catalog_db::programSchemaVersion();
    report.checks.push_back(catalog_db::DatabaseCheck{
        "integrity", false, 1, {"SQLite cannot read the file (damaged, or no SQLite database): " + error.message}});
    return report;
}

ExitCode runCheck(CommandContext& context, SubcommandArgs& args) {
    const std::filesystem::path file = catalog_db::databaseFile(context.paths);
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // the file stays as it is found
    auto database = catalog_db::Database::open(file, options);
    if (!database && database.error().code == ErrorCode::NotFound) {
        // Nothing to check is a problem: there is no catalog.
        const std::string missing = "the database does not exist (an import or 'retroverse-cli db migrate' makes it)";
        const std::string text = args.json ? jsonText(nlohmann::json{{"database", pathText(file)},
                                                                     {"exists", false},
                                                                     {"passed", false},
                                                                     {"problemCount", 1},
                                                                     {"problems", nlohmann::json::array({missing})},
                                                                     {"checks", nlohmann::json::array()}})
                                           : fmt::format("Database: {}\nProblem: {}\n", pathText(file), missing);
        return deliver(context, "check", args, text, ExitCode::ProblemsFound);
    }
    // A file SQLite cannot read (ParseError: SQLITE_CORRUPT, SQLITE_NOTADB), at open or while a check runs, is what the
    // check is for: a problem found (exit 1), not a check that could not run (exit 3).
    if (!database) {
        if (database.error().code != ErrorCode::ParseError) {
            return failed(context, "check", database.error());
        }
        const std::string text = checkText(args, file, damagedReport(database.error()), std::nullopt);
        return deliver(context, "check", args, text, ExitCode::ProblemsFound);
    }
    const auto report = catalog_db::checkDatabase(*database);
    if (!report) {
        if (report.error().code != ErrorCode::ParseError) {
            return failed(context, "check", report.error());
        }
        const std::string text = checkText(args, database->path(), damagedReport(report.error()), std::nullopt);
        return deliver(context, "check", args, text, ExitCode::ProblemsFound);
    }
    const std::string text = checkText(args, database->path(), *report, report->schemaVersion);
    return deliver(context, "check", args, text, report->passed() ? ExitCode::Ok : ExitCode::ProblemsFound);
}

// --- db info ---------------------------------------------------------------------------------------------------------

ExitCode runInfo(CommandContext& context, SubcommandArgs& args) {
    const std::filesystem::path file = catalog_db::databaseFile(context.paths);
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // the file stays as it is found
    auto database = catalog_db::Database::open(file, options);
    if (!database && database.error().code != ErrorCode::NotFound) {
        return failed(context, "info", database.error());
    }
    if (!database) {
        const std::string text =
            args.json ? jsonText(nlohmann::json{{"database", pathText(file)},
                                                {"exists", false},
                                                {"programSchemaVersion", catalog_db::programSchemaVersion()}})
                      : fmt::format("Database: {}\nExists: no (an import or 'retroverse-cli db migrate' makes it)\n"
                                    "Program schema version: {}\n",
                                    pathText(file), catalog_db::programSchemaVersion());
        return deliver(context, "info", args, text, ExitCode::Ok);
    }
    const auto info = catalog_db::describeDatabase(*database);
    if (!info) {
        return failed(context, "info", info.error());
    }
    std::string text;
    if (args.json) {
        nlohmann::json meta = nlohmann::json::object();
        for (const auto& [key, value] : info->meta) {
            meta[key] = value;
        }
        nlohmann::json tables = nlohmann::json::array();
        for (const catalog_db::TableRows& table : info->tables) {
            tables.push_back({{"name", table.name}, {"rows", table.rows}});
        }
        nlohmann::json backups = nlohmann::json::array();
        for (const std::filesystem::path& backup : info->backups) {
            backups.push_back(pathText(backup));
        }
        text = jsonText(nlohmann::json{
            {"database", pathText(info->file)},
            {"exists", true},
            {"sqliteVersion", info->sqliteVersion},
            {"schemaVersion", info->schemaVersion},
            {"programSchemaVersion", info->programSchemaVersion},
            {"journalMode", info->journalMode},
            {"pageSize", info->pageSize},
            {"pageCount", info->pageCount},
            {"freePages", info->freePages},
            {"sizes",
             {{"database", info->databaseBytes}, {"wal", info->walBytes}, {"sharedMemory", info->sharedMemoryBytes}}},
            {"meta", meta},
            {"tables", tables},
            {"backups", backups},
        });
    } else {
        text += fmt::format("Database: {}\n", pathText(info->file));
        text += fmt::format("Schema version: {} (this program: {})\n", info->schemaVersion, info->programSchemaVersion);
        text += fmt::format("SQLite: {}, journal mode {}\n", info->sqliteVersion, info->journalMode);
        text += fmt::format("Size: {} bytes (WAL {} bytes, shared memory {} bytes); {} pages of {} bytes, {} free\n",
                            info->databaseBytes, info->walBytes, info->sharedMemoryBytes, info->pageCount,
                            info->pageSize, info->freePages);
        text += "Metadata:\n";
        for (const auto& [key, value] : info->meta) {
            text += fmt::format("  {} = {}\n", key, value);
        }
        std::size_t width = 0;
        for (const catalog_db::TableRows& table : info->tables) {
            width = std::max(width, table.name.size());
        }
        text += "Tables (rows):\n";
        for (const catalog_db::TableRows& table : info->tables) {
            text += fmt::format("  {:<{}}  {}\n", table.name, width, table.rows);
        }
        text += info->backups.empty() ? std::string("Backups: none\n") : std::string("Backups (newest first):\n");
        for (const std::filesystem::path& backup : info->backups) {
            text += fmt::format("  {}\n", pathText(backup));
        }
    }
    return deliver(context, "info", args, text, ExitCode::Ok);
}

// --- db rebuild-index -----------------------------------------------------------------------------------------------

ExitCode runRebuildIndex(CommandContext& context, SubcommandArgs& args) {
    CatalogImportRequest request;
    request.name = "retroverse-cli db rebuild-index";
    request.rebuildIndex = true;
    request.json = args.json;
    request.file = &args.file;
    return runCatalogImport(context, request);
}

} // namespace

ExitCode runDb(CommandContext& context) {
    if (context.arguments.empty()) {
        context.err.write(kUsage);
        return ExitCode::UsageError;
    }
    const std::string& subcommand = context.arguments.front();
    if (subcommand == "--help" || subcommand == "-h") {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    using Runner = ExitCode (*)(CommandContext&, SubcommandArgs&);
    Runner runner = nullptr;
    if (subcommand == "migrate") {
        runner = runMigrate;
    } else if (subcommand == "check") {
        runner = runCheck;
    } else if (subcommand == "info") {
        runner = runInfo;
    } else if (subcommand == "rebuild-index") {
        runner = runRebuildIndex;
    } else {
        context.err.writeLine(fmt::format("retroverse-cli db: unknown {} '{}'",
                                          subcommand.starts_with('-') ? "option" : "subcommand", subcommand));
        context.err.writeLine("Run 'retroverse-cli db --help' for the subcommands and their options.");
        return ExitCode::UsageError;
    }
    auto args = parseArgs(context, subcommand, context.arguments.subspan(1));
    if (!args) {
        return ExitCode::UsageError;
    }
    if (args->help) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    return runner(context, *args);
}

} // namespace rv::cli
