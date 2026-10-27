// retroverse-cli worldplan undo: undoes the world's last explicit action (ADR-0015 §6, I4b step 1): under the world
// lock, the newest snapshot not yet undone (backups/snapshots/) restored byte for byte, the plan, its meta sidecar and
// overrides.json together (world::undoLastAction), the state it replaces kept as a snapshot of kind undo, which undo
// never offers (no redo). --dry-run names the snapshot and restores nothing, but still takes the lock like every
// writer (which may create world.lock and roll back an interrupted action first). The catalog's changes since the
// snapshot come back with the next update.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/version.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_snapshot_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan undo";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan undo [--library <id>] [--dry-run] [--json] [--output <file>]\n"
    "\n"
    "Undoes the world's last explicit action (a regeneration, an edit): the plan and the overrides restored\n"
    "together, byte for byte, from the newest snapshot not yet undone in\n"
    "<user data>/worlds/<library id>/backups/snapshots/, under the world's lock. The state it replaces is kept as a\n"
    "snapshot of its own, which is never undone: undo walks back through the kept snapshots, and there is no redo.\n"
    "The catalog's changes since then come back with the next 'worldplan update'.\n"
    "\n"
    "Options:\n"
    "  --library <id>   The library whose world to undo (default: the one library.json records).\n"
    "  --dry-run        Name the snapshot that would be restored; restore nothing. The world's lock is still\n"
    "                   taken, which may create world.lock and roll back an interrupted action first.\n"
    "  --json           Write the result as JSON.\n"
    "  --output <file>  Write the result to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 undone (or, with --dry-run, something to undo); 1 nothing to undo; 2 usage error; 3 the world is\n"
    "locked by another program or holds the journal of an action that cannot be rolled back (the message says how\n"
    "to repair it), a snapshot is damaged (undo is refused while one is), or a file could not be read or written.\n";

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

ExitCode nothingToUndo(CommandContext& context, std::string_view why) {
    context.err.writeLine(fmt::format("{}: {}", kName, why));
    return ExitCode::ProblemsFound;
}

} // namespace

ExitCode runWorldPlanUndo(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.option("library", "id", "The library whose world to undo.");
    parser.flag("dry-run", "Name the snapshot that would be restored; restore nothing (the lock is still taken).");
    parser.flag("json", "Write the result as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(arguments);
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    auto file = outputFileOption(*args);
    if (!file) {
        return usageError(context, file.error().toString());
    }
    ExitCode libraryFailure = ExitCode::UsageError;
    const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
    if (!library) {
        return libraryFailure;
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), *library);
    std::error_code error;
    if (!std::filesystem::exists(files.folder, error)) {
        // The lock would make the folder: a world that is not there has nothing to undo.
        return nothingToUndo(context, fmt::format("nothing to undo: the library {} has no world yet", library->str()));
    }

    auto lock = world::WorldLock::acquire(files);
    if (!lock) {
        return failure(context, "the world's lock", lock.error());
    }
    if (lock->rolledBack()) {
        context.err.writeLine(fmt::format("{}: an interrupted action was rolled back first: the snapshot {} restored",
                                          kName, *lock->rolledBack()));
    }
    world::UndoOptions options;
    options.writer = fmt::format("retroverse-cli worldplan undo ({})", core::versionBanner());
    options.dryRun = args->has("dry-run");
    auto undone = world::undoLastAction(*lock, options);
    if (!undone) {
        if (undone.error().code == ErrorCode::NotFound) {
            return nothingToUndo(context, undone.error().message);
        }
        return failure(context, "undo", undone.error());
    }
    const world::WorldSnapshot& restored = undone->restored;
    spdlog::info("worldplan undo: {} {}", options.dryRun ? "would restore" : "restored", restored.name);

    std::string text;
    if (args->has("json")) {
        CatalogJson json;
        json["library"] = library->str();
        json["dryRun"] = options.dryRun;
        json["restored"] = snapshotJson(restored);
        json["kept"] = undone->kept ? snapshotJson(*undone->kept) : CatalogJson(nullptr);
        json["planBackup"] = undone->planBackup ? CatalogJson(pathText(*undone->planBackup)) : CatalogJson(nullptr);
        json["snapshotsRemoved"] = undone->snapshotsRemoved;
        json["rolledBack"] = lock->rolledBack() ? CatalogJson(*lock->rolledBack()) : CatalogJson(nullptr);
        text = jsonText(json);
    } else {
        text = fmt::format("{} {}: the snapshot {} taken {} by {}\n", options.dryRun ? "Would undo" : "Undid",
                           snapshotActionText(restored), restored.name, restored.takenAt, restored.writer);
        text += fmt::format("  {:<17}{}\n", "Plan:", shortDigest(restored.planDigest));
        text += fmt::format("  {:<17}{}\n", "Overrides:", shortDigest(restored.overridesDigest));
        if (undone->kept) {
            text += fmt::format("  {:<17}the state replaced, as {} (never undone: no redo)\n", "Kept:",
                                undone->kept->name);
        }
        if (undone->planBackup) {
            text += fmt::format("  {:<17}the plan replaced, as {}\n", "Plan backup:",
                                pathText(undone->planBackup->filename()));
        }
        for (const std::string& removed : undone->snapshotsRemoved) {
            text += fmt::format("  {:<17}{}\n", "Rotated out:", removed);
        }
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
