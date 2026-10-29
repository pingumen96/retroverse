#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The building of retroverse.exe and the floor it shows, up to what the main thread commits (I3 step 10b's room flow,
// made the building's in I4 step 5a; ADR-0010 §9 to §12, ADR-0011 §1, §6, ADR-0013 §7, §9):
//
//   main thread (WorldFlow::update, every frame      worker pool (kHighestPriority, the world's JobGeneration)
//   and every idle wake, after the catalog's poll)
//   ---------------------------------------------   ---------------------------------------------------------------
//   the catalog opened Ready ------------------->    readWorldPlan: the world's stored plan (world-plan.json), its
//                                                    digest, a schema-1 plan lifted
//   a plan stored: ------------------------------>  previewWorld: the start floor of the stored plan as it is
//                                                    (buildFloor() with the rooms' items files, no catalog)
//   takePreview(): shown at a frame boundary <----
//   (the persisted start), then ----------------->  planBuilding (the building job): every system's snapshot in one
//                                                    read transaction on the catalog's world reader
//                                                    (takeBuildingSnapshot), the world lock, the plan read again under
//                                                    it, the selections merged by system, a first building
//                                                    (generateBuilding) or the update of its rooms (updateBuilding:
//                                                    each selection to the room holding its section, a shared or
//                                                    arcade room lacking one held), the validation, savePlan, each
//                                                    system's room items written when they changed (room_items.hpp)
//   the building planned ------------------------>  buildFloor (the floor-build job, keyed (floor, plan digest)): the
//                                                    floor's static scene (world::toFloorSceneFile) in the scene's
//                                                    frame, each collection room's layout in that frame and its cover
//                                                    requests from the items, the start room's CRT games
//   takeResult(): committed at a frame boundary <-
//   (app::WorldSession: the scene, a collection per
//   room; after a preview, reconciled with the floor
//   shown: world_reconcile.hpp)
//
// The frames (ADR-0013 §7): the scene's frame is the start room's (--start-room) when one is given, else the floor's
// (building.hpp: the elevator at the hall's west end). The floor's scene file (the floor's frame) is moved into it as
// text, its numbers rounded as the scene builder rounds them, so that the start room's own objects keep the very numbers
// of its standalone scene; every room's collection is moved there by its placement relative to the start room (exact:
// millimetres and half turns). --spawn, the input scripts' poses and the run report's walker poses are in that frame.
//
// The preview is asked for the building job only once it has been taken (shown), so that the stored floor always shows
// before the catalog is read again (ADR-0010 §12), and the catalog worker's first jobs are the quick catalog's.
//
// The catalog that does not open, or whose library gives no room (no system with a placeable title) while no plan is
// stored, gives the test room (WorldResult::testRoom) with its notice; a building that cannot be made now (the media
// root out of reach with no plan stored, a plan that breaks a hard rule, an error) gives its build with no plan, which
// the app shows as the empty shell with its notice. A start room the plan lacks once it exists ends the run (exit 2:
// WorldBuild::startRoomMissing). Nothing is written but the world's own files (under <user data>/worlds/<library id>/,
// the data-directory guard having passed when the catalog opened) and the rooms' items (<user data>/cache/world/), and
// nothing is decided at all while the media are unknown (ADR-0010 §3).
//
// An unchanged start (I4 step 6b): the building job writes, beside the rooms' items, its building stamp
// (<cache>/world/<library>/building-stamp.json: the digest of the plan it left, of the catalog's stamp it read the
// snapshots with, of the world's inputs). At the next start, before reading any snapshot, the job compares: when the
// stored plan is that plan (the file's digest), the catalog's stamp and the world's inputs are as they were, the media
// root can be read and every room's items file is there, nothing a snapshot could change has changed, so it plans
// nothing (unchangedBuilding(): outcome Unchanged, `skipped`) and the floor is built from the plan and the items files.
// Anything else runs the whole job. The world's inputs (worldInputsDigest()) are the program (its version and its
// executable's size and last write time: another build of the same version may plan otherwise), the generator's
// version, the items files' and the stamp's schemas, every file of the asset root's world/ and manifests/ folders,
// world.json, overrides.json, library.json and the media root's path. A job that leaves something to settle (a room
// kept until a regeneration, a room held, a system waiting to enter one, a notice) writes no stamp and removes the one
// there, so that its notice and warnings come at every start until an update settles them.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/collection_layers.hpp>
#include <rv/app/hub_showcase.hpp>
#include <rv/app/room_items.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/app/world_notice_ui.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

