// retroverse-cli worldplan show: a plan as text or JSON (ADR-0010 §8, ADR-0013 §12; I3 step 3, I4 step 1): the plan
// file given, or the world's plan of the library <user data>/config/library.json records (or --library names), read
// with world::readPlan (a hand-edited or newer plan is refused; a schema-1 plan lifted into the building), summarized:
// its versions and digest, its floors, its rooms (kind, floor, sections, placement), the fixtures by role, the slots
// and titles of each section with the reserve left, the dressing, the lights and the unplaced games by reason; with
// --placements every fixture with its levels and every placement too. With --scene <file>, the static part of one room
// written as a scene file (I3 step 10a): the plan's only room, or the one --room names in a building of several, its
// sign and plastics classified with the catalog's platform tags of its system when the catalog holds the plan's
// library (a custom system's, I4 step 2). With --floor <id> (I4 step 3, ADR-0013 §12), the floor's hall and rooms are
// summarized and --scene writes the whole floor: its rooms moved onto it, its hall with the elevator, its areas.

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
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
    "                      [--floor <id>] [--scene <file> [--room <id>]] [--output <file>]\n"
    "\n"
    "Shows a plan: its versions and digest, its floors and rooms (kind, floor, systems, place on the floor), the\n"
    "fixtures by role, the slots and titles of each section (featured and band face-out, spine spine-out) with the\n"
    "reserve left free, the shelf dressing, the lights and the games the rooms do not place, by reason. A plan of\n"
    "schema 1 (I3's one room) is shown lifted into the building.\n"
    "\n"
    "Arguments:\n"
    "  <plan>             A plan file (default: <user data>/worlds/<library id>/world-plan.json).\n"
    "\n"
    "Options:\n"
    "  --library <id>     The library whose plan to show (default: the one <user data>/config/library.json records).\n"
    "  --placements       Also list every fixture with its levels and every placement with its section.\n"
    "  --scene <file>     Also write the static part of a room (the shell, the fixtures, the dressing, the lights)\n"
    "                     as a scene file that 'retroverse --scene <file>' renders (prefabs and lighting from the\n"
    "                     asset root; the collection's items are not part of it): the plan's only room, or --room's.\n"
    "  --room <id>        The room whose scene --scene writes (room:snes); required when the plan holds several.\n"
    "  --floor <id>       Also summarize a floor: its hall (derived from its rooms: the elevator at its west end,\n"
    "                     its lights, its walls) and its rooms with their doorways onto it; with --scene, write the\n"
    "                     floor's static part instead of a room's: every room moved onto the floor, the hall and\n"
    "                     the elevator, the areas whose lights the app packs (not with --room).\n"
    "  --json             Write the results as JSON.\n"
    "  --output <file>    Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help         Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 there is no plan; 2 usage error (--scene on a plan of several rooms without --room or\n"
    "--floor, a --room or --floor the plan does not hold, --room with --floor); 3 the plan could not be read or is\n"
    "refused (written by a newer Retroverse, or edited by hand), or its scene could not be built or written.\n";

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

/// The ES-DE platform tags of `system` as the catalog of the user data directory records them for `library` (as
/// `worldplan generate` took them: a system systems.json does not name was classified by them); none without a
/// catalog of that library (logged at debug level).
std::vector<std::string> catalogPlatformTags(const core::Paths& paths, const catalog::LibraryId& library,
                                             const catalog::SystemName& system) {
    auto reader = openCatalogReader(paths);
    if (!reader) {
        spdlog::debug("worldplan show: no platform tags of {} from the catalog: {}", system.str(),
                      reader.error().toString());
        return {};
    }
    auto summaries = reader->query->systems();
    if (!summaries) {
        spdlog::debug("worldplan show: no platform tags of {} from the catalog: {}", system.str(),
                      summaries.error().toString());
        return {};
    }
    for (const catalog_db::SystemSummary& summary : *summaries) {
        if (summary.library == library && summary.system.name == system) {
            return summary.system.platforms;
        }
    }
    return {};
}

