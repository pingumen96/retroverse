// retroverse-cli worldplan edit: the editor's operations on the world's plan and overrides (ADR-0015 §4, §8; I4b step
// 2): a game moved into an empty slot of its room chain or swapped with another, a game pinned or unpinned in place, a
// fixture moved and turned on its room's grid, a fixture pinned or unpinned; or several as one session from an edit
// script (--script). Each is applied by world::applyEdits() to the world's plan (<user data>/worlds/<library id>/
// world-plan.json, a schema-1 plan lifted) and its overrides.json, read under the world's lock, validated by the hard
// rules, and the session saved through world::commitWorld() behind a snapshot (action "edit"), so that 'worldplan undo'
// restores it. Every manual change is pinned. A refused operation writes nothing and exits 1 naming the rule. The
// catalog is not read: an edit changes no selection (the overrides join the plan's inputDigest without the inputs).

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>
#include <rv/core/version.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_edit.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "world_plan_text.hpp"
#include "world_selection.hpp"
#include "world_snapshot_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli worldplan edit";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] worldplan edit <operation> [its options] [--library <id>] [--dry-run]\n"
    "                      [--json] [--output <file>]\n"
    "       retroverse-cli [global options] worldplan edit --script <file.json> [--library <id>] [--dry-run] [--json]\n"
    "                      [--output <file>]\n"
    "\n"
    "Edits the world's plan as the app's editor does, without a window: a game moved or swapped within its room, a\n"
    "game pinned or unpinned in place, a shelf, display case or decoration moved and turned in its room, a fixture\n"
    "pinned or unpinned. Every manual change is pinned in overrides.json, so that a regeneration keeps it. The edit is\n"
    "checked against the room's rules (no overlap, the doorway and the door-reserved sockets clear, everything\n"
    "reachable through 1.2 m) and saved with the overrides behind a snapshot, which 'worldplan undo' restores. A\n"
    "refused operation writes nothing.\n"
    "\n"
    "Operations:\n"
    "  move --game <id> --slot <slot>     Move a placed game into an empty slot of its room (a face-out one too).\n"
    "  swap --game <id> --with <id>       Swap two placed games of one room.\n"
    "  pin --game <id>                    Pin a placed game in its slot.\n"
    "  unpin --game <id>                  Remove a game's pin (the game stays where it stands).\n"
    "  move-fixture --fixture <id> --x-mm <n> --z-mm <n> [--yaw-cdeg <n>]\n"
    "                                     Move a side-wall module, a display fixture or a decoration in its room:\n"
    "                                     its pivot in the room's frame (millimetres, on the 100 mm grid through its\n"
    "                                     place in its socket), its yaw in hundredths of a degree (quarter turns,\n"
    "                                     decoration 15 degrees; default: as it stands). A poster stays on a wall.\n"
    "  pin-fixture --fixture <id>         Pin a movable fixture where it stands.\n"
    "  unpin-fixture --fixture <id>       Remove a fixture's pin (it stays where it stands).\n"
    "\n"
    "Options:\n"
    "  --script <file.json>  Apply the operations of an edit script as one session, all or nothing, one snapshot:\n"
    "                        {\"schemaVersion\": 1, \"operations\": [{\"op\": \"move\", \"game\": ..., \"slot\": ...},\n"
    "                        {\"op\": \"move-fixture\", \"fixture\": ..., \"xMm\": ..., \"zMm\": ..., \"yawCdeg\": ...},\n"
    "                        ...]} (the keys of each operation as its options above).\n"
    "  --library <id>        The library whose world to edit (default: the one library.json records).\n"
    "  --dry-run             Check and report the edit; write nothing (the world's lock is still taken).\n"
    "  --json                Write the result as JSON.\n"
    "  --output <file>       Write the result to this file (UTF-8) instead of stdout.\n"
    "  -h, --help            Show this help and exit.\n"
    "\n"
    "Exit codes: 0 edited (or nothing to change, or with --dry-run the edit passes); 1 refused (the rule is named:\n"
    "nothing written), or there is no plan, or it is refused (edited by hand, written by a newer Retroverse); 2\n"
    "usage error (an unknown operation, an option it does not take or lacks, a malformed id or number, an edit\n"
    "script that cannot be read); 3 the world's data or overrides could not be read, the world is locked by another\n"
    "program, or a file could not be written.\n";

ExitCode failure(CommandContext& context, std::string_view what, const Error& error) {
    context.err.writeLine(fmt::format("{}: {}: {}", kName, what, error.toString()));
    return ExitCode::RuntimeFailure;
}