namespace rv::app {

/// The floor the app starts on without --start-room (ADR-0013 §9: the lobby of the ground floor).
inline constexpr std::string_view kStartFloor = "ground";

/// Where the world's jobs read and write.
struct WorldPaths {
    std::filesystem::path assetRoot;
    std::filesystem::path userDataRoot;
    /// <user data>/config (world.json).
    std::filesystem::path configDir;
    /// <user data>/cache (the derived textures under cache/textures/<library id>, the rooms' items under cache/world).
    std::filesystem::path cacheDir;
};

/// The world's stored plan, as the first job reads it (readWorldPlan()).
struct StoredWorldPlan {
    catalog::LibraryId library;
    /// The plan and its file's digest; none when there is none, or it cannot be read (`error`).
    std::optional<world::StoredPlan> stored;
    /// Not NotFound: a plan refused (edited by hand: ValidationFailed, ParseError; newer: Unsupported) or unreadable.
    std::optional<Error> error;
    double milliseconds = 0.0;
};

/// Reads <user data>/worlds/<library>/world-plan.json (world::readStoredPlan), without the lock (readers need none); a
/// plan of schema 1 is lifted into the building (world::lift, ADR-0013 §5), its digest the file's.
[[nodiscard]] StoredWorldPlan readWorldPlan(const WorldPaths& paths, const catalog::LibraryId& library);

/// How the building that shows came to be.
enum class WorldOutcome : std::uint8_t {
    Generated, ///< a first plan, generated and saved
    Updated,   ///< the stored plan updated by the incremental rules and saved
    Unchanged, ///< the stored plan, which the catalog as it is now leaves as it was (nothing written)
    Kept,   ///< the stored plan shown as it is: the media unknown (nothing decided), an explicit regeneration needed,
            ///< or an update that failed or broke a hard rule (logged)
    NoPlan, ///< nothing to show: the media unknown without a stored plan, a refused plan, a first plan that breaks a
            ///< hard rule, an error
};

/// "generated", "updated", "unchanged", "kept", "no-plan".
[[nodiscard]] std::string_view toString(WorldOutcome outcome) noexcept;

/// The building job's input.
struct WorldBuildInput {
    WorldPaths paths;
    catalog::LibraryId library;
    /// Every system's snapshot (takeBuildingSnapshot()).
    BuildingSnapshot snapshot;
    /// The meta sidecar's writer (world::SaveOptions::writer).
    std::string writer;
    /// How long the world lock is waited for (world::kWorldLockWait).
    std::chrono::milliseconds lockWait = world::kWorldLockWait;
    /// The room the run starts in (--start-room): its floor is built, the scene's frame is its.
    std::optional<world::RoomId> startRoom;
};

/// A prefab of the floor that fell back to its family's (style §21's object fallbacks, assets::resolvePrefab: logged
/// once).
struct ObjectFallback {
    /// The manifest the plan names, what replaced it (assets::toString(PrefabSource)) and why.
    std::string prefab;
    std::string source;
    std::string reason;

