// retroverse-cli worldplan update: the incremental update of a previous plan (ADR-0010 §10; I3 step 9): every placement
// kept, the new titles added in the spine-out section next to their alphabetical predecessors (a dressed level, a new
// side-wall module, the room's growth when the section is full; capacity beyond the template's limit), the face-out set
// unchanged, pins protected, and the change report (--diff lists every change). The room's selection is the catalog's
// as `worldplan generate` makes it (the aliases of placed games it no longer lists asked from the catalog), or a
// canonical selection file (--selection); nothing is updated while the media are unknown (exit 1: unknown media
// change nothing). The previous plan is the world's (<user data>/worlds/<library id>/world-plan.json, read and saved
// again under the world's lock, the plan it replaces kept in backups/) or the file --previous names; a plan edited by
// hand or written by a newer Retroverse is refused, and one of another seed, preset, template or generator major
// version needs the explicit regeneration (`worldplan generate --replace`): exit 1, nothing written. The updated plan
// is validated (a hard rule broken: exit 3, nothing written) and written to the world's plan or to --plan.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
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
    "Usage: retroverse-cli [global options] worldplan update --system <system> [--selection <file>]\n"
    "                      [--previous <plan>] [--seed <n>] [--library <id>] [--plan <file>] [--diff] [--json]\n"
    "                      [--output <file>]\n"
    "\n"
    "Updates a room's plan to the catalog as it is now, moving nothing that stands: every placement is kept (also a\n"
    "missing or unreachable game's), each new title goes to the spine-out shelves next to its alphabetical\n"
    "predecessor (taking a dressed level, then a new shelf module in place of decoration, then the room's growth when\n"
    "the shelves are full), and the face-out titles stay as they are (only 'worldplan generate --replace' chooses\n"
    "them again). Pins of the world's overrides.json are never moved. Nothing is updated while the library's media root\n"
    "cannot be reached. The updated plan is validated, then saved as the world's plan (the previous one kept in the\n"
    "world's backups/ folder) and the change report goes to stdout.\n"
    "\n"
    "Options:\n"
    "  --system <system>   The room's system, as ES-DE names it (snes).\n"
    "  --selection <file>  Take the room's selection from this file (canonical JSON) instead of the catalog.\n"
    "  --previous <plan>   The plan to update (default: <user data>/worlds/<library id>/world-plan.json).\n"
    "  --seed <n>          The world seed, 0 to 18446744073709551615 (default: world.json's, else derived from the\n"
    "                      library id); a plan of another seed needs 'worldplan generate --replace'.\n"
    "  --library <id>      The library the plan belongs to (default: the one library.json records; with the catalog's\n"
    "                      selection it must be that one).\n"
    "  --plan <file>       Write the updated plan to this file instead of the world's plan (replaced when it exists;\n"
    "                      no backup).\n"
    "  --diff              List every change: the games added and newly unplaced, the fixtures added, removed and\n"
    "                      moved, the dressing removed, the slots freed, the pins.\n"
    "  --json              Write the results as JSON.\n"
    "  --output <file>     Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 updated, or nothing to change; 1 nothing written: there is no plan to update, the plan is refused\n"
    "(edited by hand or written by a newer Retroverse) or needs an explicit regeneration (another seed, preset,\n"
    "template or generator major version), the media are unknown, or the catalog has no such system; 2 usage error;\n"
    "3 the data, the catalog, the selection or the overrides could not be read, the updated plan breaks a hard rule\n"
    "of the validation, the world is locked by another program, or the plan could not be read or written.\n";

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
    parser.option("system", "system", "The room's system.");
    parser.option("selection", "file", "The room's selection (canonical JSON).");
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
    const auto systemText = args->value("system");
    if (!systemText) {
        return usageError(context, "--system is required");
    }
    auto system = catalog::SystemName::parse(*systemText);
    if (!system) {
        return usageError(context, fmt::format("--system: {}", system.error().toString()));
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
    const auto selectionFile = args->value("selection");
    Selected selected =
        selectionFile ? selectionFromFile(context, kName, *selectionFile, args->value("library"))
                      : selectionFromCatalog(context, kName, *system, args->value("library"), *config, "no update");
    if (!selected.selection) {
        return selected.failure;
    }
    auto data = world::loadWorldData(context.paths.assetRoot(), *system);
    if (!data) {
        return failure(context, "the world's data", data.error());
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), selected.library);
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
    auto previous = world::readStoredPlan(previousPath);
    if (!previous) {
        if (previous.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan to update at {} (run 'retroverse-cli worldplan generate')",
                                              kName, pathText(previousPath)));
            return ExitCode::ProblemsFound;
        }
        if (isRefusedPlan(previous.error())) {
            context.err.writeLine(fmt::format("{}: the plan is refused, nothing written: {} ('worldplan generate "
                                              "--replace' regenerates it after a backup)",
                                              kName, previous.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the plan", previous.error());
    }
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return failure(context, "the world's overrides", overrides.error());
    }
    world::UpdateInput input;
    if (selected.fromCatalog) {
        auto aliases = catalogAliases(context.paths, previous->plan, selected.snapshotGames);
        if (!aliases) {
            return catalogFailure(context, aliases.error());
        }
        input.aliases = std::move(*aliases);
    }
    input.library = selected.library;
    input.selection = std::move(*selected.selection);
    input.config = std::move(*config);
    input.overrides = std::move(*overrides);
    input.previous = std::move(previous->plan);
    input.previousDigest = std::move(previous->digest);

    const auto started = std::chrono::steady_clock::now();
    auto result = world::update(input, *data);
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
    const std::string room = result->plan.rooms.front().id.toString();
    const std::string heading = written ? fmt::format("Updated the plan of {}: {}", room, pathText(planPath))
                                        : fmt::format("The plan of {} is up to date: {}", room, pathText(planPath));
    return writeResults(context, *file, results(heading, planPath, written), ExitCode::Ok);
}

} // namespace rv::cli
