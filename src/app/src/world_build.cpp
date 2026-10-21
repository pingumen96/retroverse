#include <rv/app/world_build.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/room_items.hpp>
#include <rv/app/room_screen.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/sha256.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/world_config.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// A plan file this program does not take: edited by hand (not its canonical text), not JSON, or newer.
[[nodiscard]] bool isRefusedPlan(const Error& error) noexcept {
    return error.code == ErrorCode::ValidationFailed || error.code == ErrorCode::ParseError ||
           error.code == ErrorCode::Unsupported;
}

/// Reads a plan file (world::readStoredPlan) and lifts a schema-1 plan into the building (ADR-0013 §5; the lifted plan
/// is saved in schema 2 by its first update). The digest stays the file's. Errors: those of readStoredPlan(), of the
/// building's data, of the lift.
Expected<world::StoredPlan> readLiftedPlan(const std::filesystem::path& assetRoot, const std::filesystem::path& file) {
    auto stored = world::readStoredPlan(file);
    if (!stored || stored->plan.schemaVersion == world::kPlanSchemaVersion) {
        return stored;
    }
    auto building = world::loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    auto systems = world::loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    auto lifted = world::lift(stored->plan, *building, *systems);
    if (!lifted) {
        Error error = lifted.error();
        error.code = ErrorCode::ValidationFailed; // a plan this program does not take
        return propagate(std::move(error));
    }
    spdlog::info("World: the stored plan of schema 1 is lifted into the building (room {} on floor {})",
                 lifted->rooms.front().id.toString(), lifted->rooms.front().floor);
    return world::StoredPlan{std::move(*lifted), std::move(stored->digest)};
}

/// Logs the validation's findings of a plan made (ADR-0010 §11: every hard one), into `build`.
void collectFindings(const world::ValidationReport& validation, WorldBuild& build) {
    for (const world::ValidationFinding& finding : validation.findings) {
        if (finding.severity == world::Severity::Hard) {
            spdlog::error("World plan: hard rule {} broken by {}: {}", finding.rule, finding.subject, finding.message);
            build.hardFindings.push_back(finding);
        } else {
            ++build.softFindings;
            spdlog::debug("World plan: soft finding {} {}: {}", finding.rule, finding.subject, finding.message);
        }
    }
}

/// The plan shown is `stored` as it is, for `why`.
void keep(WorldBuild& build, const world::StoredPlan& stored, std::string_view notice, std::string reason) {
    build.outcome = WorldOutcome::Kept;
    build.plan = stored.plan;
    build.planDigest = stored.digest;
    build.notice = std::string(notice);
    build.reason = std::move(reason);
}

void noPlan(WorldBuild& build, std::string_view notice, std::string reason) {
    build.outcome = WorldOutcome::NoPlan;
    build.plan.reset();
    build.planDigest.clear();
    build.notice = std::string(notice);
    build.reason = std::move(reason);
}

/// The floor's part of `build` dropped (a floor that cannot be shown).
void dropFloor(WorldBuild& build) {
    build.scene.reset();
    build.sceneDigest.clear();
    build.undressed.clear();
    build.rooms.clear();
    build.objectFallbacks.clear();
    build.screenGames.clear();
}

/// The snapshot's entry of `game`, or nullptr (entries come by GameId).
[[nodiscard]] const catalog::RoomEntry* entryOf(const catalog::RoomSnapshot& snapshot, const catalog::GameId& game) {
    const auto found =
        std::lower_bound(snapshot.entries.begin(), snapshot.entries.end(), game,
                         [](const catalog::RoomEntry& entry, const catalog::GameId& id) { return entry.id < id; });
    return found != snapshot.entries.end() && found->id == game ? &*found : nullptr;
}

/// The systems a room holds: its sections, else its id's system.
[[nodiscard]] std::vector<catalog::SystemName> systemsOf(const world::Room& room) {
    if (!room.sections.empty()) {
        return room.sections;
    }
    return {room.id.system()};
}

/// The room of `plan` that holds `system` in its sections, or nullptr.
[[nodiscard]] const world::Room* roomHolding(const world::WorldPlan& plan, const catalog::SystemName& system) {
    for (const world::Room& room : plan.rooms) {
        const std::vector<catalog::SystemName> systems = systemsOf(room);
        if (std::find(systems.begin(), systems.end(), system) != systems.end()) {
            return &room;
        }
    }
    return nullptr;
}

/// The room items of `snapshot`'s system as the building job keeps them for the floors' builds (room_items.hpp): every
/// placement of `plan` of a game the snapshot lists, and every placement of the system's console room whatever the
/// snapshot says (a game it no longer lists keeps its slot with no text), each with its catalog title, the title of its
/// file name, its year, its cover's path and its availability (world::collectionTitles' rule).
[[nodiscard]] RoomItems itemsOfSnapshot(const RoomSnapshotResult& snapshot, const world::WorldPlan& plan) {
    RoomItems items;
    items.systemName = snapshot.systemFullName;
    items.platforms = snapshot.platformTags;
    const std::vector<world::CollectionTitle> titles = world::collectionTitles(snapshot.snapshot);
    const auto titleOf = [&](const catalog::GameId& game) -> const world::CollectionTitle* {
        const auto found =
            std::lower_bound(titles.begin(), titles.end(), game,
                             [](const world::CollectionTitle& title, const catalog::GameId& id) { return title.game < id; });
        return found != titles.end() && found->game == game ? &*found : nullptr;
    };
    const auto own = world::RoomId::forSystem(snapshot.system);
    for (const world::Placement& placement : plan.placements) {
        const catalog::RoomEntry* entry = entryOf(snapshot.snapshot, placement.game);
        const bool inOwnRoom = own && placement.slot.fixture().room() == *own;
        if (entry == nullptr && !inOwnRoom) {
            continue;
        }
        RoomItemText text;
        text.game = placement.game;
        if (const world::CollectionTitle* title = titleOf(placement.game)) {
            text.title = title->title;
            text.available = title->available;
        }
        if (entry != nullptr) {
            text.fallbackTitle = catalog::titleFromFileStem(entry->key.relativePath(), entry->directory);
            if (entry->year) {
                text.year = std::to_string(*entry->year);
            }
        }
        text.coverPath = coverPathOf(snapshot.mediaPaths, placement.game);
        items.items.push_back(std::move(text));
    }
    std::sort(items.items.begin(), items.items.end(),
              [](const RoomItemText& a, const RoomItemText& b) { return a.game < b.game; });
    items.items.erase(std::unique(items.items.begin(), items.items.end(),
                                  [](const RoomItemText& a, const RoomItemText& b) { return a.game == b.game; }),
                      items.items.end());
    return items;
}

