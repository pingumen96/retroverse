// retroverse-cli worldplan generate: a fresh plan for a system's room (ADR-0010 §2 to §8, §11; I3 step 3). The room's
// selection is the catalog's, as the app makes it (I3 step 2): SqliteCatalogQuery::roomEntries on a read-only
// connection, esde::checkRoomMedia against the media root of the library <user data>/config/library.json records, then
// world::selectRoom; nothing is generated while the media are unknown (exit 1, ADR-0010 §3). --selection takes a
// canonical selection file instead (tests, a selection kept from elsewhere). Then the world configuration (the preset
// under <user data>/config/world.json, the seed from --seed when given), the world's data under the asset root, the
// world's overrides (overrides.json: a fresh plan that does not honour a pin is refused), and world::generate, whose
// plan is validated (world::validatePlan, I3 step 4): a plan that breaks a hard rule is never written (exit 3, its
// findings on stderr). The plan is saved as the world's plan under the world lock (rv/world/world_store.hpp, I3 step
// 9), never over an existing one unless --replace asks for the explicit regeneration (the face-out set chosen again,
// the previous plan kept in backups/ first); or written to the file --plan names. Its summary goes to stdout or
// --output.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/ids.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/core/version.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_selection.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan generate";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan generate --system <system> [--selection <file>]\n"
    "                      [--seed <n>] [--library <id>] [--replace | --plan <file>] [--json] [--output <file>]\n"
    "\n"
    "Generates a fresh plan for a system's room: which titles it shows and where (the display fixtures and the hero\n"
    "band face-out, the rest spine-out in as few shelf modules as they and the growth reserve need), its fixtures,\n"
    "decoration, shelf dressing and lights. The titles are the catalog's (<user data>/database/retroverse.sqlite, made\n"
    "by an import) for the library <user data>/config/library.json records, one per title with a cover; nothing is\n"
    "generated while the library's media root cannot be reached. The plan is validated (the pins of the world's\n"
    "overrides.json included), then saved as <user data>/worlds/<library id>/world-plan.json under the world's lock,\n"
    "never over an existing plan unless --replace is given; its summary goes to stdout.\n"
    "\n"
    "Options:\n"
    "  --system <system>   The room's system, as ES-DE names it (snes).\n"
    "  --selection <file>  Take the room's selection from this file (canonical JSON: the titles in priority order and\n"
    "                      the games the room does not place) instead of the catalog.\n"
    "  --seed <n>          The world seed, 0 to 18446744073709551615 (default: world.json's, else derived from the\n"
    "                      library id).\n"
    "  --library <id>      The library the plan belongs to (default: the one library.json records; with the catalog's\n"
    "                      selection it must be that one).\n"
    "  --replace           Regenerate over the world's existing plan: the explicit regeneration, which chooses the\n"
    "                      face-out titles again; the previous plan is first kept in the world's backups/ folder\n"
    "                      (the last 10).\n"
    "  --plan <file>       Write the plan to this file instead (replaced when it exists; no backup).\n"
    "  --json              Write the summary as JSON.\n"
    "  --output <file>     Write the summary to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 nothing written: a plan exists already (without --replace), the media are unknown, or the\n"
    "catalog has no such system; 2 usage error; 3 the data, the catalog, the selection or the overrides could not be\n"
    "read, the plan breaks a hard rule of the validation, the world is locked by another program, or the plan could\n"
    "not be written.\n";

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

} // namespace