    bool operator==(const ObjectFallback&) const = default;
};

/// One collection room of the floor built (its collection, in the scene's frame).
struct RoomCollection {
    world::RoomId room;
    /// Its place on its floor (the plan's), and in the scene's frame (relative to the start room: {0, 0, 0} for it).
    world::RoomPlacement placement;
    world::RoomPlacement scenePlacement;
    /// The room's scene data (its item prefab, its media family, its theme).
    world::SceneData sceneData;
    /// Its collection in the scene's frame (world::floorCollectionLayout of its room's layout by scenePlacement).
    world::CollectionLayout layout;
    /// What the cover loader serves for the items whose text is known, in the layout's item order.
    std::vector<CoverItem> covers;
    /// The passage of its doorway onto the hall, in the scene's frame, from the floor to the doorway's top (the cover
    /// loader's view: a room whose doorway the frustum meets is in view); empty for a room without one.
    core::Aabb doorway;
    /// Its content digest in the plan (world::roomContentDigest: its record and every element of it), what the per-room
    /// reconcile compares first (I4 step 6b, ADR-0013 §5).
    std::string contentDigest;
    /// Its hero CRT's games: its placed games with media, in its featured order (room_screen.hpp, from the rooms'
    /// items; none without a media root for them: FloorSources::screenMediaRoot).
    std::vector<ScreenGame> screenGames;
    /// Its doorway camera in the scene's frame (world::doorwayCamera() moved by scenePlacement: the eye inside the
    /// doorway, looking into the room), where the directory's teleport to the room stands the walker (I4 step 7b).
    scene::CameraSetup doorwayCamera;
};

/// The floor to commit (an immutable value made on the pool, ADR-0010 §12), with the building it shows.
struct WorldBuild {
    /// The stored plan's floor shown before the building job answers (previewWorld(), I3 step 10c: outcome Kept, the
    /// covers of the items the rooms' items files know, no media root, the CRTs' games from the items files since I4
    /// step 6b); else the building's.
    bool preview = false;
    WorldOutcome outcome = WorldOutcome::NoPlan;
    /// The world notice it shows (world_notice), and why, in English (the log, the run report).
    std::string notice;
    std::string reason;
    /// The start room (--start-room), and whether the plan lacks it (the run then ends: exit 2).
    std::optional<world::RoomId> startRoom;
    bool startRoomMissing = false;
    /// The floor to build (I4 step 7b: a ride's destination, the floor the walker is on for a building's result); none:
    /// the start room's floor, else kStartFloor. The scene's frame is the start room's only on the start room's floor.
    std::optional<std::string> requestedFloor;
    /// The floor built ("ground" without a start room), and its frame in the scene's frame (the hall's: relative to the
    /// start room's placement, {0, 0, 0} without a start room).
    std::string floor;
    world::RoomPlacement floorPlacement;
    /// The elevator's proximity zone (the hall's, world::Hall::elevatorZone, from the floor to the doors' top) and the
    /// arrival camera in front of the elevator facing east (the floor scene's camera), both in the scene's frame: where
    /// the prompt shows and where a ride or the directory's own floor puts the walker (I4 step 7b, ADR-0013 §10).
    core::Aabb elevatorZone;
    scene::CameraSetup arrival;
    /// The middle of the elevator's doors on the floor (y 0), in the scene's frame: the prompt shows to a walker in the
    /// zone that faces them (elevator_prompt_ui.hpp's facesElevator()).
    glm::vec3 elevatorDoors{0.0f};
    /// The hub's directory totem (I6 step 5, ADR-0014 §3; the ground floor only, empty elsewhere): its proximity zone
    /// (world::Hub::totemZone, from the floor to the totem's top) and the middle of its screen on the floor (y 0), in
    /// the scene's frame: its prompt shows to a walker in the zone that faces it (facesTotem()).
    core::Aabb totemZone;
    glm::vec3 totemScreen{0.0f};
    /// The hub's favourites showcase (I6 step 5): its slots in the scene's frame and its proxies' data; none off the
    /// ground floor (or when its data do not load: logged).
    std::optional<HubShowcase> showcase;
    catalog::LibraryId library;
    /// The plan shown and its digest (the SHA-256 of its canonical text); none for NoPlan.
    std::optional<world::WorldPlan> plan;
    std::string planDigest;
    /// The generation's or the update's change report (none when nothing ran: Kept for unknown media, NoPlan).
    std::optional<world::ChangeReport> changes;
    /// The validation's hard findings of the plan made (logged; a plan with one is never saved or shown) and how many
    /// soft ones it had.
    std::vector<world::ValidationFinding> hardFindings;
    std::size_t softFindings = 0;
    /// The plan was written this run (savePlan), and why it could not be (the plan is shown all the same).
    bool written = false;
    /// The job planned nothing: the stored plan as the building stamp says it was made (unchangedBuilding()).
    bool skipped = false;
    std::optional<Error> saveError;
    /// The floor's static part in the scene's frame (its rooms, its hall), the SHA-256 of its scene file's text, the
    /// spans its rooms left bare.
    std::optional<scene::SceneDescription> scene;
    std::string sceneDigest;
    std::vector<world::UndressedSpan> undressed;
    /// The floor's collection rooms, by id (rooms of no item have their collection empty).
    std::vector<RoomCollection> rooms;
    /// The media root the covers are read under; none while the media are unknown (the cache alone serves them).
    std::optional<std::filesystem::path> mediaRoot;
    /// The media root the paths are relative to, also while it is out of reach (I3 step 12: the cover loader tries it
    /// again after the catalog's refresh); none when it could not be resolved.
    std::optional<std::filesystem::path> mediaRootPath;
    /// The media root the CRTs' games were read under (FloorSources::screenMediaRoot), which a ride's floor-build job
    /// reads them under again (I4 step 7b).
    std::optional<std::filesystem::path> screenMediaRoot;
    /// The hero CRT's games at the start: the start room's (RoomCollection::screenGames; none without a start room or
    /// while the media are unknown). Each room's are its RoomCollection's (the CRT of the eye's room, I4 step 6b).
    std::vector<ScreenGame> screenGames;
    /// The floor's prefabs that fell back (each room's scene data's, by the manifest the plan names).
    std::vector<ObjectFallback> objectFallbacks;
    /// Rooms' items files written this run (they changed), and why one could not be (the floor shows all the same).
    /// The floor's items whose text was known.
    bool itemsWritten = false;
    std::optional<Error> itemsError;
    std::size_t itemsRead = 0;
    /// Milliseconds on the worker: the lock and the plan read under it, the selections, the rooms' data, the
    /// generation or update with its validation (the data included), the plan's write, the rooms' items files, the
    /// floor's scene data and scene, the layouts and the cover requests, the whole job (the building's and the
    /// floor's).
    double lockMs = 0.0;
    double selectMs = 0.0;
    double dataMs = 0.0;
    double planMs = 0.0;
    double saveMs = 0.0;
    double itemsMs = 0.0;
    double sceneMs = 0.0;
    double layoutMs = 0.0;
    /// The floor's build alone (buildFloor()).
    double floorMs = 0.0;
    double milliseconds = 0.0;

