#pragma once

// The building in the app (I3 step 10b's room, the floor since I4 step 5a; ADR-0010 §12, ADR-0011 §1, §6, ADR-0012 §2,
// §3, ADR-0013 §9): what retroverse.exe shows without --scene, --model or --mock-catalog. While the building is made
// (app::WorldFlow: the stored plan read, the building job on the worker pool, the floor's build) the empty shell shows
// with the "generating" notice (the WorldGenerating state); the result is committed at a frame boundary: the floor's
// static part through the application's scene (SceneHost::showScene: a SceneInstance of its SceneDescription, the walker
// at its camera: the elevator's arrival point, or --start-room's doorway camera), each collection room's collection on
// a static instance group of its own (CollectionInstance, its registry keys carrying the room id), one cover loader over
// the floor that brings their covers and spines in within the upload budget (the view: the room that holds the eye and
// those whose doorway the frustum meets), the hover, pull-out and Focus of the room that holds the eye (WorldHover, I3
// step 13a: a pick index per room). A catalog that does not open, or a library that gives no room, gives the test room
// (SceneHost::showTestRoom), a building that cannot be made now the shell, each with its notice. Lockstep runs
// (--fixed-dt) wait, at most kLockstepWaitLimit at a time, while the floor is on its way and until the layers of every
// item in the frame's view are resident (every item of the floor before the run's last frame). Main thread only.
//
// The hero CRT (ADR-0012 §7, ADR-0013 §9) is the CRT of the room that holds the eye, with that room's games (I4 step
// 6b: ScreenRoomTracker): bound from the start in --start-room's room, none in the lobby until the eye stays in a room;
// rebound without blocking the render loop once the eye has stayed in another room for the dwell (also the hysteresis
// at doorways), the last room's kept in a hall; never while a launched game runs. The quick catalog's selection plays
// only when its game stands in the eye's room.
//
// The persisted start (I3 step 10c, world_start.cpp): with a plan stored, its start floor shows first, as it is
// (WorldFlow's preview: its covers and spines from the cache by the rooms' items of the last start, its CRTs' games from
// them too), before the catalog is even read; once a frame has shown it, the building's result reconciles it room by
// room (I4 step 6b, world_reconcile.hpp, world_live_plan.hpp): each room by its content digest, an unchanged one keeping
// its collection and its pictures (nothing uploaded again), one whose items changed applied in place, one whose covers
// changed served again; a change of the floor's static part commits the floor again with the walker kept in the frame of
// the area that holds it, moved with it; a plan that cannot be used gives the shell. A lockstep run's first frame shows
// the stored floor's view whole, the next ones the reconciled floor.
//
// The covers (I3 step 12) come in the cover loader's order (the hero item, the view by projected size, the rest by
// distance), the hero item's front shows its hero texture (drawn by the dynamic path), nothing is made or uploaded while
// a launched game runs, and a refresh of the catalog lets the loader try the media root again.
//
// The live update (I3 step 15b, world_live_update.cpp): after a refresh of the catalog the building job runs again (the
// snapshots, the update, the plan saved) while the floor shows, and its result is applied at a frame boundary with as
// little rebuilt as it needs, room by room as at the start (world_live_plan.hpp): nothing, the covers of the rooms whose
// covers changed served again, the items of the rooms whose items changed applied in place (titles placed into their
// slots, the items that stood unmoved with their pictures, the scene and the walker untouched), or the floor committed
// again with the walker kept in its area's frame. The other floors are only saved. Never while a launched game runs: a
// result that arrives meanwhile applies after it.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <rv/app/cover_requests.hpp>
#include <rv/app/game_screens.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/app/screen_room.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/math.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/static_instance_group.hpp>
#include <rv/render/texture_array_layout.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/pick_index.hpp>

