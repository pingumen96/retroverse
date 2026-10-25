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
#include <rv/catalog_db/catalog_stamp.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/core/version.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
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

/// A theme pack's label palette (sRGB "#RRGGBB", checked by its reader) as the label requests take it, one for its
/// room's items; a colour that does not parse is left out (an empty palette is the default one).
[[nodiscard]] RoomPalette labelPaletteOf(const world::ThemeMaterials& theme) {
    auto palette = std::make_shared<std::vector<assets::Rgb8>>();
    for (const std::string& hex : theme.labelPalette) {
        if (auto colour = core::parseHexColor(hex)) {
            const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0f)); };
            palette->push_back(assets::Rgb8{byte(colour->r), byte(colour->g), byte(colour->b)});
        }
    }
    return RoomPalette{std::move(palette)};
}

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

/// The room of `plan` that holds `system` in its sections (its console room, or the shared or arcade room it has a
/// section of: ADR-0013 §3), or nullptr.
[[nodiscard]] const world::Room* roomHolding(const world::WorldPlan& plan, const catalog::SystemName& system) {
    for (const world::Room& room : plan.rooms) {
        const std::vector<catalog::SystemName> systems = systemsOf(room);
        if (std::find(systems.begin(), systems.end(), system) != systems.end()) {
            return &room;
        }
    }
    return nullptr;
}

/// Whether `text` holds what its system's snapshot said of the game. A placed game the snapshot does not list has none
/// (snapshotRoomItems()): one no snapshot lists any more, and in a shared or arcade room every game of its other
/// sections, which each section's items file holds so.
[[nodiscard]] bool hasText(const RoomItemText& text) noexcept {
    return !text.title.empty() || !text.fallbackTitle.empty() || !text.year.empty() || !text.coverPath.empty() ||
           text.screen.has_value();
}

/// The text of `game` among the items of a room's `systems` (its sections, in order) and the items it is of: the first
/// section's that knows its text (hasText()), else the first that holds it at all (a game no snapshot lists any more
/// keeps its slot with no text); {nullptr, nullptr} when none holds it.
[[nodiscard]] std::pair<const RoomItemText*, const RoomItems*>
itemTextOf(const std::map<catalog::SystemName, RoomItems>& items, std::span<const catalog::SystemName> systems,
           const catalog::GameId& game) {
    std::pair<const RoomItemText*, const RoomItems*> first{nullptr, nullptr};
    for (const catalog::SystemName& system : systems) {
        const auto known = items.find(system);
        if (known == items.end()) {
            continue;
        }
        if (const RoomItemText* text = known->second.find(game)) {
            if (hasText(*text)) {
                return {text, &known->second};
            }
            if (first.first == nullptr) {
                first = {text, &known->second};
            }
        }
    }
    return first;
}

/// Every system's items of `items` in one list by GameId, each game once: the text its own system's snapshot gave it
/// when one did (hasText()).
[[nodiscard]] RoomItems mergedItems(const std::map<catalog::SystemName, RoomItems>& items) {
    RoomItems merged;
    for (const auto& [system, known] : items) {
        merged.items.insert(merged.items.end(), known.items.begin(), known.items.end());
    }
    std::stable_sort(merged.items.begin(), merged.items.end(), [](const RoomItemText& a, const RoomItemText& b) {
        if (a.game != b.game) {
            return a.game < b.game;
        }
        return hasText(a) && !hasText(b);
    });
    merged.items.erase(std::unique(merged.items.begin(), merged.items.end(),
                                   [](const RoomItemText& a, const RoomItemText& b) { return a.game == b.game; }),
                       merged.items.end());
    return merged;
}

} // namespace

