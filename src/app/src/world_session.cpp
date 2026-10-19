#include "world_session.hpp"

#include <algorithm>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/world/room_template.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "product_ui.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

/// The upload budget of an idle wake of a lockstep run: no frame waits for it, so it takes more at a time.
const gl::UploadBudget kIdleUploadBudget{std::chrono::milliseconds(8), 512};

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The placements per section and the unplaced games per reason of the plan shown, into `record` (the counts of a
/// room shown before replaced).
void countPlan(const WorldBuild& build, WorldRecord& record) {
    record.featured = record.band = record.spine = 0;
    record.placements = record.unplaced = 0;
    record.duplicateTitle = record.noCover = record.unavailable = record.capacity = 0;
    if (build.layout) {
        for (const world::CollectionItem& item : build.layout->items) {
            switch (item.section) {
            case world::Section::Featured:
                ++record.featured;
                break;
            case world::Section::Band:
                ++record.band;
                break;
            case world::Section::Spine:
                ++record.spine;
                break;
            }
        }
    }
    if (!build.plan) {
        return;
    }
    record.placements = build.plan->placements.size();
    record.unplaced = build.plan->unplaced.size();
    for (const world::UnplacedGame& game : build.plan->unplaced) {
        switch (game.reason) {
        case world::UnplacedReason::DuplicateTitle:
            ++record.duplicateTitle;
            break;
        case world::UnplacedReason::NoCover:
            ++record.noCover;
            break;
        case world::UnplacedReason::Unavailable:
            ++record.unavailable;
            break;
        case world::UnplacedReason::Capacity:
            ++record.capacity;
            break;
        }
    }
}

} // namespace

WorldSession::WorldSession(AppContext& context, Options options, core::WorkerPool& pool, SceneGraphics& graphics,
                           SceneHost host, std::optional<WorldResult> decided)
    : m_context(context), m_options(std::move(options)), m_pool(pool), m_graphics(graphics), m_host(std::move(host)),
      m_decided(std::move(decided)) {
    if (m_decided && m_decided->testRoom) {
        m_state = "test-room"; // the application shows it already
    }
    m_record.bound = true;
    m_record.state = m_state;
    m_record.room = world::RoomId::forSystem(m_options.system)
                        .map([](const world::RoomId& room) { return room.toString(); })
                        .value_or(std::string());
    if (!m_decided) {
        WorldFlow::Options flow;
        flow.paths = m_options.paths;
        flow.system = m_options.system;
        flow.writer = m_options.writer;
        flow.wake = context.wakeCallback();
        m_flow = std::make_unique<WorldFlow>(std::move(flow), pool);
        setNotice(WorldNoticeContent{std::string(world_notice::kGenerating), {}, {}}, false);
    }
}

WorldSession::~WorldSession() {
    // The loader's and the flow's jobs are cancelled before the collection they would feed goes.
    m_loader.reset();
    m_flow.reset();
    m_collection.reset();
}

Expected<scene::SceneDescription> WorldSession::shellScene(const std::filesystem::path& assetRoot,
                                                           const catalog::SystemName& system) {
    auto room = world::RoomId::forSystem(system);
    if (!room) {
        return propagate(room.error());
    }
    auto roomTemplate = world::loadRoomTemplate(assetRoot, world::kDefaultRoomTemplate);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    return world::shellSceneDescription(*room, *roomTemplate);
}

std::optional<double> WorldSession::sinceProcessStart(std::chrono::system_clock::time_point at) const {
    const std::uint64_t ticks = fileTimeTicksOf(at);
    if (m_options.processCreated == 0 || ticks < m_options.processCreated) {
        return std::nullopt;
    }
    return static_cast<double>(ticks - m_options.processCreated) / 10'000.0;
}

void WorldSession::setNotice(WorldNoticeContent notice, bool timed) {
    m_notice = std::move(notice);
    m_noticeTimed = timed;
    m_noticeSince.reset();
    m_record.notice = m_notice.notice;
}

void WorldSession::showNotice(ProductUi& product, double timeSeconds) {
    if (!m_noticeSince) {
        m_noticeSince = timeSeconds;
    }
    WorldNoticeContent shown = m_notice;
    if (m_noticeTimed && timeSeconds - *m_noticeSince >= world_notice::kShownSeconds) {
        shown = WorldNoticeContent{};
    }
    product.setWorldNotice(std::move(shown));
}

