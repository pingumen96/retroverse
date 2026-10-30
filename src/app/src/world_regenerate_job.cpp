// The regenerations and the undo of the app on the worker pool (I4b step 6; ADR-0015 §5, §6): world_regenerate.hpp.

#include <rv/app/world_regenerate.hpp>

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/time.hpp>
#include <rv/core/version.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/world_store.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::uint32_t count(std::size_t value) noexcept {
    return static_cast<std::uint32_t>(std::min<std::size_t>(value, 1'000'000'000));
}

/// The room of `plan` that holds `system` (its sections), or nullptr.
[[nodiscard]] const world::Room* roomHolding(const world::WorldPlan& plan, const catalog::SystemName& system) {
    for (const world::Room& room : plan.rooms) {
        if (std::find(room.sections.begin(), room.sections.end(), system) != room.sections.end()) {
            return &room;
        }
    }
    return nullptr;
}

/// A plan of schema 1 lifted into the building (as the app reads its stored plan, world_build.cpp).
[[nodiscard]] Expected<world::WorldPlan> lifted(world::WorldPlan plan, const world::BuildingData& building,
                                                const world::SystemsData& systems) {
    if (plan.schemaVersion == world::kPlanSchemaVersion) {
        return plan;
    }
    return world::liftIfNeeded(plan, building, systems);
}

/// The failure key of an error of the world's files or of the regeneration.
[[nodiscard]] std::string_view failureOf(const Error& error) noexcept {
    return error.code == ErrorCode::Conflict ? regenerate_failure::kBusy : regenerate_failure::kFailed;
}

/// The selections the scope's regeneration takes, as `worldplan regenerate` takes them: a system a room of `previous`
/// holds as it is (the world's scope also checks its data), another one only with a placeable title and data of its own
/// (else left out, named in `leftOut`).
[[nodiscard]] std::vector<world::RoomSelection> scopeSelections(const RegenerationInputs& inputs,
                                                                const world::WorldPlan& previous,
                                                                const world::RegenerationScope& scope,
                                                                std::vector<std::string>& leftOut) {
    std::vector<world::RoomSelection> kept;
    const bool wholeWorld = scope.kind == world::RegenerationScopeKind::World;
    for (const world::RoomSelection& selection : inputs.selections) {
        const catalog::SystemName system = selection.room.system();
        const bool standing = roomHolding(previous, system) != nullptr;
        if (standing && !wholeWorld) {
            kept.push_back(selection);
            continue;
        }
        if (!standing && selection.titles.empty()) {
            continue; // no placeable title: it would get no room
        }
        world::RoomDataRequest request;
        request.system = system;
        if (const auto tags = inputs.platformTags.find(system); tags != inputs.platformTags.end()) {
            request.platformTags = tags->second;
        }
        if (auto data = inputs.load(request); !data) {
            leftOut.push_back(system.str());
            spdlog::warn("Regeneration: {} left out of the building: no data yet: {}", system.str(),
                         data.error().message);
            continue;
        }
        kept.push_back(selection);
    }
    return kept;
}

/// The game's title in the catalog's snapshot ("" unknown).
[[nodiscard]] std::string titleOf(const RegenerationInputs* inputs, const catalog::GameId& game) {
    if (inputs == nullptr || !inputs->snapshot) {
        return {};
    }
    for (const RoomSnapshotResult& system : inputs->snapshot->systems) {
        if (system.error) {
            continue;
        }
        const std::vector<catalog::RoomEntry>& entries = system.snapshot.entries;
        const auto found =
            std::lower_bound(entries.begin(), entries.end(), game,
                             [](const catalog::RoomEntry& entry, const catalog::GameId& id) { return entry.id < id; });
        if (found != entries.end() && found->id == game) {
            return found->title;
        }
    }
    return {};
}

/// A fixture's subject for the panel by its role in `plan` ("decoration" when the plan lacks it).
[[nodiscard]] std::string subjectOf(const world::WorldPlan& plan, const world::FixtureId& fixture) {
    for (const world::Fixture& candidate : plan.fixtures) {
        if (candidate.id == fixture) {
            switch (candidate.role) {
            case world::FixtureRole::HeroModule:
            case world::FixtureRole::SideModule:
                return "module";
            case world::FixtureRole::Display:
                return "display";
            case world::FixtureRole::HeroCorner:
            case world::FixtureRole::Decoration:
                return "decoration";
            }
        }
    }
    return "decoration";
}

/// The plan stored now, read under the lock (lifted), with its digest.
[[nodiscard]] Expected<world::StoredPlan>
readStored(const world::WorldFiles& files, const world::BuildingData& building, const world::SystemsData& systems) {
    auto stored = world::readStoredPlan(files.plan);
    if (!stored) {
        return propagate(stored.error());
    }
    auto plan = lifted(std::move(stored->plan), building, systems);
    if (!plan) {
        return propagate(plan.error());
    }
    return world::StoredPlan{std::move(*plan), std::move(stored->digest)};
}

} // namespace