/// The option names each operation takes (besides the common ones).
std::vector<std::string_view> optionsOf(world::EditKind kind) {
    switch (kind) {
    case world::EditKind::Move:
        return {"game", "slot"};
    case world::EditKind::Swap:
        return {"game", "with"};
    case world::EditKind::Pin:
    case world::EditKind::Unpin:
        return {"game"};
    case world::EditKind::MoveFixture:
        return {"fixture", "x-mm", "z-mm", "yaw-cdeg"};
    case world::EditKind::PinFixture:
    case world::EditKind::UnpinFixture:
        return {"fixture"};
    }
    return {};
}

constexpr std::string_view kOperationOptions[] = {"game", "slot", "with", "fixture", "x-mm", "z-mm", "yaw-cdeg"};

/// A decimal integer option within [minimum, maximum].
Expected<std::int64_t> integerOption(const core::ParsedArgs& args, std::string_view name, std::int64_t minimum,
                                     std::int64_t maximum) {
    const std::string text = args.value(name).value_or("");
    std::int64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc() || stop != end || value < minimum || value > maximum) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("expected an integer from {} to {}, found '{}'", minimum, maximum, text),
                         "--" + std::string(name));
    }
    return value;
}

/// The operation the command line names, or why it is malformed (a usage error).
Expected<world::EditOperation> operationOf(const core::ParsedArgs& args, std::string_view name) {
    const auto kind = world::parseEditKind(name);
    if (!kind) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("unknown operation '{}' (move, swap, pin, unpin, move-fixture, pin-fixture, "
                                     "unpin-fixture)",
                                     name),
                         "operation");
    }
    const std::vector<std::string_view> takes = optionsOf(*kind);
    for (std::string_view option : kOperationOptions) {
        const bool taken = std::find(takes.begin(), takes.end(), option) != takes.end();
        if (args.has(option) && !taken) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("{} does not take --{}", name, option),
                             "operation");
        }
        if (taken && option != "yaw-cdeg" && !args.has(option)) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("{} needs --{}", name, option), "operation");
        }
    }
    world::EditOperation operation;
    operation.kind = *kind;
    const auto parseInto = [&](std::string_view option, auto parse, auto& into) -> Status {
        if (!args.has(option)) {
            return ok();
        }
        auto value = parse(*args.value(option));
        if (!value) {
            return makeError(ErrorCode::InvalidArgument, value.error().message, "--" + std::string(option));
        }
        into = std::move(*value);
        return ok();
    };
    for (Status status : {parseInto("game", catalog::GameId::parse, operation.game),
                          parseInto("with", catalog::GameId::parse, operation.with),
                          parseInto("slot", world::SlotId::parse, operation.slot),
                          parseInto("fixture", world::FixtureId::parse, operation.fixture)}) {
        if (!status) {
            return propagate(status.error());
        }
    }
    if (*kind == world::EditKind::MoveFixture) {
        auto x = integerOption(args, "x-mm", -world::kMaxFixtureOffsetMm, world::kMaxFixtureOffsetMm);
        auto z = integerOption(args, "z-mm", -world::kMaxFixtureOffsetMm, world::kMaxFixtureOffsetMm);
        if (!x || !z) {
            return propagate((!x ? x : z).error());
        }
        operation.xMm = *x;
        operation.zMm = *z;
        if (args.has("yaw-cdeg")) {
            auto yaw = integerOption(args, "yaw-cdeg", 0, 35999);
            if (!yaw) {
                return propagate(yaw.error());
            }
            operation.yawCdeg = static_cast<std::int32_t>(*yaw);
        }
    }
    return operation;
}

CatalogJson refusalJson(const world::EditRefusal& refusal) {
    return CatalogJson{{"rule", refusal.rule}, {"subject", refusal.subject}, {"message", refusal.message}};
}

} // namespace