void WorldSession::update(ProductUi& product, bool idleWake, double timeSeconds) {
    if (idleWake && !m_waitSince) {
        m_waitSince = Clock::now();
    }
    if (m_flow) {
        m_flow->update(product.catalog());
        // I3 step 10c: the stored plan's room shows first (world_start.cpp); the snapshot's result waits until a frame
        // has shown it, then reconciles it.
        if (std::optional<WorldBuild> preview = m_flow->takePreview()) {
            commitPreview(std::move(*preview));
        }
        if (!m_previewAwaitingFrame) {
            if (std::optional<WorldResult> result = m_flow->takeResult()) {
                if (m_shownPreview) {
                    reconcile(std::move(*result));
                } else {
                    commit(std::move(*result));
                }
            }
        }
    } else if (m_decided) {
        commit(*std::exchange(m_decided, std::nullopt));
    }
    if (m_collection && m_loader && !m_loader->done()) {
        const Status round =
            m_loader->update(*m_collection, idleWake ? kIdleUploadBudget : gl::UploadBudget{}, m_context.frameIndex());
        if (!round) {
            spdlog::error("Cover loader: {}", round.error().toString());
        }
        if (m_loader->done() && std::exchange(m_coverPhaseOpen, false)) {
            m_context.frameTiming().closePhase(kPhaseCoverLoad);
        }
    }
    showNotice(product, timeSeconds);
}

void WorldSession::commit(WorldResult result) {
    const Clock::time_point start = Clock::now();
    if (result.testRoom) {
        spdlog::info("World: the test room instead of the generated one: {}", result.reason);
        dropCollection();
        if (m_state != "test-room") {
            if (Status shown = m_host.showTestRoom(); !shown) {
                spdlog::error("World: the test room cannot be shown: {}", shown.error().toString());
            }
        }
        m_state = "test-room";
        m_record.reason = result.reason;
        m_record.catalogState = result.catalogState;
        setNotice(WorldNoticeContent{result.notice, result.catalogState, m_options.system.str()}, true);
        m_record.state = m_state;
        return;
    }
    if (!result.build) {
        return;
    }
    WorldBuild& build = *result.build;
    recordBuild(build);
    if (!build.plan || !build.scene || !build.layout || !build.sceneData) {
        // Nothing to show: the shell stays, with its notice for as long as it shows.
        m_state = "shell";
        setNotice(WorldNoticeContent{build.notice.empty() ? std::string(world_notice::kFailed) : build.notice,
                                     {},
                                     m_options.system.str()},
                  false);
        m_record.state = m_state;
        return;
    }
    if (!showRoom(build, false)) {
        return;
    }
    m_record.firstFrame = m_context.frameIndex();
    finishCommit(build, start);
}

void WorldSession::recordBuild(const WorldBuild& build) {
    m_record.reason = build.reason;
    m_record.library = build.library.str();
    m_record.outcome = std::string(toString(build.outcome));
    m_record.changeOutcome.clear();
    m_record.rebuild.clear();
    m_record.added = 0;
    m_record.kept = 0;
    if (build.changes) {
        m_record.changeOutcome = std::string(world::toString(build.changes->outcome));
        m_record.rebuild = std::string(world::toString(build.changes->rebuild));
        m_record.added = build.changes->added.size();
        m_record.kept = build.changes->kept;
    }
    m_record.planDigest = build.planDigest;
    m_record.planWritten = build.written;
    m_record.saveError = build.saveError ? build.saveError->toString() : std::string();
    m_record.hardFindings = build.hardFindings.size();
    m_record.softFindings = build.softFindings;
    m_record.buildMs = build.milliseconds;
    m_record.itemsWritten = build.itemsWritten;
    m_record.screenGames = build.screenGames.size();
    m_record.objectFallbacks.clear();
    for (const ObjectFallback& fallback : build.objectFallbacks) {
        m_record.objectFallbacks.push_back(ObjectFallbackRecord{fallback.prefab, fallback.source, fallback.reason});
    }
    if (m_flow) {
        const WorldFlowTimes& times = m_flow->times();
        m_record.snapshotMs = times.snapshotWorkerMs;
        if (times.snapshotAt) {
            m_record.snapshotAtMs = sinceProcessStart(*times.snapshotAt);
        }
    }
    spdlog::info("World: {} ({}){}", toString(build.outcome), build.reason.empty() ? "as the catalog is" : build.reason,
                 build.planDigest.empty() ? std::string() : ", plan " + build.planDigest);
}

