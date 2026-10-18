// retroverse-cli worldplan generate: a fresh plan for a system's room (ADR-0010 §2 to §8, I3 step 3): the room's
// selection (the catalog's, world::selectRoom: I3 step 2; meanwhile a canonical selection file, --selection), the world
// configuration (the default preset under <user data>/config/world.json, the seed from --seed when given), the world's
// data under the asset root, then world::generate. The plan is written atomically to
// <user data>/worlds/<library id>/world-plan.json, never over an existing one (--replace, the explicit regeneration
// after a backup, is I3 step 9's), or to the file --plan names; its summary goes to stdout or --output. The plan's
// validation (worldplan check) is I3 step 4's.

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/ids.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan generate";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan generate --system <system> --selection <file>\n"
    "                      [--seed <n>] [--library <id>] [--plan <file>] [--json] [--output <file>]\n"
    "\n"
    "Generates a fresh plan for a system's room: which titles it shows and where (the display fixtures and the hero\n"
    "band face-out, the rest spine-out in as few shelf modules as they and the growth reserve need), its fixtures,\n"
    "decoration, shelf dressing and lights. The plan is written to <user data>/worlds/<library id>/world-plan.json,\n"
    "never over an existing plan; its summary goes to stdout.\n"
    "\n"
    "Options:\n"
    "  --system <system>   The room's system, as ES-DE names it (snes).\n"
    "  --selection <file>  The room's selection as canonical JSON (the titles in priority order and the games the\n"
    "                      room does not place). The selection from the catalog comes with I3 step 2.\n"
    "  --seed <n>          The world seed, 0 to 18446744073709551615 (default: world.json's, else derived from the\n"
    "                      library id).\n"
    "  --library <id>      The library the plan belongs to (default: the one <user data>/config/library.json\n"
    "                      records).\n"
    "  --plan <file>       Write the plan to this file instead (replaced when it exists).\n"
    "  --json              Write the summary as JSON.\n"
    "  --output <file>     Write the summary to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 a plan exists already (nothing written); 2 usage error; 3 the data, the selection or the\n"
    "plan could not be read, generated or written.\n";

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
    const auto selectionFile = args->value("selection");
    if (!selectionFile) {
        return usageError(context, "--selection is required: the room's selection from the catalog "
                                   "(world::selectRoom) comes with I3 step 2");
    }
    std::optional<std::uint64_t> seed;
    if (const auto seedText = args->value("seed")) {
        seed = parseSeed(*seedText);
        if (!seed) {
            return usageError(context, fmt::format("--seed: not an integer from 0 to 2^64 - 1: '{}'", *seedText));
        }
    }
    ExitCode libraryFailure = ExitCode::UsageError;
    const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
    if (!library) {
        return libraryFailure;
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
    const std::filesystem::path selectionPath = core::pathFromUtf8(*selectionFile);
    auto selectionText = core::readTextFile(selectionPath);
    if (!selectionText) {
        return failure(context, "the selection", selectionText.error());
    }
    auto selection = world::readRoomSelection(*selectionText, pathText(selectionPath));
    if (!selection) {
        return failure(context, "the selection", selection.error());
    }
    auto data = world::loadWorldData(context.paths.assetRoot(), *system);
    if (!data) {
        return failure(context, "the world's data", data.error());
    }
    auto result = world::generate({*library, std::move(*selection), std::move(config)}, *data);
    if (!result) {
        return failure(context, "generation", result.error());
    }

    const auto planOption = args->value("plan");
    const std::filesystem::path planPath =
        planOption ? core::pathFromUtf8(*planOption) : defaultPlanPath(context.paths, *library);
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

    std::string summary;
    if (args->has("json")) {
        CatalogJson json = planSummaryJson(result->plan, result->changes.digestAfter);
        json["plan"] = pathText(planPath);
        summary = jsonText(json);
    } else {
        summary = planSummaryText(result->plan, result->changes.digestAfter,
                                  fmt::format("Generated the plan of {}: {}", result->plan.rooms.front().id.toString(),
                                              pathText(planPath)));
    }
    return writeResults(context, *file, summary, ExitCode::Ok);
}

} // namespace rv::cli
