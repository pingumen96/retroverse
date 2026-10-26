// retroverse-cli worldplan check: a plan's validation (ADR-0010 §11, rv/world/validation.hpp; I3 step 4). `worldplan
// check <plan.json>` reads the plan (world::readPlan: a hand-edited text or a newer schema is not a plan this program
// reads; a schema-1 plan is judged as the building it lifts into, ADR-0013 §5), the building's data, the templates its
// rooms name and the prefabs' slots from the asset root, the world's configuration (the plan's preset with
// <user data>/config/world.json over it, as the generator reads it) and, with --selection, a room selection whose every
// game must have a placement or an unplaced entry; then it reports every finding, hard and soft, and the LayoutScore.
// A room made from an older version of its template or by another generator major is judged as a kept room
// (room-kept). Exit 1 when a hard rule fails. It writes nothing but its results and the log.
//
// The rooms' lights are the plan's (I3 step 3), counted per room. Not judged yet, since nothing holds them: pins
// (overrides.json, I3 step 9).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_config.hpp>

#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan check <plan.json> [--selection <file>] [--json]\n"
    "                      [--output <file>]\n"
    "\n"
    "Validates a room plan: the hard rules (a plan that breaks one is never saved or shown) and the soft ones\n"
    "(reported), and the LayoutScore as a diagnostic. The plan is judged against the templates of its rooms and the\n"
    "prefabs' slots under the asset root, and the passages and decoration density of the world's configuration (the\n"
    "plan's preset, with <user data>/config/world.json over it).\n"
    "\n"
    "Options:\n"
    "  --selection <file>  A room selection (canonical JSON): every game it names must have a placement or an\n"
    "                      unplaced entry in the plan. Without it, missing games are not checked.\n"
    "  --json              Write the results as JSON.\n"
    "  --output <file>     Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help          Show this help and exit.\n"
    "\n"
    "Exit codes: 0 no hard rule fails; 1 a hard rule fails, or the file is not a plan this program reads;\n"
    "2 usage error (a selection that cannot be read); 3 the plan file, the configuration or the results could not be\n"
    "read or written.\n";

constexpr std::string_view kName = "retroverse-cli worldplan check";

/// The findings of one rule the text lists; the others are counted (the JSON lists them all).
constexpr std::size_t kTextFindingsPerRule = 100;

using Json = nlohmann::json;

ExitCode usageError(CommandContext& context, std::string_view message) {
    context.err.writeLine(fmt::format("{}: {}", kName, message));
    context.err.writeLine("Run 'retroverse-cli worldplan check --help' for its options.");
    return ExitCode::UsageError;
}

ExitCode runtimeFailure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

