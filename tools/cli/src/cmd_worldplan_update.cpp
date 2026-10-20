// retroverse-cli worldplan update: the incremental update of a previous building (ADR-0010 §10, ADR-0013 §5; I3 step 9,
// I4 step 1): in each room every placement kept, the new titles added in the spine-out section next to their
// alphabetical predecessors (a dressed level, a new side-wall module, the room's growth when the section is full;
// capacity beyond the template's limit), the face-out set unchanged, pins protected, a room whose template, prefab
// levels or generator major changed kept as it is (its new games unplaced as room-kept), and the change report (--diff
// lists every change). The rooms updated are those of the systems --system names (repeatable), else every console
// room of the plan; a system without a room is refused (new systems enter the building with I4 step 6a). The rooms'
// selections are the catalog's as `worldplan generate` makes them (the aliases of placed games they no longer list
// asked from the catalog), or canonical selection files (--selection, repeatable: the rooms updated are the files',
// and --system given with them names the same systems). An update of some rooms only that changes nothing keeps the
// plan's inputDigest, so it writes nothing (incremental.hpp); nothing is updated while the media
// are unknown (exit 1: unknown media change nothing). The previous plan is the world's
// (<user data>/worlds/<library id>/world-plan.json, read and saved again under the world's lock, the plan it replaces
// kept in backups/) or the file --previous names, of schema 2 or of schema 1 (lifted); a plan edited by hand or written
// by a newer Retroverse is refused, and one of another seed, preset or generator major version needs the explicit
// regeneration (`worldplan generate --replace`): exit 1, nothing written. The updated plan is validated (a hard rule
// broken: exit 3, nothing written) and written to the world's plan or to --plan.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/ids.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/core/version.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_selection.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan update";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan update [--system <system>]... [--selection <file>]...\n"
    "                      [--previous <plan>] [--seed <n>] [--library <id>] [--plan <file>] [--diff] [--json]\n"
    "                      [--output <file>]\n"
    "\n"
    "Updates the building's plan to the catalog as it is now, moving nothing that stands: in each room every\n"
    "placement is kept (also a missing or unreachable game's), each new title goes to the spine-out shelves next to\n"
    "its alphabetical predecessor (taking a dressed level, then a new shelf module in place of decoration, then the\n"
    "room's growth when the shelves are full), and the face-out titles stay as they are (only 'worldplan generate\n"
    "--replace' chooses them again). A room made from an older template or prefabs, or by another generator major, is\n"
    "kept as it is and its new games listed as unplaced (room-kept). Pins of the world's overrides.json are never\n"
    "moved. Nothing is updated while the library's media root cannot be reached. The updated plan is validated, then\n"
    "saved as the world's plan (the previous one kept in the world's backups/ folder) and the change report goes to\n"
    "stdout. A plan of schema 1 (I3's one room) is lifted into the building first.\n"
    "\n"
    "Options:\n"
    "  --system <system>   Update this system's room only, as ES-DE names it (snes); repeatable. Default: every\n"
    "                      console room of the plan. A system without a room is refused (new systems come later).\n"
    "  --selection <file>  Take a room's selection from this file (canonical JSON) instead of the catalog;\n"
    "                      repeatable, one per system: the rooms updated are the files' (--system, given too, names\n"
    "                      the same systems).\n"
    "  --previous <plan>   The plan to update (default: <user data>/worlds/<library id>/world-plan.json).\n"
    "  --seed <n>          The world seed, 0 to 18446744073709551615 (default: world.json's, else derived from the\n"
    "                      library id); a plan of another seed needs 'worldplan generate --replace'.\n"
    "  --library <id>      The library the plan belongs to (default: the one library.json records; with the catalog's\n"
    "                      selections it must be that one).\n"
    "  --plan <file>       Write the updated plan to this file instead of the world's plan (replaced when it exists;\n"
    "                      no backup).\n"
    "  --diff              List every change: the games added and newly unplaced, the fixtures added, removed and\n"
    "                      moved, the dressing removed, the slots freed, the pins, the rooms kept.\n"
    "  --json              Write the results as JSON.\n"
    "  --output <file>     Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 updated, or nothing to change; 1 nothing written: there is no plan to update, the plan is refused\n"
    "(edited by hand or written by a newer Retroverse) or needs an explicit regeneration (another seed, preset or\n"
    "generator major version), the media are unknown, the catalog has no such system, or the plan has no room for it;\n"
    "2 usage error (--system and --selection files naming other systems); 3 the data, the catalog, the selections or\n"
    "the overrides could not be read, the updated plan breaks a hard rule of the validation, the world is locked by\n"
    "another program, or the plan could not be read or written.\n";

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

/// A plan file this program does not take: edited by hand (not its canonical text), not JSON, or newer.
bool isRefusedPlan(const Error& error) {
    return error.code == ErrorCode::ValidationFailed || error.code == ErrorCode::ParseError ||
           error.code == ErrorCode::Unsupported;
}

} // namespace

