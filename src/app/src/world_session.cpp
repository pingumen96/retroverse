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
#include <rv/render/material_registry.hpp>
#include <rv/world/room_template.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "hero_textures.hpp"
#include "product_ui.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

/// The upload budget of an idle wake of a lockstep run: no frame waits for it, so it takes more at a time.
const gl::UploadBudget kIdleUploadBudget{std::chrono::milliseconds(8), 512};

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// What tells one refresh's summary from the next (RefreshSummary has no equality: its error has none), none for none.
[[nodiscard]] std::optional<std::string> refreshSignature(const std::optional<RefreshSummary>& summary) {
    if (!summary) {
        return std::nullopt;
    }
    return fmt::format("{}/{}/{}/{}/{}/{}/{}", toString(summary->outcome), summary->milliseconds, summary->entries,
                       summary->added, summary->removed, summary->changed, summary->moved);
}

} // namespace

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
    m_heroes.reset();
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
    // The coverLoad phase opens after the room's first two frames (commit()).
    if (!idleWake && m_coverPhasePending && m_nextFrame > m_coverPhaseAfter) {
        m_coverPhasePending = false;
        if (m_loader && !m_loader->done()) {
            m_context.frameTiming().openPhase(kPhaseCoverLoad);
            m_coverPhaseOpen = true;
        }
    }
    if (m_flow) {
        m_flow->update(product.catalog());
        // I3 step 10c: the stored plan's room shows first (world_start.cpp); the snapshot's result waits until a frame
        // has shown it, then reconciles it.
        if (std::optional<WorldBuild> preview = m_flow->takePreview()) {
            commitPreview(std::move(*preview));
        }
        // I3 step 15b: a live update's result waits while a launched game runs (ADR-0012 §6).
        if (!m_previewAwaitingFrame && !(m_liveFlow && product.launchActive())) {
            if (std::optional<WorldResult> result = m_flow->takeResult()) {
                if (std::exchange(m_liveFlow, false)) {
                    applyLive(std::move(*result));
                } else if (m_shownPreview) {
                    reconcile(std::move(*result));
                } else {
                    commit(std::move(*result));
                }
            }
        }
    } else if (m_decided) {
        commit(*std::exchange(m_decided, std::nullopt));
    }
    followRefresh(product);
    startLiveFlow(product);
    coverRound(product, idleWake ? kIdleUploadBudget : gl::UploadBudget{}, !idleWake);
    showNotice(product, timeSeconds);
}

CoverView WorldSession::currentView() const {
    if (!m_host.camera) {
        return CoverView{};
    }
    const CommandLineOptions& options = m_context.options();
    std::optional<float> screenshotAspect;
    if (options.screenshot && options.screenshotHeight > 0) {
        screenshotAspect = static_cast<float>(options.screenshotWidth) / static_cast<float>(options.screenshotHeight);
    }
    return CoverView::of(m_host.camera(), screenshotAspect);
}

void WorldSession::coverRound(ProductUi& product, const gl::UploadBudget& budget, bool frame) {
    if (!m_collection || !m_loader) {
        return;
    }
    // ADR-0012 §6: while a launched game runs nothing is made or uploaded.
    const bool paused = product.launchActive();
    double heroMs = 0.0;
    if (m_heroes) {
        // The hero texture shown is never evicted; the loader's media root, once known (a retry), is the heroes' too.
        if (m_loader->mediaRoot() && !m_heroesKnowRoot) {
            m_heroes->setMediaRoot(m_loader->mediaRoot());
            m_heroesKnowRoot = true;
        }
        std::vector<std::string> pinned;
        if (m_heroShown) {
            pinned.push_back(m_loader->items()[*m_heroShown].game.toString());
        }
        heroMs = m_heroes->update(paused, pinned);
    }
    const bool wasDone = m_loader->done();
    CoverLoader::Round round;
    round.view = currentView();
    round.focused = m_heroItem;
    round.paused = paused;
    round.budget = budget;
    round.spent =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(heroMs));
    round.frame = frame;
    round.frameIndex = m_context.frameIndex();
    if (const Status updated = m_loader->update(*m_collection, round); !updated) {
        spdlog::error("Cover loader: {}", updated.error().toString());
    }
    if (!wasDone && m_loader->done() && std::exchange(m_coverPhaseOpen, false)) {
        m_context.frameTiming().closePhase(kPhaseCoverLoad);
    }
}

