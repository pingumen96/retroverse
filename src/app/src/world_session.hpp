#pragma once

// The room in the app (I3 step 10b; ADR-0010 §12, ADR-0011 §1, §6, ADR-0012 §2, §3): what retroverse.exe shows without
// --scene, --model or --mock-catalog. While the room is made (app::WorldFlow: the stored plan read, the snapshot on the
// catalog worker, the plan and the scene on the worker pool) the empty room's shell shows with the "generating" notice
// (the WorldGenerating state); the result is committed at a frame boundary: the room's static part through the
// application's scene (SceneHost::showScene: a SceneInstance of its SceneDescription, the walker at its doorway
// camera), its collection on a static instance group (CollectionInstance) whose covers and spines the cover loader
// brings in within the upload budget, the pick index of its items (world::PickIndex) for the hover outline. A catalog
// that does not open or has no games of the room's system gives the test room (SceneHost::showTestRoom), a room that
// cannot be made now the shell, each with its notice. Lockstep runs (--fixed-dt) wait, at most kLockstepWaitLimit at a
// time, while the room is on its way and until the layers of every item in the frame's view are resident (every item
// before the run's last frame). Main thread only.
//
// The persisted start (I3 step 10c, world_start.cpp): with a plan stored, its room shows first, as it is (WorldFlow's
// preview: its covers and spines from the cache by the room's items of the last start), before the catalog's snapshot
// is even asked for; once a frame has shown it, the snapshot's result reconciles it (world_reconcile.hpp): an unchanged
// room moves and rebuilds nothing (its covers served again, the hero CRT given its games), a changed one is committed
// again with the walker where it stands, a plan that cannot be used gives the shell. A lockstep run's first frame shows
// the stored room's view whole, the next ones the reconciled room.
//
// The covers (I3 step 12) come in the cover loader's order (the hero item, the view by projected size, the rest by
// distance), the hero item's front shows its hero texture (drawn by the dynamic path), nothing is made or uploaded while
// a launched game runs, and a refresh of the catalog lets the loader try the media root again.
//
// The live update (I3 step 15b, world_live_update.cpp): after a refresh of the catalog the room's job runs again (the
// snapshot, world::update, the plan saved) while the room shows, and its result is applied at a frame boundary with as
// little rebuilt as it needs (world_live_plan.hpp): nothing, the covers served again, the collection's items applied in
// place (titles placed into their slots, the items that stood unmoved with their pictures, the scene and the walker
// untouched), or the room committed again with the walker kept. Never while a launched game runs.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/app/cover_requests.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/math.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/static_instance_group.hpp>
#include <rv/render/texture_array_layout.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/pick_index.hpp>

#include "world_notice_panel.hpp"

namespace rv::app {

class AppContext;
class CollectionInstance;
class CoverLoader;
class HeroTextures;
class ProductUi;
class SceneGraphics;

class WorldSession {
public:
    /// What the session asks of the application at a frame boundary.
    struct SceneHost {
        /// Shows `description` in place of the scene shown (its models from the asset root), the walker at its camera
        /// (or at --spawn). The session's collection is gone by then (the registries are cleared). Errors: the scene
        /// cannot be made.
        std::function<Status(scene::SceneDescription description)> showScene;
        /// showScene with the walker kept where it stands (I3 step 10c: the stored plan's room replaced by its
        /// reconciliation); showScene when unset.
        std::function<Status(scene::SceneDescription description)> showSceneKeepingWalker;
        /// Shows the test room (m0_testroom) in place of the scene shown.
        std::function<Status()> showTestRoom;
        /// The view camera of the frame (I3 step 12: the cover loader's priorities and the lockstep's view); without it
        /// the loader keeps the layout's order.
        std::function<const scene::Camera&()> camera;
        /// The games the hero CRT of the scene shown may play (I3 step 10c: the room's, after the snapshot; none for
        /// the stored plan's room shown before it); unset: nothing to do.
        std::function<void(std::vector<ScreenGame> games)> setScreenGames;
    };

    struct Options {
        WorldPaths paths;
        catalog::SystemName system;
        std::string writer;
        bool lockstep = false;
        /// When the process was created (FILETIME ticks): the run report's times count from it.
        std::uint64_t processCreated = 0;
    };

    /// How long a lockstep run waits between two frames, at most.
    static constexpr std::chrono::seconds kLockstepWaitLimit{10};