/// The plan of `build` from the building's snapshot (see planBuilding()).
void makePlan(const WorldBuildInput& input, const core::JobContext& job, WorldBuild& build) {
    const BuildingSnapshot& snapshot = input.snapshot;
    const world::WorldFiles files = world::worldFiles(input.paths.userDataRoot, input.library);

    // A catalog with no game of a system that gives a room, and no plan stored: the test room, nothing written into the
    // data directory (not even the world's lock), as I3's room did without snes games.
    const bool anyGame =
        std::any_of(snapshot.systems.begin(), snapshot.systems.end(), [](const RoomSnapshotResult& system) {
            return !system.error ? !system.snapshot.entries.empty() : system.error->code != ErrorCode::NotFound;
        });
    std::error_code planMissing;
    if (!snapshot.error && !anyGame && !std::filesystem::exists(files.plan, planMissing) && !planMissing) {
        noPlan(build, world_notice::kNoGames, "the catalog has no game of a system that gives a room");
        return;
    }

    // The world's lock first, then its plan read again under it (a writer reads what it replaces under the lock).
    const Clock::time_point lockStart = Clock::now();
    std::optional<world::WorldLock> lock;
    if (auto acquired = world::WorldLock::acquire(files, input.lockWait)) {
        lock.emplace(std::move(*acquired));
    } else {
        spdlog::warn("World: the world's lock: {}; the building is made without saving its plan",
                     acquired.error().toString());
    }
    std::optional<world::StoredPlan> stored;
    if (auto read = readLiftedPlan(input.paths.assetRoot, files.plan)) {
        stored = std::move(*read);
    } else if (read.error().code != ErrorCode::NotFound) {
        spdlog::warn("World: the stored plan is not used: {}", read.error().toString());
        noPlan(build, isRefusedPlan(read.error()) ? world_notice::kRefused : world_notice::kFailed,
               "the stored plan cannot be used: " + read.error().message);
        return;
    }
    build.lockMs = millisecondsSince(lockStart);
    if (job.cancelled()) {
        return;
    }
    const auto fail = [&](std::string_view notice, std::string reason) {
        if (stored) {
            keep(build, *stored, world_notice::kNone, std::move(reason));
        } else {
            noPlan(build, notice, std::move(reason));
        }
    };
    if (snapshot.error) {
        fail(world_notice::kFailed, "the catalog's snapshot failed: " + snapshot.error->message);
        return;
    }

    auto config = world::loadWorldConfig(input.paths.assetRoot, input.paths.configDir);
    if (!config) {
        noPlan(build, world_notice::kFailed, "the world's configuration: " + config.error().message);
        return;
    }
    for (const Error& issue : config->issues) {
        spdlog::warn("world.json left out: {}", issue.toString());
    }
    auto building = world::loadBuildingData(input.paths.assetRoot);
    auto systems = world::loadSystemsData(input.paths.assetRoot);
    if (!building || !systems) {
        fail(world_notice::kFailed, "the world's data: " + (!building ? building.error() : systems.error()).message);
        return;
    }
    world::WorldOverrides overrides;
    if (auto loaded = world::loadOverrides(files.overrides)) {
        overrides = std::move(*loaded);
    } else {
        spdlog::warn("World: overrides.json left out: {}", loaded.error().toString());
    }

    // The selections, one per system of the snapshot, merged by system name (the snapshot's order): nothing is decided
    // from unknown media (ADR-0010 §3).
    const Clock::time_point selectStart = Clock::now();
    std::vector<world::RoomSelection> selections;
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    for (const RoomSnapshotResult& system : snapshot.systems) {
        if (system.error) {
            if (system.error->code == ErrorCode::NotFound) {
                continue; // no entries in the catalog
            }
            fail(world_notice::kFailed,
                 fmt::format("the catalog's snapshot of {}: {}", system.system.str(), system.error->message));
            return;
        }
        auto selection = world::selectRoom(world::RoomInput{input.library, system.snapshot}, config->config);
        if (!selection) {
            const bool unknown = selection.error().code == ErrorCode::Unavailable;
            if (stored) {
                keep(build, *stored, unknown ? world_notice::kMediaUnknownKept : world_notice::kFailed,
                     (unknown ? "the media are unknown: " : "the room's selection: ") + selection.error().message);
            } else {
                noPlan(build, unknown ? world_notice::kMediaUnknown : world_notice::kFailed,
                       (unknown ? "the media are unknown, no first plan: " : "the room's selection: ") +
                           selection.error().message);
            }
            return;
        }
        platformTags[system.system] = system.platformTags;
        selections.push_back(std::move(*selection));
    }
    build.selectMs = millisecondsSince(selectStart);
    if (job.cancelled()) {
        return;
    }

    const Clock::time_point planStart = Clock::now();
    Expected<world::GenerationResult> result = makeError(ErrorCode::Unknown, "no plan made");
    if (stored) {
        // The update of the building's console rooms, as `retroverse-cli worldplan update` makes it: every room with a
        // selection updated, the others as they stand; a system without a room waits for I4 step 6a (new systems).
        std::vector<world::RoomSelection> updated;
        for (world::RoomSelection& selection : selections) {
            const bool hasRoom = std::any_of(stored->plan.rooms.begin(), stored->plan.rooms.end(),
                                             [&](const world::Room& room) {
                                                 return room.kind == world::RoomKind::Console &&
                                                        room.id == selection.room;
                                             });
            if (hasRoom) {
                updated.push_back(std::move(selection));
            } else if (!selection.titles.empty()) {
                spdlog::info("World: {} has no console room in the stored building; new systems enter it with I4 "
                             "step 6a",
                             selection.room.system().str());
            }
        }
        std::vector<world::WorldData> rooms;
        for (const world::Room& room : stored->plan.rooms) {
            const catalog::SystemName system = room.sections.empty() ? room.id.system() : room.sections.front();
            const auto tags = platformTags.find(system);
            auto data = world::loadRoomData(input.paths.assetRoot, room,
                                            tags != platformTags.end() ? std::span<const std::string>(tags->second)
                                                                       : std::span<const std::string>());
            if (!data) {
                keep(build, *stored, world_notice::kNone,
                     fmt::format("the world's data of {}: {}", room.id.toString(), data.error().message));
                return;
            }
            rooms.push_back(std::move(*data));
        }
        world::BuildingUpdateInput update;
        update.library = input.library;
        update.selections = std::move(updated);
        update.platformTags = platformTags;
        update.config = config->config;
        update.overrides = std::move(overrides);
        update.previous = stored->plan;
        update.previousDigest = stored->digest;
        if (snapshot.aliasError) {
            spdlog::warn("World: the aliases of the placed games cannot be read; the update runs without them: {}",
                         snapshot.aliasError->toString());
        } else {
            update.aliases = snapshot.aliases;
        }
        result = world::updateBuilding(update, *building, *systems, rooms);
    } else {
        // A first building, as `retroverse-cli worldplan generate` makes it: every system with a placeable title, a
        // system whose data the asset root cannot give left out (logged).
        std::vector<world::RoomSelection> generated;
        std::vector<world::WorldData> rooms;
        for (world::RoomSelection& selection : selections) {
            if (selection.titles.empty()) {
                spdlog::info("World: {} is left out of the building: no placeable title", selection.room.system().str());
                continue;
            }
            world::RoomDataRequest request;
            request.system = selection.room.system();
            if (const auto tags = platformTags.find(request.system); tags != platformTags.end()) {
                request.platformTags = tags->second;
            }
            auto data = world::loadWorldData(input.paths.assetRoot, request);
            if (!data) {
                spdlog::warn("World: {} is left out of the building: no data yet: {}", request.system.str(),
                             data.error().message);
                continue;
            }
            rooms.push_back(std::move(*data));
            generated.push_back(std::move(selection));
        }
        if (generated.empty()) {
            noPlan(build, world_notice::kNoGames, "the catalog has no system with a placeable title for a room");
            return;
        }
        world::BuildingInput generation;
        generation.library = input.library;
        generation.selections = std::move(generated);
        generation.config = config->config;
        generation.overrides = std::move(overrides);
        generation.platformTags = platformTags;
        result = world::generateBuilding(generation, *building, *systems, rooms);
    }
    build.planMs = millisecondsSince(planStart);
    if (!result) {
        if (stored) {
            // A plan of a newer generator is shown as it is and never written (its notice says how to regenerate it).
            keep(build, *stored,
                 result.error().code == ErrorCode::Unsupported ? world_notice::kRefused : world_notice::kNone,
                 "the update: " + result.error().message);
        } else {
            noPlan(build, world_notice::kFailed, "the generation: " + result.error().message);
        }
        return;
    }
    build.changes = result->changes;
    if (result->changes.outcome == world::GenerationOutcome::RequiresExplicitRegeneration) {
        keep(build, *stored, world_notice::kRegeneration,
             "the plan needs an explicit regeneration: " + result->changes.regenerationReason);
        return;
    }
    collectFindings(result->validation, build);
    if (!build.hardFindings.empty()) {
        const std::string reason = fmt::format("the {} breaks {} hard rule(s) of the validation",
                                               stored ? "updated plan" : "first plan", build.hardFindings.size());
        fail(world_notice::kFailed, reason);
        return;
    }
    // A room kept as it is until a regeneration (ADR-0013 §5: an older template version, prefab levels that changed,
    // another generator major) places nothing new: said in the log, and by the regeneration's notice when it is the
    // start room.
    for (const world::KeptRoom& kept : result->changes.keptRooms) {
        spdlog::warn("World: {} is kept as it is until 'retroverse-cli worldplan generate --replace' (its new games "
                     "are not placed): {}",
                     kept.room.toString(), kept.reason);
        if (input.startRoom && kept.room == *input.startRoom) {
            build.notice = std::string(world_notice::kRegeneration);
        }
    }
    build.plan = std::move(result->plan);
    build.planDigest = result->changes.digestAfter;
    const bool unchanged = stored && result->changes.digestAfter == stored->digest;
    build.outcome = !stored ? WorldOutcome::Generated : unchanged ? WorldOutcome::Unchanged : WorldOutcome::Updated;
    if (!unchanged) {
        const Clock::time_point saveStart = Clock::now();
        if (!lock) {
            build.saveError = Error{ErrorCode::Conflict, "the world's lock is held by another program", {}};
        } else {
            world::GenerationResult saved;
            saved.plan = *build.plan;
            saved.changes = result->changes;
            saved.validation = result->validation;
            world::SaveOptions options;
            options.writer = input.writer;
            options.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::duration<double, std::milli>(build.planMs));
            if (auto report = world::savePlan(*lock, saved, options)) {
                build.written = report->written;
                if (report->metaError) {
                    spdlog::warn("World: the plan's meta sidecar: {}", report->metaError->toString());
                }
            } else {
                build.saveError = report.error();
            }
        }
        build.saveMs = millisecondsSince(saveStart);
        if (build.saveError) {
            spdlog::warn("World: the plan is shown but not saved: {}", build.saveError->toString());
        }
    }
}