RegenerationInputsResult regenerationInputsOf(const WorldPaths& paths,
                                              std::shared_ptr<const BuildingSnapshot> snapshot) {
    RegenerationInputsResult result;
    if (!snapshot) {
        result.failure = regenerate_failure::kFailed;
        result.error = "no catalog snapshot";
        return result;
    }
    if (snapshot->error) {
        result.failure = regenerate_failure::kFailed;
        result.error = "the catalog's snapshot: " + snapshot->error->toString();
        return result;
    }
    auto inputs = std::make_shared<RegenerationInputs>();
    inputs->library = snapshot->library;
    inputs->snapshotMs = snapshot->milliseconds;
    auto config = world::loadWorldConfig(paths.assetRoot, paths.configDir);
    if (!config) {
        result.failure = regenerate_failure::kFailed;
        result.error = "the world's configuration: " + config.error().toString();
        return result;
    }
    inputs->config = std::move(config->config);
    auto building = world::loadBuildingData(paths.assetRoot);
    auto systems = world::loadSystemsData(paths.assetRoot);
    if (!building || !systems) {
        result.failure = regenerate_failure::kFailed;
        result.error = "the world's data: " + (!building ? building.error() : systems.error()).toString();
        return result;
    }
    inputs->building = std::move(*building);
    inputs->systems = std::move(*systems);
    // Each system's selection, as the building job makes them (world_build.cpp): nothing is decided from unknown media.
    const Clock::time_point selecting = Clock::now();
    for (const RoomSnapshotResult& system : snapshot->systems) {
        if (system.error) {
            if (system.error->code == ErrorCode::NotFound) {
                continue; // no entries in the catalog
            }
            result.failure = regenerate_failure::kFailed;
            result.error = fmt::format("the catalog's snapshot of {}: {}", system.system.str(), system.error->message);
            return result;
        }
        auto selection = world::selectRoom(world::RoomInput{snapshot->library, system.snapshot}, inputs->config);
        if (!selection) {
            result.failure = selection.error().code == ErrorCode::Unavailable ? regenerate_failure::kMediaUnknown
                                                                              : regenerate_failure::kFailed;
            result.error = fmt::format("the selection of {}: {}", system.system.str(), selection.error().toString());
            return result;
        }
        inputs->platformTags[system.system] = system.platformTags;
        inputs->selections.push_back(std::move(*selection));
    }
    inputs->selectMs = millisecondsSince(selecting);
    // One cache for every job of the session's inputs; a cancelled job may still run beside the next one.
    inputs->load = [load = world::assetRootLoader(paths.assetRoot),
                    mutex = std::make_shared<std::mutex>()](const world::RoomDataRequest& request) {
        const std::lock_guard lock(*mutex);
        return load(request);
    };
    inputs->snapshot = std::move(snapshot);
    result.inputs = std::move(inputs);
    return result;
}

RegenerationInputsResult regenerationInputsFor(RegenerationInputsSlot& slot, const WorldPaths& paths,
                                               WorldCatalogReader* reader, bool& taken) {
    taken = false;
    {
        std::unique_lock lock(slot.mutex);
        slot.done.wait(lock, [&] { return !slot.running; });
        if (slot.result) {
            return *slot.result;
        }
        slot.running = true;
    }
    const auto start = Clock::now();
    RegenerationInputsResult result;
    if (reader == nullptr) {
        result.failure = regenerate_failure::kFailed;
        result.error = "no catalog to read the selections from";
    } else {
        result = takeRegenerationInputs(paths, *reader);
    }
    taken = true;
    {
        const std::lock_guard lock(slot.mutex);
        slot.running = false;
        slot.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        // A failure is not kept: the next preview tries again (the media may be back).
        if (result.inputs) {
            slot.result = result;
        }
    }
    slot.done.notify_all();
    return result;
}

double warmRoomData(const RegenerationInputs& inputs, const world::WorldPlan& plan) {
    const Clock::time_point start = Clock::now();
    std::size_t failed = 0;
    const std::vector<world::RoomDataRequest> requests = world::standingRoomRequests(plan, inputs.platformTags);
    for (const world::RoomDataRequest& request : requests) {
        if (!inputs.load(request)) {
            ++failed;
        }
    }
    const double milliseconds = millisecondsSince(start);
    spdlog::info("Regeneration: the data of {} standing room(s) read ahead in {:.0f} ms{}", requests.size(),
                 milliseconds, failed > 0 ? fmt::format(" ({} not read)", failed) : std::string());
    return milliseconds;
}

bool releaseIdleInputs(RegenerationInputsSlot& slot, bool wanted) {
    if (wanted) {
        return false;
    }
    const std::lock_guard lock(slot.mutex);
    if (slot.running || !slot.result) {
        return false;
    }
    slot.result.reset();
    return true;
}