    /// The collection room `room`, or nullptr.
    [[nodiscard]] const RoomCollection* findRoom(const world::RoomId& room) const noexcept;
    /// Every collection room's items in all.
    [[nodiscard]] std::size_t itemCount() const noexcept;
};

/// What the floor-build job reads besides the plan: each system's room items (by system: a system it lacks is read from
/// its items file), the media root (the covers' and the CRTs', known), the media root the paths are relative to, and
/// the media root the CRTs' games are read under (I4 step 6b: the building's known media root; the preview's resolved
/// from the library's settings and found readable, its covers served from the cache alone all the same).
struct FloorSources {
    std::map<catalog::SystemName, RoomItems> items;
    std::optional<std::filesystem::path> mediaRoot;
    std::optional<std::filesystem::path> mediaRootPath;
    std::optional<std::filesystem::path> screenMediaRoot;
    /// The preview's and a ride's (catalogLastPlayed()): each game's latest play session that counts (seconds since
    /// 1970, catalog_db::lastPlaySessions()), which the CRT games' last plays take in place of the items files' (a game
    /// played since their last write: the CRT starts on the game the building's result would choose, ADR-0012 §7).
    /// None: the items' own.
    std::optional<std::map<catalog::GameId, std::int64_t>> lastPlayed;
};

/// What the building job's last plan was made from (see the file comment): SHA-256 digests of the plan, of the
/// catalog's stamp and of the world's inputs.
struct BuildingStamp {
    std::string planDigest;
    std::string catalog;
    std::string inputs;
    /// The validation's soft findings of the plan (WorldBuild::softFindings), which a skipped start reports.
    std::size_t softFindings = 0;