/// `value` rounded to 1 / `perUnit` (the scene builder's rounding: a number computed from the plan writes one text).
[[nodiscard]] double rounded(double value, double perUnit) noexcept {
    const double result = std::round(value * perUnit) / perUnit;
    return result == 0.0 ? 0.0 : result; // no "-0.0"
}

[[nodiscard]] double meters(std::int64_t millimetres) noexcept {
    return static_cast<double>(millimetres) / 1000.0;
}

/// A point [x, y, z] of the floor's frame (meters) in the frame of the room standing at `start`.
[[nodiscard]] Json roomPoint(const Json& point, const world::RoomPlacement& start) {
    const double sign = start.yawHalfTurns % 2 == 0 ? 1.0 : -1.0;
    return Json::array({rounded(sign * (point[0].get<double>() - meters(start.xMm)), 1e6), point[1],
                        rounded(sign * (point[2].get<double>() - meters(start.zMm)), 1e6)});
}

/// A yaw of the floor's frame (degrees) in the frame of the room standing at `start`: less half a turn for a half-turned
/// room, within (-180, 180], rounded to 1e-4.
[[nodiscard]] double roomYaw(double yawDeg, const world::RoomPlacement& start) {
    if (start.yawHalfTurns % 2 == 0) {
        return yawDeg;
    }
    double turned = yawDeg - 180.0;
    while (turned > 180.0) {
        turned -= 360.0;
    }
    while (turned <= -180.0) {
        turned += 360.0;
    }
    return rounded(turned, 1e4);
}