ExitCode deliver(CommandContext& context, std::optional<Output>& file, const std::string& text, ExitCode code) {
    if (!file) {
        context.out.write(text);
        return code;
    }
    file->write(text);
    if (auto finished = file->finish(); !finished) {
        context.err.writeLine(fmt::format("{}: cannot write the results: {}", kName, finished.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    return code;
}

std::string jsonText(const Json& json) {
    return json.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

/// The configuration the plan was generated with, as far as this program can tell: the user's (the default preset or
/// the one world.json names, world.json over it) when it is of the plan's preset, else the plan's preset alone.
Expected<world::WorldConfig> configurationFor(const CommandContext& context, const world::WorldPlan& plan) {
    auto loaded = world::loadWorldConfig(context.paths.assetRoot(), context.paths.configDir());
    if (!loaded) {
        return propagate(std::move(loaded.error()));
    }
    for (const Error& issue : loaded->issues) {
        spdlog::warn("worldplan check: world.json left out: {}", issue.toString());
    }
    if (loaded->config.preset == plan.preset) {
        return loaded->config;
    }
    auto preset = world::loadPreset(context.paths.assetRoot(), plan.preset);
    if (!preset) {
        spdlog::warn("worldplan check: the plan's preset '{}' cannot be read ({}); the passages of '{}' are used",
                     plan.preset, preset.error().toString(), loaded->config.preset);
        return loaded->config;
    }
    return *preset;
}

/// The templates the plan's rooms name, those that load; the others fail the rule room-template.
std::vector<world::RoomTemplate> templatesFor(const CommandContext& context, const world::WorldPlan& plan) {
    std::map<std::string, bool> tried;
    std::vector<world::RoomTemplate> templates;
    for (const world::Room& room : plan.rooms) {
        if (!tried.emplace(room.templateId, true).second) {
            continue;
        }
        auto roomTemplate = world::loadRoomTemplate(context.paths.assetRoot(), room.templateId);
        if (!roomTemplate) {
            spdlog::warn("worldplan check: the template '{}' cannot be read: {}", room.templateId,
                         roomTemplate.error().toString());
            continue;
        }
        templates.push_back(std::move(*roomTemplate));
    }
    return templates;
}

struct CheckSummary {
    std::string planPath;
    std::string digest;
    std::optional<std::string> selectionPath;
    const world::WorldPlan* plan = nullptr;
    const world::ValidationReport* report = nullptr;
    std::size_t hard = 0;
    std::size_t soft = 0;
};

Json resultJson(const CheckSummary& summary) {
    const world::WorldPlan& plan = *summary.plan;
    Json rooms = Json::array();
    for (const world::Room& room : plan.rooms) {
        std::size_t fixtures = 0;
        for (const world::Fixture& fixture : plan.fixtures) {
            fixtures += fixture.id.room() == room.id ? 1 : 0;
        }
        rooms.push_back({{"id", room.id.toString()},
                         {"template", room.templateId},
                         {"templateVersion", room.templateVersion},
                         {"widthMm", room.maxXMm - room.minXMm},
                         {"depthMm", room.maxZMm - room.minZMm},
                         {"growthSteps", room.growthSteps},
                         {"fixtures", fixtures}});
    }
    Json findings = Json::array();
    for (const world::ValidationFinding& finding : summary.report->findings) {
        findings.push_back({{"rule", finding.rule},
                            {"severity", world::toString(finding.severity)},
                            {"subject", finding.subject},
                            {"message", finding.message}});
    }
    return Json{{"plan", summary.planPath},
                {"readable", true},
                {"error", nullptr},
                {"digest", summary.digest},
                {"selection", summary.selectionPath ? Json(*summary.selectionPath) : Json(nullptr)},
                {"rooms", rooms},
                {"placements", plan.placements.size()},
                {"dressing", plan.dressing.size()},
                {"unplaced", plan.unplaced.size()},
                {"passed", summary.report->passed()},
                {"hardCount", summary.hard},
                {"softCount", summary.soft},
                {"layoutScorePermille",
                 summary.report->layoutScorePermille ? Json(*summary.report->layoutScorePermille) : Json(nullptr)},
                {"findings", findings}};
}

std::string resultText(const CheckSummary& summary) {
    const world::WorldPlan& plan = *summary.plan;
    std::string text = fmt::format("Plan: {}\nDigest: {}\n", summary.planPath, summary.digest);
    for (const world::Room& room : plan.rooms) {
        std::size_t fixtures = 0;
        for (const world::Fixture& fixture : plan.fixtures) {
            fixtures += fixture.id.room() == room.id ? 1 : 0;
        }
        text += fmt::format("Room {}: template {} version {}, {} x {} mm, {} growth steps, {} fixtures\n",
                            room.id.toString(), room.templateId, room.templateVersion, room.maxXMm - room.minXMm,
                            room.maxZMm - room.minZMm, room.growthSteps, fixtures);
    }
    text += fmt::format("Placements: {}, dressed spans: {}, unplaced: {}\n", plan.placements.size(),
                        plan.dressing.size(), plan.unplaced.size());
    text += summary.selectionPath ? fmt::format("Selection: {} (every game of it checked)\n", *summary.selectionPath)
                                  : std::string("Selection: none (games missing from the plan are not checked)\n");
    if (summary.report->layoutScorePermille) {
        text += fmt::format("LayoutScore: {} permille\n", *summary.report->layoutScorePermille);
    }
    text += fmt::format("Findings: {} hard, {} soft\n", summary.hard, summary.soft);
    std::map<std::string, std::size_t> listed;
    std::vector<std::pair<std::string, std::size_t>> omitted;
    for (const world::ValidationFinding& finding : summary.report->findings) {
        std::size_t& count = listed[finding.rule];
        if (count++ < kTextFindingsPerRule) {
            text += fmt::format("  {} {} {}: {}\n", world::toString(finding.severity), finding.rule,
                                finding.subject.empty() ? std::string("(plan)") : finding.subject, finding.message);
        } else if (omitted.empty() || omitted.back().first != finding.rule) {
            omitted.emplace_back(finding.rule, 1);
        } else {
            ++omitted.back().second;
        }
    }
    for (const auto& [rule, count] : omitted) {
        text += fmt::format("  ... and {} more {} findings (--json lists them all)\n", count, rule);
    }
    text += fmt::format("Result: {}\n", summary.report->passed() ? "passed" : "FAILED");
    return text;
}

/// A file that is not a plan this program reads: the check fails (exit 1) with the reason.
ExitCode notAPlan(CommandContext& context, std::optional<Output>& file, bool json, const std::string& planPath,
                  const Error& error) {
    std::string text;
    if (json) {
        text = jsonText(Json{{"plan", planPath},
                             {"readable", false},
                             {"error", error.toString()},
                             {"passed", false},
                             {"hardCount", 0},
                             {"softCount", 0},
                             {"findings", Json::array()}});
    } else {
        text = fmt::format("Plan: {}\nNot a plan this program reads: {}\nResult: FAILED\n", planPath, error.toString());
    }
    return deliver(context, file, text, ExitCode::ProblemsFound);
}

} // namespace

ExitCode runWorldPlanCheck(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.positional("plan", "The plan file (world-plan.json).", true);
    parser.option("selection", "file", "A room selection whose every game must have an entry in the plan.");
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
    const std::string planArgument = args->positional("plan").value_or("");
    if (planArgument.empty()) {
        return usageError(context, "the plan file is empty");
    }
    const bool json = args->has("json");

    world::ValidationContext validation;
    std::optional<std::string> selectionPath;
    if (args->has("selection")) {
        const std::string selectionArgument = args->value("selection").value_or("");
        if (selectionArgument.empty()) {
            return usageError(context, "--selection names no file");
        }
        const std::filesystem::path path = core::pathFromUtf8(selectionArgument);
        selectionPath = core::pathToUtf8Lossy(path);
        auto text = core::readTextFile(path);
        if (!text) {
            return usageError(context, fmt::format("the selection cannot be read: {}", text.error().toString()));
        }
        auto selection = world::readRoomSelection(*text, *selectionPath);
        if (!selection) {
            return usageError(context, fmt::format("not a room selection: {}", selection.error().toString()));
        }
        validation.selections.push_back(std::move(*selection));
    }

    const std::filesystem::path planPath = core::pathFromUtf8(planArgument);
    const std::string planText = core::pathToUtf8Lossy(planPath);
    auto text = core::readTextFile(planPath);
    if (!text) {
        return runtimeFailure(context, "cannot read the plan", text.error());
    }
    auto read = world::readPlan(*text, planText);
    if (!read) {
        return notAPlan(context, *file, json, planText, read.error());
    }
    // A schema-1 plan is judged as the building it lifts into (ADR-0013 §5).
    auto building = world::loadBuildingData(context.paths.assetRoot());
    if (!building) {
        return runtimeFailure(context, "cannot read the building's data", building.error());
    }
    auto systems = world::loadSystemsData(context.paths.assetRoot());
    if (!systems) {
        return runtimeFailure(context, "cannot read the systems' data", systems.error());
    }
    auto plan = world::liftIfNeeded(*read, *building, *systems);
    if (!plan) {
        return notAPlan(context, *file, json, planText, plan.error());
    }
    // A stored building whose ground floor's rooms stand at I4's lobby is judged as the app draws it and the next update
    // writes it: those rooms on the hub (world::placeOnHub(), I6 step 5's review); the file is not written.
    for (const world::ReplacedRoom& replaced : world::placeOnHub(plan->rooms, building->layout)) {
        spdlog::info("worldplan check: {} stands at I4's lobby; judged on the hub at x {} z {}",
                     replaced.room.toString(), replaced.to.xMm, replaced.to.zMm);
    }
    validation.building = *building;
    if (!validation.selections.empty()) {
        const world::RoomId& room = validation.selections.front().room;
        const bool listed = std::any_of(plan->rooms.begin(), plan->rooms.end(),
                                        [&](const world::Room& candidate) { return candidate.id == room; });
        if (!listed) {
            spdlog::warn("worldplan check: the selection is of {}, which the plan does not list", room.toString());
        }
    }

    auto config = configurationFor(context, *plan);
    if (!config) {
        return runtimeFailure(context, "cannot read the world's configuration", config.error());
    }
    validation.config = std::move(*config);
    validation.templates = templatesFor(context, *plan);
    validation.prefabSlots = world::manifestPrefabSlots(context.paths.assetRoot());
    // A room an update keeps as it is: made from an older version of its template, or by another generator major
    // (ADR-0013 §5). A room whose prefabs' levels changed is judged against them (prefab-slots).
    for (const world::Room& room : plan->rooms) {
        const auto current = std::find_if(validation.templates.begin(), validation.templates.end(),
                                          [&](const world::RoomTemplate& candidate) {
                                              return candidate.id == room.templateId;
                                          });
        if (current != validation.templates.end() && current->version != room.templateVersion) {
            validation.keptRooms.push_back(
                {room.id, fmt::format("its template is {} v{}, the asset root's v{}", room.templateId,
                                      room.templateVersion, current->version)});
        } else if (room.generatorVersion.major != world::kGeneratorVersion.major) {
            validation.keptRooms.push_back(
                {room.id, fmt::format("it was generated by generator {}, this one is {}",
                                      room.generatorVersion.toString(), world::kGeneratorVersion.toString())});
        }
    }
    for (const world::Room& room : plan->rooms) {
        const auto lights = std::count_if(plan->lights.begin(), plan->lights.end(), [&](const world::PlanLight& light) {
            return light.id.room() == room.id;
        });
        validation.lights.push_back({room.id, static_cast<std::uint32_t>(lights)});
    }

    const world::ValidationReport report = world::validatePlan(*plan, validation);
    CheckSummary summary;
    summary.planPath = planText;
    summary.digest = core::sha256Hex(std::string_view(*text));
    summary.selectionPath = selectionPath;
    summary.plan = &*plan;
    summary.report = &report;
    for (const world::ValidationFinding& finding : report.findings) {
        (finding.severity == world::Severity::Hard ? summary.hard : summary.soft) += 1;
    }
    spdlog::info("worldplan check: {} hard and {} soft findings, LayoutScore {}", summary.hard, summary.soft,
                 report.layoutScorePermille.value_or(0));
    const std::string result = json ? jsonText(resultJson(summary)) : resultText(summary);
    return deliver(context, *file, result, report.passed() ? ExitCode::Ok : ExitCode::ProblemsFound);
}

} // namespace rv::cli