RoomItems snapshotRoomItems(const RoomSnapshotResult& snapshot, const world::WorldPlan& plan) {
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
    // The rooms that hold the system (their sections: a shared room holds several systems, an expansion its head's).
    std::set<world::RoomId> own;
    for (const world::Room& room : plan.rooms) {
        const std::vector<catalog::SystemName> held = systemsOf(room);
        if (std::find(held.begin(), held.end(), snapshot.system) != held.end()) {
            own.insert(room.id);
        }
    }
    // The other versions of each placed title of those rooms (the CRT's priority data is gathered from them).
    std::map<catalog::GameId, std::vector<catalog::GameId>> versions;
    for (const world::UnplacedGame& unplaced : plan.unplaced) {
        if (unplaced.reason == world::UnplacedReason::DuplicateTitle && own.contains(unplaced.room)) {
            versions[unplaced.representative].push_back(unplaced.game);
        }
    }
    for (const world::Placement& placement : plan.placements) {
        const catalog::RoomEntry* entry = entryOf(snapshot.snapshot, placement.game);
        if (entry == nullptr && !own.contains(placement.slot.fixture().room())) {
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
            // What its room's hero CRT shows of it (I4 step 6b: the floor's CRTs from the items files).
            const auto others = versions.find(placement.game);
            text.screen = roomItemScreen(snapshot, placement.game,
                                         others != versions.end() ? std::span<const catalog::GameId>(others->second)
                                                                  : std::span<const catalog::GameId>());
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

namespace {

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
        // The update of the whole building, as `retroverse-cli worldplan update` makes it with no system named (I4 step
        // 6a): each system's selection goes to the room that holds its section (its console room, or the shared or
        // arcade room it has a section of: I4 step 4, sticky), also one without a placeable title, a shared or arcade
        // room updated with all its sections' selections (one lacking some is held as it is: the change report's
        // heldRooms), the rooms without a selection as they stand; every other system with a placeable title and data
        // of its own enters the building (its console room, or a section of its floor's shared room or of the arcade
        // room), the others left out (logged).
        const Clock::time_point dataStart = Clock::now();
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
        std::vector<world::RoomSelection> updated;
        for (world::RoomSelection& selection : selections) {
            const catalog::SystemName system = selection.room.system();
            if (roomHolding(stored->plan, system) != nullptr) {
                updated.push_back(std::move(selection));
                continue;
            }
            if (selection.titles.empty()) {
                spdlog::info("World: {} is left out of the building: no placeable title", system.str());
                continue;
            }
            world::RoomDataRequest request;
            request.system = system;
            if (const auto tags = platformTags.find(system); tags != platformTags.end()) {
                request.platformTags = tags->second;
            }
            auto data = world::loadWorldData(input.paths.assetRoot, request);
            if (!data) {
                spdlog::warn("World: {} is left out of the building: no data yet: {}", system.str(),
                             data.error().message);
                continue;
            }
            rooms.push_back(std::move(*data));
            updated.push_back(std::move(selection));
        }
        build.dataMs = millisecondsSince(dataStart);
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
    // A shared or arcade room some of whose sections had no selection (a system the catalog no longer lists) stands as
    // it was until every section has one (I4 step 4): said in the log.
    for (const world::KeptRoom& held : result->changes.heldRooms) {
        spdlog::warn("World: {} is held as it is: {}", held.room.toString(), held.reason);
    }
    // The rooms of cases and boxes migrated once to their families' successors (I6 step 4, ADR-0014 §2): said in the
    // log and by a notice over the floor (the next update migrates nothing, so it shows once).
    for (const world::MigratedRoom& migrated : result->changes.migratedRooms) {
        spdlog::info("World: {} is rearranged once, from the media family {} to {} (its cases or boxes now stand on "
                     "their side, the spine out)",
                     migrated.room.toString(), migrated.from, migrated.to);
        if (migrated.unplacedForCapacity > 0) {
            spdlog::warn("World: {} holds no slot for {} of its titles after its rearrangement (unplaced for capacity)",
                         migrated.room.toString(), migrated.unplacedForCapacity);
        }
    }
    // What the update left undone because it would have moved something pinned (a pinned room is not rearranged).
    for (const std::string& refused : result->changes.needsConfirmation) {
        spdlog::warn("World: left as it stands, it needs confirmation: {}", refused);
    }
    if (!result->changes.migratedRooms.empty() && build.notice.empty()) {
        build.notice = std::string(world_notice::kRoomsMigrated);
    }
    // The systems that entered the building (I4 step 6a: a room of their own, or a section of a shared or arcade room),
    // and those that wait for a held room (they enter with the update that has every section's selection).
    for (const world::SystemRoom& entered : result->changes.systemsEntered) {
        const bool opened = std::find(result->changes.roomsAdded.begin(), result->changes.roomsAdded.end(),
                                      entered.room) != result->changes.roomsAdded.end();
        spdlog::info("World: {} enters the building in {} ({})", entered.system.str(), entered.room.toString(),
                     opened ? "a new room" : "a new section");
    }
    for (const world::SystemRoom& waiting : result->changes.systemsWaiting) {
        spdlog::warn("World: {} waits to enter {}, held (a section of it has no selection)", waiting.system.str(),
                     waiting.room.toString());
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

/// The elevator's proximity zone of `hall` as a box from the floor to the doors' top, in the frame of the room standing
/// at `start` (none: the floor's).
[[nodiscard]] core::Aabb elevatorZoneBox(const world::Hall& hall, const std::optional<world::RoomPlacement>& start) {
    const world::Footprint& zone = hall.elevatorZone;
    glm::vec3 a(static_cast<float>(meters(zone.minXMm)), 0.0f, static_cast<float>(meters(zone.minZMm)));
    glm::vec3 b(static_cast<float>(meters(zone.maxXMm)), static_cast<float>(meters(world::kElevatorDoorHeightMm)),
                static_cast<float>(meters(zone.maxZMm)));
    if (start) {
        const float sign = start->yawHalfTurns % 2 == 0 ? 1.0f : -1.0f;
        const glm::vec3 origin(static_cast<float>(meters(start->xMm)), 0.0f, static_cast<float>(meters(start->zMm)));
        a = glm::vec3(sign * (a.x - origin.x), a.y, sign * (a.z - origin.z));
        b = glm::vec3(sign * (b.x - origin.x), b.y, sign * (b.z - origin.z));
    }
    return core::Aabb{glm::min(a, b), glm::max(a, b)};
}

/// The middle of the elevator's doors of `hall` on the floor (its frame on the west wall), in the frame of the room
/// standing at `start` (none: the floor's).
[[nodiscard]] glm::vec3 elevatorDoorsPoint(const world::Hall& hall, const std::optional<world::RoomPlacement>& start) {
    const world::Footprint& frame = hall.elevator;
    glm::vec3 point(static_cast<float>(meters(frame.minXMm + frame.maxXMm) / 2.0), 0.0f,
                    static_cast<float>(meters(frame.minZMm + frame.maxZMm) / 2.0));
    if (start) {
        const float sign = start->yawHalfTurns % 2 == 0 ? 1.0f : -1.0f;
        const glm::vec3 origin(static_cast<float>(meters(start->xMm)), 0.0f, static_cast<float>(meters(start->zMm)));
        point = glm::vec3(sign * (point.x - origin.x), 0.0f, sign * (point.z - origin.z));
    }
    return point;
}

/// A scene file's camera (its position, yaw and pitch; the lens left as the scene's default).
[[nodiscard]] scene::CameraSetup cameraOf(const Json& camera) {
    scene::CameraSetup setup;
    if (const auto position = camera.find("position"); position != camera.end() && position->is_array() &&
                                                         position->size() == 3) {
        setup.position = glm::vec3((*position)[0].get<float>(), (*position)[1].get<float>(), (*position)[2].get<float>());
    }
    setup.yawDeg = camera.value("yawDeg", 0.0f);
    setup.pitchDeg = camera.value("pitchDeg", 0.0f);
    return setup;
}

/// The doorway camera of `room` (world::doorwayCamera(), its own frame) in the scene's frame, where the room stands at
/// `scenePlacement`: its position moved, its yaw turned with the room (within (-180, 180]).
[[nodiscard]] scene::CameraSetup doorwayCameraIn(const world::Room& room, const world::RoomTemplate& roomTemplate,
                                                 const world::RoomPlacement& scenePlacement) {
    const world::DoorwayCamera camera = world::doorwayCamera(room, roomTemplate);
    const glm::vec4 position = world::roomToFloorMatrix(scenePlacement) *
                               glm::vec4(static_cast<float>(camera.x), static_cast<float>(camera.eyeY),
                                         static_cast<float>(camera.z), 1.0f);
    double yaw = camera.yawDeg + (scenePlacement.yawHalfTurns % 2 != 0 ? 180.0 : 0.0);
    while (yaw > 180.0) {
        yaw -= 360.0;
    }
    while (yaw <= -180.0) {
        yaw += 360.0;
    }
    scene::CameraSetup setup;
    setup.position = glm::vec3(position);
    setup.yawDeg = static_cast<float>(yaw);
    setup.pitchDeg = static_cast<float>(camera.pitchDeg);
    return setup;
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
            // The CRTs' games are read under the media root while it is known (ADR-0010 §3).
            result.sources.screenMediaRoot = result.sources.mediaRoot;
            // Each system's room items, kept for the floors' builds and the next start (written when they changed):
            // their text, their covers and their CRT's data (I4 step 6b).
            const Clock::time_point itemsStart = Clock::now();
            for (const RoomSnapshotResult& system : input.snapshot.systems) {
                if (system.error || roomHolding(*build.plan, system.system) == nullptr) {
                    continue;
                }
                RoomItems items = snapshotRoomItems(system, *build.plan);
                const std::filesystem::path file = roomItemsFile(input.paths.cacheDir, input.library, system.system);
                if (auto written = writeRoomItems(file, items)) {
                    build.itemsWritten = build.itemsWritten || *written;
                } else {
                    build.itemsError = written.error();
                    spdlog::warn("World: the items of {} for the next start cannot be kept: {}", system.system.str(),
                                 written.error().message);
                }
                result.sources.items.emplace(system.system, std::move(items));
            }
            build.itemsMs = millisecondsSince(itemsStart);
            // The building stamp (I4 step 6b): what this plan was made from, for the next start's shortcut. Only for a
            // plan saved (or unchanged) from known media, every items file kept, the catalog's stamp read, and nothing
            // that waits for another update (a room kept until a regeneration, a room held, a system waiting to enter
            // one: their notice and warnings come with the whole job, at every start until they are settled).
            const bool planned = build.outcome == WorldOutcome::Generated || build.outcome == WorldOutcome::Updated ||
                                 build.outcome == WorldOutcome::Unchanged;
            const bool settled = !build.changes || (build.changes->keptRooms.empty() &&
                                                    build.changes->heldRooms.empty() &&
                                                    build.changes->systemsWaiting.empty());
            const std::filesystem::path stampFile = buildingStampFile(input.paths.cacheDir, input.library);
            if (planned && settled && build.notice.empty() && !build.saveError && !build.itemsError &&
                result.sources.mediaRoot && !input.snapshot.catalogStamp.empty()) {
                BuildingStamp stamp;
                stamp.planDigest = build.planDigest;
                stamp.catalog = core::sha256Hex(input.snapshot.catalogStamp);
                stamp.inputs = worldInputsDigest(input.paths, input.library, *result.sources.mediaRoot);
                stamp.softFindings = build.softFindings;
                if (auto written = writeBuildingStamp(stampFile, stamp); !written) {
                    spdlog::warn("World: the building stamp cannot be kept: {}", written.error().toString());
                }
            } else {
                removeBuildingStamp(stampFile);
            }
        } else if (!job.cancelled()) {
            removeBuildingStamp(buildingStampFile(input.paths.cacheDir, input.library));
        }
    } catch (const std::exception& error) {
        noPlan(build, world_notice::kFailed, std::string("the building could not be made: ") + error.what());
    }
    build.milliseconds = millisecondsSince(start);
    spdlog::info(
        "World: the building job in {:.0f} ms ({}): lock and plan {:.0f}, selections {:.0f}, rooms' data {:.0f}, "
        "plan {:.0f}, save {:.0f}, items {:.0f}",
        build.milliseconds, toString(build.outcome), build.lockMs, build.selectMs, build.dataMs, build.planMs,
        build.saveMs, build.itemsMs);
    return result;
}

namespace {

using Json = nlohmann::json;

/// The largest building stamp read back.
constexpr std::uintmax_t kMaxStampBytes = 64 * 1024;
/// The largest input file hashed (the asset root's world data and manifests are a few kilobytes each).
constexpr std::uintmax_t kMaxInputBytes = 16ull * 1024ull * 1024ull;

/// "<name> <sha256 of its bytes>" for one input file, "<name> absent" for none.
[[nodiscard]] std::string inputLine(std::string_view name, const std::filesystem::path& file) {
    auto bytes = core::readFileShared(file, kMaxInputBytes);
    return fmt::format("{} {}\n", name, bytes ? core::sha256Hex(*bytes) : std::string("absent"));
}

/// The program as built, read once: its version banner and its executable's name, size and last write time, so that
/// a build of the same version whose code makes other plans or items never finds its predecessor's stamp valid.
[[nodiscard]] const std::string& programIdentity() {
    static const std::string identity = [] {
        const std::filesystem::path executable = core::executablePath();
        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(executable, sizeError);
        std::error_code timeError;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time(executable, timeError);
        return fmt::format("{} {} {} {}", core::versionBanner(), core::pathToUtf8Lossy(executable.filename()),
                           sizeError ? std::string("unknown") : std::to_string(size),
                           timeError ? std::string("unknown") : std::to_string(written.time_since_epoch().count()));
    }();
    return identity;
}

} // namespace

void removeBuildingStamp(const std::filesystem::path& file) {
    std::error_code error;
    if (std::filesystem::remove(file, error)) {
        spdlog::info("World: the building stamp is removed: the next start plans the building again");
    } else if (error) {
        spdlog::warn("World: the building stamp cannot be removed: {}", error.message());
    }
}

std::filesystem::path buildingStampFile(const std::filesystem::path& cacheDir, const catalog::LibraryId& library) {
    return cacheDir / "world" / core::pathFromUtf8(library.str()) / "building-stamp.json";
}

Expected<BuildingStamp> readBuildingStamp(const std::filesystem::path& file) {
    auto text = core::readTextFile(file, kMaxStampBytes);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(file);
    auto document = core::parseJson(*text, context);
    if (!document) {
        return propagate(document.error());
    }
    const Json& json = *document;
    if (!json.is_object() || !json.contains("schemaVersion") || !json["schemaVersion"].is_number_integer()) {
        return makeError(ErrorCode::ParseError, "not a building stamp", context);
    }
    if (json["schemaVersion"].get<std::int64_t>() != kBuildingStampVersion) {
        return makeError(ErrorCode::Unsupported, "another schema of the building stamp", context);
    }
    BuildingStamp stamp;
    for (const auto& [key, out] : {std::pair<const char*, std::string*>{"planDigest", &stamp.planDigest},
                                   std::pair<const char*, std::string*>{"catalog", &stamp.catalog},
                                   std::pair<const char*, std::string*>{"inputs", &stamp.inputs}}) {
        if (!json.contains(key) || !json[key].is_string()) {
            return makeError(ErrorCode::ParseError, fmt::format("the building stamp has no {}", key), context);
        }
        *out = json[key].get<std::string>();
    }
    if (!json.contains("softFindings") || !json["softFindings"].is_number_unsigned()) {
        return makeError(ErrorCode::ParseError, "the building stamp has no softFindings", context);
    }
    stamp.softFindings = json["softFindings"].get<std::size_t>();
    return stamp;
}

Expected<bool> writeBuildingStamp(const std::filesystem::path& file, const BuildingStamp& stamp) {
    auto text = core::writeCanonicalJson(Json{{"schemaVersion", kBuildingStampVersion},
                                              {"planDigest", stamp.planDigest},
                                              {"catalog", stamp.catalog},
                                              {"inputs", stamp.inputs},
                                              {"softFindings", stamp.softFindings}});
    if (!text) {
        return propagate(text.error());
    }
    if (auto existing = core::readTextFile(file, kMaxStampBytes); existing && *existing == *text) {
        return false;
    }
    if (Status written = core::writeTextFileAtomic(file, *text); !written) {
        return propagate(written.error());
    }
    return true;
}

std::string worldInputsDigest(const WorldPaths& paths, const catalog::LibraryId& library,
                              const std::filesystem::path& mediaRoot) {
    std::string text =
        fmt::format("building-stamp/{}\nprogram {}\ngenerator {}\nitems {}\nmedia {}\n", kBuildingStampVersion,
                    programIdentity(), world::kGeneratorVersion.toString(), kRoomItemsSchemaVersion,
                    core::pathToUtf8Lossy(mediaRoot));
    // The asset root's world data and prefab manifests, file by file in name order.
    for (const std::string_view folder : {"world", "manifests"}) {
        std::vector<std::pair<std::string, std::filesystem::path>> files;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(paths.assetRoot / folder, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) {
                files.emplace_back(core::pathToUtf8Lossy(std::filesystem::relative(it->path(), paths.assetRoot)),
                                   it->path());
            }
        }
        if (error) {
            text += fmt::format("{} unreadable\n", folder);
        }
        std::sort(files.begin(), files.end());
        for (const auto& [name, file] : files) {
            text += inputLine(name, file);
        }
    }
    text += inputLine("world.json", paths.configDir / "world.json");
    text += inputLine("library.json", paths.configDir / "library.json");
    text += inputLine("overrides.json", world::worldFiles(paths.userDataRoot, library).overrides);
    return core::sha256Hex(text);
}

std::optional<BuildingResult> unchangedBuilding(const WorldPaths& paths, const catalog::LibraryProfile& profile,
                                                const world::StoredPlan& stored, const std::string& catalogStamp,
                                                const std::optional<world::RoomId>& startRoom) {
    const Clock::time_point start = Clock::now();
    if (catalogStamp.empty()) {
        return std::nullopt;
    }
    auto stamp = readBuildingStamp(buildingStampFile(paths.cacheDir, profile.id));
    if (!stamp) {
        if (stamp.error().code != ErrorCode::NotFound) {
            spdlog::info("World: the building stamp is not used: {}", stamp.error().toString());
        }
        return std::nullopt;
    }
    const auto miss = [](std::string_view why) {
        spdlog::info("World: the building is planned again: {}", why);
        return std::nullopt;
    };
    if (stamp->planDigest != stored.digest) {
        return miss("the stored plan is not the one the last building job left");
    }
    if (stamp->catalog != core::sha256Hex(catalogStamp)) {
        return miss("the catalog changed since (an import, a play session, an identity)");
    }
    // The plan file as it was read (another program may have written it since).
    const world::WorldFiles files = world::worldFiles(paths.userDataRoot, profile.id);
    auto text = core::readTextFile(files.plan, kMaxInputBytes * 8);
    if (!text || core::sha256Hex(*text) != stored.digest) {
        return miss("the plan's file changed");
    }
    const std::optional<std::filesystem::path> mediaRoot = readableMediaRoot(profile);
    if (!mediaRoot) {
        return miss("the media root cannot be read now");
    }
    if (stamp->inputs != worldInputsDigest(paths, profile.id, *mediaRoot)) {
        return miss("the world's data, configuration or media root changed");
    }
    for (const world::Room& room : stored.plan.rooms) {
        for (const catalog::SystemName& system : systemsOf(room)) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(roomItemsFile(paths.cacheDir, profile.id, system), error)) {
                return miss("a room's items file is missing");
            }
        }
    }
    BuildingResult result;
    WorldBuild& build = result.build;
    build.library = profile.id;
    build.startRoom = startRoom;
    build.outcome = WorldOutcome::Unchanged;
    build.plan = stored.plan;
    build.planDigest = stored.digest;
    build.softFindings = stamp->softFindings;
    build.reason = "the catalog and the world's inputs as the stored plan was made from";
    build.skipped = true;
    result.sources.mediaRoot = mediaRoot;
    result.sources.mediaRootPath = mediaRoot;
    result.sources.screenMediaRoot = mediaRoot;
    build.milliseconds = millisecondsSince(start);
    spdlog::info("World: the building as stored ({:.1f} ms): {}", build.milliseconds, build.reason);
    return result;
}