RegenerationInputsResult takeRegenerationInputs(const WorldPaths& paths, WorldCatalogReader& reader) {
    // The systems that get a room (ADR-0013 §2), every one of the library's in one read transaction, as the building
    // job takes them; no placed game's alias is asked (a regeneration reads none).
    const SystemFilter wanted = roomSystemsFilter(paths.assetRoot);
    auto snapshot = std::make_shared<BuildingSnapshot>();
    {
        const std::lock_guard lock(reader.mutex);
        *snapshot = takeBuildingSnapshot(reader, {}, wanted);
    }
    spdlog::info("Regeneration: the catalog's snapshot of {} system(s) in {:.0f} ms, media {}",
                 snapshot->systems.size(), snapshot->milliseconds, snapshot->mediaKnown() ? "known" : "unknown");
    return regenerationInputsOf(paths, std::move(snapshot));
}

RegenerationWorld readRegenerationWorld(const WorldPaths& paths, const catalog::LibraryId& library,
                                        const KnownPlan* known) {
    const Clock::time_point start = Clock::now();
    RegenerationWorld world;
    world.library = library;
    const auto fail = [&](std::string_view failure, std::string error) {
        world.failure = std::string(failure);
        world.error = std::move(error);
        world.plan.reset();
        world.readMs = millisecondsSince(start);
        return world;
    };
    // The world as one reading saw it, without the lock (ADR-0015 §5: the preview writes nothing, not even world.lock).
    const world::WorldFiles files = world::worldFiles(paths.userDataRoot, library);
    auto read = world::readWorldUnlocked(files);
    if (!read) {
        return fail(read.error().code == ErrorCode::NotFound ? regenerate_failure::kNoPlan : failureOf(read.error()),
                    "the world's files: " + read.error().toString());
    }
    world.planDigest = read->planDigest;
    // The plan the app shows stands for the file's only when its own digest is the file's (m3/preview-time's review):
    // the app may show a stored building's ground-floor rooms at I4's lobby drawn on the hub under the stored digest
    // (buildFloor(), the editor's asShown()), and a preview of that plan would have its commit write the re-placement.
    // Its canonical text is written straight from it, sorted (a fraction of the file's parse).
    if (known != nullptr && known->plan && known->digest == read->planDigest &&
        known->plan->schemaVersion == world::kPlanSchemaVersion) {
        const Clock::time_point checking = Clock::now();
        auto sorted = std::make_shared<world::WorldPlan>(*known->plan);
        world::sortPlan(*sorted);
        if (auto digest = world::sortedPlanDigest(*sorted); digest && *digest == read->planDigest) {
            world.plan = std::move(sorted);
            world.knownPlan = true;
        } else {
            spdlog::info("Regeneration: the plan shown is not its file's ({}): the file is read",
                         digest ? "another digest" : digest.error().toString());
        }
        world.checkMs = millisecondsSince(checking);
    }
    if (!world.plan) {
        auto plan = world::readPlan(read->plan, "world-plan.json");
        if (!plan) {
            return fail(regenerate_failure::kFailed, "the plan: " + plan.error().toString());
        }
        world.plan = std::make_shared<const world::WorldPlan>(std::move(*plan));
    }
    auto overrides = read->overrides ? world::parseOverrides(*read->overrides, "overrides.json")
                                     : Expected<world::WorldOverrides>(world::WorldOverrides{});
    if (!overrides) {
        return fail(regenerate_failure::kFailed, "overrides.json: " + overrides.error().toString());
    }
    world.overrides = std::move(*overrides);
    world.readMs = millisecondsSince(start);
    return world;
}

RegenerationPreview previewRegeneration(const WorldPaths& paths, const world::RegenerationScope& scope,
                                        const RegenerationInputs& inputs, const KnownPlan* known) {
    return previewRegeneration(readRegenerationWorld(paths, inputs.library, known), scope, inputs);
}