ExitCode runWorldPlanGenerate(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.option("system", "system", "The room's system.");
    parser.option("selection", "file", "The room's selection (canonical JSON).");
    parser.option("seed", "n", "The world seed.");
    parser.option("library", "id", "The library the plan belongs to.");
    parser.flag("replace", "Regenerate over the world's existing plan, after a backup.");
    parser.option("plan", "file", "Write the plan to this file.");
    parser.flag("json", "Write the summary as JSON.");
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
    const auto planOption = args->value("plan");
    const bool replace = args->has("replace");
    if (planOption && replace) {
        return usageError(context, "--replace and --plan exclude each other");
    }

    auto config = loadWorldConfiguration(context, kName, seed);
    if (!config) {
        return ExitCode::RuntimeFailure;
    }
    const auto selectionFile = args->value("selection");
    Selected selected =
        selectionFile ? selectionFromFile(context, kName, *selectionFile, args->value("library"))
                      : selectionFromCatalog(context, kName, *system, args->value("library"), *config, "no plan");
    if (!selected.selection) {
        return selected.failure;
    }
    auto data = world::loadWorldData(context.paths.assetRoot(), *system);
    if (!data) {
        return failure(context, "the world's data", data.error());
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), selected.library);
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return failure(context, "the world's overrides", overrides.error());
    }

    // The world's plan is written under its lock, taken before anything is decided from the folder.
    std::optional<world::WorldLock> lock;
    std::error_code error;
    if (!planOption) {
        auto acquired = world::WorldLock::acquire(files);
        if (!acquired) {
            return failure(context, "the world's lock", acquired.error());
        }
        lock.emplace(std::move(*acquired));
        if (!replace && std::filesystem::exists(files.plan, error)) {
            context.err.writeLine(fmt::format(
                "{}: a plan exists already: {} ('worldplan update' updates it; 'worldplan generate --replace' "
                "regenerates it after a backup)",
                kName, pathText(files.plan)));
            return ExitCode::ProblemsFound;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    auto result = world::generate(
        {selected.library, std::move(*selected.selection), std::move(*config), std::move(*overrides)}, *data);
    if (!result) {
        return failure(context, "generation", result.error());
    }
    const auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
    const world::ValidationReport& validation = result->validation;
    const std::size_t hard = reportFindings(context, kName, validation);
    for (const std::string& pin : result->changes.needsConfirmation) {
        context.err.writeLine(fmt::format("{}: not honoured: {}", kName, pin));
    }
    if (hard > 0) {
        context.err.writeLine(fmt::format("{}: the plan breaks {} hard rule{} of the validation: nothing written",
                                          kName, hard, hard == 1 ? "" : "s"));
        return ExitCode::RuntimeFailure;
    }

    std::filesystem::path planPath;
    std::string saved;
    if (planOption) {
        planPath = core::pathFromUtf8(*planOption);
        auto text = world::writePlan(result->plan);
        if (!text) {
            return failure(context, "the plan", text.error());
        }
        if (Status written = core::writeTextFileAtomic(planPath, *text); !written) {
            return failure(context, "the plan", written.error());
        }
    } else {
        planPath = files.plan;
        world::SaveOptions options;
        options.writer = fmt::format("retroverse-cli worldplan generate{} ({})", replace ? " --replace" : "",
                                     core::versionBanner());
        options.duration = duration;
        auto report = world::savePlan(*lock, *result, options);
        if (!report) {
            return failure(context, "the plan", report.error());
        }
        if (report->backup) {
            saved = fmt::format("the previous plan kept as {}", pathText(report->backup->filename()));
        } else if (replace) {
            saved = report->written ? "no previous plan" : "the previous plan was this plan already";
        }
    }
    spdlog::info("worldplan generate: {} written ({} placements, digest {})", pathText(planPath),
                 result->plan.placements.size(), result->changes.digestAfter);

    const std::size_t soft = validation.findings.size() - hard;
    std::string summary;
    if (args->has("json")) {
        CatalogJson json = planSummaryJson(result->plan, result->changes.digestAfter);
        json["plan"] = pathText(planPath);
        json["replaced"] = replace;
        json["backup"] = saved;
        json["validation"] = CatalogJson{{"passed", true},
                                         {"soft", soft},
                                         {"layoutScorePermille", validation.layoutScorePermille
                                                                     ? CatalogJson(*validation.layoutScorePermille)
                                                                     : CatalogJson(nullptr)}};
        json["orphanedPins"] = result->changes.orphanedPins;
        summary = jsonText(json);
    } else {
        summary = planSummaryText(result->plan, result->changes.digestAfter,
                                  fmt::format("{} the plan of {}: {}", replace ? "Regenerated" : "Generated",
                                              result->plan.rooms.front().id.toString(), pathText(planPath)));
        summary += fmt::format("  {:<17}passed, {} soft finding{}, LayoutScore {}\n", "Validation:", soft,
                               soft == 1 ? "" : "s",
                               validation.layoutScorePermille ? std::to_string(*validation.layoutScorePermille)
                                                              : std::string("none"));
        if (!saved.empty()) {
            summary += fmt::format("  {:<17}{}\n", "Backup:", saved);
        }
        for (const std::string& pin : result->changes.orphanedPins) {
            summary += fmt::format("  {:<17}{}\n", "Orphaned pin:", pin);
        }
    }
    return writeResults(context, *file, summary, ExitCode::Ok);
}

} // namespace rv::cli
