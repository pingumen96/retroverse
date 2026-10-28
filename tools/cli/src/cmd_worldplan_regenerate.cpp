// retroverse-cli worldplan regenerate: the explicit regeneration of a room, a floor or the whole building (ADR-0015 §3,
// §5, §8; I4b step 5) through world::regenerate(), honouring the world's overrides (pinned fixtures first, pinned games
// reserved, the pin winning the representative), with the impact summary of the app's preview: the rooms regenerated,
// added, removed and moved, the games that change slot, the titles entering and leaving face-out, the games newly
// placed and unplaced, the fixtures added, removed and moved, the pins kept and the conflicts with their reasons.
// --drop-overrides is the consent to drop the scope's overrides; --dry-run is the preview: it reads the world's plan and
// overrides without taking the world's lock (world::readWorldUnlocked(): shared, so that a writer is never refused, and
// checked again once both are read) and writes nothing. Otherwise the plan and the overrides are read under the
// lock and saved together through world::commitWorld() behind a snapshot (action "regenerate"), which 'worldplan undo'
// restores. The selections are the catalog's, as `worldplan update` reads them (every system of the catalog that gets a
// room; the scope's systems required), or canonical selection files (--selection, repeatable). A plan that breaks a
// hard rule is never written (exit 3).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_regeneration_text.hpp"
#include "world_selection.hpp"
#include "world_snapshot_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan regenerate";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan regenerate (--room <id> | --floor <id> | --world)\n"
    "                      [--drop-overrides] [--dry-run] [--selection <file>]... [--seed <n>] [--library <id>]\n"
    "                      [--diff] [--json] [--output <file>]\n"
    "\n"
    "Regenerates a room, a floor or the whole building of the world's plan as a fresh building is generated, keeping\n"
    "what the user pinned (the world's overrides.json): pinned shelves, display cases and decoration stand where they\n"
    "were put, pinned games stay in their slots (a pinned game stays its title's version on show), and the rest is\n"
    "planned around them: the face-out titles chosen again by priority, a room of an older template made with the\n"
    "current one. A floor's systems are decided again (own or shared rooms by today's title counts, the rooms placed\n"
    "again along the hall); only the world moves a system to another floor. First the impact is summarised (rooms,\n"
    "games changing slot, titles entering and leaving face-out, fixtures, pins kept and those that cannot be honoured,\n"
    "with why); then, unless --dry-run, the plan and the overrides are saved together behind a snapshot, which\n"
    "'worldplan undo' restores. A pin that cannot be honoured is kept in overrides.json and reported, never dropped,\n"
    "unless --drop-overrides drops the scope's overrides.\n"
    "\n"
    "Options:\n"
    "  --room <id>         Regenerate this room (room:snes, area:shared:gen4, area:arcade).\n"
    "  --floor <id>        Regenerate this floor's rooms (gen4, handheld, ground, ...).\n"
    "  --world             Regenerate the whole building (as 'worldplan generate --replace').\n"
    "  --drop-overrides    Drop the scope's overrides (its pins) and regenerate without them.\n"
    "  --dry-run           Only summarise the impact: read the plan and the overrides, write nothing.\n"
    "  --selection <file>  Take a room's selection from this file (canonical JSON) instead of the catalog;\n"
    "                      repeatable, one per system; the files must give the scope's systems (the world's: every\n"
    "                      system of the stored building).\n"
    "  --seed <n>          The world seed (default: world.json's, else derived from the library id); a room or floor\n"
    "                      of a plan of another seed needs --world.\n"
    "  --library <id>      The library whose world to regenerate (default: the one library.json records).\n"
    "  --diff              List every item of the impact.\n"
    "  --json              Write the results as JSON.\n"
    "  --output <file>     Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 regenerated, nothing to change, or a dry run; 1 nothing written: there is no plan, the plan holds\n"
    "no such room or the building no such floor, a system of the scope has no selection, the plan needs --world\n"
    "(another seed, preset or generator major), the media are unknown; 2 usage error; 3 the data, the catalog, the\n"
    "selections, the plan or the overrides could not be read, the regenerated plan breaks a hard rule of the\n"
    "validation, an action is in progress or was interrupted or the world changed while it was read (a dry run), the\n"
    "world is locked by another program, or the plan could not be written.\n";

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