/// The ES-DE platform tags of every system the catalog holds for `library` (catalogPlatformTags()'s, all at once).
world::PlatformTagsBySystem catalogPlatformTagsOf(const core::Paths& paths, const catalog::LibraryId& library) {
    world::PlatformTagsBySystem tags;
    auto reader = openCatalogReader(paths);
    if (!reader) {
        spdlog::debug("worldplan show: no platform tags from the catalog: {}", reader.error().toString());
        return tags;
    }
    auto summaries = reader->query->systems();
    if (!summaries) {
        spdlog::debug("worldplan show: no platform tags from the catalog: {}", summaries.error().toString());
        return tags;
    }
    for (const catalog_db::SystemSummary& summary : *summaries) {
        if (summary.library == library && !summary.system.platforms.empty()) {
            tags[summary.system.name] = summary.system.platforms;
        }
    }
    return tags;
}

/// The theme pack each room of `plan` is drawn with now (world::roomThemePack(), ADR-0014 §1), for the summary; a room
/// whose pack cannot be resolved (logged at debug level) shows its recorded one.
std::map<world::RoomId, std::string> resolvedThemePacks(CommandContext& context, const world::WorldPlan& plan) {
    std::map<world::RoomId, std::string> packs;
    auto building = world::loadBuildingData(context.paths.assetRoot());
    auto systems = world::loadSystemsData(context.paths.assetRoot());
    if (!building || !systems) {
        spdlog::debug("worldplan show: the rooms' theme packs as recorded: {}",
                      (!building ? building.error() : systems.error()).toString());
        return packs;
    }
    for (const world::Room& room : plan.rooms) {
        if (auto pack = world::roomThemePack(context.paths.assetRoot(), room, {}, *systems, *building)) {
            packs.emplace(room.id, std::move(*pack));
        } else {
            spdlog::debug("worldplan show: {}'s theme pack as recorded: {}", room.id.toString(),
                          pack.error().toString());
        }
    }
    return packs;
}

/// A floor of the plan as `--floor` shows it: its hall and rooms, and its scene when --scene asks for it.
struct FloorView {
    world::FloorSceneBuild build;
    std::string sceneLine;
};

/// Builds the floor `floor` of `plan` (its data from the asset root, the rooms' systems classified with the catalog's
/// platform tags) and writes its scene to `scenePath` when given; nothing after a failure (reported).
std::optional<FloorView> buildFloor(CommandContext& context, const world::WorldPlan& plan, std::string_view floor,
                                    const std::optional<std::filesystem::path>& scenePath) {
    auto data = world::loadFloorSceneData(context.paths.assetRoot(), plan, floor, std::nullopt,
                                          catalogPlatformTagsOf(context.paths, plan.library));
    if (!data) {
        context.err.writeLine(fmt::format("{}: the floor's data: {}", kName, data.error().toString()));
        return std::nullopt;
    }
    auto build = world::toFloorSceneFile(plan, floor, *data);
    if (!build) {
        context.err.writeLine(fmt::format("{}: the floor's scene: {}", kName, build.error().toString()));
        return std::nullopt;
    }
    FloorView view{std::move(*build), {}};
    if (!scenePath) {
        return view;
    }
    // Read back as the app reads it, so that a file written is a scene that loads.
    if (auto parsed = scene::parseSceneDescription(view.build.sceneFile, "floor:" + std::string(floor)); !parsed) {
        context.err.writeLine(fmt::format("{}: the floor's scene: {}", kName, parsed.error().toString()));
        return std::nullopt;
    }
    if (Status written = core::writeTextFileAtomic(*scenePath, world::sceneFileText(view.build.sceneFile)); !written) {
        context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
        return std::nullopt;
    }
    for (const world::UndressedSpan& span : view.build.undressed) {
        spdlog::info("worldplan show: {} slots {} to {} left bare: {}", span.level.toString(), span.firstSlot,
                     span.firstSlot + span.count - 1, span.reason);
    }
    // The hall's lights: its ceiling lights and (I6 step 1) a fill spot before each doorway.
    const std::string hallArea = world::hallAreaId(floor);
    const auto hallLights =
        std::count_if(view.build.sceneFile["lights"].begin(), view.build.sceneFile["lights"].end(),
                      [&](const nlohmann::json& light) { return light.value("area", std::string()) == hallArea; });
    view.sceneLine = fmt::format("Scene {} of floor {}: {} objects, {} lights ({} in the hall), {} areas, {} dressed "
                                 "span(s) left bare\n",
                                 pathText(*scenePath), floor, view.build.sceneFile["objects"].size(),
                                 view.build.sceneFile["lights"].size(), hallLights,
                                 view.build.sceneFile["areas"].size(), view.build.undressed.size());
    return view;
}

