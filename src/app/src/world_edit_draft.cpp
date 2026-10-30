#include <rv/app/world_edit_draft.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_snapshots.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The snapshot's scope: the one room the operations changed, else how many, else "pins".
[[nodiscard]] std::string scopeOf(std::span<const world::EditResult> results) {
    std::set<world::RoomId> rooms;
    for (const world::EditResult& result : results) {
        rooms.insert(result.rooms.begin(), result.rooms.end());
    }
    if (rooms.size() == 1) {
        return rooms.begin()->toString();
    }
    return rooms.empty() ? std::string("pins") : fmt::format("{} rooms", rooms.size());
}

[[nodiscard]] const world::Room* findRoom(const world::WorldPlan& plan, const world::RoomId& id) noexcept {
    for (const world::Room& room : plan.rooms) {
        if (room.id == id) {
            return &room;
        }
    }
    return nullptr;
}

/// A room's data (world::loadRoomData()) reads its kind, floor, recorded media family, template and first section: the
/// same in both, the data read for one serves the other.
[[nodiscard]] bool sameRoomData(const world::Room& a, const world::Room& b) noexcept {
    const auto first = [](const world::Room& room) {
        return room.sections.empty() ? std::optional<catalog::SystemName>() : std::optional(room.sections.front());
    };
    return a.kind == b.kind && a.floor == b.floor && a.mediaFamily == b.mediaFamily && a.templateId == b.templateId &&
           first(a) == first(b);
}

} // namespace

EditDraft::EditDraft(EditBase base, std::shared_ptr<const world::EditData> data)
    : m_base(std::move(base)), m_data(std::move(data)) {}

Expected<world::EditResult> EditDraft::apply(const world::EditOperation& operation,
                                             const world::PinProvenance& provenance) {
    if (!m_data) {
        return makeError(ErrorCode::InvalidArgument, "the draft has no edit data");
    }
    world::EditedWorld edited = current();
    auto result = world::applyEdit(edited, operation, *m_data, provenance);
    if (!result) {
        return propagate(result.error());
    }
    if (!result->refused && result->changed) {
        m_steps.push_back(Step{operation, *result, std::move(edited)});
    }
    return result;
}

void EditDraft::adopt(const world::EditOperation& operation, const world::EditResult& result,
                      world::EditedWorld world) {
    if (result.refused || !result.changed) {
        return;
    }
    m_steps.push_back(Step{operation, result, std::move(world)});
}

bool EditDraft::undo() {
    if (m_steps.empty()) {
        return false;
    }
    m_steps.pop_back();
    return true;
}

std::vector<world::EditOperation> EditDraft::operations() const {
    std::vector<world::EditOperation> operations;
    operations.reserve(m_steps.size());
    for (const Step& step : m_steps) {
        operations.push_back(step.operation);
    }
    return operations;
}

std::vector<world::EditResult> EditDraft::results() const {
    std::vector<world::EditResult> results;
    results.reserve(m_steps.size());
    for (const Step& step : m_steps) {
        results.push_back(step.result);
    }
    return results;
}

bool EditDraft::pinned(const catalog::GameId& game) const noexcept {
    const std::vector<world::GamePin>& pins = current().overrides.gamePins;
    return std::any_of(pins.begin(), pins.end(), [&](const world::GamePin& pin) { return pin.game == game; });
}