    bool operator==(const BuildingStamp&) const = default;
};

/// The building stamp's schema (raised whenever what the job writes beside the plan changes: the items files').
inline constexpr int kBuildingStampVersion = 2;

/// <cacheDir>/world/<library>/building-stamp.json.
[[nodiscard]] std::filesystem::path buildingStampFile(const std::filesystem::path& cacheDir,
                                                      const catalog::LibraryId& library);
/// Reads the stamp. Errors: NotFound when there is none; ParseError for another shape; Unsupported for another schema.
[[nodiscard]] Expected<BuildingStamp> readBuildingStamp(const std::filesystem::path& file);
/// Writes the stamp atomically unless the file holds that text already; whether it wrote. Errors: those of the write.
[[nodiscard]] Expected<bool> writeBuildingStamp(const std::filesystem::path& file, const BuildingStamp& stamp);
/// Removes the stamp, when there is one (logged; a failure is logged too).
void removeBuildingStamp(const std::filesystem::path& file);
/// The digest of the world's inputs besides the catalog and the plan (see the file comment), the media root `mediaRoot`
/// included. Never fails: a file that cannot be read counts as such.
[[nodiscard]] std::string worldInputsDigest(const WorldPaths& paths, const catalog::LibraryId& library,
                                            const std::filesystem::path& mediaRoot);

/// The building job's result: the build with its plan (no floor yet) and what the floor's build reads.
struct BuildingResult {
    WorldBuild build;
    FloorSources sources;
    /// The catalog's snapshot as the job read it: how long it took and when it was done.
    std::optional<double> snapshotMs;
    std::optional<std::chrono::system_clock::time_point> snapshotAt;
};

/// The building job (see the file comment). Never fails as a whole: what went wrong is its outcome, notice and reason.
/// Returns early (NoPlan, "cancelled") once `job` is cancelled.
[[nodiscard]] BuildingResult planBuilding(const WorldBuildInput& input, const core::JobContext& job);

/// The room items of `snapshot`'s system as the building job keeps them for the floors' builds (room_items.hpp): every
/// placement of `plan` of a game the snapshot lists, and every placement in a room of `plan` that holds the system (its
/// sections: a console room, an expansion, a shared or arcade room) whatever the snapshot says (a game it no longer
/// lists keeps its slot with no title, year or cover; so does, in a shared or arcade room, a game of its other sections,
/// whose own items give its text: buildFloor() takes the section's that has it), each with its catalog title, the title
/// of its file name, its year, its cover's path and its availability (world::collectionTitles' rule); in GameId order,
/// each game once.
[[nodiscard]] RoomItems snapshotRoomItems(const RoomSnapshotResult& snapshot, const world::WorldPlan& plan);

/// The floor-build job: `build`'s floor (WorldBuild::requestedFloor, else its start room's, else kStartFloor) of its
/// plan made into its scene, its rooms' collections, content digests, cover requests, CRT games and doorway cameras, the
/// elevator's zone and arrival camera (`sources`' items, else the items files). The scene's frame is the start room's
/// on its floor, the floor's on any other. A plan that lacks the start room (no floor requested) sets startRoomMissing
/// and builds nothing; a requested floor the plan lacks, or a floor that cannot be made, leaves the build with no plan
/// and its notice (logged). Nothing for a build without a plan.
void buildFloor(const WorldPaths& paths, WorldBuild& build, const FloorSources& sources);

/// I4b step 6's review (ADR-0015 §5 as amended): `rooms` of `build`'s floor rebuilt alone from its plan, for a
/// regeneration or an undo that changed only them and kept their places. `build` is the floor shown as a template
/// (its collections, frame, elevator, arrival and doorways; no scene needed) with the plan and digest to show; each
/// room's scene data is read as buildFloor() reads it (loadSceneData() with the lighting profile and its first system's
/// platform tags) and kept in its collection (roomStaticPart() reads it), its collection, covers, content digest, CRT's
/// games and doorway camera made as buildFloor() makes them, in its place on the floor shown (its scene placement and
/// doorway kept). The other rooms stay as `build` holds them. Errors (nothing usable: build the floor): a room the plan
/// or the floor shown lacks, its data, its layout.
[[nodiscard]] Status rebuildRoomsAlone(const WorldPaths& paths, WorldBuild& build, std::span<const world::RoomId> rooms,
                                       const FloorSources& sources);

/// An unchanged start's building job (see the file comment): the stored plan `stored` as it is when the building stamp
/// says the catalog (its stamp `catalogStamp`) and the world's inputs are those it was made from, its file still that
/// plan, the media root of `profile` readable and every room's items file there; none otherwise (the whole job runs).
[[nodiscard]] std::optional<BuildingResult> unchangedBuilding(const WorldPaths& paths,
                                                              const catalog::LibraryProfile& profile,
                                                              const world::StoredPlan& stored,
                                                              const std::string& catalogStamp,
                                                              const std::optional<world::RoomId>& startRoom);

/// planBuilding() then buildFloor() (the tests' one call).
[[nodiscard]] WorldBuild buildWorld(const WorldBuildInput& input, const core::JobContext& job);

/// The preview job (I3 step 10c, the persisted start): `stored`'s start floor as it is, before the catalog is read:
/// buildFloor() with the rooms' items files (room_items.hpp, read from <cache>/world/<library>/), its covers served from
/// the derived-texture cache alone (no media root), its rooms' CRT games from the items under `screenMediaRoot` (none
/// without it), their last plays `lastPlayed`'s when given (FloorSources::lastPlayed: the catalog's play sessions, read
/// before the snapshot). Outcome Kept, `preview` set, no change report, nothing written. Never fails as a whole: a
/// floor that cannot be made has no scene (logged).
[[nodiscard]] WorldBuild previewWorld(const WorldPaths& paths, const catalog::LibraryId& library,
                                      const world::StoredPlan& stored, const std::optional<world::RoomId>& startRoom,
                                      const std::optional<std::filesystem::path>& screenMediaRoot = std::nullopt,
                                      std::optional<std::map<catalog::GameId, std::int64_t>> lastPlayed = std::nullopt);

/// Each game's latest play session that counts, in seconds since 1970 (catalog_db::lastPlaySessions()), read on the
/// world's `reader` under its mutex: FloorSources::lastPlayed of the floors built from the items files (the persisted
/// start's preview; I4 step 7b: a ride's destination, whose games may have been played since the items were written).
/// None when it cannot be read (logged: the items files' own then). On the worker pool.
[[nodiscard]] std::optional<std::map<catalog::GameId, std::int64_t>> catalogLastPlayed(WorldCatalogReader& reader);

/// The media root of the library `profile` describes as ES-DE's settings give it now (esde::resolveMediaRoot), when it
/// can be read (a folder there): the preview's CRT games are read under it (I4 step 6b). On the worker pool: it reads
/// es_settings.xml and checks the folder.
[[nodiscard]] std::optional<std::filesystem::path> readableMediaRoot(const catalog::LibraryProfile& profile);

/// The systems a building snapshot reads (takeBuildingSnapshot()): those world::classify() gives a room (ADR-0013 §2:
/// never an excluded one), by the asset root's building.json and systems.json; every system when they cannot be read
/// (the building job then fails on them, logged).
[[nodiscard]] SystemFilter roomSystemsFilter(const std::filesystem::path& assetRoot);

/// The floor `startRoom` stands on in `plan` (kStartFloor without one); none when the plan lacks the room.
[[nodiscard]] std::optional<std::string> startFloorOf(const world::WorldPlan& plan,
                                                      const std::optional<world::RoomId>& startRoom);

/// The placement of a room standing at `room` on a floor, in the frame of the room standing at `start` on it (a half
/// turn and a translation, exact): {0, 0, 0} for the start room itself.
[[nodiscard]] world::RoomPlacement relativePlacement(const world::RoomPlacement& room,
                                                     const world::RoomPlacement& start) noexcept;

/// `sceneFile` (a floor's scene file, world::toFloorSceneFile's, in the floor's frame) moved into the frame of the room
/// standing at `start`: every object's position and yaw, every light's position and target, every area's boxes and the
/// camera, the positions rounded to micrometres and the yaws to 1e-4 degrees as the scene builder rounds them (so that
/// the start room's own objects get back the numbers of its standalone scene).
void moveSceneFileToRoom(nlohmann::json& sceneFile, const world::RoomPlacement& start);

/// The empty shell shown while the building is made: the start room's template's empty room in its own frame
/// (--start-room), else the lobby of a building of no room (the ground floor's hall and its elevator).
[[nodiscard]] Expected<scene::SceneDescription> worldShellScene(const std::filesystem::path& assetRoot,
                                                                const std::optional<world::RoomId>& startRoom);

/// What the flow ends with (WorldFlow::takeResult()).
struct WorldResult {
    /// The test room instead of a generated building (no catalog; no room for any system and no plan stored), with its
    /// notice and why.
    bool testRoom = false;
    std::string notice;
    std::string reason;
    /// The catalog's state when it did not open ("no-library", ...: catalog_worker.hpp's toString), for the notice.
    std::string catalogState;
    /// The floor's build (unless testRoom).
    std::optional<WorldBuild> build;
};

/// Where the flow stands.
enum class WorldPhase : std::uint8_t {
    WaitingForCatalog, ///< the catalog opens
    ReadingPlan,       ///< the stored plan is read on the pool
    Previewing,        ///< the stored plan's start floor is made on the pool, or waits to be taken (takePreview())
    Building,          ///< the building job runs on the pool (its snapshot read within it), or waits for a refresh
    Floor,             ///< the floor-build job runs on the pool
    Ready,             ///< a result waits to be taken
    Done,              ///< taken
};

/// "waiting-for-catalog", "reading-plan", "previewing", "building", "floor", "ready", "done".
[[nodiscard]] std::string_view toString(WorldPhase phase) noexcept;

/// When each part of the flow ended, from its start (milliseconds of the steady clock).
struct WorldFlowTimes {
    std::optional<double> catalogMs;
    std::optional<double> planReadMs;
    /// The preview made (I3 step 10c), and its job's own time.
    std::optional<double> previewMs;
    std::optional<double> previewJobMs;
    /// The building planned (its snapshot read within its job), and the floor built.
    std::optional<double> snapshotMs;
    std::optional<double> builtMs;
    /// The snapshot as the building job timed it, and when it was done (the run report counts from the process's
    /// creation).
    std::optional<double> snapshotWorkerMs;
    std::optional<std::chrono::system_clock::time_point> snapshotAt;
};

/// The main thread's side of the flow (see the file comment): each update() moves it on with what the catalog and the
/// pool answered; nothing waits. Main thread only. Its jobs hold what they need: a job still running when the flow
/// goes is cancelled (its generation advanced) and its result dropped.
class WorldFlow {
public:
    struct Options {
        WorldPaths paths;
        /// --start-room: the room the run starts in (its floor built, its frame the scene's).
        std::optional<world::RoomId> startRoom;
        /// The plan's meta sidecar's writer ("retroverse <version>").
        std::string writer;
        /// Called by a job once its result is posted (AppContext::wakeCallback(): thread-safe).
        std::function<void()> wake;
        /// The stored plan's floor is made before the building job (I3 step 10c's persisted start); false for the live
        /// update after a refresh (I3 step 15b), whose floor already shows: the building job at once.
        bool preview = true;
        /// The floor the building's result is built for, asked when its floor-build job is submitted (I4 step 7b: the
        /// floor the walker is on after a ride; WorldBuild::requestedFloor); unset or none: the start room's floor.
        std::function<std::optional<std::string>()> floor;
    };