RegenerationPreview previewRegeneration(const RegenerationWorld& world, const world::RegenerationScope& scope,
                                        const RegenerationInputs& inputs) {
    const Clock::time_point start = Clock::now();
    RegenerationPreview preview;
    preview.scope = scope;
    preview.readMs = world.readMs;
    preview.knownPlan = world.knownPlan;
    preview.checkMs = world.checkMs;
    const auto fail = [&](std::string_view failure, std::string error) {
        preview.failure = std::string(failure);
        preview.error = std::move(error);
        preview.totalMs = world.readMs + millisecondsSince(start);
        spdlog::warn("Regeneration: the preview of {} failed ({}): {}", scope.toString(), preview.failure,
                     preview.error);
        return preview;
    };
    if (!world.failure.empty() || !world.plan) {
        return fail(world.failure.empty() ? regenerate_failure::kFailed : world.failure, world.error);
    }
    if (world.library != inputs.library) {
        return fail(regenerate_failure::kFailed, "the world read is of the library " + world.library.str() +
                                                     ", the catalog's inputs of " + inputs.library.str());
    }
    auto previous = lifted(*world.plan, inputs.building, inputs.systems);
    if (!previous) {
        return fail(regenerate_failure::kFailed, "the plan's lift: " + previous.error().toString());
    }
    // A plan the app shows may hold its lists in another order than its file (a scan when they are sorted).
    world::sortPlan(*previous);

    world::RegenerationInput input;
    input.scope = scope;
    input.dropOverrides = false;
    input.library = inputs.library;
    input.selections = scopeSelections(inputs, *previous, scope, preview.leftOut);
    input.platformTags = inputs.platformTags;
    input.config = inputs.config;
    input.overrides = world.overrides;
    input.previous = *previous;
    input.previousDigest = world.planDigest;
    const Clock::time_point regenerating = Clock::now();
    auto regeneration = world::regenerate(input, inputs.building, inputs.systems, inputs.load);
    preview.regenerateMs = millisecondsSince(regenerating);
    if (!regeneration) {
        return fail(regenerate_failure::kFailed, "the regeneration: " + regeneration.error().toString());
    }
    std::size_t hard = 0;
    for (const world::ValidationFinding& finding : regeneration->result.validation.findings) {
        if (finding.severity == world::Severity::Hard) {
            spdlog::error("Regeneration: the preview of {} breaks hard rule {} at {}: {}", scope.toString(),
                          finding.rule, finding.subject, finding.message);
            ++hard;
        }
    }
    if (hard > 0) {
        return fail(regenerate_failure::kInvalidPlan, fmt::format("{} hard finding(s)", hard));
    }
    preview.content = previewContentOf(*regeneration, *previous, &inputs);
    preview.planDigest = world.planDigest;
    preview.overrides = world.overrides;
    preview.previous = std::make_shared<const world::WorldPlan>(std::move(*previous));
    preview.regeneration = std::make_shared<const world::Regeneration>(std::move(*regeneration));
    preview.totalMs = world.readMs + millisecondsSince(start);
    spdlog::info("Regeneration: the preview of {} in {:.0f} ms (read {:.0f}{}, regeneration {:.0f}): {} room(s) "
                 "regenerated, {} game(s) moved, {} entering and {} leaving face-out, {} pin(s) kept, {} conflict(s){}",
                 scope.toString(), preview.totalMs, preview.readMs,
                 preview.knownPlan ? fmt::format(", the plan shown, checked in {:.0f}", preview.checkMs) : std::string(),
                 preview.regenerateMs, preview.content.roomsRegenerated, preview.content.gamesMoved,
                 preview.content.faceOutEntering, preview.content.faceOutLeaving, preview.content.pinsKept,
                 preview.content.conflictCount, preview.content.changed ? "" : "; nothing to change");
    return preview;
}

ui::RegeneratePreviewContent previewContentOf(const world::Regeneration& regeneration, const world::WorldPlan& previous,
                                              const RegenerationInputs* inputs) {
    const world::RegenerationImpact& impact = regeneration.impact;
    ui::RegeneratePreviewContent content;
    content.roomsRegenerated = count(impact.roomsRegenerated.size());
    content.roomsAdded = count(impact.roomsAdded.size());
    content.roomsRemoved = count(impact.roomsRemoved.size());
    content.roomsMoved = count(impact.roomsMoved.size());
    content.gamesMoved = count(impact.gamesMoved.size());
    content.faceOutEntering = count(impact.faceOutEntering.size());
    content.faceOutLeaving = count(impact.faceOutLeaving.size());
    content.newlyPlaced = count(impact.newlyPlaced.size());
    content.newlyUnplaced = count(impact.newlyUnplaced.size());
    content.fixturesAdded = count(impact.fixturesAdded.size());
    content.fixturesRemoved = count(impact.fixturesRemoved.size());
    content.fixturesMoved = count(impact.fixturesMoved.size());
    content.pinsKept = count(impact.pinsKept.size());
    content.conflictCount = count(impact.conflicts.size());
    for (const world::OverrideConflict& conflict : impact.conflicts) {
        if (content.conflicts.size() >= ui::regenerate_preview::kConflictsListed) {
            break;
        }
        ui::RegenerateConflict shown;
        shown.reason = std::string(world::toString(conflict.reason));
        if (conflict.pin.starts_with("game ")) {
            shown.subject = "game";
            shown.title = titleOf(inputs, conflict.game);
        } else {
            shown.subject = subjectOf(previous, conflict.fixture);
        }
        content.conflicts.push_back(std::move(shown));
    }
    content.overrides = count(impact.pinsKept.size() + impact.conflicts.size());
    content.changed = regeneration.changed;
    return content;
}

