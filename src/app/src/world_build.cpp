#include <rv/app/world_build.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <mutex>
#include <span>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/room_items.hpp>
#include <rv/app/room_screen.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/sha256.hpp>
#include <rv/world/building.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/world_config.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// A plan file this program does not take: edited by hand (not its canonical text), not JSON, or newer.
[[nodiscard]] bool isRefusedPlan(const Error& error) noexcept {
    return error.code == ErrorCode::ValidationFailed || error.code == ErrorCode::ParseError ||
           error.code == ErrorCode::Unsupported;
}

/// Reads a plan file (world::readStoredPlan) and lifts a schema-1 plan into the building (ADR-0013 §5: the app shows a
/// stored I3 room on its floor's frame later, step 5a; the lifted plan is saved in schema 2 by its first update). The
/// digest stays the file's. Errors: those of readStoredPlan(), of the building's data, of the lift.
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

/// The snapshot's entry of `game`, or nullptr (entries come by GameId).
[[nodiscard]] const catalog::RoomEntry* entryOf(const catalog::RoomSnapshot& snapshot, const catalog::GameId& game) {
    const auto found =
        std::lower_bound(snapshot.entries.begin(), snapshot.entries.end(), game,
                         [](const catalog::RoomEntry& entry, const catalog::GameId& id) { return entry.id < id; });
    return found != snapshot.entries.end() && found->id == game ? &*found : nullptr;
}

/// Fills a cover request's text for a layout's item (the game, the title, the keys and the seed are set): false when
/// nothing is known of it (no request is made then: the item keeps the pending picture).
using DescribeCover = std::function<bool(const world::CollectionItem& item, CoverItem& cover)>;

/// The scene, the layout (with `titles`) and the cover requests (`describe`) of the plan in `build` (ADR-0011 §1), and
/// the prefabs that fell back.
Status buildScene(const WorldPaths& paths, WorldBuild& build, std::span<const world::CollectionTitle> titles,
                  const DescribeCover& describe) {
    const world::WorldPlan& plan = *build.plan;
    const Clock::time_point sceneStart = Clock::now();
    // The lighting profile world.json names (I3 step 16): how the room is drawn, never what the plan holds.
    auto data = world::loadSceneData(paths.assetRoot, plan, build.room,
                                     world::roomLightingProfile(paths.assetRoot, paths.configDir));
    if (!data) {
        return propagate(data.error());
    }
    auto sceneBuild = world::toSceneFile(plan, build.room, *data);
    if (!sceneBuild) {
        return propagate(sceneBuild.error());
    }
    auto description = scene::parseSceneDescription(sceneBuild->sceneFile, build.room.toString());
    if (!description) {
        return propagate(description.error());
    }
    build.sceneDigest = core::sha256Hex(world::sceneFileText(sceneBuild->sceneFile));
    build.undressed = std::move(sceneBuild->undressed);
    for (const world::UndressedSpan& span : build.undressed) {
        spdlog::info("World scene: {} slot(s) of {} from slot {} left bare: {}", span.count, span.level.toString(),
                     span.firstSlot, span.reason);
    }
    build.sceneMs = millisecondsSince(sceneStart);
    for (const world::ScenePrefab& prefab : data->prefabs.prefabs) {
        if (prefab.source != assets::PrefabSource::Requested) {
            build.objectFallbacks.push_back(ObjectFallback{
                prefab.ref.manifest, std::string(assets::toString(prefab.source)), prefab.fallbackReason});
        }
    }

    const Clock::time_point layoutStart = Clock::now();
    auto layout = world::collectionLayout(plan, build.room, *data, titles);
    if (!layout) {
        return propagate(layout.error());
    }
    for (const world::SkippedPlacement& skipped : layout->skipped) {
        spdlog::warn("World collection: {} in {} not drawn: {}", skipped.placement.game.toString(),
                     skipped.placement.slot.toString(), skipped.reason);
    }
    const world::Room* room = nullptr;
    for (const world::Room& candidate : plan.rooms) {
        if (candidate.id == build.room) {
            room = &candidate;
        }
    }
    build.covers.reserve(layout->items.size());
    for (const world::CollectionItem& item : layout->items) {
        CoverItem cover;
        cover.game = item.game;
        cover.title = item.title;
        if (room != nullptr) {
            cover.seed = world::placementSeed(room->seed, item.game, room->mediaFamily);
        }
        cover.frontKey = item.frontLayerKey;
        cover.spineKey = item.spineLayerKey;
        if (describe(item, cover)) {
            build.covers.push_back(std::move(cover));
        }
    }
    build.layoutMs = millisecondsSince(layoutStart);
    build.sceneData = std::move(*data);
    build.scene = std::move(*description);
    build.layout = std::move(*layout);
    return ok();
}