bool WorldSession::showRoom(WorldBuild& build, bool keepWalker) {
    dropCollection();
    const std::size_t objects = build.scene->objects.size();
    const std::size_t lights = build.scene->lights.size();
    const auto& show = keepWalker && m_host.showSceneKeepingWalker ? m_host.showSceneKeepingWalker : m_host.showScene;
    if (Status shown = show(std::move(*build.scene)); !shown) {
        spdlog::error("World: the room's scene cannot be shown: {}; the test room instead", shown.error().toString());
        if (Status fallback = m_host.showTestRoom(); !fallback) {
            spdlog::error("World: the test room cannot be shown: {}", fallback.error().toString());
        }
        m_state = "test-room";
        m_record.state = m_state;
        setNotice(WorldNoticeContent{std::string(world_notice::kFailed), {}, m_options.system.str()}, true);
        return false;
    }
    m_state = "room";
    countPlan(build, m_record);
    m_record.objects = objects;
    m_record.lights = lights;
    m_record.undressed = build.undressed.size();
    m_record.items = build.layout->items.size();
    m_record.clusters = build.layout->clusters.size();
    m_record.skipped = build.layout->skipped.size();
    m_record.staticRecords = 0;
    m_record.pickTargets = 0;

    // A room of no title (every game without a cover, say) has the static part alone.
    if (!build.layout->items.empty()) {
        auto collection = CollectionInstance::create(*build.layout, *build.sceneData, m_graphics,
                                                     m_context.device().caps().uniformBufferOffsetAlignment);
        if (collection) {
            m_collection = std::move(*collection);
            m_record.staticRecords = m_collection->group().layout().instances().size();
        } else {
            spdlog::error("World: the room's collection cannot be drawn: {}", collection.error().toString());
        }
    }
    if (auto index = world::PickIndex::build(build.layout->pickTargets(), build.layout->occluders)) {
        m_pickIndex = std::move(*index);
        m_record.pickTargets = m_pickIndex.targetCount();
    } else {
        spdlog::error("World: the room's pick index cannot be built: {}", index.error().toString());
    }
    m_hover.reset();
    startCoverLoader(build.covers, build.mediaRoot, build.library);
    // The hero CRT of the scene just shown plays the room's games (I3 step 10c; none for a preview).
    if (m_host.setScreenGames) {
        m_host.setScreenGames(build.screenGames);
    }
    m_shownAt = std::chrono::system_clock::now();
    return true;
}

void WorldSession::startCoverLoader(std::vector<CoverItem> covers,
                                    const std::optional<std::filesystem::path>& mediaRoot,
                                    const catalog::LibraryId& library) {
    m_loader.reset();
    auto cacheRoot = assets::textureCacheRoot(m_options.paths.cacheDir, library.str());
    if (!cacheRoot) {
        // The items keep the pending picture (nothing is written outside the user data directory's cache).
        spdlog::error("World: the texture cache: {}; the covers are not loaded", cacheRoot.error().toString());
        return;
    }
    if (!m_collection || covers.empty()) {
        return;
    }
    CoverLoader::Options loader;
    loader.assetRoot = m_options.paths.assetRoot;
    loader.cacheRoot = std::move(*cacheRoot);
    loader.format = m_graphics.collectionArrayFormat();
    loader.mediaRoot = mediaRoot;
    loader.wake = m_context.wakeCallback();
    m_loader = std::make_unique<CoverLoader>(std::move(loader), m_pool, std::move(covers));
    if (!m_coverPhaseOpen) {
        m_context.frameTiming().openPhase(kPhaseCoverLoad);
        m_coverPhaseOpen = true;
    }
}