ExitCode runWorldPlanEdit(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{std::string(kName)};
    parser.positional("operation", "move, swap, pin, unpin, move-fixture, pin-fixture or unpin-fixture.");
    parser.option("game", "id", "The game (move, swap, pin, unpin).");
    parser.option("slot", "slot", "The slot a game moves to (move).");
    parser.option("with", "id", "The other game (swap).");
    parser.option("fixture", "id", "The fixture (move-fixture, pin-fixture, unpin-fixture).");
    parser.option("x-mm", "n", "The fixture's pivot x in its room's frame (move-fixture).");
    parser.option("z-mm", "n", "The fixture's pivot z in its room's frame (move-fixture).");
    parser.option("yaw-cdeg", "n", "The fixture's yaw in hundredths of a degree (move-fixture).");
    parser.option("script", "file", "An edit script: several operations as one session.");
    parser.option("library", "id", "The library whose world to edit.");
    parser.flag("dry-run", "Check and report the edit; write nothing.");
    parser.flag("json", "Write the result as JSON.");
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
    const auto named = args->positional("operation");
    const auto script = args->value("script");
    if (named.has_value() == script.has_value()) {
        return usageError(context, "name one operation, or give --script");
    }
    std::vector<world::EditOperation> operations;
    if (named) {
        auto operation = operationOf(*args, *named);
        if (!operation) {
            return usageError(context, operation.error().toString());
        }
        operations.push_back(std::move(*operation));
    } else {
        for (std::string_view option : kOperationOptions) {
            if (args->has(option)) {
                return usageError(context, fmt::format("--script takes no --{}: the script names its operations",
                                                       option));
            }
        }
        const std::filesystem::path path = core::pathFromUtf8(*script);
        auto text = core::readTextFile(path);
        if (!text) {
            return usageError(context, fmt::format("the edit script: {}", text.error().toString()));
        }
        auto parsed = world::parseEditScript(*text, pathText(path));
        if (!parsed) {
            return usageError(context, fmt::format("the edit script: {}", parsed.error().toString()));
        }
        operations = std::move(*parsed);
    }
    const bool dryRun = args->has("dry-run");

    ExitCode libraryFailure = ExitCode::UsageError;
    const auto library = libraryOf(context, kName, args->value("library"), libraryFailure);
    if (!library) {
        return libraryFailure;
    }
    const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot(), *library);
    std::error_code exists;
    if (!std::filesystem::exists(files.plan, exists)) {
        context.err.writeLine(fmt::format("{}: no plan to edit at {} (run 'retroverse-cli worldplan generate')",
                                          kName, pathText(files.plan)));
        return ExitCode::ProblemsFound;
    }
    // The plan and the overrides are read under the world's lock (which rolls back an interrupted action first), so
    // that nothing comes between them and the commit.
    auto lock = world::WorldLock::acquire(files);
    if (!lock) {
        return failure(context, "the world's lock", lock.error());
    }
    if (lock->rolledBack()) {
        context.err.writeLine(fmt::format("{}: an interrupted action was rolled back first: the snapshot {} restored",
                                          kName, *lock->rolledBack()));
    }
    auto stored = readPlanFile(context.paths, files.plan);
    if (!stored) {
        if (stored.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no plan to edit at {}", kName, pathText(files.plan)));
            return ExitCode::ProblemsFound;
        }
        if (stored.error().code == ErrorCode::ValidationFailed || stored.error().code == ErrorCode::ParseError ||
            stored.error().code == ErrorCode::Unsupported || stored.error().code == ErrorCode::InvalidArgument) {
            context.err.writeLine(fmt::format("{}: the plan is refused, nothing written: {}", kName,
                                              stored.error().toString()));
            return ExitCode::ProblemsFound;
        }
        return failure(context, "the plan", stored.error());
    }
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return failure(context, "the world's overrides", overrides.error());
    }
    const auto config = loadWorldConfiguration(context, kName, std::nullopt);
    if (!config) {
        return ExitCode::RuntimeFailure;
    }
    const auto info = loadBuildingInfo(context, kName);
    if (!info) {
        return ExitCode::RuntimeFailure;
    }
    // The data of every room of the floors the session edits (their templates and measured prefabs).
    world::EditData data;
    data.config = *config;
    data.building = info->building;
    for (const world::RoomId& id : world::editRooms(stored->plan, operations)) {
        const auto room = std::find_if(stored->plan.rooms.begin(), stored->plan.rooms.end(),
                                       [&](const world::Room& candidate) { return candidate.id == id; });
        auto loaded = world::loadRoomData(context.paths.assetRoot(), *room);
        if (!loaded) {
            return failure(context, fmt::format("the data of {}", id.toString()), loaded.error());
        }
        data.rooms.emplace(id, std::make_shared<const world::WorldData>(std::move(*loaded)));
    }

    world::EditedWorld world{stored->plan, *overrides};
    const world::PinProvenance provenance{world::PinOrigin::Cli, core::utcNow()};
    auto session = world::applyEdits(world, operations, data, provenance);
    if (!session) {
        return failure(context, "the edit", session.error());
    }

    std::string text;
    CatalogJson json;
    CatalogJson results = CatalogJson::array();
    for (std::size_t i = 0; i < session->results.size(); ++i) {
        const world::EditResult& result = session->results[i];
        CatalogJson entry{{"op", std::string(world::toString(operations[i].kind))},
                          {"operation", world::editText(operations[i])},
                          {"changed", result.changed},
                          {"summary", result.summary},
                          {"refused", result.refused ? refusalJson(*result.refused) : CatalogJson(nullptr)}};
        results.push_back(std::move(entry));
    }
    json["library"] = library->str();
    json["dryRun"] = dryRun;
    json["operations"] = std::move(results);

    if (session->refusedAt) {
        const world::EditRefusal& refusal = *session->results.back().refused;
        context.err.writeLine(fmt::format("{}: operation {} ({}) refused by {}: {}{}{}; nothing written", kName,
                                          *session->refusedAt + 1, world::editText(operations[*session->refusedAt]),
                                          refusal.rule, refusal.subject, refusal.subject.empty() ? "" : ": ",
                                          refusal.message));
        if (args->has("json")) {
            json["refused"] = CatalogJson{{"operation", *session->refusedAt + 1},
                                          {"rule", refusal.rule},
                                          {"subject", refusal.subject},
                                          {"message", refusal.message}};
            json["snapshot"] = nullptr;
            json["written"] = false;
            return writeResults(context, *file, jsonText(json), ExitCode::ProblemsFound);
        }
        return ExitCode::ProblemsFound;
    }

    const bool changed = std::any_of(session->results.begin(), session->results.end(),
                                     [](const world::EditResult& result) { return result.changed; });
    std::set<world::RoomId> rooms;
    for (const world::EditResult& result : session->results) {
        rooms.insert(result.rooms.begin(), result.rooms.end());
    }
    std::string scope;
    if (rooms.size() == 1) {
        scope = rooms.begin()->toString();
    } else {
        scope = rooms.empty() ? std::string("pins") : fmt::format("{} rooms", rooms.size());
    }
    auto result = world::editedResult(stored->plan, stored->digest, world, session->results);
    if (!result) {
        return failure(context, "the edited plan", result.error());
    }
    std::string snapshot;
    bool written = false;
    if (changed && !dryRun) {
        world::SaveOptions options;
        options.writer = fmt::format("retroverse-cli worldplan edit ({})", core::versionBanner());
        auto commit = world::commitWorld(*lock, world::WorldAction{"edit", scope}, *result, world.overrides, options);
        if (!commit) {
            return failure(context, "the plan", commit.error());
        }
        written = commit->written;
        if (commit->snapshot) {
            snapshot = commit->snapshot->name;
        }
    }
    spdlog::info("worldplan edit: {} operation(s), {}", operations.size(),
                 written ? "written" : (dryRun ? "dry run" : "nothing to change"));

    if (args->has("json")) {
        json["refused"] = nullptr;
        json["changed"] = changed;
        json["written"] = written;
        json["scope"] = scope;
        json["snapshot"] = snapshot.empty() ? CatalogJson(nullptr) : CatalogJson(snapshot);
        json["rebuild"] = std::string(world::toString(result->changes.rebuild));
        json["digestBefore"] = result->changes.digestBefore;
        json["digestAfter"] = result->changes.digestAfter;
        CatalogJson moved = CatalogJson::array();
        for (const world::FixtureId& fixture : result->changes.fixturesMoved) {
            moved.push_back(fixture.toString());
        }
        json["fixturesMoved"] = std::move(moved);
        json["gamePins"] = world.overrides.gamePins.size();
        json["fixturePins"] = world.overrides.fixturePins.size();
        text = jsonText(json);
    } else {
        text = fmt::format("{} the plan of {}: {} operation{}\n",
                           dryRun ? "Would edit" : (written ? "Edited" : "Nothing to change in"), library->str(),
                           operations.size(), operations.size() == 1 ? "" : "s");
        for (const world::EditResult& edit : session->results) {
            text += fmt::format("  {:<17}{}\n", edit.changed ? "Done:" : "Unchanged:", edit.summary);
        }
        text += fmt::format("  {:<17}{} game pin{}, {} fixture pin{}\n", "Pins:", world.overrides.gamePins.size(),
                            world.overrides.gamePins.size() == 1 ? "" : "s", world.overrides.fixturePins.size(),
                            world.overrides.fixturePins.size() == 1 ? "" : "s");
        text += fmt::format("  {:<17}{} -> {}\n", "Plan:", shortDigest(result->changes.digestBefore),
                            shortDigest(result->changes.digestAfter));
        if (!snapshot.empty()) {
            text += fmt::format("  {:<17}{} ('worldplan undo' restores it)\n", "Snapshot:", snapshot);
        }
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