void WorldSession::followRefresh(ProductUi& product) {
    AppCatalog& catalog = product.catalog();
    if (!catalog.supportsRefresh()) {
        return;
    }
    const RefreshState state = catalog.refreshState();
    if (!state.last || refreshSignature(state.last) == m_lastRefresh) {
        return;
    }
    m_lastRefresh = refreshSignature(state.last);
    if (m_loader && m_mediaRootPath && state.last->outcome == RefreshOutcome::Done) {
        spdlog::info("Frame {}: the catalog was refreshed; the cover loader tries the media root again",
                     m_context.frameIndex());
        m_loader->retry(*m_mediaRootPath);
    }
    if (state.last->outcome == RefreshOutcome::Done) {
        requestLiveUpdate(); // I3 step 15b
    }
}

void WorldSession::setHeroItem(world::PickId pickId) {
    m_heroItem = m_collection ? m_collection->itemOfPick(pickId) : std::nullopt;
    showHero();
}

void WorldSession::showHero() {
    if (!m_collection) {
        return;
    }
    const gl::Texture2D* texture = nullptr;
    if (m_heroItem && m_heroes && m_loader) {
        const CoverItem& item = m_loader->items()[*m_heroItem];
        m_heroes->request(item);
        texture = m_heroes->find(item.game);
    }
    const std::optional<std::size_t> shown = texture != nullptr ? m_heroItem : std::nullopt;
    if (shown != m_heroShown) {
        if (m_heroShown) {
            if (Status restored = m_collection->setFrontHidden(*m_heroShown, false); !restored) {
                spdlog::error("World: the hero's front: {}", restored.error().toString());
            }
        }
        if (shown) {
            if (Status hidden = m_collection->setFrontHidden(*shown, true); !hidden) {
                spdlog::error("World: the hero's front: {}", hidden.error().toString());
            }
        }
        m_heroShown = shown;
    }
    const unsigned textureId = texture != nullptr ? texture->id() : 0u;
    if (textureId != m_heroTexture) {
        if (Status set = m_graphics.materials().setTextureOverride(m_collection->heroMaterial(),
                                                                   gl::MaterialTextureSlot::BaseColor, textureId);
            !set) {
            spdlog::error("World: the hero texture: {}", set.error().toString());
        }
        m_heroTexture = textureId;
    }
    m_dynamicItems.clear();
    if (shown) {
        // Its attributes as the front record has them now (the hover's highlight), drawn by the dynamic path.
        if (auto item = m_collection->frontRenderItem(*shown, m_collection->heroMaterial())) {
            m_dynamicItems.push_back(*item);
        }
    }
}

bool WorldSession::settled(bool all) const {
    if (m_loader && !(all ? m_loader->done() : m_loader->viewDone())) {
        return false;
    }
    if (m_heroItem && m_heroes && m_loader && m_heroes->loading(m_loader->items()[*m_heroItem].game)) {
        return false;
    }
    return true;
}

void WorldSession::settle(ProductUi& product) {
    if (!m_options.lockstep || !m_collection || !m_loader) {
        return;
    }
    const std::optional<std::uint32_t> limit = m_context.options().frameLimit;
    const bool lastFrame = limit && m_context.frameIndex() + 1 >= *limit;
    // update() ranked the loader with the frame's view (the walker had moved), hover() asked for the hero texture.
    if (settled(lastFrame) || product.launchActive()) {
        return;
    }
    const Clock::time_point start = Clock::now();
    ++m_lockstepWaits;
    while (!settled(lastFrame)) {
        if (Clock::now() - start >= kLockstepWaitLimit) {
            if (!std::exchange(m_settleWarned, true)) {
                spdlog::warn("Frame {}: the layers of the view did not come within {} s; frames go on without them",
                             m_context.frameIndex(), kLockstepWaitLimit.count());
            }
            break;
        }
        m_loader->waitForResults(std::chrono::milliseconds(20));
        coverRound(product, kIdleUploadBudget, false);
        showHero();
    }
    m_lockstepWaitMs += millisecondsSince(start);
}

