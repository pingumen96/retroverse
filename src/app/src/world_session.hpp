#pragma once

// The building in the app (I3 step 10b's room, the floor since I4 step 5a; ADR-0010 §12, ADR-0011 §1, §6, ADR-0012 §2,
// §3, ADR-0013 §9): what retroverse.exe shows without --scene, --model or --mock-catalog. While the building is made
// (app::WorldFlow: the stored plan read, the building job on the worker pool, the floor's build) the empty shell shows
// with the "generating" notice (the WorldGenerating state); the result is committed at a frame boundary: the floor's
// static part through the application's scene (SceneHost::showScene: a SceneInstance of its SceneDescription, the
// walker at its camera: the elevator's arrival point, or --start-room's doorway camera), each collection room's
// collection on a static instance group of its own (CollectionInstance, its registry keys carrying the room id), one
// cover loader over the floor that brings their covers and spines in within the upload budget (the view: the room that
// holds the eye and those whose doorway the frustum meets), the hover, pull-out and Focus of the room that holds the
// eye (WorldHover, I3 step 13a: a pick index per room). A catalog that does not open, or a library that gives no room,
// gives the test room (SceneHost::showTestRoom), a building that cannot be made now the shell, each with its notice.
// Lockstep runs (--fixed-dt) wait, at most kLockstepWaitLimit at a time, while the floor is on its way and until the
// layers of every item in the frame's view are resident (every item of the floor before the run's last frame). Main
// thread only.
//
// The hero CRT (ADR-0012 §7, ADR-0013 §9) is the CRT of the room that holds the eye, with that room's games (I4 step
// 6b: ScreenRoomTracker): bound from the start in --start-room's room, none in the lobby until the eye stays in a room;
// rebound without blocking the render loop once the eye has stayed in another room for the dwell (also the hysteresis
// at doorways), the last room's kept in a hall; never while a launched game runs. The quick catalog's selection plays
// only when its game stands in the eye's room.
//
// The persisted start (I3 step 10c, world_start.cpp): with a plan stored, its start floor shows first, as it is
// (WorldFlow's preview: its covers and spines from the cache by the rooms' items of the last start, its CRTs' games
// from them too), before the catalog is even read; once a frame has shown it, the building's result reconciles it room
// by room (I4 step 6b, world_reconcile.hpp, world_live_plan.hpp): each room by its content digest, an unchanged one
// keeping its collection and its pictures (nothing uploaded again), one whose items changed applied in place, one whose
// covers changed served again; a change of the floor's static part commits the floor again with the walker kept in the
// frame of the area that holds it, moved with it; a plan that cannot be used gives the shell. A lockstep run's first
// frame shows the stored floor's view whole, the next ones the reconciled floor.
//
// The covers (I3 step 12) come in the cover loader's order (the hero item, the view by projected size, the rest by
// distance), the hero item's front shows its hero texture (drawn by the dynamic path), nothing is made or uploaded
// while a launched game runs, and a refresh of the catalog lets the loader try the media root again.
//
// The live update (I3 step 15b, world_live_update.cpp): after a refresh of the catalog the building job runs again (the
// snapshots, the update, the plan saved) while the floor shows, and its result is applied at a frame boundary with as
// little rebuilt as it needs, room by room as at the start (world_live_plan.hpp): nothing, the covers of the rooms
// whose covers changed served again, the items of the rooms whose items changed applied in place (titles placed into
// their slots, the items that stood unmoved with their pictures, the scene and the walker untouched), or the floor
// committed again with the walker kept in its area's frame. The other floors are only saved. Never while a launched
// game runs: a result that arrives meanwhile applies after it.
//
// The elevator and the directory (I4 step 7b, world_ride.cpp; ADR-0013 §10): in front of a floor's elevator (in its
// zone, facing its doors) the prompt shows and interact opens the directory (the product UI's), M or Select anywhere in
// free roam. Its commands, and the quick catalog's "Mostra nella stanza", lead the walker (directory_data.hpp's
// routing): a teleport on the floor shown (a room's doorway, the elevator), the Focus pose in the room the interaction
// is in, or a ride (elevator_ride.hpp): the view fades out, the destination floor is built by a floor-build job from the
// plan shown, the rooms' items files and the catalog's play sessions, the resident floor is dropped only once it is
// built and swapped at a frame boundary behind the fade (the hero CRT unbound without a join, bound again by the eye's
// room tracker), the walker at its elevator's arrival or at the room's doorway, the view fades in once the eye's area is
// resident (since I4 step 8 the covers in view of the room that holds the eye, or by 3 s after the request; under
// lockstep the frames wait instead), the rest of the room streaming in after it. A refused or failed "Mostra nella
// stanza" of the quick catalog is told by the world's notice (the directory tells its own). A building's result or a
// live update that ends during a ride applies after it (built again for the floor shown when it was built for another,
// then applied once no launched game runs). Input is suspended during the ride.
//
// The hub (I6 step 5, world_showcase.cpp; ADR-0014 §3): in front of its directory totem (its zone, facing its screen)
// the prompt reads "Indice" and interact opens the directory at its floors; its favourites showcase holds runtime
// proxies of the catalog's favourite and recent games (hub_showcase.hpp: a job on the pool reads them on the catalog's
// world reader), drawn as a collection of its own with a cover loader of its own, hovered by the frame's ray when no
// room's item is (its title label shows), interact on one opening the directory at its card; the proxies are made
// again when a launched game returns and after a refresh of the catalog. Lockstep runs wait for them like the floor's
// covers.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rv/app/cover_requests.hpp>
#include <rv/app/directory_data.hpp>
#include <rv/app/elevator_ride.hpp>
#include <rv/app/game_screens.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/scene_instance.hpp>
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
#include <rv/scene/light.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/where_is.hpp>