RegenerationCommit commitRegeneration(const WorldPaths& paths, const RegenerationPreview& preview, bool dropOverrides,
                                      const RegenerationInputs& inputs, std::string_view writer,
                                      std::chrono::milliseconds lockWait) {
    const Clock::time_point start = Clock::now();
    RegenerationCommit commit;
    const auto fail = [&](std::string_view failure, std::string error) {
        commit.failure = std::string(failure);
        commit.error = std::move(error);
        commit.totalMs = millisecondsSince(start);
        spdlog::warn("Regeneration: {} not written ({}): {}", preview.scope.toString(), commit.failure, commit.error);
        return commit;
    };
    if (!preview.failure.empty() || !preview.regeneration || !preview.previous) {
        return fail(regenerate_failure::kFailed, "no preview to commit");
    }
    const world::WorldFiles files = world::worldFiles(paths.userDataRoot, inputs.library);
    auto acquired = world::WorldLock::acquire(files, lockWait);
    if (!acquired) {
        return fail(failureOf(acquired.error()), "the world's lock: " + acquired.error().toString());
    }
    std::optional<world::WorldLock> lock;
    lock.emplace(std::move(*acquired));
    if (lock->rolledBack()) {
        spdlog::warn("Regeneration: an interrupted action was rolled back first (the snapshot {} restored)",
                     *lock->rolledBack());
    }
    // The plan and the overrides read again under the lock (a writer reads what it replaces under the lock).
    // Its text's digest first: the plan is parsed only when the preview's result cannot be committed (I4b step 6's
    // review: reading the reference library's plan costs about 0.4 s).
    auto text = core::readTextFile(files.plan);
    if (!text) {
        return fail(text.error().code == ErrorCode::NotFound ? regenerate_failure::kNoPlan : regenerate_failure::kFailed,
                    "the plan: " + text.error().toString());
    }
    const std::string storedDigest = core::sha256Hex(*text);
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return fail(regenerate_failure::kFailed, "overrides.json: " + overrides.error().toString());
    }
    commit.lockMs = millisecondsSince(start);

    // ADR-0015 §5 and I4b step 5's note: the preview's own result when the world is as it read it and the consent keeps
    // the overrides; else, the overrides kept, the scope regenerated again on the world as it is now. The drop is
    // consented to for the customizations the preview named: over another plan or other overrides it is refused (I4b
    // step 6's review), so that it never removes one the user did not see.
    const bool unchanged = storedDigest == preview.planDigest && *overrides == preview.overrides;
    std::shared_ptr<const world::Regeneration> regeneration;
    // The plan the regeneration replaces (the items files of the rooms that changed against it are written again).
    std::shared_ptr<const world::WorldPlan> previous = preview.previous;
    if (dropOverrides && !unchanged) {
        return fail(regenerate_failure::kChanged,
                    "the plan or the overrides changed since the preview; the drop is not made over them");
    }
    if (!dropOverrides && unchanged) {
        regeneration = preview.regeneration;
        commit.reused = true;
    } else {
        if (!unchanged) {
            spdlog::warn("Regeneration: the world changed since the preview of {}; it is regenerated again",
                         preview.scope.toString());
        }
        auto parsed = world::readPlan(*text, "world-plan.json");
        if (!parsed) {
            return fail(regenerate_failure::kFailed, "the plan: " + parsed.error().toString());
        }
        auto stored = lifted(std::move(*parsed), inputs.building, inputs.systems);
        if (!stored) {
            return fail(regenerate_failure::kFailed, "the plan's lift: " + stored.error().toString());
        }
        std::vector<std::string> leftOut;
        world::RegenerationInput input;
        input.scope = preview.scope;
        input.dropOverrides = dropOverrides;
        input.library = inputs.library;
        input.selections = scopeSelections(inputs, *stored, preview.scope, leftOut);
        input.platformTags = inputs.platformTags;
        input.config = inputs.config;
        input.overrides = *overrides;
        input.previous = std::move(*stored);
        input.previousDigest = storedDigest;
        const Clock::time_point regenerating = Clock::now();
        auto again = world::regenerate(input, inputs.building, inputs.systems, inputs.load);
        commit.regenerateMs = millisecondsSince(regenerating);
        if (!again) {
            return fail(regenerate_failure::kFailed, "the regeneration: " + again.error().toString());
        }
        regeneration = std::make_shared<const world::Regeneration>(std::move(*again));
        previous = std::make_shared<const world::WorldPlan>(std::move(input.previous));
    }
    for (const world::ValidationFinding& finding : regeneration->result.validation.findings) {
        if (finding.severity == world::Severity::Hard) {
            return fail(regenerate_failure::kInvalidPlan,
                        fmt::format("hard rule {} broken at {}: {}", finding.rule, finding.subject, finding.message));
        }
    }
    commit.impact = regeneration->impact;
    commit.changed = regeneration->changed;
    const Clock::time_point committing = Clock::now();
    if (regeneration->changed) {
        world::SaveOptions options;
        options.writer = std::string(writer);
        auto written = world::commitWorld(*lock, world::WorldAction{"regenerate", preview.scope.toString()},
                                          regeneration->result, regeneration->overrides, options);
        if (!written) {
            return fail(written.error().code == ErrorCode::ValidationFailed ? regenerate_failure::kInvalidPlan
                                                                            : regenerate_failure::kFailed,
                        "the commit: " + written.error().toString());
        }
        commit.written = written->written;
        if (written->snapshot) {
            commit.snapshot = written->snapshot->name;
        }
        // The plan stored now is the regeneration's (savePlan()'s text of it, its digest the commit's): nothing is
        // read back (I4b step 6's review: it cost a read and a parse of the whole plan).
        commit.planDigest = written->plan.digest;
        commit.plan = std::make_shared<const world::WorldPlan>(regeneration->result.plan);
    } else {
        // Nothing to write: the plan stored is the regeneration's previous one.
        commit.planDigest = storedDigest;
        commit.plan = std::make_shared<const world::WorldPlan>(regeneration->result.plan);
    }
    commit.commitMs = millisecondsSince(committing);
    lock.reset(); // the lock goes before the items (they are the cache's, not the world's)
    const Clock::time_point keeping = Clock::now();
    if (commit.changed && inputs.snapshot) {
        commit.items = keepRoomItems(paths, inputs.library, *inputs.snapshot, *commit.plan,
                                     previous.get());
    } else if (commit.changed) {
        removeBuildingStamp(buildingStampFile(paths.cacheDir, inputs.library));
    }
    commit.itemsMs = millisecondsSince(keeping);
    commit.totalMs = millisecondsSince(start);
    spdlog::info(
        "Regeneration: {} {} ({}{}; {:.0f} ms: lock and read {:.0f}, regeneration {:.0f}, commit {:.0f}, items "
        "{:.0f})",
        preview.scope.toString(), commit.written ? "written" : "nothing to write",
        dropOverrides ? "the scope's overrides dropped" : "the overrides kept",
        commit.snapshot.empty() ? std::string() : ", snapshot " + commit.snapshot, commit.totalMs, commit.lockMs,
        commit.regenerateMs, commit.commitMs, commit.itemsMs);
    return commit;
}

