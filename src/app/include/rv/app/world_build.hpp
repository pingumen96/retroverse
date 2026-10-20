#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The generated room of retroverse.exe, up to what the main thread commits (I3 step 10b; ADR-0010 §9 to §12, ADR-0011
// §1, §6):
//
//   main thread (WorldFlow::update, every frame      worker pool (kHighestPriority,       catalog worker
//   and every idle wake, after the catalog's poll)   the world's JobGeneration)
//   ---------------------------------------------   ---------------------------------    ---------------------------
//   the catalog opened Ready with games of the
//   room's system ------------------------------->  readWorldPlan: the world's stored
//                                                    plan (world-plan.json), its digest
//   a plan stored: ------------------------------>  previewWorld (I3 step 10c): the
//                                                    stored plan's room as it is, the
//                                                    room's items' text from the cache
//   takePreview(): shown at a frame boundary <-----
//   (the persisted start), then
//   requestRoomSnapshot(placed games) <------------
//     ------------------------------------------------------------------------------>  takeRoomSnapshot: the bulk
//                                                                                         query, the media root, the
//                                                                                         paths, the aliases
//   the snapshot taken (poll) ------------------->  buildWorld: the world lock, the
//                                                    plan read again under it, the
//                                                    selection, a first plan (generate)
//                                                    or the incremental update, the
//                                                    validation, savePlan, the scene
//                                                    data, the scene, the collection's
//                                                    layout, the items' cover requests
//   takeResult(): committed at a frame boundary <--
//   (app::WorldSession: the scene, the collection;
//   after a preview, reconciled with the room shown:
//   world_reconcile.hpp)
//
// The preview is asked for the snapshot only once it has been taken (shown), so that the stored room always shows
// before the snapshot answers (ADR-0010 §12: "shows the room at once, then reconciles it"), and the catalog worker's
// first jobs are the quick catalog's.
//
// The catalog that does not open, or has no games of the room's system, gives the test room (WorldResult::testRoom)
// with its notice; a room that cannot be made now (the media root out of reach with no plan stored, a plan that breaks
// a hard rule, an error) gives its build with no plan, which the app shows as the empty room's shell with its notice.
// Nothing is written but the world's own files (under <user data>/worlds/<library id>/, the data-directory guard having
// passed when the catalog opened) and nothing is written at all while the media are unknown (ADR-0010 §3).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/collection_layers.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/app/world_notice_ui.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

namespace rv::app {

/// The system whose room the app generates and shows (ES-DE's name; ADR-0010: one room, SNES). Since I4 step 1 the
/// plan is a building (ADR-0013), of which the app updates and shows this system's room in its own frame; the floors
/// come with I4 step 5a.
inline constexpr std::string_view kRoomSystem = "snes";

/// Where the world's jobs read and write.
struct WorldPaths {
    std::filesystem::path assetRoot;
    std::filesystem::path userDataRoot;
    /// <user data>/config (world.json).
    std::filesystem::path configDir;
    /// <user data>/cache (the derived textures under cache/textures/<library id>).
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

/// How the room that shows came to be.
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

/// What the second job reads.
struct WorldBuildInput {
    WorldPaths paths;
    catalog::LibraryId library;
    RoomSnapshotResult snapshot;
    /// The meta sidecar's writer (world::SaveOptions::writer).
    std::string writer;
    /// How long the world lock is waited for (world::kWorldLockWait).
    std::chrono::milliseconds lockWait = world::kWorldLockWait;
};

/// A prefab of the room that fell back to its family's (style §21's object fallbacks, assets::resolvePrefab: logged
/// once).
struct ObjectFallback {
    /// The manifest the plan names, what replaced it (assets::toString(PrefabSource)) and why.
    std::string prefab;
    std::string source;
    std::string reason;