std::string_view rowText(world::Row row) {
    return row == world::Row::North ? "north row" : "south row";
}

std::string floorText(const world::FloorSceneBuild& build) {
    const world::Hall& hall = build.hall;
    std::string text = fmt::format("Floor {}: {} {} x {} mm, {} mm high, {} light(s) every {} mm, the elevator at its "
                                   "west end; {} room(s)\n",
                                   hall.floor, hall.lobby ? "lobby" : "hall", hall.interior.maxXMm, hall.widthMm,
                                   hall.heightMm, hall.lights.size(), world::kHallLightPitchMm, build.rooms.size());
    for (const world::FloorDoorway& doorway : hall.doorways) {
        text += fmt::format("  {}  {}, doorway x {} to {} mm{}\n", doorway.room.toString(), rowText(doorway.row),
                            doorway.minMm, doorway.maxMm, doorway.opensOntoHall() ? " onto the hall" : ", off the hall");
    }
    return text;
}

CatalogJson floorJson(const world::FloorSceneBuild& build) {
    const world::Hall& hall = build.hall;
    CatalogJson rooms = CatalogJson::array();
    for (const world::FloorDoorway& doorway : hall.doorways) {
        rooms.push_back(CatalogJson{{"id", doorway.room.toString()},
                                    {"row", std::string(rowText(doorway.row))},
                                    {"doorway", CatalogJson{{"minXMm", doorway.minMm},
                                                            {"maxXMm", doorway.maxMm},
                                                            {"opensOntoHall", doorway.opensOntoHall()}}}});
    }
    return CatalogJson{{"floor", hall.floor},
                       {"lobby", hall.lobby},
                       {"hall", CatalogJson{{"lengthMm", hall.interior.maxXMm},
                                            {"widthMm", hall.widthMm},
                                            {"heightMm", hall.heightMm},
                                            {"lights", hall.lights.size()},
                                            {"walls", hall.walls.size()}}},
                       {"rooms", std::move(rooms)}};
}

/// Writes the scene file of the plan's room `room`; the line the summary gains, or nothing after a failure (reported).
std::optional<std::string> writeScene(CommandContext& context, const world::WorldPlan& plan, const world::RoomId& room,
                                      const std::filesystem::path& scenePath) {
    // The room's first system's platform tags: its sign and plastics as its generation classified it.
    std::vector<std::string> platformTags;
    if (const auto held = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                       [&](const world::Room& candidate) { return candidate.id == room; });
        held != plan.rooms.end()) {
        platformTags = catalogPlatformTags(context.paths, plan.library,
                                           held->sections.empty() ? room.system() : held->sections.front());
    }
    auto data = world::loadSceneData(context.paths.assetRoot(), plan, room, std::nullopt, platformTags);
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
    return fmt::format("Scene {} of {}: {} objects, {} lights, {} dressed span(s) left bare\n", pathText(scenePath),
                       room.toString(), build->sceneFile["objects"].size(), build->sceneFile["lights"].size(),
                       build->undressed.size());
}

} // namespace