    /// The room of the run: made by a flow on `pool` (`decided` set: no flow, that result at once, the test room the
    /// application already shows for want of a catalog).
    WorldSession(AppContext& context, Options options, core::WorkerPool& pool, SceneGraphics& graphics, SceneHost host,
                 std::optional<WorldResult> decided = std::nullopt);
    ~WorldSession();
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

    /// The empty room of the system's template (the shell shown while the room is made).
    [[nodiscard]] static Expected<scene::SceneDescription> shellScene(const std::filesystem::path& assetRoot,
                                                                      const catalog::SystemName& system);

    /// One step, after the product UI's update or idle wake (its catalog polled): the flow moved on, its result
    /// committed (the scene replaced: call it at a frame boundary), the notice updated at the run's time
    /// `timeSeconds`; and the covers' round (within the frame's budget, or a larger one in an idle wake).
    void update(ProductUi& product, bool idleWake, double timeSeconds);
    /// The hover, once per frame: `ray` from the reticle while the world has the input, nothing otherwise (the hover
    /// lets go at once). The hovered item is outlined.
    void hover(const std::optional<core::Ray>& ray);
    /// A frame was rendered (the lockstep wait starts again).
    void frameRendered();
    /// Lockstep runs (I3 step 12, ADR-0011 §6): after the frame's walker and hover moved, waits within the frame until
    /// every item in its view (and the hero texture of the focused item) is resident, every item of the room before
    /// the run's last frame, at most kLockstepWaitLimit; nothing in other runs.
    void settle(ProductUi& product);
    /// The item whose hero texture the room shows in place of its front (I3 step 12: the hovered one; step 13a's
    /// focus, pull-out and inspection set it): its pick id, or kNoPick.
    void setHeroItem(world::PickId pickId);
    /// What the dynamic path draws besides the scene's items this frame: the hero texture's front.
    [[nodiscard]] std::span<const scene::RenderItem> dynamicItems() const noexcept { return m_dynamicItems; }
    /// The Stats panel's "Collection" lines (nothing without a room).
    [[nodiscard]] std::vector<std::string> statusLines() const;

    /// The room's static groups for the view (none before its collection exists).
    [[nodiscard]] std::span<const gl::StaticInstanceGroup* const> staticGroups() const noexcept;
    /// A lockstep run must wait before its next frame: the room or its layers are on their way (within the limit).
    [[nodiscard]] bool waits() const;
    /// The collection goes (before the application replaces the scene and clears the registries).
    void dropCollection();

    /// The end of the run: the world and residency records.
    void record(RunReport& report) const;

private:
    using Clock = std::chrono::steady_clock;

    void commit(WorldResult result);
    /// The build's outcome, change report, plan and times into the record (logged).
    void recordBuild(const WorldBuild& build);
    /// Shows `build`'s room (it has its plan, scene, data and layout): the scene through the host (the walker kept
    /// where it stands with `keepWalker`), the collection, the pick index, the cover loader, the CRT's games. False
    /// when the scene could not be shown (the test room then, with its notice).
    [[nodiscard]] bool showRoom(WorldBuild& build, bool keepWalker);
    /// A cover loader for `covers` on the collection shown, in place of any (none without a collection or covers).
    void startCoverLoader(std::vector<CoverItem> covers, const std::optional<std::filesystem::path>& mediaRoot,
                          const catalog::LibraryId& library);
    /// The commit's times, state, notice and log.
    void finishCommit(const WorldBuild& build, Clock::time_point start);
    /// I3 step 10c, the persisted start (world_start.cpp): the stored plan's room shown before the snapshot answers,
    /// and the snapshot's result reconciled with it (world_reconcile.hpp).
    void commitPreview(WorldBuild preview);
    void reconcile(WorldResult result);
    /// The cover loader's and the hero textures' round: within `budget` (`frame`: a frame's round, not an idle wake's
    /// or a lockstep wait's), paused while a launched game runs.
    void coverRound(ProductUi& product, const gl::UploadBudget& budget, bool frame);
    /// The hero texture of the hero item shown in place of its front once it is resident (the dynamic item).
    void showHero();
    /// The frame's view for the loader (the window's camera, and the screenshot's aspect).
    [[nodiscard]] CoverView currentView() const;
    /// What a lockstep frame waits for is in: the loader's view (`all`: every item) and the hero item's texture.
    [[nodiscard]] bool settled(bool all) const;
    /// A refresh that ended since the last round lets the loader try the media root again.
    void followRefresh(ProductUi& product);
    /// Shows `notice` from the next showNotice() on: for world_notice::kShownSeconds of the run's time (`timed`), or
    /// until another replaces it.
    void setNotice(WorldNoticeContent notice, bool timed);
    /// I3 step 15b, the live update (world_live_update.cpp): a refresh ended (the room's job runs again once nothing
    /// else does), the job started, its result applied; what the room shows kept for the next comparison; the
    /// collection's items applied in place (false: not applicable, commit the room again); the hero's front given back
    /// to the static path before the loader is replaced.
    void requestLiveUpdate();
    void startLiveFlow(ProductUi& product);
    void applyLive(WorldResult result);
    void keepShown(const WorldBuild& build);
    [[nodiscard]] bool applyItems(WorldBuild& build, const LiveUpdatePlan& plan, WorldLiveUpdateRecord& record);
    void releaseHero();
    void showNotice(ProductUi& product, double timeSeconds);
    [[nodiscard]] std::optional<double> sinceProcessStart(std::chrono::system_clock::time_point at) const;