#include <rv/app/cabinet_screens.hpp>
#include <rv/app/video_settings.hpp>

#include "screen_service.hpp"
#include "world_card.hpp"
#include "world_edit.hpp"
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
        /// none: a floor whose eye's room has no CRT bound yet); unset, the scene's first CRT (the shell, the test
        /// room).
        std::optional<std::string> heroScreenPrefix;
        /// With `keepWalker`: the move of the area that holds the walker (I4 step 6b, ADR-0013 §7: the walker kept in
        /// its frame); none: it stays where it stands.
        std::optional<FrameMove> walkerMove;
        /// Without `keepWalker`: the walker at the scene's camera even when --spawn was given (I4 step 7b: a ride's
        /// destination, its camera the elevator's arrival or a room's doorway).
        bool atCamera = false;
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
        /// I4 step 7b: puts the walker at `camera` (its feet the eye less the walker's eye height, its yaw and pitch),
        /// the view's camera with it (a teleport on the floor shown); and the walker's pose now.
        std::function<void(const scene::CameraSetup& camera)> teleport;
        std::function<scene::WalkerPose()> walker;
        /// I4b step 4: replaces the scene's objects and lights of a room with its static part rebuilt alone
        /// (SceneInstance::replaceObjects(): the collision world and the lights following, the walker where it stands);
        /// false when it could not (logged).
        std::function<bool(const SceneObjectsChange& change)> replaceObjects;
        /// I4b step 4: the indices of the scene's render items of the objects whose ids `match` accepts (a fixture the
        /// editor holds leaves its place).
        std::function<std::vector<std::size_t>(const std::function<bool(std::string_view id)>& match)> sceneItemsOf;
        /// I4b step 4's review: the scene's lights whose names `match` accepts, with their indices into the scene's
        /// lights; and lights set at their indices, the scene's lighting packed again (a held lamp's light follows it).
        std::function<std::vector<std::pair<std::size_t, scene::Light>>(
            const std::function<bool(std::string_view name)>& match)>
            sceneLightsOf;
        std::function<void(std::span<const std::pair<std::size_t, scene::Light>> lights)> setSceneLights;
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
    /// The inspection's dimming of the frame's view (I3 step 13b: WorldInspection::apply); the editor's outline colour
    /// (I4b step 4: a held fixture's wider outline, red over a refused place, the rest dimmed a little).
    void applyInspection(gl::RenderView& view) {
        m_interaction->applyInspection(view);
        applyEditOutline(view);
    }
    /// The games the screens follow (I3 step 13b, app::screenSelection()): the item in Focus or Inspect (`world`), and
    /// `catalogSelection` (the quick catalog's, while it is open) when its game stands in the eye's room (`catalog`).
    /// Kept apart since I5 step 4's review: the arcade's hero cabinet follows the quick catalog's alone.
    struct ScreenSelections {
        std::optional<std::string> world;
        std::optional<std::string> catalog;
    };
    [[nodiscard]] ScreenSelections screenSelections(const std::optional<std::string>& catalogSelection) const;
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

    // --- I4 step 7b: the elevator, the directory and the ride (world_ride.cpp; ADR-0013 §10) -----------------------

    /// A ride runs: the input is suspended (the walker, the interaction, the product UI read nothing).
    [[nodiscard]] bool riding() const noexcept { return m_ride != nullptr; }
    /// How dark the ride's fade makes the view: 0 not at all, 1 black.
    [[nodiscard]] float fade() const noexcept;
    /// The world's reduceMotion (world.json): every transition instant, the change of an area's environment too.
    [[nodiscard]] bool reduceMotion() const noexcept { return m_reduceMotion; }
    /// The walker stands in the elevator's proximity zone of the floor shown, facing its doors (facesElevator()).
    [[nodiscard]] bool atElevator() const;
    /// atElevator() in free roam with nothing hovered and no ride: the prompt shows, interact opens the directory (the
    /// interaction's state of this frame, after interact()).
    [[nodiscard]] bool idleAtElevator() const;
    /// I6 step 5: the walker stands in the hub's directory totem's zone, facing its screen (facesTotem()); and so in
    /// free roam with nothing hovered and no ride (idleAtElevator()'s conditions): its prompt ("Indice") shows,
    /// interact opens the directory at its floors.
    [[nodiscard]] bool atTotem() const;
    [[nodiscard]] bool idleAtTotem() const;
    /// Opens the directory on `tab` with the building's floors and rooms and where the walker is (`via`: "elevator" or
    /// "map", the run report's); nothing without a product UI's catalog of rooms.
    void openDirectory(ProductUi& product, ui::DirectoryTab tab, std::string_view via);
    /// I6 step 5: a proxy of the hub's favourites showcase is hovered (free roam, nothing of a room hovered); interact on
    /// it opens the directory at its game's card (`via` "showcase").
    [[nodiscard]] bool showcaseHovered() const noexcept { return m_showcaseHovered != world::kNoPick; }
    void openShowcaseCard(ProductUi& product);
    /// The elevator's prompt of this frame: shown naming `key` (the totem's with `totem`, I6 step 5), or hidden (none);
    /// the frames it shows are counted.
    void elevatorPrompt(ProductUi& product, std::optional<std::string> key, bool totem = false);

    /// The eye is in the room whose screens are bound (I5 step 5: in a hall, the last room's screens are silent); true
    /// without areas or before any room is bound.
    [[nodiscard]] bool eyeInScreenRoom() const;

    // --- I5 step 4: the arcade cabinets playing their clips (world_cabinets.cpp; ADR-0016 §6) -----------------------

    /// The cabinets that ask for their clips on this frame, seen by `camera` at the run's time `timeSeconds`
    /// (app::chooseCabinets: the focused or inspected one, else the attract one, none in the editor), with their room's
    /// pool of video materials (`slots` of them, made at the first ask); none outside the bound room of the interaction,
    /// or in a room whose items are no screens. `video`: video.json's cabinet keys.
    [[nodiscard]] std::pair<std::vector<CabinetScreen>, CabinetPool> cabinetScreens(const scene::Camera& camera,
                                                                                    double timeSeconds,
                                                                                    std::size_t slots,
                                                                                    const VideoSettings& video);
    /// After the screens' frame (ScreenService::cabinetsShown()): the cabinets whose screens show their clips have
    /// their fronts drawn with their pooled materials on the dynamic path (their static fronts hidden), the others their
    /// covers again.
    void showCabinets(std::span<const CabinetShown> shown);

    // --- I4b step 3: the editor (world_edit.cpp; ADR-0015 §4) -----------------------------------------------------

    /// An edit session runs (the state Edit): the directory, the quick catalog, rides and live updates wait.
    [[nodiscard]] bool editing() const noexcept { return m_edit != nullptr; }
    /// The editor's menu is open, or its command (Salva, Esci senza salvare) runs: the walker neither walks nor looks.
    [[nodiscard]] bool editHoldsWalker() const noexcept;