void buildFloor(const WorldPaths& paths, WorldBuild& build, const FloorSources& sources) {
    if (!build.plan) {
        return;
    }
    const Clock::time_point start = Clock::now();
    try {
        const world::WorldPlan& plan = *build.plan;
        const std::optional<std::string> startFloor = startFloorOf(plan, build.startRoom);
        if (!startFloor && !build.requestedFloor) {
            build.startRoomMissing = true;
            spdlog::error("World: the building has no room {} (--start-room)", build.startRoom->toString());
            return;
        }
        // I4 step 7b: a ride's destination (or the floor the walker is on), else the start room's.
        const std::optional<std::string> floor = build.requestedFloor ? build.requestedFloor : startFloor;
        if (std::none_of(plan.floors.begin(), plan.floors.end(),
                         [&](const world::Floor& known) { return known.id == *floor; })) {
            spdlog::error("World: the building has no floor {}", *floor);
            dropFloor(build);
            noPlan(build, world_notice::kFailed, "the building has no floor " + *floor);
            return;
        }
        build.floor = *floor;
        // The scene's frame: the start room's on its own floor, the floor's on any other.
        std::optional<world::RoomPlacement> startPlacement;
        if (build.startRoom && startFloor == floor) {
            for (const world::Room& room : plan.rooms) {
                if (room.id == *build.startRoom) {
                    startPlacement = room.placement;
                }
            }
        }
        build.floorPlacement =
            startPlacement ? relativePlacement(world::RoomPlacement{}, *startPlacement) : world::RoomPlacement{};
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
        }
        // I4 step 7b: the elevator's zone and the arrival camera (the floor scene's camera, moved with it), in the
        // scene's frame.
        build.elevatorZone = elevatorZoneBox(sceneBuild->hall, startPlacement);
        build.elevatorDoors = elevatorDoorsPoint(sceneBuild->hall, startPlacement);
        build.arrival = cameraOf(sceneBuild->sceneFile["camera"]);
        if (startPlacement) {
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

        // Each collection room's layout in the scene's frame, its cover requests (ADR-0011 §1), its content digest and
        // its CRT's games (I4 step 6b).
        const Clock::time_point layoutStart = Clock::now();
        const std::vector<world::CollectionTitle> titles = collectionTitlesOf(mergedItems(items), plan);
        std::map<world::RoomId, std::string> digests;
        if (auto computed = world::roomContentDigests(plan)) {
            digests = std::move(*computed);
        } else {
            spdlog::warn("World: the rooms' content digests: {}; every room is compared whole",
                         computed.error().toString());
        }
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
            collection.doorwayCamera =
                doorwayCameraIn(*room, roomData->second.roomTemplate, collection.scenePlacement);
            const std::vector<catalog::SystemName> held = systemsOf(*room);
            // The room's pack's label palette (I6 step 3) for its generated spines and labels.
            const RoomPalette palette = labelPaletteOf(roomData->second.theme);
            collection.covers.reserve(collection.layout.items.size());
            for (const world::CollectionItem& item : collection.layout.items) {
                // A shared or arcade room's game: its own section's text (each section's items hold the others' games
                // with none).
                const auto [text, source] = itemTextOf(items, held, item.game);
                if (text == nullptr) {
                    continue; // nothing known of it: it keeps the pending picture
                }
                CoverItem cover;
                cover.game = item.game;
                cover.title = item.title;
                cover.seed = world::placementSeed(room->seed, item.game, room->mediaFamily);
                cover.palette = palette;
                cover.frontKey = item.frontLayerKey;
                cover.spineKey = item.spineLayerKey;
                // The room's family's label shape (ADR-0013 §4): the 16-bit cartridges keep I3's fit.
                cover.label = roomData->second.mediaFamily.coverAspect();
                // Its family's spine label shape (I6 step 4's review): the printed spine of a case or box on its side.
                cover.spine = roomData->second.mediaFamily.spineAspect();
                cover.systemName = source->systemName;
                cover.fallbackTitle = text->fallbackTitle;
                cover.year = text->year;
                cover.coverPath = text->coverPath;
                collection.covers.push_back(std::move(cover));
            }
            build.itemsRead += collection.covers.size();
            if (const auto digest = digests.find(room->id); digest != digests.end()) {
                collection.contentDigest = digest->second;
            }
            // Its hero CRT plays its placed games (I3 step 10c; ADR-0013 §9): a shared or arcade room's, its sections'
            // in the room's featured order, each game's data from its own section's items.
            if (sources.screenMediaRoot) {
                collection.screenGames = roomScreenGames(
                    collection.layout,
                    [&](const catalog::GameId& game) { return itemTextOf(items, held, game).first; },
                    *sources.screenMediaRoot);
            }
            if (sources.lastPlayed) {
                // The catalog's sessions now, as the building's items will hold them (a game played since they were
                // written: the CRT's start choice is the building's).
                for (ScreenGame& game : collection.screenGames) {
                    const auto id = catalog::GameId::parse(game.id);
                    const auto played = id ? sources.lastPlayed->find(*id) : sources.lastPlayed->end();
                    game.lastPlayed = played != sources.lastPlayed->end() ? std::optional<std::int64_t>(played->second)
                                                                          : std::nullopt;
                }
            }
            if (build.startRoom && room->id == *build.startRoom) {
                build.screenGames = collection.screenGames;
            }
            collection.sceneData = std::move(roomData->second);
            build.rooms.push_back(std::move(collection));
        }
        build.layoutMs = millisecondsSince(layoutStart);
        build.floorMs = millisecondsSince(start);
        build.scene = std::move(*description);
        if (sources.mediaRoot) {
            build.mediaRoot = sources.mediaRoot;
        }
        build.mediaRootPath = sources.mediaRootPath;
        build.screenMediaRoot = sources.screenMediaRoot;
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
                        const std::optional<world::RoomId>& startRoom,
                        const std::optional<std::filesystem::path>& screenMediaRoot,
                        std::optional<std::map<catalog::GameId, std::int64_t>> lastPlayed) {
    WorldBuild build;
    build.preview = true;
    build.library = library;
    build.startRoom = startRoom;
    keep(build, stored, world_notice::kNone, "the stored plan, shown before the catalog is read");
    FloorSources sources;
    sources.screenMediaRoot = screenMediaRoot;
    sources.lastPlayed = std::move(lastPlayed);
    buildFloor(paths, build, sources);
    if (!build.scene && !build.startRoomMissing) {
        spdlog::error("World: the stored plan's floor cannot be shown before the catalog is read: {}", build.reason);
    }
    return build;
}