UndoPreview previewUndo(const WorldPaths& paths, const catalog::LibraryId& library, const world::WorldPlan* plan) {
    const Clock::time_point start = Clock::now();
    UndoPreview preview;
    const world::WorldFiles files = world::worldFiles(paths.userDataRoot, library);
    const auto fail = [&](std::string_view failure, std::string error) {
        preview.failure = std::string(failure);
        preview.error = std::move(error);
        preview.totalMs = millisecondsSince(start);
        spdlog::info("Undo: nothing to offer ({}): {}", preview.failure, preview.error);
        return preview;
    };
    if (const std::optional<world::InterruptedAction> interrupted = world::checkInterruptedAction(files)) {
        return fail(regenerate_failure::kBusy,
                    "an action is in progress or was interrupted (" + interrupted->snapshot + ")");
    }
    const world::SnapshotScan scan = world::scanSnapshots(files);
    if (!scan.damaged.empty()) {
        return fail(regenerate_failure::kDamaged, "a damaged snapshot folder: " + scan.damaged.front().name);
    }
    const std::optional<world::WorldSnapshot> next = world::nextUndo(scan.snapshots);
    if (!next) {
        return fail(regenerate_failure::kNothingToUndo, "no explicit action to undo");
    }
    preview.content.undoAction = next->action;
    preview.content.undoScope = next->scope;
    preview.content.undoWhen = localSnapshotTime(next->takenAt);
    if (next->scope.starts_with("floor:")) {
        preview.content.undoScopeTitle.floor = next->scope.substr(6);
    } else if (next->scope != "world" && plan != nullptr) {
        if (const auto room = world::RoomId::parse(next->scope)) {
            preview.content.undoScopeTitle.roomKind = room->kind() == world::RoomIdKind::System   ? "console"
                                                      : room->kind() == world::RoomIdKind::Shared ? "shared"
                                                                                                  : "arcade";
            preview.content.undoScopeTitle.roomTitle =
                room->kind() == world::RoomIdKind::System ? room->system().str() : std::string();
        }
    }
    preview.snapshot = next;
    preview.totalMs = millisecondsSince(start);
    spdlog::info("Undo: the next undo restores {} ({} of {}, {})", next->name, next->action, next->scope,
                 next->takenAt);
    return preview;
}