private:
    // I4b step 3 (world_edit.cpp).
    /// The room the editor can edit now ("Modifica stanza": the room that holds the eye, with its collection, while no
    /// floor is on its way, no ride, no launch, no session), if any.
    [[nodiscard]] std::optional<std::string> editableRoom(const ProductUi& product) const;
    /// A session begins in `room` (`via` "directory" or "card"; `held` the game "Sposta" took), or is refused with the
    /// world's notice.
    void beginEdit(ProductUi& product, const world::RoomId& room, std::string_view via,
                   std::optional<catalog::GameId> held);
    /// The editor's frame in place of the interaction's (interact()).
    [[nodiscard]] InteractionResult editFrame(const WorldHover::Frame& frame);
    /// Every update: a job's answer taken, the bar shown (hidden once no session runs).
    void followEdit(ProductUi& product);
    void takeEditAnswer(EditJobAnswer answer);
    void requestEditOperation(const world::EditOperation& operation);
    void requestEditDisplay(EditJob job, world::EditedWorld world, std::string digest, world::RebuildLevel rebuild,
                            std::vector<world::RoomId> rooms, std::string sceneDigest);
    void requestEditSave();
    /// Submits `work` as the session's one job (its answer into the session's box, the host woken).
    void submitEditJob(EditJob job, std::function<EditJobAnswer(const core::JobContext&)> work);
    /// What the session's jobs read (the plan state's library and media, the floor, the edited room, its data).
    [[nodiscard]] EditJobContext editJobContext() const;
    /// `build` shown for the session (`fullFloor`: its own scene, else the shown one's with its rooms laid out again):
    /// applied in place room by room when the static part is the same, else the floor committed again. With
    /// `statics` (rooms' static parts rebuilt alone), the update is planned before anything changes; when it cannot
    /// be shown in place once a room's objects were replaced, or needs the floor committed again, the floor of
    /// `floorPlan` (the plan `build` shows, as stored) is built here and committed, `committedFloor` set: the scene
    /// never shows a part the session did not adopt. False when it could not be shown (nothing changed).
    [[nodiscard]] bool showEditBuild(WorldBuild& build, bool fullFloor, std::span<const RoomStaticPart> statics = {},
                                     const world::WorldPlan* floorPlan = nullptr, bool* committedFloor = nullptr);
    /// The floor of `plan` (digest `digest`) built on the main thread and committed for the session (showEditBuild()'s
    /// fallback); false when it cannot be.
    [[nodiscard]] bool commitEditFloor(const world::WorldPlan* plan, const std::string& digest, bool* committedFloor);
    /// The edited room's targets from the session's floor shown.
    void bindEditRoom();
    /// The held item's static records hidden, the target outlined (every frame), or both cleared.
    void syncEditFlags();
    void clearEditFlags();
    /// The session ends (`end`: "saved", "discarded", "exited", "failed"), its record kept.
    void endEdit(std::string_view end);
    /// The view's items in Edit: the held game in the hand, smaller, and where it would stand in an empty slot.
    [[nodiscard]] std::span<const scene::RenderItem> editViewItems(std::span<const scene::RenderItem> sceneItems,
                                                                   const scene::Camera& camera);
    /// A frame was rendered: the time an edit took to show.
    void editFrameRendered();
    /// A lockstep run waits for the session's job.
    [[nodiscard]] bool editWaits() const noexcept;
    /// I4b step 4: a held fixture's place judged on the pool (the latest asked waits for the one in flight), its answer
    /// given to the interaction; the held fixture's scene items and games left out of their places (or given back).
    void requestEditProbe(const world::EditOperation& probe);
    void followEditProbe();
    void syncHeldFixture();
    /// The held fixture let go of: its scene items and games given back, its lights put back where they stood (none
    /// when the scene is about to be replaced: `restore` false).
    void releaseHeldFixture(bool restore);
    /// A held lamp's light where the lamp would stand (ADR-0015 §4: it follows the lamp; the scene's lights moved in
    /// place, no object rebuilt), once per new place.
    void moveHeldLights();
    /// The view's items of a held fixture: drawn where it would stand, outlined.
    void appendHeldFixture(std::span<const scene::RenderItem> sceneItems, CollectionInstance& collection);
    /// The outline of a held fixture (the inspection's dimming's view too): 3.5 px at 1080, the rest of the view dimmed
    /// a little, a lighter red over a refused place.
    void applyEditOutline(gl::RenderView& view) const;
    /// The collection of the edited room, if shown.
    [[nodiscard]] CollectionInstance* editCollection() const;

    using Clock = std::chrono::steady_clock;

    /// What the floor-build jobs of rides read (I4 step 7b): the plan shown, its digest and library, the media roots
    /// its floor was built with.
    struct PlanState {
        std::shared_ptr<const world::WorldPlan> plan;
        std::string digest;
        catalog::LibraryId library;
        std::optional<std::filesystem::path> mediaRoot;
        std::optional<std::filesystem::path> mediaRootPath;
        std::optional<std::filesystem::path> screenMediaRoot;
    };
    /// What a floor-build job posts (world_ride.cpp): the floor built, the generation it was asked under, whether it is
    /// a building's result built again for the floor shown; the host's wake.
    struct FloorJobBox {
        std::mutex mutex;
        std::optional<WorldBuild> build;
        std::uint64_t generation = 0;
        bool retarget = false;
        std::function<void()> wake;
    };
    /// "Mostra nella stanza" of `game` once the walker stands in `room` (after a teleport or a ride).
    struct PendingShow {
        catalog::GameId game;
        std::string room;
    };

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
    /// with `keepWalker`, moved with the area that holds it, I4 step 6b; else at the scene's camera, --spawn not
    /// applied with `atCamera`: a ride's destination), a collection per room (with `keepWalker` each taking its room's
    /// pictures), their pick indices, the cover loader, the bound room's CRT. False when the scene could not be shown
    /// (the test room then, with its notice). `walker`: where the walker was kept, for the record.
    [[nodiscard]] bool showFloor(WorldBuild& build, bool keepWalker, std::optional<WalkerKeptRecord>* walker = nullptr,
                                 bool atCamera = false);
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
    /// I4 step 6b: `plan`'s rooms (Unchanged, Textures, Items) applied to the floor shown, room by room: the Items
    /// rooms' collections in place, the cover loader again (adopting what showed: an unchanged room's pictures stay,
    /// nothing uploaded again) when some room's covers or items changed, the bound CRT's games when they changed. False
    /// when the items cannot be applied in place (commit the floor again).
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

    // I4 step 7b (world_ride.cpp).
    /// What the world can do now, for the directory's commands and "Mostra nella stanza" (directory_data.hpp).
    [[nodiscard]] WorldMoveConditions moveConditions(const ProductUi& product) const;
    /// The directory's commands and the quick catalog's "Mostra nella stanza" taken where they lead (a ride, a teleport,
    /// the Focus pose), a refusal back to the directory; then a "Mostra nella stanza" a ride or a teleport left pending.
    void followCommands(ProductUi& product);
    void executeMove(ProductUi& product, const WorldMove& move, const std::optional<ui::DirectoryCommand>& command,
                     const std::optional<catalog::GameId>& game);
    /// A teleport on the floor shown: to `room`'s doorway ("": the elevator's arrival), the interaction in that room.
    void teleportTo(const std::string& room, RideReason reason);
    /// A ride for `move` (the directory's `command`, if any: reopened when the ride fails; else "Mostra nella stanza" of
    /// the quick catalog, whose failure the world's notice tells).
    void startRide(ProductUi& product, const WorldMove& move, const std::optional<ui::DirectoryCommand>& command);
    /// The floor-build job of `skeleton` (a WorldBuild with its requested floor, library, start room and digest; its
    /// plan the plan shown's when it has none), the latest asked winning; `retarget`: a building's result built for
    /// another floor than the one shown (applied as a live update once built, never while a launched game runs), else a
    /// ride's destination. `reader` (the catalog's world reader, if any): the CRTs' last plays from its sessions now
    /// (catalogLastPlayed()), else the items files'.
    void requestFloor(WorldBuild skeleton, bool retarget, std::shared_ptr<WorldCatalogReader> reader);
    /// The floor-build job's answer, if one came (its generation the latest).
    [[nodiscard]] std::optional<WorldBuild> takeFloorBuild(bool& retarget);
    /// The ride's frame (or idle wake): its floor job answered, its fade moved, the floor swapped, the ride ended.
    void followRide(ProductUi& product, bool idleWake, double deltaSeconds);
    /// One frame of the ride under way, `elapsedMs` after its request.
    void stepRide(ProductUi& product, double deltaSeconds, double elapsedMs);
    void swapRide();
    void finishRide();
    /// The eye's area resident on the ride's destination: every item of the room that holds the eye that is in view done
    /// (I4 step 8; a hall has none), the loader having ranked them since the swap.
    [[nodiscard]] bool rideViewResident() const;
    /// I4 step 8: the destination room of the last ride followed until every item of it is resident (its record's
    /// roomResidentMs, `elapsedMs` after the ride's request), while the walker's floor is the ride's.
    void followRideRoom(double elapsedMs);
    // I6 step 5: the hub's favourites showcase (world_showcase.cpp).
    /// What a showcase job posts: its content, the generation it was asked under; the host's wake.
    struct ShowcaseBox {
        std::mutex mutex;
        std::optional<ShowcaseContent> content;
        std::uint64_t generation = 0;
        std::function<void()> wake;
    };
    /// A floor with a showcase is shown (`build`'s): its slots kept, its content asked for.
    void startShowcase(const WorldBuild& build);
    /// Its content asked of the pool (the catalog's world reader open; else asked again next frame).
    void requestShowcase(ProductUi& product);
    /// Every frame: a launch's return or a refresh asks for the content again, an answer is shown.
    void followShowcase(ProductUi& product);
    void showShowcase(ShowcaseContent content);
    /// Asks for the showcase's content again (a refresh that keeps the floor); nothing off the hub.
    void wantShowcase() noexcept {
        if (m_showcaseSpec) {
            m_showcaseWanted = true;
        }
    }
    void dropShowcase();
    /// Its cover loader's round (as the floor's: paused while a launched game runs).
    void showcaseRound(ProductUi& product, const gl::UploadBudget& budget, bool frame);
    /// The frame's hover of the showcase's proxies (nothing while a room's item is hovered or the interaction is not in
    /// free roam).
    void hoverShowcase(const WorldHover::Frame& frame);
    [[nodiscard]] bool showcaseSettled() const;

    /// The plan shown's state for the rides' floor jobs, the floor's elevator, arrival and doorways, its floors' record.
    void keepPlan(const WorldBuild& build);
    /// The directory's floors and rooms and where the walker is, given to `product` when they changed.
    void syncDirectory(ProductUi& product);
    /// The interaction's index of the room `room` of the floor shown, if it has a collection.
    [[nodiscard]] std::optional<std::size_t> interactionRoomOf(std::string_view room) const;
    /// A building's result for another floor than the one shown (a ride happened since its floor was asked): its floor
    /// built again for the floor shown (`reader`: requestFloor()'s), then applied as a live update. True when it was
    /// taken so.
    [[nodiscard]] bool retargetResult(WorldResult& result, std::shared_ptr<WorldCatalogReader> reader);

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

    /// I4 step 7b: the plan shown (its state for the rides' floor jobs, its where-is index), the floor shown ("" none),
    /// its elevator's zone, arrival camera and rooms' doorway cameras (the scene's frame), the directory's building made
    /// from the plan (the digest it was made from).
    std::optional<PlanState> m_planState;
    std::optional<world::PlanIndex> m_planIndex;
    std::string m_floor;
    core::Aabb m_elevatorZone;
    glm::vec3 m_elevatorDoors{0.0f};
    /// I6 step 5: the hub's directory totem's zone and screen (empty off the ground floor).
    core::Aabb m_totemZone;
    glm::vec3 m_totemScreen{0.0f};
    /// I6 step 5: the showcase of the floor shown (none off the hub), its job's mailbox and generation, whether a job
    /// runs and whether a new content is wanted (a launch returned, a refresh ended) once it answers, the proxies' games,
    /// layout, collection, cover loader and pick index, the hovered proxy, whether a launch was active last frame.
    std::optional<HubShowcase> m_showcaseSpec;
    std::shared_ptr<ShowcaseBox> m_showcaseBox;
    core::JobGeneration m_showcaseGeneration;
    bool m_showcaseAsked = false;
    bool m_showcaseWanted = false;
    std::vector<ShowcaseGame> m_showcaseGames;
    /// The covers of the content shown (a content equal to it, games and covers, is not drawn again).
    std::vector<CoverItem> m_showcaseCovers;
    world::CollectionLayout m_showcaseLayout;
    std::unique_ptr<CollectionInstance> m_showcase;
    std::unique_ptr<CoverLoader> m_showcaseLoader;
    std::unique_ptr<world::PickIndex> m_showcasePicks;
    world::PickId m_showcaseHovered = world::kNoPick;
    bool m_showcaseLaunchSeen = false;
    ShowcaseRecord m_showcaseRecord;
    scene::CameraSetup m_arrival;
    std::map<std::string, scene::CameraSetup> m_doorways;
    ui::DirectoryBuilding m_directoryBuilding;
    std::string m_directoryPlan;
    /// The ride, when it was asked (the steady clock), the directory's command that asked it, the destination built.
    std::unique_ptr<ElevatorRide> m_ride;
    Clock::time_point m_rideStart;
    std::optional<ui::DirectoryCommand> m_rideCommand;
    std::optional<WorldBuild> m_rideBuild;
    /// I4 step 8: the room the last ride swapped to (reasons room and showInRoom), until it is resident whole.
    std::optional<std::string> m_rideRoom;
    /// The floor-build job's mailbox and generation (the latest asked wins), and a building's result waiting for its
    /// floor built again for the floor shown (retargetResult()), then, once built, for a launched game's end.
    std::shared_ptr<FloorJobBox> m_floorBox;
    core::JobGeneration m_floorGeneration;
    bool m_retargetPending = false;
    std::optional<WorldResult> m_retargetResult;
    /// "Mostra nella stanza" waiting for the walker to stand in its room (after a ride).
    std::optional<PendingShow> m_pendingShow;
    /// The run's time at the last frame's update (the ride's fades), and whether the world's reduceMotion is on.
    std::optional<double> m_lastTime;
    bool m_reduceMotion = false;
    /// Frame updates so far, and the count when the ride swapped its floor: the eye's area is checked from the next
    /// frame on, whether the swap came in a frame or in a lockstep wait (the same frames on every run).
    std::uint64_t m_updates = 0;
    std::uint64_t m_swapUpdate = 0;

    /// I4b step 3: the edit session (none outside the editor), the sessions so far, whether the bar shows.
    std::unique_ptr<WorldEditState> m_edit;
    std::uint64_t m_editSessions = 0;
    bool m_editBarShown = false;

    // I5 step 4 (world_cabinets.cpp).
    /// A cabinet's front showing its clip: its room (an index into m_rooms), its item's pick id and the pooled material.
    struct CabinetFront {
        std::size_t room = 0;
        world::PickId pick = world::kNoPick;
        scene::MaterialHandle material;
    };
    /// Attract's choice; the cabinets asked for on this frame (by screen id: their room and pick); the fronts showing.
    CabinetAttract m_cabinetAttract;
    std::map<std::string, std::pair<std::size_t, world::PickId>, std::less<>> m_cabinetRequests;
    std::vector<CabinetFront> m_cabinetFronts;
    /// The frame whose cabinet fronts checkCabinetFronts() looked at last.
    std::optional<std::uint64_t> m_cabinetsCheckedFrame;
    /// The cabinets' fronts drawn by the dynamic path this frame (those not pulled out: a pulled one's copy carries it).
    void appendCabinetFronts(std::vector<scene::RenderItem>& items) const;
    /// Once a frame: a cabinet's clip drawn while its static front is drawn too, or its item gone, is counted
    /// (WorldRecord::cabinetCoverBeneathFrames: 0).
    void checkCabinetFronts();
    /// Every cabinet's own front again (before the collections go, or when none plays).
    void dropCabinetFronts();
};

/// The placements per section of `build`'s floor and the unplaced games per reason of its plan, into `record`
/// (world_session.cpp; I3 step 15b's live update counts them again).
void countPlan(const WorldBuild& build, WorldRecord& record);

} // namespace rv::app