/// The plan of `build` from the snapshot (see buildWorld()).
void makePlan(const WorldBuildInput& input, const core::JobContext& job, WorldBuild& build) {
    const RoomSnapshotResult& snapshot = input.snapshot;
    const world::WorldFiles files = world::worldFiles(input.paths.userDataRoot, input.library);

    // The world's lock first, then its plan read again under it (a writer reads what it replaces under the lock).
    const Clock::time_point lockStart = Clock::now();
    std::optional<world::WorldLock> lock;
    if (auto acquired = world::WorldLock::acquire(files, input.lockWait)) {
        lock.emplace(std::move(*acquired));
    } else {
        spdlog::warn("World: the world's lock: {}; the room is made without saving its plan",
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
    if (snapshot.error) {
        if (stored) {
            keep(build, *stored, world_notice::kNone, "the room's snapshot failed: " + snapshot.error->message);
        } else {
            noPlan(build, world_notice::kFailed, "the room's snapshot failed: " + snapshot.error->message);
        }
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
    // The data of the building and of every room: a stored room's with its recorded family, theme pack and template
    // (ADR-0013 §3), a first plan's room's with its system's.
    std::vector<world::WorldData> rooms;
    const auto noData = [&](const Error& error) {
        if (stored) {
            keep(build, *stored, world_notice::kNone, "the world's data: " + error.message);
        } else {
            noPlan(build, world_notice::kFailed, "the world's data: " + error.message);
        }
    };
    if (stored) {
        for (const world::Room& room : stored->plan.rooms) {
            auto data = world::loadRoomData(input.paths.assetRoot, room);
            if (!data) {
                noData(data.error());
                return;
            }
            rooms.push_back(std::move(*data));
        }
    } else {
        auto data = world::loadWorldData(input.paths.assetRoot, snapshot.system);
        if (!data) {
            noData(data.error());
            return;
        }
        rooms.push_back(std::move(*data));
    }
    auto building = world::loadBuildingData(input.paths.assetRoot);
    auto systems = world::loadSystemsData(input.paths.assetRoot);
    if (!building || !systems) {
        noData(!building ? building.error() : systems.error());
        return;
    }
    world::WorldOverrides overrides;
    if (auto loaded = world::loadOverrides(files.overrides)) {
        overrides = std::move(*loaded);
    } else {
        spdlog::warn("World: overrides.json left out: {}", loaded.error().toString());
    }

    // The selection: nothing is decided from unknown media (ADR-0010 §3).
    const Clock::time_point selectStart = Clock::now();
    auto selection = world::selectRoom(world::RoomInput{input.library, snapshot.snapshot}, config->config);
    build.selectMs = millisecondsSince(selectStart);
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
    if (job.cancelled()) {
        return;
    }

    const Clock::time_point planStart = Clock::now();
    Expected<world::GenerationResult> result = makeError(ErrorCode::Unknown, "no plan made");
    if (stored) {
        // The room of the snapshot's system is updated; the building's other rooms stay as they are (the app's
        // building job, which updates every room, is I4 step 5a's).
        world::BuildingUpdateInput update;
        update.library = input.library;
        update.selections = {std::move(*selection)};
        update.config = config->config;
        update.overrides = std::move(overrides);
        update.previous = stored->plan;
        update.previousDigest = stored->digest;
        update.aliases = snapshot.aliases;
        result = world::updateBuilding(update, *building, *systems, rooms);
    } else {
        world::BuildingInput generation;
        generation.library = input.library;
        generation.selections = {std::move(*selection)};
        generation.config = config->config;
        generation.overrides = std::move(overrides);
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
        if (stored) {
            keep(build, *stored, world_notice::kNone, reason);
        } else {
            noPlan(build, world_notice::kFailed, reason);
        }
        return;
    }
    // A room kept as it is until a regeneration (ADR-0013 §5: an older template version, prefab levels that changed,
    // another generator major) places nothing new: said in the log, and by the regeneration's notice when it is the
    // room shown (I3 showed it for the same changes, which refused the whole plan then).
    for (const world::KeptRoom& kept : result->changes.keptRooms) {
        spdlog::warn("World: {} is kept as it is until 'retroverse-cli worldplan generate --replace' (its new games "
                     "are not placed): {}",
                     kept.room.toString(), kept.reason);
        if (kept.room == build.room) {
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
    case WorldPhase::Snapshot:
        return "snapshot";
    case WorldPhase::Building:
        return "building";
    case WorldPhase::Ready:
        return "ready";
    case WorldPhase::Done:
        return "done";
    }
    return "unknown";
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

WorldBuild buildWorld(const WorldBuildInput& input, const core::JobContext& job) {
    const Clock::time_point start = Clock::now();
    WorldBuild build;
    build.library = input.library;
    try {
        auto room = world::RoomId::forSystem(input.snapshot.system);
        if (!room) {
            noPlan(build, world_notice::kFailed, room.error().message);
            return build;
        }
        build.room = *room;
        makePlan(input, job, build);
        if (job.cancelled()) {
            noPlan(build, world_notice::kFailed, "cancelled");
            return build;
        }
        if (build.plan) {
            if (input.snapshot.snapshot.mediaKnown) {
                build.mediaRoot = input.snapshot.mediaRoot;
            }
            build.mediaRootPath = input.snapshot.mediaRoot;
            const RoomSnapshotResult& snapshot = input.snapshot;
            const std::vector<world::CollectionTitle> titles = world::collectionTitles(snapshot.snapshot);
            const DescribeCover describe = [&](const world::CollectionItem& item, CoverItem& cover) {
                cover.systemName = snapshot.systemFullName;
                if (const catalog::RoomEntry* entry = entryOf(snapshot.snapshot, item.game)) {
                    cover.fallbackTitle = catalog::titleFromFileStem(entry->key.relativePath(), entry->directory);
                    if (entry->year) {
                        cover.year = std::to_string(*entry->year);
                    }
                }
                cover.coverPath = coverPathOf(snapshot.mediaPaths, item.game);
                return true;
            };
            if (Status scene = buildScene(input.paths, build, titles, describe); !scene) {
                spdlog::error("World: the room's scene cannot be made: {}", scene.error().toString());
                build.scene.reset();
                build.sceneData.reset();
                build.layout.reset();
                build.covers.clear();
                build.objectFallbacks.clear();
                noPlan(build, world_notice::kFailed, "the room's scene: " + scene.error().message);
            } else {
                // I3 step 10c: the hero CRT's games, and the room's items for the next start's preview.
                build.screenGames = roomScreenGames(*build.plan, *build.layout, snapshot);
                const std::filesystem::path items = roomItemsFile(input.paths.cacheDir, input.library, snapshot.system);
                if (auto written = writeRoomItems(items, roomItemsOf(build.covers, *build.layout))) {
                    build.itemsWritten = *written;
                } else {
                    build.itemsError = written.error();
                    spdlog::warn("World: the room's items for the next start cannot be kept: {}",
                                 written.error().message);
                }
            }
        }
    } catch (const std::exception& error) {
        noPlan(build, world_notice::kFailed, std::string("the room could not be made: ") + error.what());
    }
    build.milliseconds = millisecondsSince(start);
    return build;
}

WorldBuild previewWorld(const WorldPaths& paths, const catalog::LibraryId& library, const catalog::SystemName& system,
                        const world::StoredPlan& stored) {
    const Clock::time_point start = Clock::now();
    WorldBuild build;
    build.preview = true;
    build.library = library;
    try {
        auto room = world::RoomId::forSystem(system);
        if (!room) {
            noPlan(build, world_notice::kFailed, room.error().message);
            return build;
        }
        build.room = *room;
        keep(build, stored, world_notice::kNone, "the stored plan, shown before the catalog's snapshot");
        // The room's items as they last showed: a missing file leaves the items pending until the snapshot.
        RoomItems items;
        if (auto read = readRoomItems(roomItemsFile(paths.cacheDir, library, system))) {
            items = std::move(*read);
        } else if (read.error().code != ErrorCode::NotFound) {
            spdlog::warn("World: the room's items kept at the last start are not used: {}", read.error().message);
        }
        const std::vector<world::CollectionTitle> titles = collectionTitlesOf(items, stored.plan);
        const DescribeCover describe = [&](const world::CollectionItem& item, CoverItem& cover) {
            const RoomItemText* text = items.find(item.game);
            if (text == nullptr) {
                return false;
            }
            cover.systemName = items.systemName;
            cover.fallbackTitle = text->fallbackTitle;
            cover.year = text->year;
            cover.coverPath = text->coverPath;
            return true;
        };
        if (Status scene = buildScene(paths, build, titles, describe); !scene) {
            spdlog::error("World: the stored plan's room cannot be shown before the snapshot: {}",
                          scene.error().toString());
            build.scene.reset();
            build.sceneData.reset();
            build.layout.reset();
            build.covers.clear();
            build.objectFallbacks.clear();
            noPlan(build, world_notice::kFailed, "the stored plan's scene: " + scene.error().message);
        }
        build.itemsRead = build.covers.size();
    } catch (const std::exception& error) {
        noPlan(build, world_notice::kFailed, std::string("the stored plan's room could not be made: ") + error.what());
    }
    build.milliseconds = millisecondsSince(start);
    return build;
}

// --- The flow -------------------------------------------------------------------------------------------------------

struct WorldFlow::Mailbox {
    std::mutex mutex;
    std::optional<StoredWorldPlan> stored;
    std::optional<WorldBuild> preview;
    std::optional<WorldBuild> build;
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

void WorldFlow::requestSnapshot(AppCatalog& catalog) {
    RoomSnapshotRequest request;
    request.serial = ++m_serial;
    request.system = m_options.system;
    if (m_stored && m_stored->stored) {
        request.placedGames.reserve(m_stored->stored->plan.placements.size());
        for (const world::Placement& placement : m_stored->stored->plan.placements) {
            request.placedGames.push_back(placement.game);
        }
    }
    if (!catalog.requestRoomSnapshot(std::move(request))) {
        WorldResult result;
        result.build = WorldBuild{};
        noPlan(*result.build, world_notice::kFailed, "the catalog cannot take the room's snapshot now");
        finish(std::move(result));
        return;
    }
    m_phase = WorldPhase::Snapshot;
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
        if (!catalog.hasGamesOf(m_options.system)) {
            WorldResult result;
            result.testRoom = true;
            result.notice = std::string(world_notice::kNoGames);
            result.reason = fmt::format("the catalog has no games of {}", m_options.system.str());
            finish(std::move(result));
            return;
        }
        m_library = catalog.roomLibrary();
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
            WorldResult result;
            result.build = WorldBuild{};
            noPlan(*result.build, world_notice::kFailed, "the world's job: " + submitted.error().message);
            finish(std::move(result));
            return;
        }
        m_phase = WorldPhase::ReadingPlan;
        return;
    }
    case WorldPhase::ReadingPlan: {
        std::optional<StoredWorldPlan> stored;
        {
            const std::lock_guard lock(m_mailbox->mutex);
            if (m_mailbox->generation == m_generation.current()) {
                stored = std::exchange(m_mailbox->stored, std::nullopt);
            }
        }
        if (!stored) {
            return;
        }
        m_times.planReadMs = elapsedMs();
        if (stored->error) {
            spdlog::warn("World: the stored plan cannot be read: {}", stored->error->toString());
        }
        m_stored = std::move(stored);
        if (m_stored->stored && m_options.preview) {
            // I3 step 10c: the stored plan's room first; the snapshot once it has been taken.
            const std::uint64_t generation = m_generation.current();
            core::JobOptions jobOptions;
            jobOptions.priority = core::WorkerPool::kHighestPriority;
            jobOptions.generation = &m_generation;
            const Status submitted = m_pool.submit(
                [mailbox = m_mailbox, paths = m_options.paths, library = m_library, system = m_options.system,
                 stored = m_stored->stored, generation](const core::JobContext& job) {
                    if (job.cancelled()) {
                        return;
                    }
                    WorldBuild preview = previewWorld(paths, library, system, *stored);
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
            spdlog::warn("World: the stored plan's room cannot be made before the snapshot: {}",
                         submitted.error().message);
        }
        requestSnapshot(catalog);
        return;
    }
    case WorldPhase::Previewing: {
        if (m_previewTaken) {
            requestSnapshot(catalog);
            return;
        }
        if (m_preview) {
            return; // made, waiting to be taken
        }
        {
            const std::lock_guard lock(m_mailbox->mutex);
            if (m_mailbox->generation == m_generation.current()) {
                m_preview = std::exchange(m_mailbox->preview, std::nullopt);
            }
        }
        if (m_preview) {
            m_times.previewMs = elapsedMs();
            m_times.previewJobMs = m_preview->milliseconds;
        }
        return;
    }
    case WorldPhase::Snapshot: {
        std::optional<RoomSnapshotResult> snapshot = catalog.takeRoomSnapshot();
        if (snapshot && snapshot->serial != m_serial) {
            snapshot.reset(); // an answer to an earlier request
        }
        if (!snapshot) {
            if (catalog.workerEnded()) {
                WorldResult result;
                result.build = WorldBuild{};
                noPlan(*result.build, world_notice::kFailed, "the catalog worker ended before the room's snapshot");
                finish(std::move(result));
            }
            return;
        }
        m_times.snapshotMs = elapsedMs();
        m_times.snapshotWorkerMs = snapshot->milliseconds;
        m_times.snapshotAt = snapshot->at;
        WorldBuildInput input;
        input.paths = m_options.paths;
        input.library = m_library;
        input.snapshot = std::move(*snapshot);
        input.writer = m_options.writer;
        const std::uint64_t generation = m_generation.current();
        core::JobOptions jobOptions;
        jobOptions.priority = core::WorkerPool::kHighestPriority;
        jobOptions.generation = &m_generation;
        const Status submitted = m_pool.submit(
            [mailbox = m_mailbox, input = std::move(input), generation](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                WorldBuild build = buildWorld(input, job);
                if (job.cancelled()) {
                    return;
                }
                mailbox->post(generation, [&](Mailbox& box) { box.build = std::move(build); });
            },
            jobOptions);
        if (!submitted) {
            WorldResult result;
            result.build = WorldBuild{};
            noPlan(*result.build, world_notice::kFailed, "the world's job: " + submitted.error().message);
            finish(std::move(result));
            return;
        }
        m_phase = WorldPhase::Building;
        return;
    }
    case WorldPhase::Building: {
        std::optional<WorldBuild> build;
        {
            const std::lock_guard lock(m_mailbox->mutex);
            if (m_mailbox->generation == m_generation.current()) {
                build = std::exchange(m_mailbox->build, std::nullopt);
            }
        }
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