UndoCommit commitUndo(const WorldPaths& paths, const catalog::LibraryId& library, std::string_view expected,
                      std::string_view writer, const BuildingSnapshot* snapshot, const world::WorldPlan* before,
                      std::chrono::milliseconds lockWait) {
    const Clock::time_point start = Clock::now();
    UndoCommit commit;
    const auto fail = [&](std::string_view failure, std::string error) {
        commit.failure = std::string(failure);
        commit.error = std::move(error);
        commit.totalMs = millisecondsSince(start);
        spdlog::warn("Undo: nothing restored ({}): {}", commit.failure, commit.error);
        return commit;
    };
    const world::WorldFiles files = world::worldFiles(paths.userDataRoot, library);
    auto acquired = world::WorldLock::acquire(files, lockWait);
    if (!acquired) {
        return fail(failureOf(acquired.error()), "the world's lock: " + acquired.error().toString());
    }
    std::optional<world::WorldLock> lock;
    lock.emplace(std::move(*acquired));
    commit.lockMs = millisecondsSince(start);
    world::UndoOptions options;
    options.writer = std::string(writer);
    options.dryRun = true;
    auto next = world::undoLastAction(*lock, options);
    if (!next) {
        return fail(next.error().code == ErrorCode::NotFound           ? regenerate_failure::kNothingToUndo
                    : next.error().code == ErrorCode::ValidationFailed ? regenerate_failure::kDamaged
                                                                       : regenerate_failure::kFailed,
                    "the undo: " + next.error().toString());
    }
    if (next->restored.name != expected) {
        return fail(regenerate_failure::kChanged,
                    fmt::format("the next undo restores {}, not {} (another writer acted meanwhile)",
                                next->restored.name, expected));
    }
    const Clock::time_point undoing = Clock::now();
    options.dryRun = false;
    auto undone = world::undoLastAction(*lock, options);
    commit.undoMs = millisecondsSince(undoing);
    if (!undone) {
        return fail(regenerate_failure::kFailed, "the undo: " + undone.error().toString());
    }
    commit.written = true;
    commit.restored = undone->restored.name;
    commit.kept = undone->kept ? undone->kept->name : std::string();
    // The plan restored, read back: the snapshot held it as text (perhaps of schema 1), not as a plan in memory.
    auto building = world::loadBuildingData(paths.assetRoot);
    auto systems = world::loadSystemsData(paths.assetRoot);
    if (!building || !systems) {
        commit.error = "the world's data: " + (!building ? building.error() : systems.error()).toString();
    } else if (auto now = readStored(files, *building, *systems); !now) {
        // A world without its plan (the first action created it), or one that cannot be read.
        commit.error = "the plan restored: " + now.error().toString();
    } else {
        commit.planDigest = now->digest;
        commit.plan = std::make_shared<const world::WorldPlan>(std::move(now->plan));
    }
    if (!commit.error.empty()) {
        // The undo stands, written: the floor follows the stored plan through a live update.
        spdlog::warn("Undo: written, but {}", commit.error);
    }
    lock.reset();
    const Clock::time_point keeping = Clock::now();
    if (snapshot != nullptr && commit.plan) {
        commit.items = keepRoomItems(paths, library, *snapshot, *commit.plan, before);
    } else {
        // The plan is no longer the one the stamp names, whatever could be read of it.
        removeBuildingStamp(buildingStampFile(paths.cacheDir, library));
    }
    commit.itemsMs = millisecondsSince(keeping);
    commit.totalMs = millisecondsSince(start);
    spdlog::info("Undo: {} restored, the state it replaced kept as {} ({:.0f} ms: lock {:.0f}, undo {:.0f}, items "
                 "{:.0f})",
                 commit.restored, commit.kept, commit.totalMs, commit.lockMs, commit.undoMs, commit.itemsMs);
    return commit;
}

std::set<world::RoomId> roomsWithOtherItems(const world::WorldPlan& before, const world::WorldPlan& after) {
    struct Content {
        const world::Room* room = nullptr;
        std::vector<const world::Placement*> placements;
        std::vector<const world::UnplacedGame*> unplaced;
    };
    const auto byRoom = [](const world::WorldPlan& plan) {
        std::map<world::RoomId, Content> rooms;
        for (const world::Room& room : plan.rooms) {
            rooms[room.id].room = &room;
        }
        for (const world::Placement& placement : plan.placements) {
            rooms[placement.slot.fixture().room()].placements.push_back(&placement);
        }
        for (const world::UnplacedGame& game : plan.unplaced) {
            rooms[game.room].unplaced.push_back(&game);
        }
        return rooms;
    };
    const auto same = [](const auto& a, const auto& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto* x, const auto* y) { return *x == *y; });
    };
    const std::map<world::RoomId, Content> was = byRoom(before);
    const std::map<world::RoomId, Content> now = byRoom(after);
    std::set<world::RoomId> changed;
    for (const auto& [id, content] : now) {
        const auto old = was.find(id);
        if (old == was.end() || (old->second.room == nullptr) != (content.room == nullptr) ||
            (content.room != nullptr && !(*old->second.room == *content.room)) ||
            !same(old->second.placements, content.placements) || !same(old->second.unplaced, content.unplaced)) {
            changed.insert(id);
        }
    }
    for (const auto& [id, content] : was) {
        if (!now.contains(id)) {
            changed.insert(id);
        }
    }
    return changed;
}