std::vector<std::string> WorldSession::statusLines() const {
    if (!m_collection || !m_loader) {
        return {};
    }
    std::vector<std::string> lines = m_loader->statusLines(*m_collection);
    if (m_heroes) {
        lines.push_back(
            fmt::format("Hero textures: {} of {} held ({:.1f} MiB){}", m_heroes->resident(), HeroLru::kDefaultCapacity,
                        static_cast<double>(m_heroes->bytes()) / (1024.0 * 1024.0), m_heroShown ? ", one shown" : ""));
    }
    return lines;
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
    if (build.mediaRootPath) {
        m_mediaRootPath = build.mediaRootPath; // the cover loader's retry after a refresh (I3 step 12)
    }
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
    // I3 step 15b: a room committed again over the one shown (the walker kept) keeps its items' pictures: the arrays
    // pass to the new collection and the cover loader's work is adopted, so only new or changed pictures go up.
    std::optional<CollectionPictures> pictures;
    std::unique_ptr<CoverLoader> shownLoader;
    if (keepWalker && m_collection) {
        pictures = m_collection->takePictures();
        shownLoader = std::move(m_loader);
    }
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
        auto collection =
            CollectionInstance::create(*build.layout, *build.sceneData, m_graphics,
                                       m_context.device().caps().uniformBufferOffsetAlignment, std::move(pictures));
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
    if (m_collection && shownLoader) {
        m_loader = std::move(shownLoader); // adopted by the new loader (startCoverLoader)
    }
    startCoverLoader(build.covers, build.mediaRoot, build.library);
    keepShown(build); // I3 step 15b: the next live update compares with it
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
    std::unique_ptr<CoverLoader> previous = std::move(m_loader);
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
    // Each item's box in the room: the loader ranks by it (in view, projected size, distance).
    m_loader = std::make_unique<CoverLoader>(std::move(loader), m_pool, std::move(covers), m_collection->itemBounds());
    if (previous) {
        // The same collection served again (I3 step 10c's textures reconciliation): only the pictures that changed go
        // up again.
        m_loader->adoptShown(*previous);
        previous.reset();
    }
    if (!m_heroes) {
        HeroTextures::Options heroes;
        heroes.wake = m_context.wakeCallback();
        heroes.anisotropy = gl::clampAnisotropy(8.0f, m_context.device().caps().maxAnisotropy);
        m_heroes = std::make_unique<HeroTextures>(std::move(heroes), m_pool);
        m_heroesKnowRoot = false;
    }
    if (mediaRoot) {
        m_heroes->setMediaRoot(mediaRoot);
        m_heroesKnowRoot = true;
    }
    // The coverLoad phase opens after the room's first two frames (update()): the frame that commits it and the next,
    // which waits for the new scene's first GPU work (300 to 350 ms measured on snes-stress in either array format),
    // are the commit's hitch, as the run's first 30 frames are its warm-up.
    if (!m_coverPhaseOpen && !m_coverPhasePending) {
        m_coverPhasePending = true;
        m_coverPhaseAfter = m_context.frameIndex() + 1;
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
    if (hovered != m_collection->hovered()) {
        if (Status set = m_collection->setHovered(hovered); !set) {
            spdlog::error("World: the hover outline: {}", set.error().toString());
        } else {
            ++m_record.hoverChanges;
            spdlog::debug("Frame {}: hover {}", m_context.frameIndex(),
                          hovered == world::kNoPick ? std::string("none") : fmt::format("{:08X}", hovered));
        }
    }
    // I3 step 12: the hovered item is the hero item until step 13a's focus takes that role.
    setHeroItem(m_collection->hovered());
}

void WorldSession::frameRendered() {
    if (m_waitSince && Clock::now() - *m_waitSince >= kLockstepWaitLimit && !m_waitWarned) {
        spdlog::warn("Frame {}: the room did not come within {} s; frames go on without it", m_context.frameIndex(),
                     kLockstepWaitLimit.count());
        m_waitWarned = true;
    }
    m_waitSince.reset();
    m_nextFrame = m_context.frameIndex() + 1;
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
    // Before the run's last frame the whole room, before the others the frame's view (ADR-0011 §6).
    const std::optional<std::uint32_t> limit = m_context.options().frameLimit;
    const bool lastFrameNext = limit && m_nextFrame + 1 >= *limit;
    const bool pending = m_decided.has_value() || (m_flow && m_flow->phase() != WorldPhase::Done) ||
                         (m_collection && !settled(lastFrameNext));
    if (!pending) {
        return false;
    }
    // The stored plan's room, its layers in, is shown by a frame before its reconciliation is waited for (I3 step 10c).
    if (m_previewAwaitingFrame && (!m_collection || settled(lastFrameNext))) {
        return false;
    }
    return !m_waitSince || Clock::now() - *m_waitSince < kLockstepWaitLimit;
}

void WorldSession::dropCollection() {
    m_coverPhasePending = false;
    m_dynamicItems.clear();
    m_heroItem.reset();
    m_heroShown.reset();
    m_heroTexture = 0;
    m_heroes.reset();
    m_loader.reset();
    m_collection.reset();
    m_pickIndex = world::PickIndex{};
    m_hover.reset();
    m_shown.reset();
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
        if (const auto viewResident = m_loader->viewResidentAt(); viewResident && m_shownAt) {
            residency.viewResidentAfterShownMs =
                std::chrono::duration<double, std::milli>(*viewResident - *m_shownAt).count();
        }
        residency.lockstepWaits = m_lockstepWaits;
        residency.lockstepWaitMs = m_lockstepWaitMs;
        if (m_heroes) {
            m_heroes->record(residency.hero);
            if (m_heroShown) {
                residency.hero.shownGame = m_loader->items()[*m_heroShown].game.toString();
            }
        }
        report.residency = std::move(residency);
    }
}

} // namespace rv::app