    WorldFlow(Options options, core::WorkerPool& pool);
    ~WorldFlow();
    WorldFlow(const WorldFlow&) = delete;
    WorldFlow& operator=(const WorldFlow&) = delete;

    /// After the catalog's poll (ProductUi::update or onIdle): asks what the next step needs, takes what came.
    void update(AppCatalog& catalog);

    [[nodiscard]] WorldPhase phase() const noexcept { return m_phase; }
    /// Something the floor depends on is still on its way (lockstep runs wait for it between frames).
    [[nodiscard]] bool busy() const noexcept { return m_phase != WorldPhase::Ready && m_phase != WorldPhase::Done; }
    /// The result, once (phase Ready, then Done).
    [[nodiscard]] std::optional<WorldResult> takeResult();
    /// The stored plan's floor (previewWorld()), once, when a plan was stored (phase Previewing). The building job is
    /// asked for at the next update() after it is taken: show it at this frame boundary.
    [[nodiscard]] std::optional<WorldBuild> takePreview();
    [[nodiscard]] const WorldFlowTimes& times() const noexcept { return m_times; }

    /// What the jobs and the flow share (world_build.cpp).
    struct Mailbox;

private:
    void finish(WorldResult result);
    /// Submits the building job (the stored plan's placed games asked for their aliases), once no refresh of the
    /// catalog runs: the job then reads the catalog the refresh made (I3's room snapshot queued behind a refresh on the
    /// catalog worker likewise), so that a lockstep run is the same every time.
    void submitBuilding(AppCatalog& catalog);
    /// Submits the floor-build job of the building's result.
    void submitFloor(BuildingResult result);
    /// The flow ends with `build`, an error before any plan (the shell's notice "failed").
    void fail(std::string reason);
    [[nodiscard]] double elapsedMs() const;

    Options m_options;
    core::WorkerPool& m_pool;
    core::JobGeneration m_generation;
    std::shared_ptr<Mailbox> m_mailbox;
    WorldPhase m_phase = WorldPhase::WaitingForCatalog;
    std::optional<WorldResult> m_result;
    catalog::LibraryId m_library;
    std::shared_ptr<WorldCatalogReader> m_reader;
    std::optional<StoredWorldPlan> m_stored;
    /// The preview made, until taken; whether it was taken.
    std::optional<WorldBuild> m_preview;
    bool m_previewTaken = false;
    /// The building job waits for a refresh of the catalog to end (submitBuilding()).
    bool m_buildingQueued = false;
    std::chrono::steady_clock::time_point m_start;
    WorldFlowTimes m_times;
};

} // namespace rv::app