#include "world_card.hpp"
#include "world_hover.hpp"
#include "world_launch.hpp"
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
    /// How the application shows a scene for the session.
    struct SceneShow {
        /// The walker stays where it stands (I3 step 10c: the stored floor replaced by its reconciliation; a floor
        /// committed again over itself), else it stands at the scene's camera (or at --spawn).
        bool keepWalker = false;
        /// The hero CRT the scene's screen service binds: the first crt object whose id starts with this prefix ("" for
        /// none: a floor whose eye's room has no CRT bound yet); unset, the scene's first CRT (the shell, the test room).
        std::optional<std::string> heroScreenPrefix;
        /// With `keepWalker`: the move of the area that holds the walker (I4 step 6b, ADR-0013 §7: the walker kept in
        /// its frame); none: it stays where it stands.
        std::optional<FrameMove> walkerMove;
        /// The games the hero CRT binds with (its room's).
        std::vector<ScreenGame> screenGames;
    };

    /// What the session asks of the application at a frame boundary.
    struct SceneHost {
        /// Shows `description` in place of the scene shown (its models from the asset root). The session's collections
        /// are gone by then (the registries are cleared). Errors: the scene cannot be made.
        std::function<Status(scene::SceneDescription description, const SceneShow& show)> showScene;
        /// Shows the test room (m0_testroom) in place of the scene shown.
        std::function<Status()> showTestRoom;
        /// The view camera of the frame (I3 step 12: the cover loader's priorities and the lockstep's view); without it
        /// the loader keeps the layout's order.
        std::function<const scene::Camera&()> camera;
        /// The games the hero CRT of the scene shown may play (I3 step 10c: the start room's, after the building's
        /// result; none for the stored floor shown before it); unset: nothing to do.
        std::function<void(std::vector<ScreenGame> games)> setScreenGames;
        /// The area that holds the eye (SceneInstance::areaId(): a room's id, "hall:<floor>"; "" in a scene without
        /// areas): the room whose items the interaction picks and the cover loader's view (I4 step 5a).
        std::function<std::string()> eyeArea;
        /// Binds the hero CRT to the scene's CRT whose object id starts with `prefix` (a room's), with `games` (I4
        /// step 6b: the CRT of the eye's room); false when the scene has none.
        std::function<bool(const std::string& prefix, std::vector<ScreenGame> games)> bindScreen;
    };

    struct Options {
        WorldPaths paths;
        /// --start-room (I4 step 5a): the room the run starts in; none: the ground floor's lobby.
        std::optional<world::RoomId> startRoom;
        std::string writer;
        bool lockstep = false;
        /// When the process was created (FILETIME ticks): the run report's times count from it.
        std::uint64_t processCreated = 0;
    };

    /// How long a lockstep run waits between two frames, at most.
    static constexpr std::chrono::seconds kLockstepWaitLimit{10};

    /// The building of the run: made by a flow on `pool` (`decided` set: no flow, that result at once, the test room the
    /// application already shows for want of a catalog).
    WorldSession(AppContext& context, Options options, core::WorkerPool& pool, SceneGraphics& graphics, SceneHost host,
                 std::optional<WorldResult> decided = std::nullopt);
    ~WorldSession();
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

    /// The empty shell shown while the building is made (world_build.hpp's worldShellScene()).
    [[nodiscard]] static Expected<scene::SceneDescription> shellScene(const std::filesystem::path& assetRoot,
                                                                      const std::optional<world::RoomId>& startRoom);

    /// One step, after the product UI's update or idle wake (its catalog polled): the flow moved on, its result
    /// committed (the scene replaced: call it at a frame boundary), the notice updated at the run's time
    /// `timeSeconds`; and the covers' round (within the frame's budget, or a larger one in an idle wake).
    void update(ProductUi& product, bool idleWake, double timeSeconds);
    /// The world's interaction, once per frame before the walker moves (I3 step 13a: WorldHover::update in the room
    /// that holds the eye): the hover of the frame's ray, the pull-out, Focus, the world's Back levels and the quick
    /// catalog in the World context.
    [[nodiscard]] InteractionResult interact(const WorldHover::Frame& frame);
    /// The interaction's input context now (what the walker gets: walkerActionsFor()).
    [[nodiscard]] WorldContext context() const noexcept { return m_interaction->interaction().context(); }
    /// The view's render items: the scene's, the pulled items' parts (WorldHover::viewItems: the inspected one in the
    /// hand in front of `camera`, I3 step 13b), the hero texture's front of an item in its slot (dynamicItems()).
    [[nodiscard]] std::span<const scene::RenderItem> viewItems(std::span<const scene::RenderItem> sceneItems,
                                                               const scene::Camera& camera);
    /// The inspection's dimming of the frame's view (I3 step 13b: WorldInspection::apply).
    void applyInspection(gl::RenderView& view) { m_interaction->applyInspection(view); }
    /// The game the hero CRT follows (I3 step 13b, screenSelection()): `catalogSelection` (the quick catalog's, while
    /// it is open), else the item in Focus or Inspect.
    [[nodiscard]] std::optional<std::string> screenSelection(const std::optional<std::string>& catalogSelection) const;
    /// The card beside the inspected item (I3 step 14a, world_card.hpp): before interact(), what its input presses
    /// this frame (for WorldHover::Frame::card); after it, its content given to the product UI with the item's hero
    /// texture as its cover and the interaction's `result` recorded.
    [[nodiscard]] std::optional<CardAction> cardPress(const input::ActionState* actions, ProductUi& product);
    void showCard(const InteractionResult& result, ProductUi& product);
    /// Launching from the world and "Mostra nella stanza" (I3 step 15a, world_launch.hpp): before interact(), the quick
    /// catalog's room placement, a "Mostra nella stanza" taken to its item and a launch's end given to the
    /// interaction (`walker`: the walker as the frame starts); after it, Avvia asked of the product UI and the restore
    /// recorded (`walker`: where the frame leaves the walker).
    void beforeInteraction(ProductUi& product, const scene::WalkerPose& walker);
    void afterInteraction(const InteractionResult& result, ProductUi& product, const scene::WalkerPose& walker);
    /// The other versions of `game`'s title in the building's plan (I3 step 14b: Informazioni); none without one.
    [[nodiscard]] std::vector<catalog::GameId> otherVersionsOf(const catalog::GameId& game) const {
        return m_versionGroups.othersOf(game);
    }
    /// The hover's title label as `camera` shows it in a view of `viewSize` pixels (WorldHover::labelFor).
    [[nodiscard]] std::optional<WorldLabelContent> label(const scene::Camera& camera, glm::ivec2 viewSize);
    /// A frame was rendered (the lockstep wait starts again).
    void frameRendered();
    /// Lockstep runs (I3 step 12, ADR-0011 §6): after the frame's walker and hover moved, waits within the frame until
    /// every item in its view (and the hero texture of the focused item) is resident, every item of the floor before
    /// the run's last frame, at most kLockstepWaitLimit; nothing in other runs.
    void settle(ProductUi& product);
    /// The item whose hero texture the floor shows in place of its front (I3 step 12; interact() sets it each frame to
    /// the interaction's focused item, else its hovered one, in the interaction's room): its pick id, or kNoPick.
    void setHeroItem(world::PickId pickId);
    /// What the dynamic path draws besides the scene's items this frame: the hero texture's front.
    [[nodiscard]] std::span<const scene::RenderItem> dynamicItems() const noexcept { return m_dynamicItems; }
    /// The Stats panel's "Collection" lines (nothing without a collection).
    [[nodiscard]] std::vector<std::string> statusLines() const;

    /// The floor's static groups for the view, one per collection room (none before its collections exist).
    [[nodiscard]] std::span<const gl::StaticInstanceGroup* const> staticGroups() const noexcept { return m_groups; }
    /// A lockstep run must wait before its next frame: the floor or its layers are on their way (within the limit).
    [[nodiscard]] bool waits() const;
    /// The collections go (before the application replaces the scene and clears the registries).
    void dropCollection();

    /// The end of the run: the world and residency records.
    void record(RunReport& report) const;