ExitCode runWorldPlanRegenerate(CommandContext& context, std::span<const std::string> arguments) {
    const auto started = std::chrono::steady_clock::now();
    core::ArgParser parser{std::string(kName)};
    parser.option("room", "id", "Regenerate this room.");
    parser.option("floor", "id", "Regenerate this floor's rooms.");
    parser.flag("world", "Regenerate the whole building.");
    parser.flag("drop-overrides", "Drop the scope's overrides.");
    parser.flag("dry-run", "Only summarise the impact; write nothing.");
    parser.option("selection", "file", "A room's selection (canonical JSON; repeatable).");
    parser.option("seed", "n", "The world seed.");
    parser.option("library", "id", "The library whose world to regenerate.");
    parser.flag("diff", "List every item of the impact.");
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
    const auto roomOption = args->value("room");
    const auto floorOption = args->value("floor");
    const bool worldOption = args->has("world");
    if ((roomOption ? 1 : 0) + (floorOption ? 1 : 0) + (worldOption ? 1 : 0) != 1) {
        return usageError(context, "name one scope: --room <id>, --floor <id> or --world");
    }
    world::RegenerationScope scope = world::RegenerationScope::ofWorld();
    if (roomOption) {
        auto room = world::RoomId::parse(*roomOption);
        if (!room) {
            return usageError(context, fmt::format("--room: {}", room.error().toString()));
        }
        scope = world::RegenerationScope::ofRoom(*room);
    } else if (floorOption) {
        scope = world::RegenerationScope::ofFloor(*floorOption);
    }
    std::optional<std::uint64_t> seed;
    if (const auto seedText = args->value("seed")) {
        seed = parseWorldSeed(*seedText);
        if (!seed) {
            return usageError(context, fmt::format("--seed: not an integer from 0 to 2^64 - 1: '{}'", *seedText));
        }
    }
    const bool dryRun = args->has("dry-run");
    const bool dropOverrides = args->has("drop-overrides");

    auto config = loadWorldConfiguration(context, kName, seed);
    if (!config) {
        return ExitCode::RuntimeFailure;
    }
    const auto info = loadBuildingInfo(context, kName);
    if (!info) {
        return ExitCode::RuntimeFailure;
    }
    if (floorOption && info->building.floor(*floorOption) == nullptr) {
        context.err.writeLine(fmt::format("{}: the building has no floor {} (building.json)", kName, *floorOption));
        return ExitCode::ProblemsFound;
    }
    ExitCode libraryFailure = ExitCode::UsageError;
    const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
    if (!library) {
        return libraryFailure;
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), *library);

    // A dry run reads without the lock and writes nothing (not even world.lock): the plan and the overrides read
    // together (world::readWorldUnlocked(): with full sharing, so that a writer's commit is never refused, checked
    // again once read); a journal there means the two may disagree until its action ends or is rolled back.
    std::optional<world::WorldLock> lock;
    std::optional<world::UnlockedWorldRead> unlocked;
    if (dryRun) {
        auto read = world::readWorldUnlocked(files);
        if (!read && read.error().code == ErrorCode::Conflict) {
            context.err.writeLine(fmt::format("{}: {}. Nothing previewed", kName, read.error().toString()));
            return ExitCode::RuntimeFailure;
        }
        if (read) {
            unlocked.emplace(std::move(*read));
        } else if (read.error().code != ErrorCode::NotFound) {
            return failure(context, "the world's files", read.error());
        }
    } else {
        auto acquired = world::WorldLock::acquire(files);
        if (!acquired) {
            return failure(context, "the world's lock", acquired.error());
        }
        lock.emplace(std::move(*acquired));
        if (lock->rolledBack()) {
            context.err.writeLine(fmt::format("{}: an interrupted action was rolled back first: the snapshot {} "
                                              "restored",
                                              kName, *lock->rolledBack()));
        }
    }
    const auto readStored = [&]() -> Expected<PlanFile> {
        if (!dryRun) {
            return readPlanFile(context.paths, files.plan);
        }
        if (!unlocked) {
            return makeError(ErrorCode::NotFound, "file does not exist", pathText(files.plan));
        }
        auto plan = world::readPlan(unlocked->plan, pathText(files.plan.filename()));
        if (!plan) {
            return propagate(plan.error());
        }
        return liftedPlanFile(context.paths, world::StoredPlan{std::move(*plan), unlocked->planDigest});
    };
    auto stored = readStored();
    if (!stored) {
        if (stored.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan to regenerate at {} (run 'retroverse-cli worldplan "
                                              "generate')",
                                              kName, pathText(files.plan)));
            return ExitCode::ProblemsFound;
        }
        if (stored.error().code == ErrorCode::ValidationFailed || stored.error().code == ErrorCode::ParseError ||
            stored.error().code == ErrorCode::Unsupported || stored.error().code == ErrorCode::InvalidArgument) {
            context.err.writeLine(fmt::format("{}: the plan is refused, nothing written: {} ('worldplan generate "
                                              "--replace' replaces it)",
                                              kName, stored.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the plan", stored.error());
    }
    const world::WorldPlan& previous = stored->plan;
    // The scope's systems, whose selections are required.
    std::vector<catalog::SystemName> required;
    if (roomOption) {
        const world::RoomId head = scope.room.head();
        const auto room = std::find_if(previous.rooms.begin(), previous.rooms.end(),
                                       [&](const world::Room& candidate) { return candidate.id == head; });
        if (room == previous.rooms.end()) {
            context.err.writeLine(fmt::format("{}: the plan holds no room {}", kName, scope.room.toString()));
            return ExitCode::ProblemsFound;
        }
        required = room->sections;
    } else if (floorOption) {
        for (const world::Room& room : previous.rooms) {
            if (room.floor == *floorOption) {
                required.insert(required.end(), room.sections.begin(), room.sections.end());
            }
        }
    }

    // The selections: every system of the catalog that gets a room (the scope's required), or the files'.
    const auto selecting = std::chrono::steady_clock::now();
    const std::vector<std::string> selectionFiles = args->values("selection");
    Selected selected = !selectionFiles.empty()
                            ? selectionsFromFiles(context, kName, selectionFiles, args->value("library"))
                            : selectionsFromCatalog(context, kName, required, args->value("library"), *config,
                                                    "no regeneration", *info, true);
    if (!selected.ok) {
        return selected.failure;
    }
    if (worldOption && !selectionFiles.empty()) {
        std::vector<std::string> missing;
        for (const world::Room& room : previous.rooms) {
            for (const catalog::SystemName& system : room.sections) {
                const bool given = std::any_of(selected.selections.begin(), selected.selections.end(),
                                               [&](const world::RoomSelection& selection) {
                                                   return selection.room.system() == system;
                                               });
                if (!given) {
                    missing.push_back(system.str());
                }
            }
        }
        if (!missing.empty()) {
            return usageError(context, fmt::format("the --selection files leave out {} of the stored building: "
                                                   "--world regenerates the whole building",
                                                   fmt::join(missing, ", ")));
        }
    }
    const double selectionsMs = millisecondsSince(selecting);

    // The rooms' data through one loader. A system that would enter as a new room (any for the world, those no room
    // holds otherwise) whose data the asset root cannot give yet is left out, as 'worldplan generate' and 'worldplan
    // update' leave it out (a file's system fails the command).
    const auto reading = std::chrono::steady_clock::now();
    world::RoomDataLoader load = world::assetRootLoader(context.paths.assetRoot());
    {
        std::vector<world::RoomSelection> kept;
        for (world::RoomSelection& selection : selected.selections) {
            const bool standing = std::any_of(previous.rooms.begin(), previous.rooms.end(), [&](const world::Room& room) {
                return std::find(room.sections.begin(), room.sections.end(), selection.room.system()) !=
                       room.sections.end();
            });
            if (standing && !worldOption) {
                kept.push_back(std::move(selection));
                continue;
            }
            world::RoomDataRequest request;
            request.system = selection.room.system();
            if (const auto tags = selected.platformTags.find(request.system); tags != selected.platformTags.end()) {
                request.platformTags = tags->second;
            }
            if (auto data = load(request); !data) {
                if (!selectionFiles.empty()) {
                    return failure(context, fmt::format("the world's data of {}", request.system.str()),
                                   data.error());
                }
                selected.leftOut.emplace_back(request.system, "no data yet: " + data.error().message);
                continue;
            }
            kept.push_back(std::move(selection));
        }
        selected.selections = std::move(kept);
    }
    const double dataMs = millisecondsSince(reading);
    for (const auto& [system, why] : selected.leftOut) {
        context.err.writeLine(fmt::format("{}: {} left out of the building: {}", kName, system.str(), why));
    }
    auto overrides = !dryRun ? world::loadOverrides(files.overrides)
                     : unlocked->overrides ? world::parseOverrides(*unlocked->overrides,
                                                                   pathText(files.overrides.filename()))
                                           : Expected<world::WorldOverrides>(world::WorldOverrides{});
    if (!overrides) {
        return failure(context, "the world's overrides", overrides.error());
    }

    world::RegenerationInput input;
    input.scope = scope;
    input.dropOverrides = dropOverrides;
    input.library = selected.library;
    input.selections = std::move(selected.selections);
    input.platformTags = std::move(selected.platformTags);
    input.config = std::move(*config);
    input.overrides = std::move(*overrides);
    input.previous = previous;
    input.previousDigest = stored->digest;
    const auto regenerating = std::chrono::steady_clock::now();
    auto regeneration = world::regenerate(input, info->building, info->systems, load);
    const double regenerationMs = millisecondsSince(regenerating);
    if (!regeneration) {
        const ErrorCode code = regeneration.error().code;
        if (code == ErrorCode::NotFound || code == ErrorCode::Unsupported) {
            context.err.writeLine(fmt::format("{}: nothing written: {}", kName, regeneration.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the regeneration", regeneration.error());
    }
    const world::GenerationResult& result = regeneration->result;
    const world::RegenerationImpact& impact = regeneration->impact;
    const std::size_t hard = reportFindings(context, kName, result.validation);
    if (hard > 0) {
        context.err.writeLine(fmt::format("{}: the regenerated plan breaks {} hard rule{} of the validation: nothing "
                                          "written",
                                          kName, hard, hard == 1 ? "" : "s"));
        return ExitCode::RuntimeFailure;
    }
    for (const world::OverrideConflict& conflict : impact.conflicts) {
        spdlog::warn("worldplan regenerate: a pin not honoured, kept in overrides.json: {}", conflictText(conflict));
    }

    std::string snapshot;
    bool written = false;
    if (!dryRun && regeneration->changed) {
        world::SaveOptions options;
        options.writer = fmt::format("retroverse-cli worldplan regenerate ({})", core::versionBanner());
        options.duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<double, std::milli>(regenerationMs));
        auto commit = world::commitWorld(*lock, world::WorldAction{"regenerate", scope.toString()}, result,
                                         regeneration->overrides, options);
        if (!commit) {
            return failure(context, "the plan", commit.error());
        }
        written = commit->written;
        if (commit->snapshot) {
            snapshot = commit->snapshot->name;
        }
    }
    const double totalMs = millisecondsSince(started);
    spdlog::info("worldplan regenerate {}: {} ({} regenerated, {} conflicts, {:.0f} ms)", scope.toString(),
                 written ? "written" : (dryRun ? "dry run" : "nothing to change"), impact.roomsRegenerated.size(),
                 impact.conflicts.size(), regenerationMs);

    const std::size_t soft = result.validation.findings.size() - hard;
    std::string text;
    if (args->has("json")) {
        CatalogJson json;
        json["scope"] = scope.toString();
        json["library"] = library->str();
        json["plan"] = pathText(files.plan);
        json["dryRun"] = dryRun;
        json["dropOverrides"] = dropOverrides;
        json["changed"] = regeneration->changed;
        json["written"] = written;
        json["snapshot"] = snapshot.empty() ? CatalogJson(nullptr) : CatalogJson(snapshot);
        json["impact"] = impactJson(impact);
        json["validation"] = CatalogJson{{"passed", true}, {"soft", soft}};
        json["digestBefore"] = result.changes.digestBefore;
        json["digestAfter"] = result.changes.digestAfter;
        json["gamePins"] = regeneration->overrides.gamePins.size();
        json["fixturePins"] = regeneration->overrides.fixturePins.size();
        json["timings"] = CatalogJson{{"selectionsMs", selectionsMs},
                                      {"dataMs", dataMs},
                                      {"regenerationMs", regenerationMs},
                                      {"totalMs", totalMs}};
        CatalogJson leftOut = CatalogJson::array();
        for (const auto& [system, why] : selected.leftOut) {
            leftOut.push_back(CatalogJson{{"system", system.str()}, {"reason", why}});
        }
        json["leftOut"] = std::move(leftOut);
        text = jsonText(json);
    } else {
        const std::string heading =
            dryRun ? "Would regenerate"
                   : (written ? "Regenerated" : (regeneration->changed ? "Regenerated" : "Nothing to change in"));
        text = fmt::format("{} {} of the plan of {}: {}\n", heading, scope.toString(), roomsText(result.plan),
                           pathText(files.plan));
        text += fmt::format("  {:<17}{}\n", "Overrides:",
                            dropOverrides ? "the scope's dropped" : "kept (conflicts kept and reported)");
        text += impactText(impact, args->has("diff"));
        text += fmt::format("  {:<17}passed, {} soft finding{}\n", "Validation:", soft, soft == 1 ? "" : "s");
        text += fmt::format("  {:<17}{} -> {}\n", "Plan:", shortDigest(result.changes.digestBefore),
                            shortDigest(result.changes.digestAfter));
        if (!snapshot.empty()) {
            text += fmt::format("  {:<17}{} ('worldplan undo' restores it)\n", "Snapshot:", snapshot);
        }
        text += fmt::format("  {:<17}{:.0f} ms (selections {:.0f} ms, data {:.0f} ms, regeneration {:.0f} ms)\n",
                            "Time:", totalMs, selectionsMs, dataMs, regenerationMs);
        for (const auto& [system, why] : selected.leftOut) {
            text += fmt::format("  {:<17}{}: {}\n", "Left out:", system.str(), why);
        }
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
