// retroverse-cli worldplan generate: a fresh plan for a system's room (ADR-0010 §2 to §8, §11; I3 step 3). The room's
// selection is the catalog's, as the app makes it (I3 step 2): SqliteCatalogQuery::roomEntries on a read-only
// connection, esde::checkRoomMedia against the media root of the library <user data>/config/library.json records, then
// world::selectRoom; nothing is generated while the media are unknown (exit 1, ADR-0010 §3). --selection takes a
// canonical selection file instead (tests, a selection kept from elsewhere). Then the world configuration (the preset
// under <user data>/config/world.json, the seed from --seed when given), the world's data under the asset root, and
// world::generate, whose plan is validated (world::validatePlan, I3 step 4): a plan that breaks a hard rule is never
// written (exit 3, its findings on stderr). The plan is written atomically to
// <user data>/worlds/<library id>/world-plan.json, never over an existing one (--replace, the explicit regeneration
// after a backup, is I3 step 9's), or to the file --plan names; its summary goes to stdout or --output.

#include <charconv>
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
#include <rv/catalog/room_entry.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "library_snapshot.hpp"
#include "world_plan_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan generate";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan generate --system <system> [--selection <file>]\n"
    "                      [--seed <n>] [--library <id>] [--plan <file>] [--json] [--output <file>]\n"
    "\n"
    "Generates a fresh plan for a system's room: which titles it shows and where (the display fixtures and the hero\n"
    "band face-out, the rest spine-out in as few shelf modules as they and the growth reserve need), its fixtures,\n"
    "decoration, shelf dressing and lights. The titles are the catalog's (<user data>/database/retroverse.sqlite, made\n"
    "by an import) for the library <user data>/config/library.json records, one per title with a cover; nothing is\n"
    "generated while the library's media root cannot be reached. The plan is validated, then written to\n"
    "<user data>/worlds/<library id>/world-plan.json, never over an existing plan; its summary goes to stdout.\n"
    "\n"
    "Options:\n"
    "  --system <system>   The room's system, as ES-DE names it (snes).\n"
    "  --selection <file>  Take the room's selection from this file (canonical JSON: the titles in priority order and\n"
    "                      the games the room does not place) instead of the catalog.\n"
    "  --seed <n>          The world seed, 0 to 18446744073709551615 (default: world.json's, else derived from the\n"
    "                      library id).\n"
    "  --library <id>      The library the plan belongs to (default: the one library.json records; with the catalog's\n"
    "                      selection it must be that one).\n"
    "  --plan <file>       Write the plan to this file instead (replaced when it exists).\n"
    "  --json              Write the summary as JSON.\n"
    "  --output <file>     Write the summary to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 nothing written: a plan exists already, the media are unknown, or the catalog has no such\n"
    "system; 2 usage error; 3 the data, the catalog or the selection could not be read, the plan breaks a hard rule of\n"
    "the validation, or it could not be written.\n";

std::optional<std::uint64_t> parseSeed(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty() ||
        (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    return value;
}

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

/// A selection and the library it is for, or how the command ends.
struct Selected {
    std::optional<world::RoomSelection> selection;
    catalog::LibraryId library;
    ExitCode failure = ExitCode::RuntimeFailure;
};

/// The selection of a canonical selection file, for the library --library names or library.json records.
Selected selectionFromFile(CommandContext& context, const std::string& file, const std::optional<std::string>& given) {
    Selected selected;
    const auto library = libraryOf(context, kName, given, selected.failure);
    if (!library) {
        return selected;
    }
    selected.library = *library;
    const std::filesystem::path path = core::pathFromUtf8(file);
    auto text = core::readTextFile(path);
    if (!text) {
        selected.failure = failure(context, "the selection", text.error());
        return selected;
    }
    auto selection = world::readRoomSelection(*text, pathText(path));
    if (!selection) {
        selected.failure = failure(context, "the selection", selection.error());
        return selected;
    }
    selected.selection = std::move(*selection);
    return selected;
}

/// The catalog's selection for `system`, as the app's catalog worker makes it (I3 step 2): the bulk snapshot, the media
/// root checked, world::selectRoom.
Selected selectionFromCatalog(CommandContext& context, const catalog::SystemName& system,
                              const std::optional<std::string>& given, const world::WorldConfig& config) {
    Selected selected;
    const auto profile = readRecordedLibrary(context, kName, selected.failure);
    if (!profile) {
        return selected;
    }
    if (given && *given != profile->id.str()) {
        context.err.writeLine(fmt::format("{}: --library {} is not the catalog's library {} (library.json); a "
                                          "selection of another library comes with --selection",
                                          kName, *given, profile->id.str()));
        selected.failure = ExitCode::UsageError;
        return selected;
    }
    selected.library = profile->id;
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        selected.failure = catalogFailure(context, reader.error());
        return selected;
    }
    auto snapshot = reader->query->roomEntries(system);
    if (!snapshot) {
        if (snapshot.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: the catalog has no system {}", kName, system.str()));
            selected.failure = ExitCode::ProblemsFound;
            return selected;
        }
        selected.failure = catalogFailure(context, snapshot.error());
        return selected;
    }
    esde::EsdeMediaResolver resolver(mediaRootOf(*profile));
    esde::checkRoomMedia(*snapshot, resolver);
    auto selection = world::selectRoom(world::RoomInput{profile->id, std::move(*snapshot)}, config);
    if (!selection) {
        if (selection.error().code == ErrorCode::Unavailable) {
            // Unknown media decide nothing (ADR-0010 §3): no plan until the media root can be reached.
            context.err.writeLine(fmt::format("{}: no plan while the media are unknown: {}", kName,
                                              selection.error().message));
            selected.failure = ExitCode::ProblemsFound;
            return selected;
        }
        selected.failure = failure(context, "the room's selection", selection.error());
        return selected;
    }
    selected.selection = std::move(*selection);
    return selected;
}

} // namespace