void WorldSession::finishCommit(const WorldBuild& build, Clock::time_point start) {
    m_record.committedAtMs = sinceProcessStart(std::chrono::system_clock::now());
    m_record.commitMs = millisecondsSince(start);
    m_record.snapshotToCommitMs.reset();
    if (m_record.committedAtMs && m_record.snapshotAtMs) {
        m_record.snapshotToCommitMs = *m_record.committedAtMs - *m_record.snapshotAtMs;
    }
    m_record.state = m_state;
    setNotice(WorldNoticeContent{build.notice, {}, m_options.system.str()}, true);
    spdlog::info("Frame {}: the room is shown ({} object(s), {} light(s), {} item(s) in {} cluster(s)); committed in "
                 "{:.1f} ms",
                 m_context.frameIndex(), m_record.objects, m_record.lights, m_record.items, m_record.clusters,
                 *m_record.commitMs);
}

void WorldSession::hover(const std::optional<core::Ray>& ray) {
    if (!m_collection) {
        return;
    }
    world::PickId hovered = world::kNoPick;
    if (ray) {
        hovered = m_hover.update(m_pickIndex.sample(*ray, m_hover.hovered()));
    } else {
        m_hover.reset();
    }
    if (hovered == m_collection->hovered()) {
        return;
    }
    if (Status set = m_collection->setHovered(hovered); !set) {
        spdlog::error("World: the hover outline: {}", set.error().toString());
        return;
    }
    ++m_record.hoverChanges;
    spdlog::debug("Frame {}: hover {}", m_context.frameIndex(),
                  hovered == world::kNoPick ? std::string("none") : fmt::format("{:08X}", hovered));
}

void WorldSession::frameRendered() {
    if (m_waitSince && Clock::now() - *m_waitSince >= kLockstepWaitLimit && !m_waitWarned) {
        spdlog::warn("Frame {}: the room did not come within {} s; frames go on without it", m_context.frameIndex(),
                     kLockstepWaitLimit.count());
        m_waitWarned = true;
    }
    m_waitSince.reset();
    if (std::exchange(m_previewAwaitingFrame, false) && m_record.preview) {
        // The stored plan's room as its first frame showed it (I3 step 10c); the snapshot's result may come now.
        m_record.preview->frontsResident =
            m_collection ? m_collection->arrays().residentLayers(gl::CollectionClass::Front) : 0;
        m_record.preview->spinesResident =
            m_collection ? m_collection->arrays().residentLayers(gl::CollectionClass::Spine) : 0;
    }
}

std::span<const gl::StaticInstanceGroup* const> WorldSession::staticGroups() const noexcept {
    if (!m_collection) {
        return {};
    }
    return m_collection->groups();
}

bool WorldSession::waits() const {
    if (!m_options.lockstep) {
        return false;
    }
    const bool pending =
        m_decided.has_value() || (m_flow && m_flow->phase() != WorldPhase::Done) || (m_loader && !m_loader->done());
    if (!pending) {
        return false;
    }
    // The stored plan's room, its layers in, is shown by a frame before its reconciliation is waited for (I3 step 10c).
    if (m_previewAwaitingFrame && (!m_loader || m_loader->done())) {
        return false;
    }
    return !m_waitSince || Clock::now() - *m_waitSince < kLockstepWaitLimit;
}

void WorldSession::dropCollection() {
    m_loader.reset();
    m_collection.reset();
    m_pickIndex = world::PickIndex{};
    m_hover.reset();
    if (std::exchange(m_coverPhaseOpen, false)) {
        m_context.frameTiming().closePhase(kPhaseCoverLoad);
    }
}

void WorldSession::record(RunReport& report) const {
    WorldRecord world = m_record;
    world.state = m_state;
    if (m_collection) {
        if (const world::PickId hovered = m_collection->hovered(); hovered != world::kNoPick) {
            world.hoveredPickId = fmt::format("{:08X}", hovered);
            if (const std::optional<catalog::GameId> game = m_collection->gameOf(hovered)) {
                world.hoveredGame = game->toString();
            }
        }
    }
    report.world = std::move(world);
    if (m_collection && m_loader) {
        ResidencyRecord residency;
        m_loader->record(residency, *m_collection);
        if (m_shownAt) {
            residency.shownAtMs = sinceProcessStart(*m_shownAt);
        }
        if (const auto resident = m_loader->allResidentAt()) {
            residency.allResidentAtMs = sinceProcessStart(*resident);
            if (m_shownAt) {
                residency.residentAfterShownMs =
                    std::chrono::duration<double, std::milli>(*resident - *m_shownAt).count();
            }
        }
        report.residency = std::move(residency);
    }
}

} // namespace rv::app