std::optional<std::filesystem::path> readableMediaRoot(const catalog::LibraryProfile& profile) {
    const auto settings = esde::readSettings(profile.esdeHome);
    const esde::ResolvedRoot root = esde::resolveMediaRoot(settings ? *settings : esde::EsdeSettings{}, profile);
    if (!root.path) {
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::path(core::extendedLengthPath(*root.path)), error) || error) {
        return std::nullopt;
    }
    return root.path;
}

// --- The flow -------------------------------------------------------------------------------------------------------

std::optional<std::map<catalog::GameId, std::int64_t>> catalogLastPlayed(WorldCatalogReader& reader) {
    const std::lock_guard lock(reader.mutex);
    if (!reader.query) {
        return std::nullopt;
    }
    auto sessions = catalog_db::lastPlaySessions(reader.query->connection());
    if (!sessions) {
        spdlog::warn("World: the play sessions cannot be read for the floor's CRTs: {}", sessions.error().toString());
        return std::nullopt;
    }
    std::map<catalog::GameId, std::int64_t> seconds;
    for (const auto& [game, started] : *sessions) {
        seconds.emplace(game, std::chrono::duration_cast<std::chrono::seconds>(started.time_since_epoch()).count());
    }
    return seconds;
}

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
    std::shared_ptr<const world::StoredPlan> stored;
    if (m_stored && m_stored->stored && m_options.preview) {
        stored = std::make_shared<const world::StoredPlan>(*m_stored->stored);
    }
    const Status submitted = m_pool.submit(
        [mailbox = m_mailbox, paths = m_options.paths, library = m_library, reader = m_reader,
         writer = m_options.writer, startRoom = m_options.startRoom, placed = std::move(placed), stored,
         generation](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            // I4 step 6b: an unchanged start plans nothing (the building stamp), before any snapshot is read.
            if (stored) {
                std::string catalogStamp;
                {
                    const std::lock_guard lock(reader->mutex);
                    if (reader->query) {
                        if (auto read = catalog_db::catalogStamp(reader->query->connection(), library)) {
                            catalogStamp = std::move(*read);
                        }
                    }
                }
                if (std::optional<BuildingResult> unchanged =
                        unchangedBuilding(paths, reader->profile, *stored, catalogStamp, startRoom)) {
                    mailbox->post(generation, [&](Mailbox& box) { box.building = std::move(*unchanged); });
                    return;
                }
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
    // I4 step 7b: the floor the walker is on now (a ride may have taken it off the start room's floor).
    if (m_options.floor) {
        if (std::optional<std::string> floor = m_options.floor()) {
            result.build.requestedFloor = std::move(floor);
        }
    }
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
                 stored = m_stored->stored, profile = m_reader->profile, reader = m_reader,
                 generation](const core::JobContext& job) {
                    if (job.cancelled()) {
                        return;
                    }
                    // The rooms' CRTs play their games from the start (I4 step 6b), read under the library's media
                    // root when it can be read now, each game's last play the catalog's (the CRT starts on the game
                    // the building's result chooses); the covers come from the cache alone all the same.
                    WorldBuild preview = previewWorld(paths, library, *stored, startRoom, readableMediaRoot(profile),
                                                      catalogLastPlayed(*reader));
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