ExitCode runWorldPlanGenerate(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.option("system", "system", "The room's system.");
    parser.option("selection", "file", "The room's selection (canonical JSON).");
    parser.option("seed", "n", "The world seed.");
    parser.option("library", "id", "The library the plan belongs to.");
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
        seed = parseSeed(*seedText);
        if (!seed) {
            return usageError(context, fmt::format("--seed: not an integer from 0 to 2^64 - 1: '{}'", *seedText));
        }
    }

    auto loaded = world::loadWorldConfig(context.paths.assetRoot(), context.paths.configDir());
    if (!loaded) {
        return failure(context, "the world configuration", loaded.error());
    }
    for (const Error& issue : loaded->issues) {
        spdlog::warn("world.json left out: {}", issue.toString());
    }
    world::WorldConfig config = std::move(loaded->config);
    if (seed) {
        config.seed = seed;
    }
    const auto selectionFile = args->value("selection");
    Selected selected = selectionFile ? selectionFromFile(context, *selectionFile, args->value("library"))
                                      : selectionFromCatalog(context, *system, args->value("library"), config);
    if (!selected.selection) {
        return selected.failure;
    }
    auto data = world::loadWorldData(context.paths.assetRoot(), *system);
    if (!data) {
        return failure(context, "the world's data", data.error());
    }
    auto result = world::generate({selected.library, std::move(*selected.selection), std::move(config)}, *data);
    if (!result) {
        return failure(context, "generation", result.error());
    }
    const world::ValidationReport& validation = result->validation;
    std::size_t hard = 0;
    for (const world::ValidationFinding& finding : validation.findings) {
        if (finding.severity == world::Severity::Hard) {
            ++hard;
            context.err.writeLine(fmt::format("{}: {} {}: {}", kName, finding.rule, finding.subject, finding.message));
        } else {
            spdlog::warn("worldplan generate: soft finding {} {}: {}", finding.rule, finding.subject, finding.message);
        }
    }
    if (hard > 0) {
        context.err.writeLine(
            fmt::format("{}: the plan breaks {} hard rule{} of the validation: nothing written", kName, hard,
                        hard == 1 ? "" : "s"));
        return ExitCode::RuntimeFailure;
    }

    const auto planOption = args->value("plan");
    const std::filesystem::path planPath =
        planOption ? core::pathFromUtf8(*planOption) : defaultPlanPath(context.paths, selected.library);
    std::error_code error;
    if (!planOption && std::filesystem::exists(planPath, error)) {
        context.err.writeLine(fmt::format("{}: a plan exists already: {} ('worldplan generate --replace', I3 step 9, "
                                          "regenerates it after a backup)",
                                          kName, pathText(planPath)));
        return ExitCode::ProblemsFound;
    }
    auto text = world::writePlan(result->plan);
    if (!text) {
        return failure(context, "the plan", text.error());
    }
    if (Status written = core::writeTextFileAtomic(planPath, *text); !written) {
        return failure(context, "the plan", written.error());
    }
    spdlog::info("worldplan generate: {} written ({} placements, digest {})", pathText(planPath),
                 result->plan.placements.size(), result->changes.digestAfter);

    const std::size_t soft = validation.findings.size() - hard;
    std::string summary;
    if (args->has("json")) {
        CatalogJson json = planSummaryJson(result->plan, result->changes.digestAfter);
        json["plan"] = pathText(planPath);
        json["validation"] = CatalogJson{{"passed", true},
                                         {"soft", soft},
                                         {"layoutScorePermille", validation.layoutScorePermille
                                                                     ? CatalogJson(*validation.layoutScorePermille)
                                                                     : CatalogJson(nullptr)}};
        summary = jsonText(json);
    } else {
        summary = planSummaryText(result->plan, result->changes.digestAfter,
                                  fmt::format("Generated the plan of {}: {}", result->plan.rooms.front().id.toString(),
                                              pathText(planPath)));
        summary += fmt::format("  {:<17}passed, {} soft finding{}, LayoutScore {}\n", "Validation:", soft,
                               soft == 1 ? "" : "s",
                               validation.layoutScorePermille ? std::to_string(*validation.layoutScorePermille)
                                                              : std::string("none"));
    }
    return writeResults(context, *file, summary, ExitCode::Ok);
}

} // namespace rv::cli