/// The doorway's passage of `room` on the floor `hall` derives (its interior to its wall's outer face), as a box from the
/// floor to the doorway's top, in the frame of the room standing at `start` (none: the floor's).
[[nodiscard]] core::Aabb doorwayBox(const world::Hall& hall, const world::RoomId& room,
                                    const std::optional<world::RoomPlacement>& start) {
    for (const world::FloorDoorway& doorway : hall.doorways) {
        if (doorway.room != room) {
            continue;
        }
        const world::Footprint& passage = doorway.passage;
        glm::vec3 a(static_cast<float>(meters(passage.minXMm)), 0.0f, static_cast<float>(meters(passage.minZMm)));
        glm::vec3 b(static_cast<float>(meters(passage.maxXMm)), static_cast<float>(meters(doorway.heightMm)),
                    static_cast<float>(meters(passage.maxZMm)));
        if (start) {
            const float sign = start->yawHalfTurns % 2 == 0 ? 1.0f : -1.0f;
            const glm::vec3 origin(static_cast<float>(meters(start->xMm)), 0.0f,
                                   static_cast<float>(meters(start->zMm)));
            a = glm::vec3(sign * (a.x - origin.x), a.y, sign * (a.z - origin.z));
            b = glm::vec3(sign * (b.x - origin.x), b.y, sign * (b.z - origin.z));
        }
        return core::Aabb{glm::min(a, b), glm::max(a, b)};
    }
    return core::Aabb{};
}

/// The rooms' items `build`'s floor shows: `sources`' for the systems it has, the items files' for the others.
[[nodiscard]] std::map<catalog::SystemName, RoomItems> floorItems(const WorldPaths& paths, const WorldBuild& build,
                                                                  std::span<const world::Room* const> rooms,
                                                                  const FloorSources& sources) {
    std::map<catalog::SystemName, RoomItems> items;
    for (const world::Room* room : rooms) {
        for (const catalog::SystemName& system : systemsOf(*room)) {
            if (items.contains(system)) {
                continue;
            }
            if (const auto known = sources.items.find(system); known != sources.items.end()) {
                items.emplace(system, known->second);
                continue;
            }
            // The room's items as they last showed: a missing file leaves its items pending until the building's.
            if (auto read = readRoomItems(roomItemsFile(paths.cacheDir, build.library, system))) {
                items.emplace(system, std::move(*read));
            } else if (read.error().code != ErrorCode::NotFound) {
                spdlog::warn("World: the items of {} kept at the last start are not used: {}", system.str(),
                             read.error().message);
            }
        }
    }
    return items;
}

} // namespace

std::string_view toString(WorldOutcome outcome) noexcept {
    switch (outcome) {
    case WorldOutcome::Generated:
        return "generated";
    case WorldOutcome::Updated:
        return "updated";
    case WorldOutcome::Unchanged:
        return "unchanged";
    case WorldOutcome::Kept:
        return "kept";
    case WorldOutcome::NoPlan:
        return "no-plan";
    }
    return "unknown";
}

std::string_view toString(WorldPhase phase) noexcept {
    switch (phase) {
    case WorldPhase::WaitingForCatalog:
        return "waiting-for-catalog";
    case WorldPhase::ReadingPlan:
        return "reading-plan";
    case WorldPhase::Previewing:
        return "previewing";
    case WorldPhase::Building:
        return "building";
    case WorldPhase::Floor:
        return "floor";
    case WorldPhase::Ready:
        return "ready";
    case WorldPhase::Done:
        return "done";
    }
    return "unknown";
}

const RoomCollection* WorldBuild::findRoom(const world::RoomId& room) const noexcept {
    for (const RoomCollection& collection : rooms) {
        if (collection.room == room) {
            return &collection;
        }
    }
    return nullptr;
}

std::size_t WorldBuild::itemCount() const noexcept {
    std::size_t count = 0;
    for (const RoomCollection& collection : rooms) {
        count += collection.layout.items.size();
    }
    return count;
}

StoredWorldPlan readWorldPlan(const WorldPaths& paths, const catalog::LibraryId& library) {
    const Clock::time_point start = Clock::now();
    StoredWorldPlan result;
    result.library = library;
    auto stored = readLiftedPlan(paths.assetRoot, world::worldFiles(paths.userDataRoot, library).plan);
    if (stored) {
        result.stored = std::move(*stored);
    } else if (stored.error().code != ErrorCode::NotFound) {
        result.error = stored.error();
    }
    result.milliseconds = millisecondsSince(start);
    return result;
}

SystemFilter roomSystemsFilter(const std::filesystem::path& assetRoot) {
    auto building = world::loadBuildingData(assetRoot);
    auto systems = world::loadSystemsData(assetRoot);
    if (!building || !systems) {
        spdlog::warn("World: the building's data cannot be read, every system is snapshotted: {}",
                     (!building ? building.error() : systems.error()).toString());
        return {};
    }
    auto data = std::make_shared<const std::pair<world::BuildingData, world::SystemsData>>(std::move(*building),
                                                                                         std::move(*systems));
    return [data](const catalog::SystemName& system, std::span<const std::string> platforms) {
        return world::classify(system, platforms, data->second, data->first).hasRoom();
    };
}

std::optional<std::string> startFloorOf(const world::WorldPlan& plan, const std::optional<world::RoomId>& startRoom) {
    if (!startRoom) {
        return std::string(kStartFloor);
    }
    for (const world::Room& room : plan.rooms) {
        if (room.id == *startRoom) {
            return room.floor;
        }
    }
    return std::nullopt;
}

world::RoomPlacement relativePlacement(const world::RoomPlacement& room, const world::RoomPlacement& start) noexcept {
    const std::int64_t sign = start.yawHalfTurns % 2 == 0 ? 1 : -1;
    world::RoomPlacement relative;
    relative.xMm = sign * (room.xMm - start.xMm);
    relative.zMm = sign * (room.zMm - start.zMm);
    relative.yawHalfTurns = (room.yawHalfTurns + start.yawHalfTurns) % 2;
    return relative;
}