ExitCode runWorldPlanShow(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.positional("plan", "A plan file.");
    parser.option("library", "id", "The library whose plan to show.");
    parser.flag("placements", "Also list every fixture and placement.");
    parser.flag("json", "Write the results as JSON.");
    parser.option("scene", "file", "Also write a room's static part as a scene file.");
    parser.option("room", "id", "The room whose scene --scene writes.");
    parser.option("floor", "id", "Also summarize a floor; with --scene, write the floor's scene.");
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
    auto read = readPlanFile(context.paths, planPath);
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan at {} (run 'retroverse-cli worldplan generate')", kName,
                                              pathText(planPath)));
            return ExitCode::ProblemsFound;
        }
        context.err.writeLine(fmt::format("{}: {}", kName, read.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    const world::WorldPlan* plan = &read->plan;
    const std::string& digest = read->digest;
    std::optional<std::string> sceneLine;
    std::optional<world::RoomId> sceneRoom;
    std::optional<FloorView> floorView;
    if (const auto floorId = args->value("floor")) {
        if (args->value("room")) {
            return usageError(context, "--room and --floor exclude each other: --scene writes a room or a floor");
        }
        const bool held = std::any_of(plan->floors.begin(), plan->floors.end(),
                                      [&](const world::Floor& floor) { return floor.id == *floorId; });
        if (!held) {
            return usageError(context, fmt::format("--floor: the plan holds no floor {}", *floorId));
        }
        std::optional<std::filesystem::path> scenePath;
        if (const auto scene = args->value("scene")) {
            scenePath = core::pathFromUtf8(*scene);
        }
        floorView = buildFloor(context, *plan, *floorId, scenePath);
        if (!floorView) {
            return ExitCode::RuntimeFailure;
        }
    }
    if (const auto roomText = args->value("room")) {
        if (!args->value("scene")) {
            return usageError(context, "--room names the room whose scene --scene writes");
        }
        auto room = world::RoomId::parse(*roomText);
        if (!room) {
            return usageError(context, fmt::format("--room: {}", room.error().toString()));
        }
        const bool held = std::any_of(plan->rooms.begin(), plan->rooms.end(),
                                      [&](const world::Room& candidate) { return candidate.id == *room; });
        if (!held) {
            return usageError(context, fmt::format("--room: the plan holds no room {}", room->toString()));
        }
        sceneRoom = std::move(*room);
    }
    if (const auto scenePath = args->value("scene"); scenePath && !floorView) {
        if (!sceneRoom) {
            if (plan->rooms.empty()) {
                context.err.writeLine(fmt::format("{}: the plan has no room, so no scene", kName));
                return ExitCode::RuntimeFailure;
            }
            if (plan->rooms.size() > 1) {
                return usageError(context, fmt::format("the plan holds {} rooms: --room <id> names the one whose "
                                                       "scene --scene writes ({})",
                                                       plan->rooms.size(), roomsText(*plan)));
            }
            sceneRoom = plan->rooms.front().id;
        }
        sceneLine = writeScene(context, *plan, *sceneRoom, core::pathFromUtf8(*scenePath));
        if (!sceneLine) {
            return ExitCode::RuntimeFailure;
        }
    }
    std::string results;
    if (args->has("json")) {
        CatalogJson json = planSummaryJson(*plan, digest);
        json["plan"] = pathText(planPath);
        json["fileSchemaVersion"] = read->fileSchema;
        json["lifted"] = read->lifted();
        if (args->has("placements")) {
            json = listingJson(*plan, std::move(json));
        }
        if (floorView) {
            json["floorScene"] = floorJson(floorView->build);
            if (const auto scenePath = args->value("scene")) {
                json["scene"] = *scenePath;
                json["sceneFloor"] = floorView->build.hall.floor;
            }
        } else if (const auto scenePath = args->value("scene")) {
            json["scene"] = *scenePath;
            json["sceneRoom"] = sceneRoom->toString();
        }
        results = jsonText(json);
    } else {
        results = planSummaryText(*plan, digest,
                                  fmt::format("Plan {}{}", pathText(planPath),
                                              read->lifted() ? " (schema 1, lifted into the building)" : ""),
                                  resolvedThemePacks(context, *plan));
        if (args->has("placements")) {
            results += listingText(*plan);
        }
        if (floorView) {
            results += floorText(floorView->build);
            results += floorView->sceneLine;
        }
        if (sceneLine) {
            results += *sceneLine;
        }
    }
    return writeResults(context, *file, results, ExitCode::Ok);
}

} // namespace rv::cli
