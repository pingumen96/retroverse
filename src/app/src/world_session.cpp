#include "world_session.hpp"

#include <algorithm>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/app/world_label_ui.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/world/world_config.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "hero_textures.hpp"
#include "product_ui.hpp"
#include "world_regenerate_state.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

/// The upload budget of an idle wake of a lockstep run: no frame waits for it, so it takes more at a time.
const gl::UploadBudget kIdleUploadBudget{std::chrono::milliseconds(8), 512};

/// Frames the screenBind phase holds after a rebind of the hero CRT (I4 step 6b): the rebind's frame, the previous
/// clip's thread ending and the next clip's first frames (about a second at 60 frames a second).
constexpr std::uint64_t kScreenBindPhaseFrames = 60;

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

/// The placements per section of the floor shown and the unplaced games per reason of the building's plan, into
/// `record` (the counts of a floor shown before replaced).
void countPlan(const WorldBuild& build, WorldRecord& record) {
    record.featured = record.band = record.spine = 0;
    record.placements = record.unplaced = 0;
    record.duplicateTitle = record.noCover = record.unavailable = record.capacity = record.roomKept = 0;
    for (const RoomCollection& room : build.rooms) {
        for (const world::CollectionItem& item : room.layout.items) {
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
        case world::UnplacedReason::RoomKept:
            ++record.roomKept;
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
    // The world's reduceMotion (world.json, ADR-0012 §3): every transition of the interaction instant.
    InteractionSettings interaction;
    if (auto config = world::loadWorldConfig(m_options.paths.assetRoot, m_options.paths.configDir)) {
        interaction.reduceMotion = config->config.reduceMotion;
    }
    if (interaction.reduceMotion) {
        spdlog::info("World: reduceMotion (world.json): every transition of the room is instant");
    }
    m_reduceMotion = interaction.reduceMotion;
    m_interaction = std::make_unique<WorldHover>(context, interaction);
    // I4 step 7b: the rides' floor-build jobs post here and wake the host.
    m_floorBox = std::make_shared<FloorJobBox>();
    m_floorBox->wake = context.wakeCallback();
    // I4 step 6b: the start room's CRT is bound from the start; the lobby's none until the eye stays in a room.
    m_screenRooms.reset(m_options.startRoom ? std::optional<std::string>(m_options.startRoom->toString())
                                            : std::nullopt);
    m_record.bound = true;
    m_record.state = m_state;
    if (!m_decided) {
        WorldFlow::Options flow;
        flow.paths = m_options.paths;
        flow.startRoom = m_options.startRoom;
        flow.writer = m_options.writer;
        flow.wake = context.wakeCallback();
        flow.floor = [this]() -> std::optional<std::string> {
            return m_floor.empty() ? std::nullopt : std::optional<std::string>(m_floor);
        };
        m_flow = std::make_unique<WorldFlow>(std::move(flow), pool);
        setNotice(WorldNoticeContent{std::string(world_notice::kGenerating), {}, {}}, false);
    }
}

WorldSession::~WorldSession() {
    // The loader's and the flow's jobs are cancelled before the collections they would feed go; the interaction lets
    // go of them first.
    m_heroes.reset();
    m_loader.reset();
    m_flow.reset();
    m_pool.cancel(m_floorGeneration);
    if (m_floorBox) {
        const std::lock_guard lock(m_floorBox->mutex);
        m_floorBox->wake = nullptr;
    }
    m_pool.cancel(m_showcaseGeneration);
    if (m_showcaseBox) {
        const std::lock_guard lock(m_showcaseBox->mutex);
        m_showcaseBox->wake = nullptr;
    }
    if (m_regenerations) {
        // I4b step 6: a preview's or a commit's job posts into its box harmlessly (a commit under way still ends).
        m_pool.cancel(m_regenerations->generation);
        const std::lock_guard lock(m_regenerations->box->mutex);
        m_regenerations->box->wake = nullptr;
    }
    // I4b step 3: an edit session's job is cancelled too and wakes nothing (its box, shared, takes a late answer).
    if (m_edit) {
        m_pool.cancel(m_edit->generation);
        if (m_edit->box) {
            const std::lock_guard lock(m_edit->box->mutex);
            m_edit->box->wake = nullptr;
        }
    }
    m_showcaseLoader.reset();
    m_showcase.reset();
    m_interaction.reset();
    m_rooms.clear();
}

Expected<scene::SceneDescription> WorldSession::shellScene(const std::filesystem::path& assetRoot,
                                                           const std::optional<world::RoomId>& startRoom) {
    return worldShellScene(assetRoot, startRoom);
}

std::optional<double> WorldSession::sinceProcessStart(std::chrono::system_clock::time_point at) const {
    const std::uint64_t ticks = fileTimeTicksOf(at);
    if (m_options.processCreated == 0 || ticks < m_options.processCreated) {
        return std::nullopt;
    }
    return static_cast<double>(ticks - m_options.processCreated) / 10'000.0;
}

std::string WorldSession::noticeRoom() const {
    return m_options.startRoom ? m_options.startRoom->toString() : std::string();
}

void WorldSession::setNotice(WorldNoticeContent notice, bool timed) {
    m_notice = std::move(notice);
    m_noticeTimed = timed;
    m_noticeSince.reset();
    m_record.notice = m_notice.notice;
    m_record.noticeRoom = m_notice.startRoom;
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
    // The frame's time step (the ride's fades); an idle wake has none.
    double deltaSeconds = 0.0;
    if (!idleWake) {
        ++m_updates;
        deltaSeconds = m_lastTime ? timeSeconds - *m_lastTime : 0.0;
        m_lastTime = timeSeconds;
    }
    // The coverLoad phase opens after the floor's first two frames (commit()).
    if (!idleWake && m_coverPhasePending && m_nextFrame > m_coverPhaseAfter) {
        m_coverPhasePending = false;
        if (m_loader && !m_loader->done()) {
            m_context.frameTiming().openPhase(kPhaseCoverLoad);
            m_coverPhaseOpen = true;
        }
    }
    if (m_flow) {
        m_flow->update(product.catalog());
        // I3 step 10c: the stored plan's floor shows first (world_start.cpp); the building's result waits until a
        // frame has shown it, then reconciles it.
        if (std::optional<WorldBuild> preview = m_flow->takePreview()) {
            commitPreview(std::move(*preview));
        }
        // I3 step 15b: a live update's result waits while a launched game runs (ADR-0012 §6); I4 step 7b: a result
        // waits for a ride to end (ADR-0013 §10), and one built for another floor than the one shown is built again
        // for it first.
        if (!m_previewAwaitingFrame && !(m_liveFlow && product.launchActive()) && !m_ride && !m_retargetPending) {
            if (std::optional<WorldResult> result = m_flow->takeResult()) {
                if (retargetResult(*result, product.catalog().worldReader())) {
                    m_liveFlow = false;
                } else if (std::exchange(m_liveFlow, false)) {
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
    // I4 step 7b: the ride (its floor's job, its fade, the floor swapped behind it).
    followRide(product, idleWake, deltaSeconds);
    followRefresh(product);
    startLiveFlow(product);
    if (!idleWake) {
        followScreenRoom(product, timeSeconds);
    }
    coverRound(product, idleWake ? kIdleUploadBudget : gl::UploadBudget{}, !idleWake);
    // I6 step 5: the hub's favourites showcase (world_showcase.cpp).
    followShowcase(product);
    showcaseRound(product, idleWake ? kIdleUploadBudget : gl::UploadBudget{}, !idleWake);
    showNotice(product, timeSeconds);
    syncDirectory(product);
    followEdit(product);         // I4b step 3
    followRegeneration(product); // I4b step 6
}

std::vector<ScreenGame> WorldSession::screenGamesOf(const std::string& room) const {
    if (m_shown) {
        for (const ShownCollection& collection : m_shown->rooms) {
            if (collection.room.toString() == room) {
                return collection.screenGames;
            }
        }
    }
    return {};
}

std::optional<std::string> WorldSession::floorScreenRoom(const WorldBuild& build) const {
    const std::optional<std::string>& bound = m_screenRooms.bound();
    if (!bound) {
        return std::nullopt;
    }
    for (const RoomCollection& room : build.rooms) {
        if (room.room.toString() == *bound) {
            return bound;
        }
    }
    return std::nullopt;
}

void WorldSession::followScreenRoom(ProductUi& product, double timeSeconds) {
    const std::uint64_t frame = m_context.frameIndex();
    if (m_screenPhaseUntil && frame >= *m_screenPhaseUntil) {
        m_context.frameTiming().closePhase(kPhaseScreenBind);
        m_screenPhaseUntil.reset();
    }
    // ADR-0012 §6: nothing changes on the CRT while a launched game runs.
    if (m_state != "room" || m_rooms.empty() || !m_shown || !m_host.bindScreen || product.launchActive()) {
        return;
    }
    const std::string area = m_host.eyeArea ? m_host.eyeArea() : std::string();
    std::optional<std::string_view> room;
    for (const FloorRoom& floorRoom : m_rooms) {
        if (floorRoom.id.toString() == area && !m_screenless.contains(area)) {
            room = area;
        }
    }
    const std::optional<std::string> was = m_screenRooms.bound();
    const std::optional<std::string> bind = m_screenRooms.update(room, timeSeconds);
    if (!bind) {
        return;
    }
    std::vector<ScreenGame> games = screenGamesOf(*bind);
    const std::size_t count = games.size();
    if (!m_host.bindScreen(*bind + "/", std::move(games))) {
        spdlog::warn("Frame {}: {} has no CRT to bind; the hero CRT stays {}", frame, *bind, was.value_or("unbound"));
        m_screenless.insert(*bind);
        m_screenRooms.reset(was);
        return;
    }
    spdlog::info("Frame {}: the hero CRT is {}'s now, with its {} game(s) (the eye stayed there {:g} s)", frame, *bind,
                 count, kScreenDwellSeconds);
    m_record.screenGames = count;
    if (!m_screenPhaseUntil) {
        m_context.frameTiming().openPhase(kPhaseScreenBind);
    }
    m_screenPhaseUntil = frame + kScreenBindPhaseFrames;
}

bool WorldSession::eyeInScreenRoom() const {
    const std::optional<std::string>& bound = m_screenRooms.bound();
    if (!bound || !m_host.eyeArea) {
        return true;
    }
    return m_host.eyeArea() == *bound;
}

std::optional<std::size_t> WorldSession::eyeRoom() const {
    if (!m_host.eyeArea) {
        return m_rooms.size() == 1 ? std::optional<std::size_t>(0) : std::nullopt;
    }
    const std::string area = m_host.eyeArea();
    for (std::size_t index = 0; index < m_rooms.size(); ++index) {
        if (m_rooms[index].id.toString() == area) {
            return index;
        }
    }
    return std::nullopt;
}

CollectionInstance* WorldSession::activeCollection() const {
    return m_interaction ? m_interaction->collection() : nullptr;
}

std::optional<WorldSession::FloorItem> WorldSession::floorItemOf(world::PickId pickId) const {
    CollectionInstance* collection = activeCollection();
    if (collection == nullptr || pickId == world::kNoPick || !m_loader) {
        return std::nullopt;
    }
    const std::optional<std::size_t> item = collection->itemOfPick(pickId);
    const std::optional<catalog::GameId> game = collection->gameOf(pickId);
    if (!item || !game) {
        return std::nullopt;
    }
    const std::optional<std::size_t> loaderItem = m_loader->itemOfGame(*game);
    if (!loaderItem) {
        return std::nullopt;
    }
    for (std::size_t room = 0; room < m_rooms.size(); ++room) {
        if (m_rooms[room].collection.get() == collection) {
            return FloorItem{room, *item, *loaderItem};
        }
    }
    return std::nullopt;
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
    CoverView view = CoverView::of(m_host.camera(), screenshotAspect);
    // ADR-0013 §9: the view is the items of the room that holds the eye and of the rooms whose doorway the frustum
    // meets; a floor of one room sees it whole (as I3's room). The loader's groups are the rooms' indices.
    if (m_rooms.size() > 1) {
        std::vector<core::Aabb> doorways;
        doorways.reserve(m_rooms.size());
        for (const FloorRoom& room : m_rooms) {
            doorways.push_back(room.doorway);
        }
        view.groups = coverViewGroups(view, doorways, eyeRoom());
    }
    return view;
}

void WorldSession::syncCollections() {
    m_collections.clear();
    m_groups.clear();
    for (const FloorRoom& room : m_rooms) {
        m_collections.push_back(room.collection.get());
        if (room.collection) {
            for (const gl::StaticInstanceGroup* group : room.collection->groups()) {
                m_groups.push_back(group);
            }
        }
    }
    // I6 step 5: the hub's showcase (its own loader serves it: not one of the floor loader's collections).
    if (m_showcase && !m_showcaseLayout.items.empty()) {
        for (const gl::StaticInstanceGroup* group : m_showcase->groups()) {
            m_groups.push_back(group);
        }
    }
}

void WorldSession::coverRound(ProductUi& product, const gl::UploadBudget& budget, bool frame) {
    if (m_rooms.empty() || !m_loader) {
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
            pinned.push_back(m_loader->items()[m_heroShown->loaderItem].game.toString());
        }
        heroMs = m_heroes->update(paused, pinned);
    }
    const bool wasDone = m_loader->done();
    CoverLoader::Round round;
    round.view = currentView();
    round.focused = m_heroItem ? std::optional<std::size_t>(m_heroItem->loaderItem) : std::nullopt;
    round.paused = paused;
    round.budget = budget;
    round.spent =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(heroMs));
    round.frame = frame;
    round.frameIndex = m_context.frameIndex();
    if (const Status updated = m_loader->update(m_collections, round); !updated) {
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
    m_heroItem = floorItemOf(pickId);
    showHero();
}

void WorldSession::showHero() {
    if (m_rooms.empty()) {
        return;
    }
    const gl::Texture2D* texture = nullptr;
    if (m_heroItem && m_heroes && m_loader) {
        const CoverItem& item = m_loader->items()[m_heroItem->loaderItem];
        m_heroes->request(item);
        texture = m_heroes->find(item.game);
    }
    const std::optional<FloorItem> shown = texture != nullptr ? m_heroItem : std::nullopt;
    if (shown != m_heroShown) {
        if (m_heroShown && m_rooms[m_heroShown->room].collection) {
            if (Status restored = m_rooms[m_heroShown->room].collection->setFrontHidden(m_heroShown->item, false);
                !restored) {
                spdlog::error("World: the hero's front: {}", restored.error().toString());
            }
        }
        if (shown && m_rooms[shown->room].collection) {
            if (Status hidden = m_rooms[shown->room].collection->setFrontHidden(shown->item, true); !hidden) {
                spdlog::error("World: the hero's front: {}", hidden.error().toString());
            }
        }
        m_heroShown = shown;
    }
    // The hero material of the room that shows the hero texture holds it; the room that showed it before lets it go.
    const std::optional<std::size_t> textureRoom = shown ? std::optional<std::size_t>(shown->room) : m_heroTextureRoom;
    const unsigned textureId = texture != nullptr ? texture->id() : 0u;
    if (m_heroTextureRoom && textureRoom != m_heroTextureRoom && m_rooms[*m_heroTextureRoom].collection) {
        static_cast<void>(m_graphics.materials().setTextureOverride(
            m_rooms[*m_heroTextureRoom].collection->heroMaterial(), gl::MaterialTextureSlot::BaseColor, 0u));
        m_heroTexture = 0;
    }
    m_heroTextureRoom = textureRoom;
    if (textureRoom && textureId != m_heroTexture && m_rooms[*textureRoom].collection) {
        if (Status set = m_graphics.materials().setTextureOverride(m_rooms[*textureRoom].collection->heroMaterial(),
                                                                   gl::MaterialTextureSlot::BaseColor, textureId);
            !set) {
            spdlog::error("World: the hero texture: {}", set.error().toString());
        }
        m_heroTexture = textureId;
    }
    m_dynamicItems.clear();
    if (shown && m_rooms[shown->room].collection) {
        // Its attributes as the front record has them now (the hover's highlight), drawn by the dynamic path.
        CollectionInstance& collection = *m_rooms[shown->room].collection;
        if (auto item = collection.frontRenderItem(shown->item, collection.heroMaterial())) {
            m_dynamicItems.push_back(*item);
        }
    }
}

bool WorldSession::settled(bool all) const {
    if (m_loader && !(all ? m_loader->done() : m_loader->viewDone())) {
        return false;
    }
    if (!showcaseSettled()) {
        return false;
    }
    if (m_heroItem && m_heroes && m_loader && m_heroes->loading(m_loader->items()[m_heroItem->loaderItem].game)) {
        return false;
    }
    return true;
}

void WorldSession::settle(ProductUi& product) {
    if (!m_options.lockstep || m_rooms.empty() || !m_loader) {
        return;
    }
    const std::optional<std::uint32_t> limit = m_context.options().frameLimit;
    const bool lastFrame = limit && m_context.frameIndex() + 1 >= *limit;
    // update() ranked the loader with the frame's view (the walker had moved), interact() asked for the hero texture.
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
        followShowcase(product);
        showcaseRound(product, kIdleUploadBudget, false);
        showHero();
    }
    m_lockstepWaitMs += millisecondsSince(start);
}

std::vector<std::string> WorldSession::statusLines() const {
    if (m_rooms.empty() || !m_loader) {
        return {};
    }
    std::vector<std::string> lines = m_loader->statusLines(m_collections);
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
        spdlog::info("World: the test room instead of the generated building: {}", result.reason);
        dropCollection();
        if (m_state != "test-room") {
            if (Status shown = m_host.showTestRoom(); !shown) {
                spdlog::error("World: the test room cannot be shown: {}", shown.error().toString());
            }
        }
        m_state = "test-room";
        m_record.reason = result.reason;
        m_record.catalogState = result.catalogState;
        setNotice(WorldNoticeContent{result.notice, result.catalogState, noticeRoom()}, true);
        m_record.state = m_state;
        return;
    }
    if (!result.build) {
        return;
    }
    WorldBuild& build = *result.build;
    recordBuild(build);
    if (build.startRoomMissing) {
        // ADR-0013 §9: a start room the building lacks ends the run (exit 2), the shell shown meanwhile.
        m_context.failRun(Error{ErrorCode::NotFound, "the building has no such room",
                                "--start-room=" + build.startRoom->toString()},
                          ExitCode::InitializationFailed);
        m_state = "shell";
        m_record.state = m_state;
        return;
    }
    if (!build.plan || !build.scene) {
        // Nothing to show: the shell stays, with its notice for as long as it shows.
        m_state = "shell";
        setNotice(WorldNoticeContent{build.notice.empty() ? std::string(world_notice::kFailed) : build.notice,
                                     {},
                                     noticeRoom()},
                  false);
        m_record.state = m_state;
        return;
    }
    if (!showFloor(build, false)) {
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
    m_record.migratedRooms.clear();
    m_record.replacedRooms.clear();
    m_record.needsConfirmation.clear();
    if (build.changes) {
        m_record.changeOutcome = std::string(world::toString(build.changes->outcome));
        m_record.rebuild = std::string(world::toString(build.changes->rebuild));
        m_record.added = build.changes->added.size();
        m_record.kept = build.changes->kept;
        for (const world::MigratedRoom& migrated : build.changes->migratedRooms) {
            m_record.migratedRooms.push_back(
                {migrated.room.toString(), migrated.from, migrated.to, migrated.unplacedForCapacity});
        }
        for (const world::ReplacedRoom& replaced : build.changes->replacedRooms) {
            m_record.replacedRooms.push_back({replaced.room.toString(), replaced.from.xMm, replaced.from.zMm,
                                              replaced.to.xMm, replaced.to.zMm});
        }
        m_record.needsConfirmation = build.changes->needsConfirmation;
    }
    m_record.planDigest = build.planDigest;
    m_record.planWritten = build.written;
    m_record.saveError = build.saveError ? build.saveError->toString() : std::string();
    m_record.hardFindings = build.hardFindings.size();
    m_record.softFindings = build.softFindings;
    m_record.buildMs = build.milliseconds;
    if (!build.preview) {
        WorldJobRecord job;
        job.lockMs = build.lockMs;
        job.selectMs = build.selectMs;
        job.dataMs = build.dataMs;
        job.planMs = build.planMs;
        job.saveMs = build.saveMs;
        job.itemsMs = build.itemsMs;
        job.floorMs = build.floorMs;
        job.totalMs = build.milliseconds;
        job.skipped = build.skipped;
        m_record.job = job;
    }
    if (build.mediaRootPath) {
        m_mediaRootPath = build.mediaRootPath; // the cover loader's retry after a refresh (I3 step 12)
    }
    m_record.itemsWritten = build.itemsWritten;
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

bool WorldSession::showFloor(WorldBuild& build, bool keepWalker, std::optional<WalkerKeptRecord>* walker,
                             bool atCamera) {
    // I3 step 15b: a floor committed again over the one shown (the walker kept) keeps its items' pictures: each room's
    // arrays pass to its new collection and the cover loader's work is adopted, so only new or changed pictures go up.
    // I4 step 6b (ADR-0013 §7): the walker (and a Focus or Inspect pose) stays in the frame of the area that holds it,
    // moved with it (a room grown, the start room grown under the scene's frame).
    std::optional<FrameMove> walkerMoved;
    if (keepWalker && m_shown) {
        const std::string area = m_host.eyeArea ? m_host.eyeArea() : std::string();
        walkerMoved = walkerMove(*m_shown, build, area);
        if (walker != nullptr) {
            WalkerKeptRecord kept;
            kept.area = area;
            if (walkerMoved) {
                kept.shift = glm::vec3(walkerMoved->matrix[3]);
                kept.halfTurn = walkerMoved->halfTurn;
            }
            *walker = kept;
        }
        if (walkerMoved) {
            spdlog::info("World: the walker moves with {} ({:.3f}, {:.3f}, {:.3f}){}", area, walkerMoved->matrix[3].x,
                         walkerMoved->matrix[3].y, walkerMoved->matrix[3].z,
                         walkerMoved->halfTurn ? ", half a turn" : "");
        }
    }
    std::map<world::RoomId, CollectionPictures> pictures;
    std::unique_ptr<CoverLoader> shownLoader;
    if (keepWalker) {
        for (FloorRoom& room : m_rooms) {
            if (room.collection) {
                pictures.emplace(room.id, room.collection->takePictures());
            }
        }
        shownLoader = std::move(m_loader);
    }
    dropCollection();
    const std::size_t objects = build.scene->objects.size();
    const std::size_t lights = build.scene->lights.size();
    SceneShow show;
    show.keepWalker = keepWalker;
    show.walkerMove = walkerMoved;
    show.atCamera = atCamera;
    // The hero CRT: the bound room's (the start room's at first, then the eye's: I4 step 6b) when it is on this floor,
    // with its games; none (the lobby) until the eye stays in a room.
    const std::optional<std::string> screenRoom = floorScreenRoom(build);
    if (!screenRoom) {
        m_screenRooms.reset(std::nullopt);
    }
    std::vector<ScreenGame> screenGames;
    if (screenRoom) {
        for (const RoomCollection& room : build.rooms) {
            if (room.room.toString() == *screenRoom) {
                screenGames = room.screenGames;
            }
        }
    }
    show.heroScreenPrefix = screenRoom ? *screenRoom + "/" : std::string();
    show.screenGames = screenGames;
    if (Status shown = m_host.showScene(std::move(*build.scene), show); !shown) {
        spdlog::error("World: the floor's scene cannot be shown: {}; the test room instead", shown.error().toString());
        if (Status fallback = m_host.showTestRoom(); !fallback) {
            spdlog::error("World: the test room cannot be shown: {}", fallback.error().toString());
        }
        m_state = "test-room";
        m_record.state = m_state;
        setNotice(WorldNoticeContent{std::string(world_notice::kFailed), {}, noticeRoom()}, true);
        return false;
    }
    m_state = "room";
    countPlan(build, m_record);
    m_record.objects = objects;
    m_record.lights = lights;
    m_record.undressed = build.undressed.size();
    m_record.floor = build.floor;
    m_record.rooms.clear();
    if (build.plan) {
        for (const world::Room* room : world::roomsOnFloor(*build.plan, build.floor)) {
            m_record.rooms.push_back(WorldRoomRecord{room->id.toString(), room->floor, room->placement.xMm,
                                                     room->placement.zMm, room->placement.yawHalfTurns});
        }
    }
    m_record.items = 0;
    m_record.clusters = 0;
    m_record.skipped = 0;
    m_record.staticRecords = 0;
    m_record.pickTargets = 0;

    // A collection per room (a room of no title, every game without a cover say, has its static part alone), and the
    // hover, the pull-out and Focus over each (I3 step 13a: its pick targets and navigation; a copy, as the persisted
    // start keeps the shown build's layouts to reconcile them, I3 step 10c).
    std::size_t interactionRooms = 0;
    for (RoomCollection& room : build.rooms) {
        FloorRoom floorRoom;
        floorRoom.id = room.room;
        floorRoom.doorway = room.doorway;
        m_record.items += room.layout.items.size();
        m_record.clusters += room.layout.clusters.size();
        m_record.skipped += room.layout.skipped.size();
        if (!room.layout.items.empty()) {
            std::optional<CollectionPictures> kept;
            if (auto found = pictures.find(room.room); found != pictures.end()) {
                kept = std::move(found->second);
                pictures.erase(found);
            }
            auto collection =
                CollectionInstance::create(room.layout, room.sceneData, m_graphics,
                                           m_context.device().caps().uniformBufferOffsetAlignment, std::move(kept));
            if (collection) {
                floorRoom.collection = std::move(*collection);
                m_record.staticRecords += floorRoom.collection->group().layout().instances().size();
            } else {
                spdlog::error("World: the collection of {} cannot be drawn: {}", room.room.toString(),
                              collection.error().toString());
            }
        }
        // The interaction's rooms are those with a collection, in the floor's order.
        if (floorRoom.collection) {
            floorRoom.interactionRoom = interactionRooms++;
            m_record.pickTargets += m_interaction->addRoom(room.layout, floorRoom.collection.get());
        }
        m_rooms.push_back(std::move(floorRoom));
    }
    syncCollections();
    const std::optional<std::size_t> eye = eyeRoom();
    m_interaction->setActiveRoom(eye ? m_rooms[*eye].interactionRoom : std::nullopt);
    if (!m_groups.empty()) {
        m_card.bind();
    }
    startCoverLoader(build, std::move(shownLoader));
    startShowcase(build); // I6 step 5: the hub's favourites showcase, its content asked for
    keepShown(build); // I3 step 15b: the next live update compares with it
    // The hero CRT of the scene just shown plays its room's games (bound with them by the scene's show).
    m_record.screenGames = screenGames.size();
    if (m_host.setScreenGames && screenRoom) {
        m_host.setScreenGames(std::move(screenGames));
    }
    m_shownAt = std::chrono::system_clock::now();
    return true;
}

void WorldSession::startCoverLoader(const WorldBuild& build, std::unique_ptr<CoverLoader> previous) {
    // The hero item and the one shown are indices of the loader replaced (FloorItem::loaderItem): the new loader's
    // list may be ordered and sized otherwise (only the items whose text is known have covers), so the hero's front
    // goes back to the static path and the next frame's interact() picks the hero again from the new list.
    releaseHero();
    if (!previous) {
        previous = std::move(m_loader);
    }
    m_loader.reset();
    auto cacheRoot = assets::textureCacheRoot(m_options.paths.cacheDir, build.library.str());
    if (!cacheRoot) {
        // The items keep the pending picture (nothing is written outside the user data directory's cache).
        spdlog::error("World: the texture cache: {}; the covers are not loaded", cacheRoot.error().toString());
        return;
    }
    // The floor's items, room after room: each cover with its item's box in the scene and its room (the loader's group).
    std::vector<CoverItem> covers;
    std::vector<core::Aabb> bounds;
    std::vector<std::uint32_t> groups;
    for (std::size_t index = 0; index < m_rooms.size() && index < build.rooms.size(); ++index) {
        if (!m_rooms[index].collection) {
            continue;
        }
        const RoomCollection& room = build.rooms[index];
        for (const CoverItem& cover : room.covers) {
            const world::CollectionItem* item = room.layout.findGame(cover.game);
            if (item == nullptr) {
                continue;
            }
            covers.push_back(cover);
            bounds.push_back(item->worldBounds());
            groups.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (covers.empty()) {
        return;
    }
    CoverLoader::Options loader;
    loader.assetRoot = m_options.paths.assetRoot;
    loader.cacheRoot = std::move(*cacheRoot);
    loader.format = m_graphics.collectionArrayFormat();
    loader.mediaRoot = build.mediaRoot;
    loader.wake = m_context.wakeCallback();
    // Each item's box on the floor: the loader ranks by it (in view, projected size, distance).
    m_loader = std::make_unique<CoverLoader>(std::move(loader), m_pool, std::move(covers), std::move(bounds),
                                             std::move(groups));
    if (previous) {
        // The same collections served again (I3 step 10c's textures reconciliation): only the pictures that changed
        // go up again; an unchanged room's items, their pictures showing, are done as they are (I4 step 6b).
        m_loader->adoptShown(*previous, m_collections);
        previous.reset();
    }
    if (!m_heroes) {
        HeroTextures::Options heroes;
        heroes.wake = m_context.wakeCallback();
        heroes.anisotropy = gl::clampAnisotropy(8.0f, m_context.device().caps().maxAnisotropy);
        m_heroes = std::make_unique<HeroTextures>(std::move(heroes), m_pool);
        m_heroesKnowRoot = false;
    }
    if (build.mediaRoot) {
        m_heroes->setMediaRoot(build.mediaRoot);
        m_heroesKnowRoot = true;
    }
    // The coverLoad phase opens after the floor's first two frames (update()): the frame that commits it and the next,
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
    setNotice(WorldNoticeContent{build.notice, {}, noticeRoom()}, true);
    spdlog::info("Frame {}: the room is shown: the floor {} ({} object(s), {} light(s), {} item(s) in {} cluster(s) "
                 "of {} collection(s)); committed in {:.1f} ms",
                 m_context.frameIndex(), build.floor, m_record.objects, m_record.lights, m_record.items,
                 m_record.clusters, m_groups.size(), *m_record.commitMs);
}

InteractionResult WorldSession::interact(const WorldHover::Frame& frame) {
    if (m_edit) {
        return editFrame(frame); // I4b step 3: the editor reads the frame instead (world_edit.cpp)
    }
    // The interaction works in the room that holds the eye (ADR-0013 §9: a pick index per room; none in a hall).
    if (!m_rooms.empty()) {
        const std::optional<std::size_t> eye = eyeRoom();
        m_interaction->setActiveRoom(eye ? m_rooms[*eye].interactionRoom : std::nullopt);
    }
    InteractionResult result = m_interaction->update(frame);
    hoverShowcase(frame); // I6 step 5
    // The hero item (I3 step 12): the focused item (Focus, Inspect), else the hovered one.
    const WorldInteraction& interaction = m_interaction->interaction();
    setHeroItem(interaction.focused() != world::kNoPick ? interaction.focused() : interaction.hovered());
    return result;
}

std::span<const scene::RenderItem> WorldSession::viewItems(std::span<const scene::RenderItem> sceneItems,
                                                           const scene::Camera& camera) {
    if (m_edit) {
        return editViewItems(sceneItems, camera); // I4b step 3
    }
    // The held item's front shows the hero texture when it is the hero item shown (I3 step 13b's record).
    CollectionInstance* active = activeCollection();
    const bool heroInActive =
        m_heroShown && active != nullptr && m_rooms[m_heroShown->room].collection.get() == active;
    const world::PickId heroPick = heroInActive ? active->pickOf(m_heroShown->item) : world::kNoPick;
    m_interaction->inspection().setHeroFront(heroPick != world::kNoPick &&
                                             heroPick == m_interaction->interaction().focused());
    const std::span<const scene::RenderItem> items = m_interaction->viewItems(sceneItems, camera);
    // The hero texture's front at the item's static pose; a pulled hero item's front travels with its dynamic copy. I5
    // step 4: a cabinet showing its clip draws its front with its video material instead (world_cabinets.cpp).
    const bool heroPulled = heroInActive && m_interaction->isPulled(heroPick);
    const bool heroVideo = m_heroShown && m_rooms[m_heroShown->room].collection &&
                           m_rooms[m_heroShown->room].collection->videoFront(m_heroShown->item).has_value();
    const bool heroFront = !m_dynamicItems.empty() && !heroPulled && !heroVideo;
    if (!heroFront && m_cabinetFronts.empty()) {
        return items;
    }
    m_viewItems.assign(items.begin(), items.end());
    if (heroFront) {
        m_viewItems.insert(m_viewItems.end(), m_dynamicItems.begin(), m_dynamicItems.end());
    }
    checkCabinetFronts();
    appendCabinetFronts(m_viewItems);
    return m_viewItems;
}

WorldSession::ScreenSelections
WorldSession::screenSelections(const std::optional<std::string>& catalogSelection) const {
    // ADR-0013 §9: the quick catalog's selection plays only when its game stands in the eye's room, whose CRT is the
    // one bound (I4 step 6b); else the CRT keeps what it plays.
    std::optional<std::string> catalog;
    if (catalogSelection && !m_rooms.empty()) {
        const std::optional<std::size_t> eye = eyeRoom();
        const std::optional<std::string>& bound = m_screenRooms.bound();
        if (eye && bound && m_rooms[*eye].id.toString() == *bound && m_shown) {
            const ShownCollection* room = m_shown->find(m_rooms[*eye].id);
            const auto game = catalog::GameId::parse(*catalogSelection);
            if (room != nullptr && game && room->layout.findGame(*game) != nullptr) {
                catalog = catalogSelection;
            }
        }
    } else {
        catalog = catalogSelection; // the test room, a scene of no building
    }
    // I5 step 4's review: the two apart, so that the arcade's hero cabinet follows the quick catalog's alone.
    return ScreenSelections{
        app::screenSelection(std::nullopt, m_interaction->interaction().state(), m_interaction->focusedGame()),
        std::move(catalog)};
}

std::optional<CardAction> WorldSession::cardPress(const input::ActionState* actions, ProductUi& product) {
    return m_card.press(m_interaction->interaction(), actions, product, m_context.frameIndex());
}

void WorldSession::showCard(const InteractionResult& result, ProductUi& product) {
    const WorldInteraction& interaction = m_interaction->interaction();
    const world::CollectionItem* item = m_interaction->item(interaction.focused());
    const gl::Texture2D* hero = item != nullptr && m_heroes ? m_heroes->find(item->game) : nullptr;
    // The shape its hero picture was made at (its room's family's label, ADR-0013 §4).
    assets::LabelAspect coverShape;
    if (item != nullptr && m_loader) {
        if (const std::optional<std::size_t> index = m_loader->itemOfGame(item->game)) {
            coverShape = m_loader->items()[*index].label;
        }
    }
    std::uint32_t others = 0;
    if (item != nullptr) {
        if (const auto found = m_otherVersions.find(item->game.value()); found != m_otherVersions.end()) {
            others = found->second;
        }
    }
    // I4b step 3: "Sposta" while the item's room is the one the editor can edit now (ADR-0015 §4: not the showcase, a
    // hall, during a launch or with a floor on its way).
    bool movable = false;
    if (item != nullptr && interaction.state() == WorldState::Inspect) {
        const std::optional<world::RoomId> room = m_interaction->activeRoomId();
        const std::optional<std::string> editable = editableRoom(product);
        movable = room && editable && *editable == room->toString();
    }
    m_card.update(interaction, result, item, hero, coverShape, others, movable, product, m_context.frameIndex());
    // Vedi media and Informazioni of the inspected game (I3 step 14b): the product UI's screens, over the card.
    if (item != nullptr && result.showMedia) {
        product.openGameMedia(item->game, item->title, kScreenFromCard);
    } else if (item != nullptr && result.showInfo) {
        product.openGameInfo(item->game, item->title, kScreenFromCard);
    }
}

void WorldSession::beforeInteraction(ProductUi& product, const scene::WalkerPose& walker) {
    // I4 step 7b: the directory's commands and "Mostra nella stanza" taken where they lead (world_ride.cpp).
    followCommands(product);
    m_launch.beforeInteraction(product, *m_interaction, walker, m_context.frameIndex());
}

void WorldSession::afterInteraction(const InteractionResult& result, ProductUi& product,
                                    const scene::WalkerPose& walker) {
    m_launch.afterInteraction(result, product, *m_interaction, walker, m_context.frameIndex());
    // I4b step 3: "Sposta" on the card: the editor with the item held.
    if (result.editMove && !m_edit) {
        const world::CollectionItem* item = m_interaction->item(*result.editMove);
        const std::optional<world::RoomId> room = m_interaction->activeRoomId();
        if (item != nullptr && room) {
            beginEdit(product, *room, "card", item->game);
        }
    }
}

std::optional<WorldLabelContent> WorldSession::label(const scene::Camera& camera, glm::ivec2 viewSize) {
    if (std::optional<WorldLabelContent> label = m_interaction->labelFor(camera, viewSize)) {
        return label;
    }
    // I6 step 5: a proxy of the hub's showcase hovered.
    const world::CollectionItem* item = m_showcaseLayout.find(m_showcaseHovered);
    if (item == nullptr || viewSize.x <= 0 || viewSize.y <= 0) {
        return std::nullopt;
    }
    scene::Camera view = camera;
    view.setAspect(static_cast<float>(viewSize.x) / static_cast<float>(viewSize.y));
    const auto anchor = labelAnchor(item->worldBounds(), view.viewProjection(), viewSize, world_label::kTopMargin);
    if (!anchor) {
        return std::nullopt;
    }
    return WorldLabelContent{item->title, true, anchor->x, anchor->y};
}

void WorldSession::frameRendered() {
    if (m_waitSince && Clock::now() - *m_waitSince >= kLockstepWaitLimit && !m_waitWarned) {
        spdlog::warn("Frame {}: the building did not come within {} s; frames go on without it",
                     m_context.frameIndex(), kLockstepWaitLimit.count());
        m_waitWarned = true;
    }
    m_waitSince.reset();
    m_nextFrame = m_context.frameIndex() + 1;
    editFrameRendered();         // I4b step 3
    regenerationFrameRendered(); // I4b step 6
    if (std::exchange(m_previewAwaitingFrame, false) && m_record.preview) {
        // The stored plan's floor as its first frame showed it (I3 step 10c); the building's result may come now.
        std::uint64_t fronts = 0;
        std::uint64_t spines = 0;
        for (const CollectionInstance* collection : m_collections) {
            if (collection != nullptr) {
                fronts += collection->arrays().residentLayers(gl::CollectionClass::Front);
                spines += collection->arrays().residentLayers(gl::CollectionClass::Spine);
            }
        }
        m_record.preview->frontsResident = fronts;
        m_record.preview->spinesResident = spines;
    }
}

bool WorldSession::waits() const {
    if (!m_options.lockstep) {
        return false;
    }
    // Before the run's last frame the whole floor, before the others the frame's view (ADR-0011 §6). I4 step 7b: a
    // ride's destination being built once the view faded out, then the eye's area on it (the waits replace the ride's
    // timeout), and a building's result built again for the floor shown (once built, it waits for a launch's end: no
    // reason to wait).
    const std::optional<std::uint32_t> limit = m_context.options().frameLimit;
    const bool lastFrameNext = limit && m_nextFrame + 1 >= *limit;
    const bool ridePending =
        m_ride && (m_ride->awaitsFloor() || (m_ride->phase() == RidePhase::AwaitingView && !rideViewResident()));
    // A result that waits for the ride to end is no reason to wait.
    const bool flowPending =
        m_flow && m_flow->phase() != WorldPhase::Done && !(m_ride && m_flow->phase() == WorldPhase::Ready);
    const bool pending = m_decided.has_value() || flowPending || (!m_rooms.empty() && !settled(lastFrameNext)) ||
                         ridePending || (m_retargetPending && !m_retargetResult) || !showcaseSettled() || editWaits() ||
                         regenerationWaits();
    if (!pending) {
        return false;
    }
    // The stored plan's floor, its layers in, is shown by a frame before its reconciliation is waited for (I3 step
    // 10c).
    if (m_previewAwaitingFrame && (m_rooms.empty() || settled(lastFrameNext))) {
        return false;
    }
    return !m_waitSince || Clock::now() - *m_waitSince < kLockstepWaitLimit;
}

void WorldSession::dropCollection() {
    dropCabinetFronts(); // I5 step 4: the cabinets' fronts go with their collections
    m_coverPhasePending = false;
    m_dynamicItems.clear();
    m_heroItem.reset();
    m_heroShown.reset();
    m_heroTexture = 0;
    m_heroTextureRoom.reset();
    m_heroes.reset();
    m_loader.reset();
    m_interaction->clear();
    m_rooms.clear();
    dropShowcase(); // I6 step 5
    syncCollections();
    m_shown.reset();
    m_launch.setPlan(nullptr);
    // I4 step 7b: no floor shown (the plan's state stays for the next one: keepShown()).
    m_floor.clear();
    m_elevatorZone = core::Aabb{};
    m_elevatorDoors = glm::vec3(0.0f);
    m_totemZone = core::Aabb{};
    m_totemScreen = glm::vec3(0.0f);
    m_doorways.clear();
    if (std::exchange(m_coverPhaseOpen, false)) {
        m_context.frameTiming().closePhase(kPhaseCoverLoad);
    }
}

std::vector<RoomUpdateRecord> WorldSession::roomRecords(const LiveUpdatePlan& plan, bool rebuilt) {
    std::vector<RoomUpdateRecord> records;
    for (const RoomUpdate& room : plan.rooms) {
        RoomUpdateRecord record;
        record.room = room.room.toString();
        const bool own = room.kind == RoomUpdateKind::Added || room.kind == RoomUpdateKind::Removed;
        record.kind = rebuilt && !own ? std::string("rebuilt") : std::string(toString(room.kind));
        record.kept = room.difference.kept;
        record.moved = room.difference.moved;
        record.flagsChanged = room.difference.flagsChanged;
        record.added = room.difference.added;
        record.removed = room.difference.removed;
        record.coversChanged = room.coversChanged;
        records.push_back(std::move(record));
    }
    return records;
}

void WorldSession::record(RunReport& report) const {
    WorldRecord world = m_record;
    if (m_edit) {
        world.edits.push_back(m_edit->record); // I4b step 3: a session still open at the run's end
    }
    world.state = m_state;
    world.screenRoom = m_screenRooms.bound().value_or(std::string());
    // The room that holds the eye at the end (I4 step 5a), "" in a hall or before a floor showed.
    world.room.clear();
    if (m_state == "room" && m_host.eyeArea) {
        const std::string area = m_host.eyeArea();
        for (const WorldRoomRecord& room : m_record.rooms) {
            if (room.id == area) {
                world.room = area;
            }
        }
    }
    // A ride the run ended in (I4 step 7b): its record so far.
    if (m_ride) {
        world.rides.push_back(m_ride->record());
    }
    world.hoverChanges = m_interaction->hoverChanges();
    world.showcase = m_showcaseRecord; // I6 step 5
    m_interaction->record(world.interaction);
    world.card = m_card.record();
    world.launch = m_launch.record();
    if (CollectionInstance* collection = activeCollection()) {
        if (const world::PickId hovered = collection->hovered(); hovered != world::kNoPick) {
            world.hoveredPickId = fmt::format("{:08X}", hovered);
            if (const std::optional<catalog::GameId> game = collection->gameOf(hovered)) {
                world.hoveredGame = game->toString();
            }
        }
    }
    report.world = std::move(world);
    if (!m_rooms.empty() && m_loader) {
        ResidencyRecord residency;
        m_loader->record(residency, m_collections);
        // Each collection room's own (ADR-0013 §9: every room samples its own arrays; the loader's groups are the
        // rooms' indices).
        for (std::size_t index = 0; index < m_rooms.size(); ++index) {
            const FloorRoom& room = m_rooms[index];
            if (!room.collection) {
                continue;
            }
            RoomResidencyRecord record;
            record.room = room.id.toString();
            record.items = room.collection->itemCount();
            record.viewItems = m_loader->viewItemsOf(static_cast<std::uint32_t>(index));
            const gl::CollectionArrays& arrays = room.collection->arrays();
            record.frontsResident = arrays.residentLayers(gl::CollectionClass::Front);
            record.spinesResident = arrays.residentLayers(gl::CollectionClass::Spine);
            record.pending = 2 * record.items -
                             std::min<std::uint64_t>(2 * record.items, record.frontsResident + record.spinesResident);
            record.arrayBytes = arrays.estimatedBytes();
            const CollectionArraysSampled sampled = room.collection->sampledArrays(m_graphics.materials());
            record.arrays = sampled.arrays;
            record.ownArrays = sampled.own;
            record.uploads = room.collection->uploads();
            record.reuploads = room.collection->reuploads();
            residency.rooms.push_back(std::move(record));
        }
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
                residency.hero.shownGame = m_loader->items()[m_heroShown->loaderItem].game.toString();
            }
        }
        report.residency = std::move(residency);
    }
}

} // namespace rv::app
