// retroverse-cli worldplan snapshots: the world's paired snapshots of the plan and the overrides (ADR-0015 §6, I4b step
// 1): <user data>/worlds/<library id>/backups/snapshots/, newest first, each with the action it came before, its
// scope, who took it and when, its files' digests, and whether an undo restored it; then the snapshot the next
// 'worldplan undo' restores. Reads only: no lock is taken and nothing is written (a journal of an interrupted action is
// reported; the next writer's lock rolls it back).

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_snapshot_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan snapshots";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan snapshots [--library <id>] [--json] [--output <file>]\n"
    "\n"
    "Lists the world's snapshots, newest first: the plan and the overrides as they were before each explicit action\n"
    "(a regeneration, an edit, an undo), kept in <user data>/worlds/<library id>/backups/snapshots/ (the last 20).\n"
    "Each names its action and scope, who took it and when, and whether an undo restored it; an undo's own snapshot\n"
    "(the state it replaced) is never undone. The last line names the snapshot 'worldplan undo' restores next.\n"
    "Nothing is written.\n"
    "\n"
    "Options:\n"
    "  --library <id>   The library whose world to read (default: the one library.json records).\n"
    "  --json           Write the list as JSON.\n"
    "  --output <file>  Write the list to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok (no snapshot included); 2 usage error (no library).\n";

} // namespace

ExitCode runWorldPlanSnapshots(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.option("library", "id", "The library whose world to read.");
    parser.flag("json", "Write the list as JSON.");
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
    const std::vector<world::WorldSnapshot> snapshots = world::listSnapshots(files);
    const std::optional<world::WorldSnapshot> next = world::nextUndo(snapshots);
    std::error_code error;
    const bool interrupted = std::filesystem::exists(files.transaction, error);

    std::string text;
    if (args->has("json")) {
        CatalogJson json;
        json["library"] = library->str();
        json["folder"] = pathText(files.snapshots);
        json["kept"] = world::kSnapshotsKept;
        CatalogJson list = CatalogJson::array();
        for (const world::WorldSnapshot& snapshot : snapshots) {
            list.push_back(snapshotJson(snapshot));
        }
        json["snapshots"] = std::move(list);
        json["nextUndo"] = next ? CatalogJson(next->name) : CatalogJson(nullptr);
        json["interruptedAction"] = interrupted;
        text = jsonText(json);
    } else {
        text = fmt::format("Snapshots of the world of {}: {} (newest first, the last {} kept)\n", library->str(),
                           pathText(files.snapshots), world::kSnapshotsKept);
        if (snapshots.empty()) {
            text += "  none\n";
        }
        for (const world::WorldSnapshot& snapshot : snapshots) {
            std::string state;
            if (snapshot.kind == world::SnapshotKind::Undo) {
                state = fmt::format(", the state an undo of {} replaced (never undone)", snapshot.undoes);
            } else if (snapshot.undone) {
                state = ", undone";
            }
            text += fmt::format("  {}  {}, {} by {}; plan {}, overrides {}{}\n", snapshot.name,
                                snapshotActionText(snapshot), snapshot.takenAt, snapshot.writer,
                                shortDigest(snapshot.planDigest), shortDigest(snapshot.overridesDigest), state);
        }
        text += next ? fmt::format("Next undo:       {} ({})\n", next->name, snapshotActionText(*next))
                     : std::string("Next undo:       nothing to undo\n");
        if (interrupted) {
            text += "Interrupted:     an action was interrupted between its writes; the next writer rolls it back\n";
        }
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