std::map<catalog::SystemName, RoomItems> keepRoomItems(const WorldPaths& paths, const catalog::LibraryId& library,
                                                       const BuildingSnapshot& snapshot, const world::WorldPlan& plan,
                                                       const world::WorldPlan* before) {
    // The systems whose items may change: those of the rooms that changed (in either plan), else every one.
    std::optional<std::set<catalog::SystemName>> changedSystems;
    if (before != nullptr) {
        changedSystems.emplace();
        const std::set<world::RoomId> rooms = roomsWithOtherItems(*before, plan);
        for (const world::WorldPlan* side : {before, &plan}) {
            for (const world::Room& room : side->rooms) {
                if (rooms.contains(room.id)) {
                    changedSystems->insert(room.sections.begin(), room.sections.end());
                    if (room.id.kind() == world::RoomIdKind::System) {
                        changedSystems->insert(room.id.system());
                    }
                }
            }
        }
    }
    std::map<catalog::SystemName, RoomItems> kept;
    for (const RoomSnapshotResult& system : snapshot.systems) {
        if (system.error || roomHolding(plan, system.system) == nullptr ||
            (changedSystems && !changedSystems->contains(system.system))) {
            continue;
        }
        RoomItems items = snapshotRoomItems(system, plan);
        const std::filesystem::path file = roomItemsFile(paths.cacheDir, library, system.system);
        if (auto written = writeRoomItems(file, items); !written) {
            spdlog::warn("Regeneration: the items of {} for the next start cannot be kept: {}", system.system.str(),
                         written.error().message);
        }
        kept.emplace(system.system, std::move(items));
    }
    // The plan is no longer the one the building stamp names: the next start runs the building job (I4 step 6b).
    removeBuildingStamp(buildingStampFile(paths.cacheDir, library));
    return kept;
}

bool regenerationCovers(std::string_view area, std::string_view scope, const world::WorldPlan* before) {
    const auto room = world::RoomId::parse(area);
    if (scope == "world") {
        return room.has_value() || area.starts_with("hall:"); // every room, every hall
    }
    if (!room) {
        // A hall: a floor's regeneration places its rooms again (I4b step 6's review: the walker goes to its elevator);
        // a room's never changes it.
        return area.starts_with("hall:") && scope.starts_with("floor:") && area.substr(5) == scope.substr(6);
    }
    if (scope.starts_with("floor:")) {
        const std::string_view floor = scope.substr(6);
        if (before == nullptr) {
            return true;
        }
        return std::any_of(before->rooms.begin(), before->rooms.end(), [&](const world::Room& candidate) {
            return candidate.id == *room && candidate.floor == floor;
        });
    }
    const auto scoped = world::RoomId::parse(scope);
    return scoped && scoped->head() == room->head(); // a room scope covers its chain (ADR-0015 §7)
}

std::optional<world::RoomId> confirmationRoomOf(const std::vector<world::RoomId>& rooms,
                                                const world::WorldPlan& plan) {
    for (const world::RoomId& room : rooms) {
        if (std::any_of(plan.rooms.begin(), plan.rooms.end(),
                        [&](const world::Room& candidate) { return candidate.id == room; })) {
            return room;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<world::RoomId>> roomsChangedAlone(const world::WorldPlan& before,
                                                            const world::WorldPlan& after, std::string_view floor) {
    // The floor's rooms by id, each record without its generator version (an update or a regeneration stamps it).
    const auto onFloor = [&](const world::WorldPlan& plan) {
        std::map<world::RoomId, world::Room> rooms;
        for (const world::Room& room : plan.rooms) {
            if (room.floor == floor) {
                world::Room record = room;
                record.generatorVersion = {};
                rooms.emplace(room.id, std::move(record));
            }
        }
        return rooms;
    };
    if (onFloor(before) != onFloor(after)) {
        return std::nullopt; // a room added, removed, moved, grown or of another template: the hall may change
    }
    // The floor's rooms' digests alone (the whole plan's documents cost a few hundred milliseconds on a large plan).
    std::set<world::RoomId> rooms;
    for (const world::Room& room : after.rooms) {
        if (room.floor == floor) {
            rooms.insert(room.id);
        }
    }
    auto digestsBefore = world::roomContentDigests(before, rooms);
    auto digestsAfter = world::roomContentDigests(after, rooms);
    if (!digestsBefore || !digestsAfter) {
        return std::nullopt;
    }
    std::vector<world::RoomId> changed;
    for (const world::Room& room : after.rooms) {
        if (room.floor != floor) {
            continue;
        }
        const auto was = digestsBefore->find(room.id);
        const auto now = digestsAfter->find(room.id);
        if (was == digestsBefore->end() || now == digestsAfter->end() || was->second != now->second) {
            changed.push_back(room.id);
        }
    }
    return changed;
}

std::string localSnapshotTime(std::string_view utc) {
    auto time = core::parseUtcIso8601(utc);
    if (!time) {
        return {};
    }
    const std::time_t seconds = std::chrono::system_clock::to_time_t(*time);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) {
        return {};
    }
    return fmt::format("{:02}/{:02}/{:04} {:02}:{:02}", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
                       local.tm_hour, local.tm_min);
}

} // namespace rv::app