void moveSceneFileToRoom(nlohmann::json& sceneFile, const world::RoomPlacement& start) {
    if (start == world::RoomPlacement{}) {
        return;
    }
    const bool turned = start.yawHalfTurns % 2 != 0;
    if (auto objects = sceneFile.find("objects"); objects != sceneFile.end()) {
        for (Json& object : *objects) {
            if (object.contains("position")) {
                object["position"] = roomPoint(object["position"], start);
            }
            if (turned) {
                Json rotation = object.contains("rotationDeg") ? object["rotationDeg"] : Json::array({0.0, 0.0, 0.0});
                rotation[1] = roomYaw(rotation[1].get<double>(), start);
                object["rotationDeg"] = std::move(rotation);
            }
        }
    }
    if (auto lights = sceneFile.find("lights"); lights != sceneFile.end()) {
        for (Json& light : *lights) {
            if (light.contains("position")) {
                light["position"] = roomPoint(light["position"], start);
            }
            if (light.contains("target")) {
                light["target"] = roomPoint(light["target"], start);
            }
        }
    }
    if (auto areas = sceneFile.find("areas"); areas != sceneFile.end()) {
        for (Json& area : *areas) {
            for (Json& box : area["boxes"]) {
                const Json a = roomPoint(box["min"], start);
                const Json b = roomPoint(box["max"], start);
                Json min = Json::array();
                Json max = Json::array();
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    min.push_back(std::min(a[axis].get<double>(), b[axis].get<double>()));
                    max.push_back(std::max(a[axis].get<double>(), b[axis].get<double>()));
                }
                box["min"] = std::move(min);
                box["max"] = std::move(max);
            }
        }
    }
    if (auto camera = sceneFile.find("camera"); camera != sceneFile.end()) {
        if (camera->contains("position")) {
            (*camera)["position"] = roomPoint((*camera)["position"], start);
        }
        if (turned && camera->contains("yawDeg")) {
            (*camera)["yawDeg"] = roomYaw((*camera)["yawDeg"].get<double>(), start);
        }
    }
}

Expected<scene::SceneDescription> worldShellScene(const std::filesystem::path& assetRoot,
                                                  const std::optional<world::RoomId>& startRoom) {
    if (startRoom) {
        auto roomTemplate = world::loadRoomTemplate(assetRoot, world::kDefaultRoomTemplate);
        if (!roomTemplate) {
            return propagate(roomTemplate.error());
        }
        return world::shellSceneDescription(*startRoom, *roomTemplate);
    }
    // The lobby of a building of no room: the ground floor's hall, its elevator, its entrance.
    world::WorldPlan plan;
    plan.floors.push_back(world::Floor{std::string(world::kGroundFloor), 0});
    auto data = world::loadFloorSceneData(assetRoot, plan, world::kGroundFloor);
    if (!data) {
        return propagate(data.error());
    }
    return world::toFloorSceneDescription(plan, world::kGroundFloor, *data);
}