Expected<EditSaveOutcome> saveDraft(const EditDraft& draft, const EditSaveOptions& options) {
    const Clock::time_point start = Clock::now();
    const world::WorldFiles files = world::worldFiles(options.paths.userDataRoot, options.library);
    // The plan and the overrides are read under the world's lock (which rolls back an interrupted action first), so
    // that nothing comes between them and the commit.
    auto lock = world::WorldLock::acquire(files, options.lockWait);
    if (!lock) {
        return propagate(lock.error());
    }
    if (lock->rolledBack()) {
        spdlog::warn("Editor: an interrupted action was rolled back first: the snapshot {} restored",
                     *lock->rolledBack());
    }
    // The plan as stored: when its text is still the one the session began from (its digest the base's), the base's plan,
    // which was read from it (parsing it again costs about 0.4 s on the reference library's 19,985 placements); else
    // read again.
    StoredWorldPlan stored;
    if (auto text = core::readTextFile(files.plan); text && core::sha256Hex(*text) == draft.base().planDigest) {
        stored.library = options.library;
        stored.stored = world::StoredPlan{draft.base().world.plan, draft.base().planDigest};
    } else {
        stored = readWorldPlan(options.paths, options.library);
    }
    if (stored.error) {
        return propagate(*stored.error);
    }
    if (!stored.stored) {
        return makeError(ErrorCode::NotFound, "no plan is stored", files.plan.string());
    }
    auto overrides = world::loadOverrides(files.overrides);
    if (!overrides) {
        return propagate(overrides.error());
    }

    EditSaveOutcome outcome;
    outcome.readMs = millisecondsSince(start);
    Clock::time_point part = Clock::now();
    const std::vector<world::EditOperation> operations = draft.operations();
    std::vector<world::EditResult> results;
    world::EditedWorld edited;
    if (stored.stored->digest == draft.base().planDigest && *overrides == draft.base().world.overrides) {
        // Nothing changed since the session began: the draft as it is.
        edited = draft.current();
        results = draft.results();
    } else {
        // Replayed onto the world as it is now (ADR-0015 §4): the data of every room the replay touches, read again for
        // a room the stored plan holds otherwise than the session's base did (another writer regenerated or migrated
        // it: its family, template, kind, floor or first section).
        outcome.replayed = true;
        world::EditData data = *draft.data();
        for (const world::RoomId& id : world::editRooms(stored.stored->plan, operations)) {
            const world::Room* room = findRoom(stored.stored->plan, id);
            if (room == nullptr) {
                continue;
            }
            if (data.rooms.contains(id)) {
                const world::Room* before = findRoom(draft.base().world.plan, id);
                if (before != nullptr && sameRoomData(*before, *room)) {
                    continue;
                }
                spdlog::info("Editor: the data of {} is read again for the replay (the stored plan holds it otherwise)",
                             id.toString());
            }
            auto loaded = world::loadRoomData(options.paths.assetRoot, *room);
            if (!loaded) {
                return propagate(loaded.error());
            }
            data.rooms.insert_or_assign(id, std::make_shared<const world::WorldData>(std::move(*loaded)));
        }
        edited = world::EditedWorld{stored.stored->plan, *overrides};
        auto session = world::applyEdits(edited, operations, data, options.provenance);
        if (!session) {
            return propagate(session.error());
        }
        if (session->refusedAt) {
            outcome.refusedAt = session->refusedAt;
            outcome.refused = session->results.back().refused;
            outcome.saved = world::EditedWorld{stored.stored->plan, *overrides};
            outcome.planDigest = stored.stored->digest;
            outcome.milliseconds = millisecondsSince(start);
            spdlog::warn("Editor: the session's operation {} no longer applies to the stored plan ({}: {}); nothing "
                         "written",
                         *outcome.refusedAt + 1, outcome.refused ? outcome.refused->rule : std::string(),
                         outcome.refused ? outcome.refused->message : std::string());
            return outcome;
        }
        results = std::move(session->results);
    }
    // The plan's digest after is the commit's (it serialises the plan once: serialising it costs about 0.45 s on the
    // reference library's 19,985 placements).
    auto result = world::editedResult(stored.stored->plan, stored.stored->digest, edited, results, false);
    if (!result) {
        return propagate(result.error());
    }
    outcome.resultMs = millisecondsSince(part);
    part = Clock::now();
    world::SaveOptions save;
    save.writer = options.writer;
    auto commit =
        world::commitWorld(*lock, world::WorldAction{"edit", scopeOf(results)}, *result, edited.overrides, save);
    if (!commit) {
        return propagate(commit.error());
    }
    outcome.commitMs = millisecondsSince(part);
    part = Clock::now();
    outcome.written = commit->written;
    if (commit->snapshot) {
        outcome.snapshot = commit->snapshot->name;
    }
    // The digest of the plan's text saved (or found there already): planDigest() of the plan.
    outcome.planDigest = commit->plan.digest;
    outcome.saved = world::EditedWorld{result->plan, edited.overrides};
    const double digestMs = millisecondsSince(part);
    outcome.milliseconds = millisecondsSince(start);
    spdlog::info("Editor: saved in {:.0f} ms: read {:.0f} ms, result {:.0f} ms, commit {:.0f} ms, digest {:.0f} ms",
                 outcome.milliseconds, outcome.readMs, outcome.resultMs, outcome.commitMs, digestMs);
    return outcome;
}

