// retroverse-cli worldplan show: a plan as text or JSON (ADR-0010 §8, I3 step 3): the plan file given, or the world's
// plan of the library <user data>/config/library.json records (or --library names), read with world::readPlan (a
// hand-edited or newer plan is refused), summarized: its versions and digest, the room, the fixtures by role, the
// slots and titles of each section with the reserve left, the dressing, the lights and the unplaced games by reason;
// with --placements every fixture with its levels and every placement too. With --scene <file>, its static part written
// as a scene file (I3 step 10a).

#include <filesystem>
#include <map>
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
#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan show";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan show [<plan>] [--library <id>] [--placements] [--json]\n"
    "                      [--scene <file>] [--output <file>]\n"
    "\n"
    "Shows a plan: its versions and digest, the room, the fixtures by role, the slots and titles of each section\n"
    "(featured and band face-out, spine spine-out) with the reserve left free, the shelf dressing, the lights and the\n"
    "games the room does not place, by reason.\n"
    "\n"
    "Arguments:\n"
    "  <plan>             A plan file (default: <user data>/worlds/<library id>/world-plan.json).\n"
    "\n"
    "Options:\n"
    "  --library <id>     The library whose plan to show (default: the one <user data>/config/library.json records).\n"
    "  --placements       Also list every fixture with its levels and every placement with its section.\n"
    "  --scene <file>     Also write the static part of the plan's room (the shell, the fixtures, the dressing, the\n"
    "                     lights) as a scene file that 'retroverse --scene <file>' renders (prefabs and lighting\n"
    "                     from the asset root; the collection's items are not part of it).\n"
    "  --json             Write the results as JSON.\n"
    "  --output <file>    Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help         Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 there is no plan; 2 usage error; 3 the plan could not be read or is refused (written by\n"
    "a newer Retroverse, or edited by hand), or its scene could not be built or written.\n";

/// The section of every slot level of the plan's fixtures.
std::map<std::pair<world::FixtureId, std::uint32_t>, world::Section> sectionsOf(const world::WorldPlan& plan) {
    std::map<std::pair<world::FixtureId, std::uint32_t>, world::Section> sections;
    for (const world::Fixture& fixture : plan.fixtures) {
        for (const world::FixtureLevel& level : fixture.levels) {
            sections[{fixture.id, level.level}] = level.section;
        }
    }
    return sections;
}

std::string sectionOf(const std::map<std::pair<world::FixtureId, std::uint32_t>, world::Section>& sections,
                      const world::SlotId& slot) {
    const auto found = sections.find({slot.fixture(), slot.level()});
    return found != sections.end() ? std::string(world::toString(found->second)) : std::string("none");
}

std::string listingText(const world::WorldPlan& plan) {
    std::string text = "Fixtures:\n";
    for (const world::Fixture& fixture : plan.fixtures) {
        std::string levels;
        for (const world::FixtureLevel& level : fixture.levels) {
            levels += fmt::format("{}{}: {} {}", levels.empty() ? "" : ", ", level.level, level.slots,
                                  world::toString(level.section));
        }
        text += fmt::format("  {}  {}  {} ({})  at {}, {}, {} mm, yaw {}.{:02} deg, {} x {} x {} mm{}\n",
                            fixture.id.toString(), world::toString(fixture.role), fixture.prefab.manifest,
                            fixture.prefab.kind, fixture.xMm, fixture.yMm, fixture.zMm, fixture.yawCdeg / 100,
                            fixture.yawCdeg % 100, fixture.widthMm, fixture.heightMm, fixture.depthMm,
                            levels.empty() ? "" : "; levels " + levels);
    }
    const auto sections = sectionsOf(plan);
    text += "Placements:\n";
    for (const world::Placement& placement : plan.placements) {
        text += fmt::format("  {}  {}  {}\n", placement.slot.toString(), placement.game.toString(),
                            sectionOf(sections, placement.slot));
    }
    return text;
}

CatalogJson listingJson(const world::WorldPlan& plan, CatalogJson json) {
    CatalogJson fixtures = CatalogJson::array();
    for (const world::Fixture& fixture : plan.fixtures) {
        CatalogJson levels = CatalogJson::array();
        for (const world::FixtureLevel& level : fixture.levels) {
            levels.push_back(CatalogJson{
                {"level", level.level}, {"slots", level.slots}, {"section", world::toString(level.section)}});
        }
        fixtures.push_back(CatalogJson{{"id", fixture.id.toString()},
                                       {"role", world::toString(fixture.role)},
                                       {"manifest", fixture.prefab.manifest},
                                       {"kind", fixture.prefab.kind},
                                       {"xMm", fixture.xMm},
                                       {"yMm", fixture.yMm},
                                       {"zMm", fixture.zMm},
                                       {"yawCdeg", fixture.yawCdeg},
                                       {"levels", std::move(levels)}});
    }
    const auto sections = sectionsOf(plan);
    CatalogJson placements = CatalogJson::array();
    for (const world::Placement& placement : plan.placements) {
        placements.push_back(CatalogJson{{"slot", placement.slot.toString()},
                                         {"game", placement.game.toString()},
                                         {"section", sectionOf(sections, placement.slot)}});
    }
    json["fixtureList"] = std::move(fixtures);
    json["placementList"] = std::move(placements);
    return json;
}