private:
    using Clock = std::chrono::steady_clock;

    /// A collection room of the floor shown.
    struct FloorRoom {
        world::RoomId id;
        /// Its collection (none for a room of no item, or one that could not be drawn).
        std::unique_ptr<CollectionInstance> collection;
        /// Its doorway's passage in the scene's frame (the cover loader's view).
        core::Aabb doorway;
        /// Its index among the interaction's rooms (WorldHover::addRoom), if it has a collection.
        std::optional<std::size_t> interactionRoom;
    };

    /// An item of the floor: its room (an index into m_rooms), its index in the room's collection, its index among the
    /// cover loader's items (valid for that loader only: startCoverLoader() lets go of the hero items).
    struct FloorItem {
        std::size_t room = 0;
        std::size_t item = 0;
        std::size_t loaderItem = 0;

        bool operator==(const FloorItem&) const = default;
    };

    void commit(WorldResult result);
    /// The build's outcome, change report, plan and times into the record (logged).
    void recordBuild(const WorldBuild& build);
    /// Shows `build`'s floor (it has its plan and scene): the scene through the host (the walker kept where it stands
    /// with `keepWalker`, moved with the area that holds it, I4 step 6b), a collection per room (with `keepWalker` each
    /// taking its room's pictures), their pick indices, the cover loader, the bound room's CRT. False when the scene
    /// could not be shown (the test room then, with its notice). `walker`: where the walker was kept, for the record.
    [[nodiscard]] bool showFloor(WorldBuild& build, bool keepWalker,
                                 std::optional<WalkerKeptRecord>* walker = nullptr);
    /// A cover loader for `build`'s rooms' covers on the collections shown, in place of any (none without collections
    /// or covers); `previous` (else the loader replaced) is adopted. The hero item is let go first (releaseHero()): its
    /// loader index is the replaced loader's.
    void startCoverLoader(const WorldBuild& build, std::unique_ptr<CoverLoader> previous = nullptr);
    /// The commit's times, state, notice and log.
    void finishCommit(const WorldBuild& build, Clock::time_point start);
    /// I3 step 10c, the persisted start (world_start.cpp): the stored plan's floor shown before the building's result,
    /// and that result reconciled with it (world_reconcile.hpp).
    void commitPreview(WorldBuild preview);
    void reconcile(WorldResult result);
    /// The cover loader's and the hero textures' round: within `budget` (`frame`: a frame's round, not an idle wake's
    /// or a lockstep wait's), paused while a launched game runs.
    void coverRound(ProductUi& product, const gl::UploadBudget& budget, bool frame);
    /// The hero texture of the hero item shown in place of its front once it is resident (the dynamic item).
    void showHero();
    /// The frame's view for the loader (the window's camera, and the screenshot's aspect; the rooms it sees).
    [[nodiscard]] CoverView currentView() const;
    /// What a lockstep frame waits for is in: the loader's view (`all`: every item) and the hero item's texture.
    [[nodiscard]] bool settled(bool all) const;
    /// A refresh that ended since the last round lets the loader try the media root again.
    void followRefresh(ProductUi& product);
    /// Shows `notice` from the next showNotice() on: for world_notice::kShownSeconds of the run's time (`timed`), or
    /// until another replaces it.
    void setNotice(WorldNoticeContent notice, bool timed);
    /// I3 step 15b, the live update (world_live_update.cpp): a refresh ended (the building job runs again once nothing
    /// else does), the job started, its result applied; what the floor shows kept for the next comparison; each room's
    /// collection applied in place (false: not applicable, commit the floor again); the hero's front given back to the
    /// static path before the loader is replaced.
    void requestLiveUpdate();
    void startLiveFlow(ProductUi& product);
    void applyLive(WorldResult result);
    void keepShown(const WorldBuild& build);
    [[nodiscard]] bool applyItems(WorldBuild& build, const LiveUpdatePlan& plan, WorldLiveUpdateRecord& record);
    /// I4 step 6b: `plan`'s rooms (Unchanged, Textures, Items) applied to the floor shown, room by room: the Items rooms'
    /// collections in place, the cover loader again (adopting what showed: an unchanged room's pictures stay, nothing
    /// uploaded again) when some room's covers or items changed, the bound CRT's games when they changed. False when
    /// the items cannot be applied in place (commit the floor again).
    [[nodiscard]] bool applyRooms(WorldBuild& build, const LiveUpdatePlan& plan, WorldLiveUpdateRecord& record);
    /// The record of each room's part of `plan` (`rebuilt`: the floor was committed again around them).
    [[nodiscard]] static std::vector<RoomUpdateRecord> roomRecords(const LiveUpdatePlan& plan, bool rebuilt);
    /// I4 step 6b: the CRT of the room that holds the eye, once it stayed there (the dwell), at the run's time
    /// `timeSeconds`; never while a launched game runs.
    void followScreenRoom(ProductUi& product, double timeSeconds);
    /// The games of the CRT of `room` on the floor shown (none for a room it lacks).
    [[nodiscard]] std::vector<ScreenGame> screenGamesOf(const std::string& room) const;
    /// The room whose CRT a floor shown binds: the one bound when it is on the floor, else none ("" prefix).
    [[nodiscard]] std::optional<std::string> floorScreenRoom(const WorldBuild& build) const;
    void releaseHero();
    /// The interaction (I3 step 13a) lets go of the floor's items before they are applied in place: hover, Focus and
    /// pull-out reset, every pulled-out item's records shown in its slot again.
    void releaseInteraction();
    void showNotice(ProductUi& product, double timeSeconds);
    [[nodiscard]] std::optional<double> sinceProcessStart(std::chrono::system_clock::time_point at) const;
    /// The room (an index into m_rooms) that holds the eye, if a collection room does.
    [[nodiscard]] std::optional<std::size_t> eyeRoom() const;
    /// The interaction's room's collection, if any.
    [[nodiscard]] CollectionInstance* activeCollection() const;
    /// The floor item of the interaction's room's item `pickId`, if any.
    [[nodiscard]] std::optional<FloorItem> floorItemOf(world::PickId pickId) const;
    /// The collections by room (null for a room without one), as the loader's groups.
    void syncCollections();
    /// The room the notices name: the start room's id as --start-room gives it ("room:snes", "area:arcade",
    /// "area:shared:handheld"), else none.
    [[nodiscard]] std::string noticeRoom() const;

    AppContext& m_context;
    Options m_options;
    core::WorkerPool& m_pool;
    SceneGraphics& m_graphics;
    SceneHost m_host;
    std::unique_ptr<WorldFlow> m_flow;
    std::optional<WorldResult> m_decided;
    /// The floor's collection rooms (by the build's order), their collections as the loader's groups, their static
    /// groups for the view.
    std::vector<FloorRoom> m_rooms;
    std::vector<CollectionInstance*> m_collections;
    std::vector<const gl::StaticInstanceGroup*> m_groups;
    std::unique_ptr<CoverLoader> m_loader;
    /// The hover, the pull-out and Focus over the room that holds the eye (I3 step 13a).
    std::unique_ptr<WorldHover> m_interaction;
    /// The card beside the inspected item, and the plan's other versions of each placed title (I3 step 14a).
    WorldCard m_card;
    std::map<std::uint64_t, std::uint32_t> m_otherVersions;
    /// Launching from the world and "Mostra nella stanza" (I3 step 15a).
    WorldLaunch m_launch;
    /// The other versions of each title for Informazioni (I3 step 14b), from the same plan.
    VersionGroups m_versionGroups;
    /// The view's items when the dynamic path draws more than the scene's (viewItems()).
    std::vector<scene::RenderItem> m_viewItems;
    std::unique_ptr<HeroTextures> m_heroes;
    bool m_heroesKnowRoot = false;
    /// The hero item, the one whose front shows its hero texture now, the texture and the room whose hero material
    /// holds it, the dynamic item that draws it.
    std::optional<FloorItem> m_heroItem;
    std::optional<FloorItem> m_heroShown;
    unsigned m_heroTexture = 0;
    std::optional<std::size_t> m_heroTextureRoom;
    std::vector<scene::RenderItem> m_dynamicItems;
    /// The media root the floor's paths are relative to, also while it is out of reach (the loader's retry), and the
    /// last refresh seen.
    std::optional<std::filesystem::path> m_mediaRootPath;
    std::optional<std::string> m_lastRefresh;
    /// The next frame to render (lockstep: the run's last frame waits for the whole floor), the frames that waited for
    /// their view and how long.
    std::uint64_t m_nextFrame = 0;
    std::uint64_t m_lockstepWaits = 0;
    double m_lockstepWaitMs = 0.0;
    bool m_settleWarned = false;
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
    /// The stored plan's floor shown (its plan, layouts and cover requests; its scene given to the host), until the
    /// building's result reconciles it; whether no frame has shown it yet (the result waits for one).
    std::optional<WorldBuild> m_shownPreview;
    bool m_previewAwaitingFrame = false;
    /// I3 step 15b: what the floor shown is (none without a floor), a live update asked for (a refresh ended) and when,
    /// and whether m_flow is a live update's.
    std::optional<ShownFloor> m_shown;
    /// I4 step 6b: the room whose CRT is bound (the eye's after the dwell), and the frame the rebind's phase closes.
    ScreenRoomTracker m_screenRooms;
    std::optional<std::uint64_t> m_screenPhaseUntil;
    /// Rooms whose CRT could not be bound (none found in the scene): never asked again.
    std::set<std::string> m_screenless;
    bool m_liveRequested = false;
    std::uint64_t m_liveRequestFrame = 0;
    std::optional<Clock::time_point> m_liveRequestedAt;
    bool m_liveFlow = false;
};

/// The placements per section of `build`'s floor and the unplaced games per reason of its plan, into `record`
/// (world_session.cpp; I3 step 15b's live update counts them again).
void countPlan(const WorldBuild& build, WorldRecord& record);

} // namespace rv::app