Expected<world::EditData> prepareEditData(const WorldPaths& paths, const world::WorldPlan& plan,
                                          std::string_view floor) {
    auto config = world::loadWorldConfig(paths.assetRoot, paths.configDir);
    if (!config) {
        return propagate(config.error());
    }
    for (const Error& issue : config->issues) {
        spdlog::warn("Editor: world.json left out: {}", issue.toString());
    }
    auto building = world::loadBuildingData(paths.assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    world::EditData data;
    data.config = std::move(config->config);
    data.building = std::move(*building);
    for (const world::Room& room : plan.rooms) {
        if (room.floor != floor) {
            continue;
        }
        auto loaded = world::loadRoomData(paths.assetRoot, room);
        if (!loaded) {
            return propagate(loaded.error());
        }
        data.rooms.emplace(room.id, std::make_shared<const world::WorldData>(std::move(*loaded)));
    }
    return data;
}

Expected<RoomStaticPart> roomStaticPart(const WorldBuild& shown, const world::WorldPlan& plan,
                                        const world::RoomId& room) {
    const RoomCollection* collection = shown.findRoom(room);
    if (collection == nullptr || collection->sceneData.roomTemplate.id.empty()) {
        return makeError(ErrorCode::NotFound, "the floor shown keeps no scene data of the room", room.toString());
    }
    auto part = world::roomFloorScene(plan, room, collection->sceneData);
    if (!part) {
        return propagate(part.error());
    }
    // A scene file of the room's part alone, read back as the floor's is (its lights name its area).
    nlohmann::json area{
        {"id", room.toString()},
        {"boxes", nlohmann::json::array({nlohmann::json{{"min", {0.0, 0.0, 0.0}}, {"max", {1.0, 1.0, 1.0}}}})}};
    nlohmann::json file{{"schemaVersion", scene::kSceneSchemaVersion},
                        {"name", "edit:" + room.toString()},
                        {"lights", std::move(part->lights)},
                        {"objects", std::move(part->objects)},
                        {"areas", nlohmann::json::array({std::move(area)})}};
    // In the scene's frame as buildFloor() moved the floor shown: its floorPlacement (relativePlacement() of no
    // placement in the start room's on the start room's floor, none elsewhere) read back as the start room's placement.
    const world::RoomPlacement& floor = shown.floorPlacement;
    const std::int64_t sign = floor.yawHalfTurns % 2 == 0 ? 1 : -1;
    moveSceneFileToRoom(file, world::RoomPlacement{-sign * floor.xMm, -sign * floor.zMm, floor.yawHalfTurns % 2});
    auto description = scene::parseSceneDescription(file, "edit:" + room.toString());
    if (!description) {
        return propagate(description.error());
    }
    return RoomStaticPart{room, std::move(description->objects), std::move(description->lights),
                          std::move(part->undressed)};
}

Expected<WorldBuild> editedRoomCollections(const WorldBuild& shown, const world::WorldPlan& plan,
                                           std::string planDigest, std::span<const world::RoomId> rooms) {
    WorldBuild build = shown;
    build.plan = plan;
    build.planDigest = std::move(planDigest);
    // The titles and covers the rooms' items showed (an edit changes no title), gathered over every room laid out
    // again: a game moved or swapped across a room chain (I4b step 8) takes its own into the room it enters.
    std::vector<RoomCollection*> edited;
    std::vector<world::CollectionTitle> titles;
    std::map<catalog::GameId, CoverItem> covers;
    for (const world::RoomId& id : rooms) {
        const auto room = std::find_if(build.rooms.begin(), build.rooms.end(),
                                       [&](const RoomCollection& candidate) { return candidate.room == id; });
        if (room == build.rooms.end()) {
            return makeError(ErrorCode::NotFound, "the floor shown has no such room", id.toString());
        }
        if (std::find(edited.begin(), edited.end(), &*room) != edited.end()) {
            continue;
        }
        edited.push_back(&*room);
        for (const world::CollectionItem& item : room->layout.items) {
            titles.push_back(world::CollectionTitle{item.game, item.title,
                                                    (item.flags & scene::instance_flags::kUnavailable) == 0});
        }
        for (CoverItem& cover : room->covers) {
            covers.emplace(cover.game, std::move(cover));
        }
    }
    for (RoomCollection* room : edited) {
        auto layout = world::collectionLayout(plan, room->room, room->sceneData, titles);
        if (!layout) {
            return propagate(layout.error());
        }
        room->layout = world::floorCollectionLayout(std::move(*layout), room->scenePlacement);
        room->covers.clear();
        const auto planned = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                          [&](const world::Room& candidate) { return candidate.id == room->room; });
        for (const world::CollectionItem& item : room->layout.items) {
            if (auto found = covers.find(item.game); found != covers.end()) {
                // Its generated pictures drawn from the seed of the room it stands in (buildFloor()'s), the same for
                // a game of the room, its new room's for one that entered from another room of the chain.
                if (planned != plan.rooms.end()) {
                    found->second.seed = world::placementSeed(planned->seed, item.game, planned->mediaFamily);
                }
                room->covers.push_back(std::move(found->second));
            }
        }
        room->contentDigest.clear();
    }
    return build;
}

} // namespace rv::app
