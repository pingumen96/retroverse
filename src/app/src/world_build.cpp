#include <rv/app/world_build.hpp>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/metadata.hpp>
#include <rv/world/incremental.hpp>
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

/// The scene, the layout and the cover requests of the plan in `build` (ADR-0011 §1).
Status buildScene(const WorldBuildInput& input, WorldBuild& build) {
    const world::WorldPlan& plan = *build.plan;
    const Clock::time_point sceneStart = Clock::now();
    auto data = world::loadSceneData(input.paths.assetRoot, plan, build.room);
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
    build.undressed = std::move(sceneBuild->undressed);
    for (const world::UndressedSpan& span : build.undressed) {
        spdlog::info("World scene: {} slot(s) of {} from slot {} left bare: {}", span.count, span.level.toString(),
                     span.firstSlot, span.reason);
    }
    build.sceneMs = millisecondsSince(sceneStart);

    const Clock::time_point layoutStart = Clock::now();
    const catalog::RoomSnapshot& snapshot = input.snapshot.snapshot;
    const std::vector<world::CollectionTitle> titles = world::collectionTitles(snapshot);
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
        cover.systemName = input.snapshot.systemFullName;
        if (const catalog::RoomEntry* entry = entryOf(snapshot, item.game)) {
            cover.fallbackTitle = catalog::titleFromFileStem(entry->key.relativePath(), entry->directory);
            if (entry->year) {
                cover.year = std::to_string(*entry->year);
            }
        }
        if (room != nullptr) {
            cover.seed = world::placementSeed(room->seed, item.game, room->mediaFamily);
        }
        cover.coverPath = coverPathOf(input.snapshot.mediaPaths, item.game);
        cover.frontKey = item.frontLayerKey;
        cover.spineKey = item.spineLayerKey;
        build.covers.push_back(std::move(cover));
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
    if (auto read = world::readStoredPlan(files.plan)) {
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
    auto data = world::loadWorldData(input.paths.assetRoot, snapshot.system);
    if (!data) {
        if (stored) {
            keep(build, *stored, world_notice::kNone, "the world's data: " + data.error().message);
        } else {
            noPlan(build, world_notice::kFailed, "the world's data: " + data.error().message);
        }
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
        world::UpdateInput update;
        update.library = input.library;
        update.selection = std::move(*selection);
        update.config = config->config;
        update.overrides = std::move(overrides);
        update.previous = stored->plan;
        update.previousDigest = stored->digest;
        update.aliases = snapshot.aliases;
        result = world::update(update, *data);
    } else {
        world::GenerationInput generation;
        generation.library = input.library;
        generation.selection = std::move(*selection);
        generation.config = config->config;
        generation.overrides = std::move(overrides);
        result = world::generate(generation, *data);
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
    auto stored = world::readStoredPlan(world::worldFiles(paths.userDataRoot, library).plan);
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
            if (Status scene = buildScene(input, build); !scene) {
                spdlog::error("World: the room's scene cannot be made: {}", scene.error().toString());
                build.scene.reset();
                build.sceneData.reset();
                build.layout.reset();
                build.covers.clear();
                noPlan(build, world_notice::kFailed, "the room's scene: " + scene.error().message);
            }
        }
    } catch (const std::exception& error) {
        noPlan(build, world_notice::kFailed, std::string("the room could not be made: ") + error.what());
    }
    build.milliseconds = millisecondsSince(start);
    return build;
}

// --- The flow -------------------------------------------------------------------------------------------------------

struct WorldFlow::Mailbox {
    std::mutex mutex;
    std::optional<StoredWorldPlan> stored;
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
        RoomSnapshotRequest request;
        request.serial = ++m_serial;
        request.system = m_options.system;
        if (stored->stored) {
            request.placedGames.reserve(stored->stored->plan.placements.size());
            for (const world::Placement& placement : stored->stored->plan.placements) {
                request.placedGames.push_back(placement.game);
            }
        }
        m_stored = std::move(stored);
        if (!catalog.requestRoomSnapshot(std::move(request))) {
            WorldResult result;
            result.build = WorldBuild{};
            noPlan(*result.build, world_notice::kFailed, "the catalog cannot take the room's snapshot now");
            finish(std::move(result));
            return;
        }
        m_phase = WorldPhase::Snapshot;
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