/// Writes the scene file of the plan's room; the line the summary gains, or nothing after a failure (reported).
std::optional<std::string> writeScene(CommandContext& context, const world::WorldPlan& plan,
                                      const std::filesystem::path& scenePath) {
    if (plan.rooms.empty()) {
        context.err.writeLine(fmt::format("{}: the plan has no room, so no scene", kName));
        return std::nullopt;
    }
    const world::RoomId& room = plan.rooms.front().id;
    auto data = world::loadSceneData(context.paths.assetRoot(), plan, room);
    if (!data) {
        context.err.writeLine(fmt::format("{}: the scene's data: {}", kName, data.error().toString()));
        return std::nullopt;
    }
    auto build = world::toSceneFile(plan, room, *data);
    if (!build) {
        context.err.writeLine(fmt::format("{}: the scene: {}", kName, build.error().toString()));
        return std::nullopt;
    }
    // Read back as the app reads it, so that a file written is a scene that loads.
    if (auto parsed = scene::parseSceneDescription(build->sceneFile, room.toString()); !parsed) {
        context.err.writeLine(fmt::format("{}: the scene: {}", kName, parsed.error().toString()));
        return std::nullopt;
    }
    if (Status written = core::writeTextFileAtomic(scenePath, world::sceneFileText(build->sceneFile)); !written) {
        context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
        return std::nullopt;
    }
    for (const world::UndressedSpan& span : build->undressed) {
        spdlog::info("worldplan show: {} slots {} to {} left bare: {}", span.level.toString(), span.firstSlot,
                     span.firstSlot + span.count - 1, span.reason);
    }
    return fmt::format("Scene {}: {} objects, {} lights, {} dressed span(s) left bare\n", pathText(scenePath),
                       build->sceneFile["objects"].size(), build->sceneFile["lights"].size(), build->undressed.size());
}

} // namespace

ExitCode runWorldPlanShow(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.positional("plan", "A plan file.");
    parser.option("library", "id", "The library whose plan to show.");
    parser.flag("placements", "Also list every fixture and placement.");
    parser.flag("json", "Write the results as JSON.");
    parser.option("scene", "file", "Also write the room's static part as a scene file.");
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
    std::filesystem::path planPath;
    if (const auto given = args->positional("plan")) {
        if (args->value("library")) {
            return usageError(context, "a plan file and --library exclude each other");
        }
        planPath = core::pathFromUtf8(*given);
    } else {
        ExitCode libraryFailure = ExitCode::UsageError;
        const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
        if (!library) {
            return libraryFailure;
        }
        planPath = defaultPlanPath(context.paths, *library);
    }
    auto text = core::readTextFile(planPath);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan at {} (run 'retroverse-cli worldplan generate')", kName,
                                              pathText(planPath)));
            return ExitCode::ProblemsFound;
        }
        context.err.writeLine(fmt::format("{}: {}", kName, text.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    auto plan = world::readPlan(*text, pathText(planPath.filename()));
    if (!plan) {
        context.err.writeLine(fmt::format("{}: {}", kName, plan.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    const std::string digest = core::sha256Hex(*text);
    std::optional<std::string> sceneLine;
    if (const auto scenePath = args->value("scene")) {
        sceneLine = writeScene(context, *plan, core::pathFromUtf8(*scenePath));
        if (!sceneLine) {
            return ExitCode::RuntimeFailure;
        }
    }
    std::string results;
    if (args->has("json")) {
        CatalogJson json = planSummaryJson(*plan, digest);
        json["plan"] = pathText(planPath);
        if (args->has("placements")) {
            json = listingJson(*plan, std::move(json));
        }
        if (const auto scenePath = args->value("scene")) {
            json["scene"] = *scenePath;
        }
        results = jsonText(json);
    } else {
        results = planSummaryText(*plan, digest, fmt::format("Plan {}", pathText(planPath)));
        if (args->has("placements")) {
            results += listingText(*plan);
        }
        if (sceneLine) {
            results += *sceneLine;
        }
    }
    return writeResults(context, *file, results, ExitCode::Ok);
}

} // namespace rv::cli