ExitCode runWorldPlanUpdate(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.option("system", "system", "A system whose room to update (repeatable).");
    parser.option("selection", "file", "A room's selection (canonical JSON; repeatable).");
    parser.option("previous", "plan", "The plan to update.");
    parser.option("seed", "n", "The world seed.");
    parser.option("library", "id", "The library the plan belongs to.");
    parser.option("plan", "file", "Write the updated plan to this file.");
    parser.flag("diff", "List every change.");
    parser.flag("json", "Write the results as JSON.");
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
    std::vector<catalog::SystemName> systems;
    for (const std::string& text : args->values("system")) {
        auto system = catalog::SystemName::parse(text);
        if (!system) {
            return usageError(context, fmt::format("--system: {}", system.error().toString()));
        }
        if (std::find(systems.begin(), systems.end(), *system) == systems.end()) {
            systems.push_back(std::move(*system));
        }
    }
    std::optional<std::uint64_t> seed;
    if (const auto seedText = args->value("seed")) {
        seed = parseWorldSeed(*seedText);
        if (!seed) {
            return usageError(context, fmt::format("--seed: not an integer from 0 to 2^64 - 1: '{}'", *seedText));
        }
    }

    auto config = loadWorldConfiguration(context, kName, seed);
    if (!config) {
        return ExitCode::RuntimeFailure;
    }
    const auto info = loadBuildingInfo(context, kName);
    if (!info) {
        return ExitCode::RuntimeFailure;
    }
    const std::vector<std::string> selectionFiles = args->values("selection");
    ExitCode libraryFailure = ExitCode::UsageError;
    const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
    if (!library) {
        return libraryFailure;
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), *library);
    const auto planOption = args->value("plan");
    const auto previousOption = args->value("previous");
    const std::filesystem::path previousPath = previousOption ? core::pathFromUtf8(*previousOption) : files.plan;

    // The world's plan is read and written again under its lock, so that no other writer comes between.
    std::optional<world::WorldLock> lock;
    if (!planOption) {
        auto acquired = world::WorldLock::acquire(files);
        if (!acquired) {
            return failure(context, "the world's lock", acquired.error());
        }
        lock.emplace(std::move(*acquired));
    }
    auto previous = readPlanFile(context.paths, previousPath);
    if (!previous) {
        if (previous.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan to update at {} (run 'retroverse-cli worldplan generate')",
                                              kName, pathText(previousPath)));
            return ExitCode::ProblemsFound;
        }
        if (isRefusedPlan(previous.error()) || previous.error().code == ErrorCode::InvalidArgument) {
            context.err.writeLine(fmt::format("{}: the plan is refused, nothing written: {} ('worldplan generate "
                                              "--replace' regenerates it after a backup)",
                                              kName, previous.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the plan", previous.error());
    }
    // The systems whose rooms to update: those named, else every console room's.
    if (systems.empty() && selectionFiles.empty()) {
        for (const world::Room& room : previous->plan.rooms) {
            if (room.kind == world::RoomKind::Console) {
                systems.push_back(room.id.system());
            }
        }
    }
    for (const catalog::SystemName& system : systems) {
        const bool hasRoom = std::any_of(previous->plan.rooms.begin(), previous->plan.rooms.end(),
                                         [&](const world::Room& room) {
                                             return std::find(room.sections.begin(), room.sections.end(), system) !=
                                                    room.sections.end();
                                         });
        if (!hasRoom) {
            context.err.writeLine(fmt::format("{}: the plan has no room for {}: nothing written (new systems enter "
                                              "the building with I4 step 6a)",
                                              kName, system.str()));
            return ExitCode::ProblemsFound;
        }
    }
    Selected selected =
        !selectionFiles.empty()
            ? selectionsFromFiles(context, kName, selectionFiles, args->value("library"))
            : selectionsFromCatalog(context, kName, systems, args->value("library"), *config, "no update", *info);
    if (!selected.ok) {
        return selected.failure;
    }
    if (!selectionFiles.empty() && !systems.empty()) {
        // The rooms updated are the files' systems': --system, given with them, names the same ones.
        std::vector<std::string> named;
        for (const world::RoomSelection& selection : selected.selections) {
            named.push_back(selection.room.system().str());
        }
        std::vector<std::string> asked;
        for (const catalog::SystemName& system : systems) {
            asked.push_back(system.str());
        }
        std::sort(named.begin(), named.end());
        std::sort(asked.begin(), asked.end());
        if (named != asked) {
            return usageError(context, fmt::format("--system names {}, the --selection files {}: with both, they name "
                                                   "the same systems",
                                                   fmt::join(asked, ", "), fmt::join(named, ", ")));
        }
    }
    for (const world::RoomSelection& selection : selected.selections) {
        const bool hasRoom = std::any_of(previous->plan.rooms.begin(), previous->plan.rooms.end(),
                                         [&](const world::Room& room) { return room.id == selection.room; });
        if (!hasRoom) {
            context.err.writeLine(fmt::format("{}: the plan has no room for {}: nothing written (new systems enter "
                                              "the building with I4 step 6a)",
                                              kName, selection.room.system().str()));
            return ExitCode::ProblemsFound;
        }
    }
    // The data of every room of the plan, with its recorded family, theme pack and template.
    std::vector<world::WorldData> rooms;
    for (const world::Room& room : previous->plan.rooms) {
        const catalog::SystemName system = room.sections.empty() ? room.id.system() : room.sections.front();
        const auto tags = selected.platformTags.find(system);
        auto data = world::loadRoomData(context.paths.assetRoot(), room,
                                        tags != selected.platformTags.end() ? std::span<const std::string>(tags->second)
                                                                            : std::span<const std::string>());
        if (!data) {
            return failure(context, fmt::format("the world's data of {}", room.id.toString()), data.error());
        }
        rooms.push_back(std::move(*data));
    }
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return failure(context, "the world's overrides", overrides.error());
    }
    world::BuildingUpdateInput input;
    if (selected.fromCatalog) {
        auto aliases = catalogAliases(context.paths, previous->plan, selected.snapshotGames);
        if (!aliases) {
            return catalogFailure(context, aliases.error());
        }
        input.aliases = std::move(*aliases);
    }
    input.library = selected.library;
    input.selections = std::move(selected.selections);
    input.platformTags = std::move(selected.platformTags);
    input.config = std::move(*config);
    input.overrides = std::move(*overrides);
    const bool lifted = previous->lifted();
    input.previous = std::move(previous->plan);
    input.previousDigest = std::move(previous->digest);

    const auto started = std::chrono::steady_clock::now();
    auto result = world::updateBuilding(input, info->building, info->systems, rooms);
    if (!result) {
        if (result.error().code == ErrorCode::Unsupported) {
            context.err.writeLine(fmt::format("{}: the plan is refused, nothing written: {}", kName,
                                              result.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the update", result.error());
    }
    const auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
    const world::ChangeReport& changes = result->changes;
    const bool json = args->has("json");
    const bool detailed = args->has("diff");
    const auto results = [&](std::string_view heading, const std::filesystem::path& planPath, bool written) {
        if (json) {
            CatalogJson document;
            document["plan"] = pathText(planPath);
            document["written"] = written;
            document["lifted"] = lifted;
            document["changes"] = changeReportJson(changes);
            document["summary"] = planSummaryJson(result->plan, changes.digestAfter);
            return jsonText(document);
        }
        return fmt::format("{}\n", heading) + changeReportText(changes, detailed);
    };
    if (changes.outcome == world::GenerationOutcome::RequiresExplicitRegeneration) {
        context.err.writeLine(fmt::format("{}: the plan needs an explicit regeneration ({}): nothing written; "
                                          "'worldplan generate --replace' regenerates it after a backup",
                                          kName, changes.regenerationReason));
        return writeResults(context, *file,
                            results(fmt::format("Kept the plan {}: it needs an explicit regeneration",
                                                pathText(previousPath)),
                                    previousPath, false),
                            ExitCode::ProblemsFound);
    }
    const std::size_t hard = reportFindings(context, kName, result->validation);
    if (hard > 0) {
        context.err.writeLine(fmt::format("{}: the updated plan breaks {} hard rule{} of the validation: nothing "
                                          "written",
                                          kName, hard, hard == 1 ? "" : "s"));
        return ExitCode::RuntimeFailure;
    }
    for (const std::string& change : changes.needsConfirmation) {
        spdlog::warn("worldplan update: not applied, needs a confirmation: {}", change);
    }
    for (const world::KeptRoom& kept : changes.keptRooms) {
        spdlog::warn("worldplan update: {} kept as it is: {}", kept.room.toString(), kept.reason);
    }

    std::filesystem::path planPath = files.plan;
    bool written = true;
    if (planOption) {
        planPath = core::pathFromUtf8(*planOption);
        auto text = world::writePlan(result->plan);
        if (!text) {
            return failure(context, "the plan", text.error());
        }
        if (Status status = core::writeTextFileAtomic(planPath, *text); !status) {
            return failure(context, "the plan", status.error());
        }
    } else {
        world::SaveOptions options;
        options.writer = fmt::format("retroverse-cli worldplan update ({})", core::versionBanner());
        options.duration = duration;
        auto report = world::savePlan(*lock, *result, options);
        if (!report) {
            return failure(context, "the plan", report.error());
        }
        written = report->written;
    }
    spdlog::info("worldplan update: {} {} ({} added, {} kept, rebuild {}, digest {})", pathText(planPath),
                 written ? "written" : "unchanged", changes.added.size(), changes.kept,
                 world::toString(changes.rebuild), changes.digestAfter);
    const std::string roomList = roomsText(result->plan);
    const std::string heading =
        written ? fmt::format("Updated the plan of {}{}: {}", roomList, lifted ? " (lifted from schema 1)" : "",
                              pathText(planPath))
                : fmt::format("The plan of {} is up to date: {}", roomList, pathText(planPath));
    return writeResults(context, *file, results(heading, planPath, written), ExitCode::Ok);
}

} // namespace rv::cli