    AppContext& m_context;
    Options m_options;
    core::WorkerPool& m_pool;
    SceneGraphics& m_graphics;
    SceneHost m_host;
    std::unique_ptr<WorldFlow> m_flow;
    std::optional<WorldResult> m_decided;
    std::unique_ptr<CollectionInstance> m_collection;
    std::unique_ptr<CoverLoader> m_loader;
    std::unique_ptr<HeroTextures> m_heroes;
    bool m_heroesKnowRoot = false;
    /// The hero item (its index in the collection), the one whose front shows its hero texture now, the texture, the
    /// dynamic item that draws it.
    std::optional<std::size_t> m_heroItem;
    std::optional<std::size_t> m_heroShown;
    unsigned m_heroTexture = 0;
    std::vector<scene::RenderItem> m_dynamicItems;
    /// The media root the room's paths are relative to, also while it is out of reach (the loader's retry), and the
    /// last refresh seen.
    std::optional<std::filesystem::path> m_mediaRootPath;
    std::optional<std::string> m_lastRefresh;
    /// The next frame to render (lockstep: the run's last frame waits for the whole room), the frames that waited for
    /// their view and how long.
    std::uint64_t m_nextFrame = 0;
    std::uint64_t m_lockstepWaits = 0;
    double m_lockstepWaitMs = 0.0;
    bool m_settleWarned = false;
    world::PickIndex m_pickIndex;
    world::HoverTracker m_hover;
    /// What showed, the notice and when it was set (seconds of the run's time; it hides after its time unless the
    /// shell shows), the record.
    std::string m_state = "generating";
    WorldNoticeContent m_notice;
    std::optional<double> m_noticeSince;
    bool m_noticeTimed = false;
    WorldRecord m_record;
    std::optional<std::chrono::system_clock::time_point> m_shownAt;
    bool m_coverPhaseOpen = false;
    /// The coverLoad phase waits to open until the frame after m_coverPhaseAfter (commit(), update()).
    bool m_coverPhasePending = false;
    std::uint64_t m_coverPhaseAfter = 0;
    std::optional<std::chrono::steady_clock::time_point> m_waitSince;
    bool m_waitWarned = false;
    /// The stored plan's room shown (its plan, layout and cover requests; its scene given to the host), until the
    /// snapshot's result reconciles it; whether no frame has shown it yet (the result waits for one).
    std::optional<WorldBuild> m_shownPreview;
    bool m_previewAwaitingFrame = false;
    /// I3 step 15b: what the room shown is (none without a room), a live update asked for (a refresh ended) and when,
    /// and whether m_flow is a live update's.
    std::optional<ShownRoom> m_shown;
    bool m_liveRequested = false;
    std::uint64_t m_liveRequestFrame = 0;
    std::optional<Clock::time_point> m_liveRequestedAt;
    bool m_liveFlow = false;
};

/// The placements per section and the unplaced games per reason of `build`'s plan and layout, into `record`
/// (world_session.cpp; I3 step 15b's live update counts them again).
void countPlan(const WorldBuild& build, WorldRecord& record);

} // namespace rv::app
