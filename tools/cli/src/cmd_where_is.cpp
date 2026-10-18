// retroverse-cli where-is: where a game stands in its room's plan, from the plan's index (ADR-0010 §9; I3 step 9):
// `where-is <gameId>` reads the world's plan (<user data>/worlds/<library id>/world-plan.json) or the file --plan
// names, indexes it (world::PlanIndex: no table, the plan alone) and names the game's room, fixture, slot and section,
// or the reason it is not placed (a duplicate title with its representative and where that one stands). Nothing is
// written but the log.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rv/catalog/ids.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli where-is";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] where-is <gameId> [--plan <file>] [--library <id>] [--json]\n"
    "                      [--output <file>]\n"
    "\n"
    "Tells where a game stands in its room's plan: the room, the fixture and its slot (level and index, 0 the lowest\n"
    "level and the leftmost slot as one faces the fixture), and whether it stands face-out (on a display fixture or\n"
    "in the hero band) or spine-out; or why the room does not place it (another version of its title stands there,\n"
    "it has no cover, its file was missing, or the room is full), with the version that stands for it.\n"
    "\n"
    "Arguments:\n"
    "  <gameId>           The game's GameId ('g' and 16 hexadecimal digits, as 'retroverse-cli games' lists it).\n"
    "\n"
    "Options:\n"
    "  --plan <file>      The plan to ask (default: <user data>/worlds/<library id>/world-plan.json).\n"
    "  --library <id>     The library whose plan to ask (default: the one <user data>/config/library.json records).\n"
    "  --json             Write the answer as JSON.\n"
    "  --output <file>    Write the answer to this file (UTF-8) instead of stdout.\n"
    "  -h, --help         Show this help and exit.\n"
    "\n"
    "Exit codes: 0 answered (placed, or not placed with its reason); 1 the plan does not hold the game, or there is\n"
    "no plan; 2 usage error; 3 the plan could not be read or is refused (edited by hand or written by a newer\n"
    "Retroverse).\n";

std::string faceText(world::Section section) {
    return world::isFaceOut(section) ? "face-out" : "spine-out";
}

CatalogJson placeJson(const world::GamePlace& place) {
    return CatalogJson{{"slot", place.slot.toString()},
                       {"fixture", place.slot.fixture().toString()},
                       {"level", place.slot.level()},
                       {"index", place.slot.index()},
                       {"role", world::toString(place.role)},
                       {"manifest", place.prefab.manifest},
                       {"kind", place.prefab.kind},
                       {"section", world::toString(place.section)},
                       {"faceOut", world::isFaceOut(place.section)}};
}

std::string answerText(const world::GameLocation& location) {
    std::string text;
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("  {:<17}{}\n", fmt::format("{}:", label), value);
    };
    if (location.place) {
        const world::GamePlace& place = *location.place;
        text += fmt::format("{} stands in {}\n", location.game.toString(), location.room.toString());
        line("Slot", fmt::format("{} (level {}, slot {})", place.slot.toString(), place.slot.level(),
                                 place.slot.index()));
        line("Fixture", fmt::format("{} ({}, {})", place.slot.fixture().toString(), world::toString(place.role),
                                    place.prefab.manifest));
        line("Section", fmt::format("{} ({})", world::toString(place.section), faceText(place.section)));
        return text;
    }
    text += fmt::format("{} is not placed in {}: {}\n", location.game.toString(), location.room.toString(),
                        world::toString(*location.reason));
    if (location.representative.isValid()) {
        line("Representative", location.representative.toString());
        if (location.representativePlace) {
            line("It stands at", fmt::format("{} ({})", location.representativePlace->slot.toString(),
                                             faceText(location.representativePlace->section)));
        }
    }
    return text;
}

} // namespace

ExitCode runWhereIs(CommandContext& context) {
    core::ArgParser parser{std::string(kName)};
    parser.positional("gameId", "The game's GameId.");
    parser.option("plan", "file", "The plan to ask.");
    parser.option("library", "id", "The library whose plan to ask.");
    parser.flag("json", "Write the answer as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(context.arguments);
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
    const auto gameText = args->positional("gameId");
    if (!gameText) {
        return usageError(context, "a GameId is required");
    }
    auto game = catalog::GameId::parse(*gameText);
    if (!game) {
        return usageError(context, game.error().toString());
    }
    std::filesystem::path planPath;
    if (const auto given = args->value("plan")) {
        if (args->value("library")) {
            return usageError(context, "--plan and --library exclude each other");
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
    auto stored = world::readStoredPlan(planPath);
    if (!stored) {
        if (stored.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan at {} (run 'retroverse-cli worldplan generate')", kName,
                                              pathText(planPath)));
            return ExitCode::ProblemsFound;
        }
        context.err.writeLine(fmt::format("{}: {}", kName, stored.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    const world::PlanIndex index(stored->plan);
    const auto location = index.find(*game);
    if (!location) {
        context.err.writeLine(fmt::format("{}: the plan {} does not hold {} (not a game of its room's system, or one "
                                          "the catalog did not list when the plan was last updated)",
                                          kName, pathText(planPath), game->toString()));
        return ExitCode::ProblemsFound;
    }
    std::string answer;
    if (args->has("json")) {
        CatalogJson json;
        json["game"] = location->game.toString();
        json["room"] = location->room.toString();
        json["plan"] = pathText(planPath);
        json["digest"] = stored->digest;
        json["placed"] = location->placed();
        json["place"] = location->place ? placeJson(*location->place) : CatalogJson(nullptr);
        json["reason"] = location->reason ? CatalogJson(world::toString(*location->reason)) : CatalogJson(nullptr);
        json["representative"] = location->representative.isValid()
                                     ? CatalogJson(location->representative.toString())
                                     : CatalogJson(nullptr);
        json["representativePlace"] =
            location->representativePlace ? placeJson(*location->representativePlace) : CatalogJson(nullptr);
        answer = jsonText(json);
    } else {
        answer = answerText(*location);
    }
    return writeResults(context, *file, answer, ExitCode::Ok);
}

} // namespace rv::cli