    bool operator==(const ObjectFallback&) const = default;
};

/// The room to commit (an immutable value made on the pool, ADR-0010 §12).
struct WorldBuild {
    /// The stored plan's room shown before the catalog's snapshot answers (previewWorld(), I3 step 10c: outcome Kept,
    /// the covers of the room's items the cache knows, no media root, no CRT games); else the room's job's result.
    bool preview = false;
    WorldOutcome outcome = WorldOutcome::NoPlan;
    /// The world notice it shows (world_notice), and why, in English (the log, the run report).
    std::string notice;
    std::string reason;
    world::RoomId room;
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
    std::optional<Error> saveError;
    /// The static part (toSceneFile read back), the data it was made with (the item's prefab, the theme) and the spans
    /// it left bare; the collection's layout and, in its item order, what the loader serves for each item.
    std::optional<scene::SceneDescription> scene;
    /// The SHA-256 of the static part's scene file (world::toSceneFile's text): the live update (I3 step 15b,
    /// world_live_plan.hpp) applies a build of the same static part without a rebuild of the scene.
    std::string sceneDigest;
    std::optional<world::SceneData> sceneData;
    std::vector<world::UndressedSpan> undressed;
    std::optional<world::CollectionLayout> layout;
    std::vector<CoverItem> covers;
    /// The media root the covers are read under; none while the media are unknown (the cache alone serves them).
    std::optional<std::filesystem::path> mediaRoot;
    /// The media root the room's paths are relative to, also while it is out of reach (I3 step 12: the cover loader
    /// tries it again after the catalog's refresh); none when it could not be resolved.
    std::optional<std::filesystem::path> mediaRootPath;
    /// The hero CRT's games (room_screen.hpp; none for a preview or while the media are unknown).
    std::vector<ScreenGame> screenGames;
    /// The room's prefabs that fell back (the scene data's, by the manifest the plan names).
    std::vector<ObjectFallback> objectFallbacks;
    /// The room's items (room_items.hpp) written this run (they changed), and why they could not be (the room shows
    /// all the same). A preview reads them: `itemsRead` of its items were known.
    bool itemsWritten = false;
    std::optional<Error> itemsError;
    std::size_t itemsRead = 0;
    /// Milliseconds on the worker: the lock and the plan read under it, the selection, the generation or update with
    /// its validation, the plan's write, the scene data and scene, the layout and the cover requests, the whole job.
    double lockMs = 0.0;
    double selectMs = 0.0;
    double planMs = 0.0;
    double saveMs = 0.0;
    double sceneMs = 0.0;
    double layoutMs = 0.0;
    double milliseconds = 0.0;
};

/// The second job (see the file comment). Never fails as a whole: what went wrong is its outcome, notice and reason.
/// Returns early (NoPlan, "cancelled") once `job` is cancelled.
[[nodiscard]] WorldBuild buildWorld(const WorldBuildInput& input, const core::JobContext& job);

/// The preview job (I3 step 10c, the persisted start): `stored`'s room of `system` as it is, before the catalog's
/// snapshot: its scene, its collection's layout with the titles of the room's items as they last showed
/// (room_items.hpp, read from <cache>/world/<library>/), and the cover requests of the items they know, served from the
/// derived-texture cache alone (no media root: the snapshot says whether it can be reached). Outcome Kept, `preview`
/// set, no change report, nothing written. Never fails as a whole: a room that cannot be made has no scene (logged).
[[nodiscard]] WorldBuild previewWorld(const WorldPaths& paths, const catalog::LibraryId& library,
                                      const catalog::SystemName& system, const world::StoredPlan& stored);

/// What the flow ends with (WorldFlow::takeResult()).
struct WorldResult {
    /// The test room instead of a generated one (no catalog, no games of the system), with its notice and why.
    bool testRoom = false;
    std::string notice;
    std::string reason;
    /// The catalog's state when it did not open ("no-library", ...: catalog_worker.hpp's toString), for the notice.
    std::string catalogState;
    /// The room's build (unless testRoom).
    std::optional<WorldBuild> build;
};

/// Where the flow stands.
enum class WorldPhase : std::uint8_t {
    WaitingForCatalog, ///< the catalog opens
    ReadingPlan,       ///< the stored plan is read on the pool
    Previewing,        ///< the stored plan's room is made on the pool, or waits to be taken (takePreview())
    Snapshot,          ///< the catalog worker takes the room's snapshot
    Building,          ///< the room is made on the pool
    Ready,             ///< a result waits to be taken
    Done,              ///< taken
};

/// "waiting-for-catalog", "reading-plan", "previewing", "snapshot", "building", "ready", "done".
[[nodiscard]] std::string_view toString(WorldPhase phase) noexcept;

/// When each part of the flow ended, from its start (milliseconds of the steady clock).
struct WorldFlowTimes {
    std::optional<double> catalogMs;
    std::optional<double> planReadMs;
    /// The preview made (I3 step 10c), and its job's own time.
    std::optional<double> previewMs;
    std::optional<double> previewJobMs;
    std::optional<double> snapshotMs;
    std::optional<double> builtMs;
    /// The snapshot as the worker timed it, and when the worker was done with it (the run report counts from the
    /// process's creation).
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
        catalog::SystemName system;
        /// The plan's meta sidecar's writer ("retroverse <version>").
        std::string writer;
        /// Called by a job once its result is posted (AppContext::wakeCallback(): thread-safe).
        std::function<void()> wake;
        /// The stored plan's room is made before the snapshot is asked for (I3 step 10c's persisted start); false for
        /// the live update after a refresh (I3 step 15b), whose room already shows: the snapshot at once.
        bool preview = true;
    };

    WorldFlow(Options options, core::WorkerPool& pool);
    ~WorldFlow();
    WorldFlow(const WorldFlow&) = delete;
    WorldFlow& operator=(const WorldFlow&) = delete;

    /// After the catalog's poll (ProductUi::update or onIdle): asks what the next step needs, takes what came.
    void update(AppCatalog& catalog);

    [[nodiscard]] WorldPhase phase() const noexcept { return m_phase; }
    /// Something the room depends on is still on its way (lockstep runs wait for it between frames).
    [[nodiscard]] bool busy() const noexcept { return m_phase != WorldPhase::Ready && m_phase != WorldPhase::Done; }
    /// The result, once (phase Ready, then Done).
    [[nodiscard]] std::optional<WorldResult> takeResult();
    /// The stored plan's room (previewWorld()), once, when a plan was stored (phase Previewing). The snapshot is asked
    /// for at the next update() after it is taken: show it at this frame boundary.
    [[nodiscard]] std::optional<WorldBuild> takePreview();
    [[nodiscard]] const WorldFlowTimes& times() const noexcept { return m_times; }

    /// What the jobs and the flow share (world_build.cpp).
    struct Mailbox;

private:
    void finish(WorldResult result);
    /// Asks the catalog worker for the room's snapshot (the stored plan's placed games for their aliases).
    void requestSnapshot(AppCatalog& catalog);
    [[nodiscard]] double elapsedMs() const;

    Options m_options;
    core::WorkerPool& m_pool;
    core::JobGeneration m_generation;
    std::shared_ptr<Mailbox> m_mailbox;
    WorldPhase m_phase = WorldPhase::WaitingForCatalog;
    std::optional<WorldResult> m_result;
    catalog::LibraryId m_library;
    std::optional<StoredWorldPlan> m_stored;
    /// The preview made, until taken; whether it was taken.
    std::optional<WorldBuild> m_preview;
    bool m_previewTaken = false;
    std::uint64_t m_serial = 0;
    std::chrono::steady_clock::time_point m_start;
    WorldFlowTimes m_times;
};

} // namespace rv::app