BuildingResult planBuilding(const WorldBuildInput& input, const core::JobContext& job) {
    const Clock::time_point start = Clock::now();
    BuildingResult result;
    WorldBuild& build = result.build;
    build.library = input.library;
    build.startRoom = input.startRoom;
    result.snapshotMs = input.snapshot.milliseconds;
    result.snapshotAt = input.snapshot.at;
    try {
        makePlan(input, job, build);
        if (job.cancelled()) {
            noPlan(build, world_notice::kFailed, "cancelled");
            return result;
        }
        if (build.plan) {
            // The media root of the snapshots (one for the library), known when every system's media were.
            for (const RoomSnapshotResult& system : input.snapshot.systems) {
                if (!system.error && system.mediaRoot) {
                    result.sources.mediaRootPath = system.mediaRoot;
                    break;
                }
            }
            if (input.snapshot.mediaKnown()) {
                result.sources.mediaRoot = result.sources.mediaRootPath;
            }
            // Each system's room items, kept for the floors' builds and the next start (written when they changed).
            const world::Room* startRoom = nullptr;
            if (input.startRoom) {
                for (const world::Room& room : build.plan->rooms) {
                    if (room.id == *input.startRoom) {
                        startRoom = &room;
                    }
                }
            }
            for (const RoomSnapshotResult& system : input.snapshot.systems) {
                if (system.error || roomHolding(*build.plan, system.system) == nullptr) {
                    continue;
                }
                RoomItems items = itemsOfSnapshot(system, *build.plan);
                const std::filesystem::path file = roomItemsFile(input.paths.cacheDir, input.library, system.system);
                if (auto written = writeRoomItems(file, items)) {
                    build.itemsWritten = build.itemsWritten || *written;
                } else {
                    build.itemsError = written.error();
                    spdlog::warn("World: the items of {} for the next start cannot be kept: {}", system.system.str(),
                                 written.error().message);
                }
                result.sources.items.emplace(system.system, std::move(items));
                // The start room's systems' snapshots: its hero CRT's games (room_screen.hpp).
                if (startRoom != nullptr) {
                    const std::vector<catalog::SystemName> held = systemsOf(*startRoom);
                    if (std::find(held.begin(), held.end(), system.system) != held.end()) {
                        result.sources.snapshots.emplace(system.system,
                                                         std::make_shared<const RoomSnapshotResult>(system));
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        noPlan(build, world_notice::kFailed, std::string("the building could not be made: ") + error.what());
    }
    build.milliseconds = millisecondsSince(start);
    return result;
}

void buildFloor(const WorldPaths& paths, WorldBuild& build, const FloorSources& sources) {
    if (!build.plan) {
        return;
    }
    const Clock::time_point start = Clock::now();
    try {
        const world::WorldPlan& plan = *build.plan;
        const std::optional<std::string> floor = startFloorOf(plan, build.startRoom);
        if (!floor) {
            build.startRoomMissing = true;
            spdlog::error("World: the building has no room {} (--start-room)", build.startRoom->toString());
            return;
        }
        build.floor = *floor;
        std::optional<world::RoomPlacement> startPlacement;
        if (build.startRoom) {
            for (const world::Room& room : plan.rooms) {
                if (room.id == *build.startRoom) {
                    startPlacement = room.placement;
                }
            }
        }
        const std::vector<const world::Room*> rooms = world::roomsOnFloor(plan, *floor);
        const std::map<catalog::SystemName, RoomItems> items = floorItems(paths, build, rooms, sources);
        world::PlatformTagsBySystem tags;
        for (const auto& [system, known] : items) {
            if (!known.platforms.empty()) {
                tags[system] = known.platforms;
            }
        }

        // The floor's static part (ADR-0013 §6): its rooms and its hall, moved into the scene's frame.
        const Clock::time_point sceneStart = Clock::now();
        auto data = world::loadFloorSceneData(paths.assetRoot, plan, *floor,
                                              world::roomLightingProfile(paths.assetRoot, paths.configDir), tags);
        if (!data) {
            spdlog::error("World: the floor {} cannot be made: {}", *floor, data.error().toString());
            dropFloor(build);
            noPlan(build, world_notice::kFailed, "the floor's data: " + data.error().message);
            return;
        }
        auto sceneBuild = world::toFloorSceneFile(plan, *floor, *data);
        if (!sceneBuild) {
            spdlog::error("World: the floor {} cannot be made: {}", *floor, sceneBuild.error().toString());
            dropFloor(build);
            noPlan(build, world_notice::kFailed, "the floor's scene: " + sceneBuild.error().message);
            return;
        }
        if (startPlacement) {
            moveSceneFileToRoom(sceneBuild->sceneFile, *startPlacement);
            // The start room's doorway camera, as its standalone scene has it (ADR-0013 §9: --start-room), seeing as
            // far as the floor's.
            const auto roomData = data->rooms.find(*build.startRoom);
            if (roomData != data->rooms.end()) {
                if (auto roomScene = world::toSceneFile(plan, *build.startRoom, roomData->second)) {
                    Json camera = roomScene->sceneFile["camera"];
                    const double floorFar = sceneBuild->sceneFile["camera"].value("far", 0.0);
                    if (camera.contains("far") && camera["far"].get<double>() < floorFar) {
                        camera["far"] = floorFar;
                    }
                    sceneBuild->sceneFile["camera"] = std::move(camera);
                }
            }
        }
        auto description = scene::parseSceneDescription(sceneBuild->sceneFile, "floor:" + *floor);
        if (!description) {
            spdlog::error("World: the floor {} cannot be read back: {}", *floor, description.error().toString());
            dropFloor(build);
            noPlan(build, world_notice::kFailed, "the floor's scene: " + description.error().message);
            return;
        }
        build.sceneDigest = core::sha256Hex(world::sceneFileText(sceneBuild->sceneFile));
        build.undressed = std::move(sceneBuild->undressed);
        for (const world::UndressedSpan& span : build.undressed) {
            spdlog::info("World scene: {} slot(s) of {} from slot {} left bare: {}", span.count, span.level.toString(),
                         span.firstSlot, span.reason);
        }
        std::set<std::tuple<std::string, std::string, std::string>> fallbacks;
        for (const auto& [id, roomData] : data->rooms) {
            for (const world::ScenePrefab& prefab : roomData.prefabs.prefabs) {
                if (prefab.source == assets::PrefabSource::Requested) {
                    continue;
                }
                ObjectFallback fallback{prefab.ref.manifest, std::string(assets::toString(prefab.source)),
                                        prefab.fallbackReason};
                if (fallbacks.emplace(fallback.prefab, fallback.source, fallback.reason).second) {
                    build.objectFallbacks.push_back(std::move(fallback));
                }
            }
        }
        build.sceneMs = millisecondsSince(sceneStart);

        // Each collection room's layout in the scene's frame and its cover requests (ADR-0011 §1).
        const Clock::time_point layoutStart = Clock::now();
        RoomItems merged;
        for (const auto& [system, known] : items) {
            merged.items.insert(merged.items.end(), known.items.begin(), known.items.end());
        }
        std::sort(merged.items.begin(), merged.items.end(),
                  [](const RoomItemText& a, const RoomItemText& b) { return a.game < b.game; });
        const std::vector<world::CollectionTitle> titles = collectionTitlesOf(merged, plan);
        for (const world::Room* room : rooms) {
            const auto roomData = data->rooms.find(room->id);
            if (roomData == data->rooms.end()) {
                continue;
            }
            auto layout = world::collectionLayout(plan, room->id, roomData->second, titles);
            if (!layout) {
                spdlog::error("World: the collection of {} cannot be laid out: {}; its room shows without it",
                              room->id.toString(), layout.error().toString());
                continue;
            }
            for (const world::SkippedPlacement& skipped : layout->skipped) {
                spdlog::warn("World collection: {} in {} not drawn: {}", skipped.placement.game.toString(),
                             skipped.placement.slot.toString(), skipped.reason);
            }
            RoomCollection collection;
            collection.room = room->id;
            collection.placement = room->placement;
            collection.scenePlacement =
                startPlacement ? relativePlacement(room->placement, *startPlacement) : room->placement;
            collection.layout = world::floorCollectionLayout(std::move(*layout), collection.scenePlacement);
            collection.doorway = doorwayBox(sceneBuild->hall, room->id, startPlacement);
            const std::vector<catalog::SystemName> held = systemsOf(*room);
            collection.covers.reserve(collection.layout.items.size());
            for (const world::CollectionItem& item : collection.layout.items) {
                const RoomItemText* text = nullptr;
                const RoomItems* source = nullptr;
                for (const catalog::SystemName& system : held) {
                    const auto known = items.find(system);
                    if (known != items.end()) {
                        if (const RoomItemText* found = known->second.find(item.game)) {
                            text = found;
                            source = &known->second;
                            break;
                        }
                    }
                }
                if (text == nullptr) {
                    continue; // nothing known of it: it keeps the pending picture
                }
                CoverItem cover;
                cover.game = item.game;
                cover.title = item.title;
                cover.seed = world::placementSeed(room->seed, item.game, room->mediaFamily);
                cover.frontKey = item.frontLayerKey;
                cover.spineKey = item.spineLayerKey;
                // The room's family's label shape (ADR-0013 §4): the 16-bit cartridges keep I3's fit.
                cover.label = roomData->second.mediaFamily.coverAspect();
                cover.systemName = source->systemName;
                cover.fallbackTitle = text->fallbackTitle;
                cover.year = text->year;
                cover.coverPath = text->coverPath;
                collection.covers.push_back(std::move(cover));
            }
            build.itemsRead += collection.covers.size();
            // The start room's hero CRT plays its systems' games (I3 step 10c; none for a preview).
            if (build.startRoom && room->id == *build.startRoom) {
                for (const catalog::SystemName& system : held) {
                    const auto snapshot = sources.snapshots.find(system);
                    if (snapshot != sources.snapshots.end() && snapshot->second) {
                        std::vector<ScreenGame> games = roomScreenGames(plan, collection.layout, *snapshot->second);
                        build.screenGames.insert(build.screenGames.end(), std::make_move_iterator(games.begin()),
                                                 std::make_move_iterator(games.end()));
                    }
                }
            }
            collection.sceneData = std::move(roomData->second);
            build.rooms.push_back(std::move(collection));
        }
        build.layoutMs = millisecondsSince(layoutStart);
        build.scene = std::move(*description);
        if (sources.mediaRoot) {
            build.mediaRoot = sources.mediaRoot;
        }
        build.mediaRootPath = sources.mediaRootPath;
    } catch (const std::exception& error) {
        dropFloor(build);
        noPlan(build, world_notice::kFailed, std::string("the floor could not be made: ") + error.what());
    }
    build.milliseconds += millisecondsSince(start);
}

WorldBuild buildWorld(const WorldBuildInput& input, const core::JobContext& job) {
    BuildingResult result = planBuilding(input, job);
    buildFloor(input.paths, result.build, result.sources);
    return std::move(result.build);
}

WorldBuild previewWorld(const WorldPaths& paths, const catalog::LibraryId& library, const world::StoredPlan& stored,
                        const std::optional<world::RoomId>& startRoom) {
    WorldBuild build;
    build.preview = true;
    build.library = library;
    build.startRoom = startRoom;
    keep(build, stored, world_notice::kNone, "the stored plan, shown before the catalog is read");
    buildFloor(paths, build, FloorSources{});
    if (!build.scene && !build.startRoomMissing) {
        spdlog::error("World: the stored plan's floor cannot be shown before the catalog is read: {}", build.reason);
    }
    return build;
}

// --- The flow -------------------------------------------------------------------------------------------------------

struct WorldFlow::Mailbox {
    std::mutex mutex;
    std::optional<StoredWorldPlan> stored;
    std::optional<WorldBuild> preview;
    std::optional<BuildingResult> building;
    std::optional<WorldBuild> floor;
    /// The generation the results were made under (dropped when it is no longer the flow's).
    std::uint64_t generation = 0;
    std::function<void()> wake;

    template <typename Fill>
    void post(std::uint64_t madeUnder, Fill&& fill) {
        std::function<void()> wakeHost;
        {
            const std::lock_guard lock(mutex);
            generation = madeUnder;
            fill(*this);
            wakeHost = wake;
        }
        if (wakeHost) {
            wakeHost();
        }
    }

    /// Takes `slot` when it was made under `current`.
    template <typename T>
    std::optional<T> take(std::optional<T>& slot, std::uint64_t current) {
        const std::lock_guard lock(mutex);
        if (generation != current) {
            return std::nullopt;
        }
        return std::exchange(slot, std::nullopt);
    }
};

WorldFlow::WorldFlow(Options options, core::WorkerPool& pool)
    : m_options(std::move(options)), m_pool(pool), m_mailbox(std::make_shared<Mailbox>()), m_start(Clock::now()) {
    m_mailbox->wake = m_options.wake;
}

WorldFlow::~WorldFlow() {
    // A job still running sees cancelled() and its result goes nowhere: it posts into the mailbox it shares.
    m_pool.cancel(m_generation);
    const std::lock_guard lock(m_mailbox->mutex);
    m_mailbox->wake = nullptr;
}

double WorldFlow::elapsedMs() const {
    return millisecondsSince(m_start);
}

void WorldFlow::finish(WorldResult result) {
    m_result = std::move(result);
    m_phase = WorldPhase::Ready;
}

void WorldFlow::fail(std::string reason) {
    WorldResult result;
    result.build = WorldBuild{};
    result.build->startRoom = m_options.startRoom;
    noPlan(*result.build, world_notice::kFailed, std::move(reason));
    finish(std::move(result));
}

std::optional<WorldResult> WorldFlow::takeResult() {
    if (m_phase != WorldPhase::Ready) {
        return std::nullopt;
    }
    m_phase = WorldPhase::Done;
    return std::exchange(m_result, std::nullopt);
}

std::optional<WorldBuild> WorldFlow::takePreview() {
    if (m_phase != WorldPhase::Previewing || !m_preview) {
        return std::nullopt;
    }
    m_previewTaken = true;
    return std::exchange(m_preview, std::nullopt);
}

void WorldFlow::submitBuilding(AppCatalog& catalog) {
    m_phase = WorldPhase::Building;
    m_buildingQueued = catalog.refreshState().running;
    if (m_buildingQueued) {
        return; // after the refresh's commit (update())
    }
    std::vector<catalog::GameId> placed;
    if (m_stored && m_stored->stored) {
        placed.reserve(m_stored->stored->plan.placements.size());
        for (const world::Placement& placement : m_stored->stored->plan.placements) {
            placed.push_back(placement.game);
        }
    }
    const std::uint64_t generation = m_generation.current();
    core::JobOptions jobOptions;
    jobOptions.priority = core::WorkerPool::kHighestPriority;
    jobOptions.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [mailbox = m_mailbox, paths = m_options.paths, library = m_library, reader = m_reader,
         writer = m_options.writer, startRoom = m_options.startRoom, placed = std::move(placed),
         generation](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            // The systems that get a room (ADR-0013 §2: classify(); excluded ones never), read in one transaction.
            const SystemFilter wanted = roomSystemsFilter(paths.assetRoot);
            WorldBuildInput input;
            input.paths = paths;
            input.library = library;
            input.writer = writer;
            input.startRoom = startRoom;
            {
                const std::lock_guard lock(reader->mutex);
                input.snapshot = takeBuildingSnapshot(*reader, placed, wanted);
            }
            spdlog::info("Building snapshot: {} system(s) in {:.1f} ms, media {}{}", input.snapshot.systems.size(),
                         input.snapshot.milliseconds, input.snapshot.mediaKnown() ? "known" : "unknown",
                         input.snapshot.error ? ", failed: " + input.snapshot.error->toString() : std::string());
            if (job.cancelled()) {
                return;
            }
            BuildingResult result = planBuilding(input, job);
            if (job.cancelled()) {
                return;
            }
            mailbox->post(generation, [&](Mailbox& box) { box.building = std::move(result); });
        },
        jobOptions);
    if (!submitted) {
        fail("the building's job: " + submitted.error().message);
        return;
    }
    m_phase = WorldPhase::Building;
}

void WorldFlow::submitFloor(BuildingResult result) {
    const std::uint64_t generation = m_generation.current();
    core::JobOptions jobOptions;
    jobOptions.priority = core::WorkerPool::kHighestPriority;
    jobOptions.generation = &m_generation;
    auto shared = std::make_shared<BuildingResult>(std::move(result));
    const Status submitted = m_pool.submit(
        [mailbox = m_mailbox, paths = m_options.paths, shared, generation](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            WorldBuild build = std::move(shared->build);
            buildFloor(paths, build, shared->sources);
            if (job.cancelled()) {
                return;
            }
            mailbox->post(generation, [&](Mailbox& box) { box.floor = std::move(build); });
        },
        jobOptions);
    if (!submitted) {
        fail("the floor's job: " + submitted.error().message);
        return;
    }
    m_phase = WorldPhase::Floor;
}

void WorldFlow::update(AppCatalog& catalog) {
    switch (m_phase) {
    case WorldPhase::WaitingForCatalog: {
        const CatalogState state = catalog.roomCatalogState();
        if (state == CatalogState::Opening && !catalog.workerEnded()) {
            return;
        }
        m_times.catalogMs = elapsedMs();
        if (state != CatalogState::Ready) {
            WorldResult result;
            result.testRoom = true;
            result.notice = std::string(world_notice::kNoCatalog);
            result.catalogState = std::string(toString(state));
            result.reason = fmt::format("the ES-DE catalog is not open ({})", toString(state));
            finish(std::move(result));
            return;
        }
        m_library = catalog.roomLibrary();
        m_reader = catalog.worldReader();
        if (!m_reader) {
            fail("the catalog has no reader for the building");
            return;
        }
        const std::uint64_t generation = m_generation.current();
        core::JobOptions jobOptions;
        jobOptions.priority = core::WorkerPool::kHighestPriority;
        jobOptions.generation = &m_generation;
        const Status submitted = m_pool.submit(
            [mailbox = m_mailbox, paths = m_options.paths, library = m_library,
             generation](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                StoredWorldPlan stored = readWorldPlan(paths, library);
                mailbox->post(generation, [&](Mailbox& box) { box.stored = std::move(stored); });
            },
            jobOptions);
        if (!submitted) {
            fail("the world's job: " + submitted.error().message);
            return;
        }
        m_phase = WorldPhase::ReadingPlan;
        return;
    }
    case WorldPhase::ReadingPlan: {
        std::optional<StoredWorldPlan> stored = m_mailbox->take(m_mailbox->stored, m_generation.current());
        if (!stored) {
            return;
        }
        m_times.planReadMs = elapsedMs();
        if (stored->error) {
            spdlog::warn("World: the stored plan cannot be read: {}", stored->error->toString());
        }
        m_stored = std::move(stored);
        if (m_stored->stored && m_options.preview) {
            // I3 step 10c: the stored plan's floor first; the building job once it has been taken.
            const std::uint64_t generation = m_generation.current();
            core::JobOptions jobOptions;
            jobOptions.priority = core::WorkerPool::kHighestPriority;
            jobOptions.generation = &m_generation;
            const Status submitted = m_pool.submit(
                [mailbox = m_mailbox, paths = m_options.paths, library = m_library, startRoom = m_options.startRoom,
                 stored = m_stored->stored, generation](const core::JobContext& job) {
                    if (job.cancelled()) {
                        return;
                    }
                    WorldBuild preview = previewWorld(paths, library, *stored, startRoom);
                    if (job.cancelled()) {
                        return;
                    }
                    mailbox->post(generation, [&](Mailbox& box) { box.preview = std::move(preview); });
                },
                jobOptions);
            if (submitted) {
                m_phase = WorldPhase::Previewing;
                return;
            }
            spdlog::warn("World: the stored plan's floor cannot be made before the building: {}",
                         submitted.error().message);
        }
        submitBuilding(catalog);
        return;
    }
    case WorldPhase::Previewing: {
        if (m_previewTaken) {
            submitBuilding(catalog);
            return;
        }
        if (m_preview) {
            return; // made, waiting to be taken
        }
        m_preview = m_mailbox->take(m_mailbox->preview, m_generation.current());
        if (m_preview) {
            m_times.previewMs = elapsedMs();
            m_times.previewJobMs = m_preview->milliseconds;
        }
        return;
    }
    case WorldPhase::Building: {
        if (m_buildingQueued) {
            if (catalog.refreshState().running) {
                return;
            }
            spdlog::info("World: the catalog's refresh ended; the building is planned from the catalog it made");
            submitBuilding(catalog);
            return;
        }
        // The job reads the catalog on its own connection: it answers also when the catalog's worker ended meanwhile.
        std::optional<BuildingResult> building = m_mailbox->take(m_mailbox->building, m_generation.current());
        if (!building) {
            return;
        }
        m_times.snapshotMs = elapsedMs();
        m_times.snapshotWorkerMs = building->snapshotMs;
        m_times.snapshotAt = building->snapshotAt;
        if (!building->build.plan) {
            WorldResult result;
            if (building->build.notice == world_notice::kNoGames) {
                // No system of the catalog gives a room and no plan is stored: the test room (I3's no-games).
                result.testRoom = true;
                result.notice = std::string(world_notice::kNoGames);
                result.reason = building->build.reason;
            } else {
                result.notice = building->build.notice;
                result.reason = building->build.reason;
                result.build = std::move(building->build);
            }
            m_times.builtMs = elapsedMs();
            finish(std::move(result));
            return;
        }
        submitFloor(std::move(*building));
        return;
    }
    case WorldPhase::Floor: {
        std::optional<WorldBuild> build = m_mailbox->take(m_mailbox->floor, m_generation.current());
        if (!build) {
            return;
        }
        m_times.builtMs = elapsedMs();
        WorldResult result;
        result.notice = build->notice;
        result.reason = build->reason;
        result.build = std::move(build);
        finish(std::move(result));
        return;
    }
    case WorldPhase::Ready:
    case WorldPhase::Done:
        return;
    }
}

} // namespace rv::app
