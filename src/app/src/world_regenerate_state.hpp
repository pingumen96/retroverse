#pragma once

// The regenerations and the undo in the app (I4b step 6; ADR-0015 §5, §6): their state, kept by app::WorldSession
// (m_regenerations) and driven by its methods in world_regenerate.cpp. GPU-free itself.
//
//   main thread (WorldSession)                         worker pool (one job at a time, the state's JobGeneration)
//   ------------------------------------------------   ---------------------------------------------------------------
//   the directory's "Modifiche" tab shown ------------> the inputs (prewarm): the catalog's snapshot and selections,
//                                                       kept in a slot until the catalog is refreshed
//   "Rigenera stanza / piano / mondo": the preview ---> the inputs (the slot's, else taken or waited for), the dry run
//   panel opens, computing                              (previewRegeneration(); an undo: previewUndo())
//   the impact shown, the choices <-------------------
//   Rigenera / Scarta e rigenera / Annulla la modifica> the commit under the world lock (commitRegeneration(),
//   (the panel waits: committing)                       commitUndo()); the rooms it changed rebuilt alone when the
//                                                       floor's other rooms and its hall stand as they did (step 4's
//                                                       path), else the floor shown built from the plan stored now
//   the rooms' static parts replaced and their      <-
//   collections applied in place (or the floor
//   reconciled room by room, or committed again), the
//   walker at its room's doorway when that room was
//   regenerated (the elevator when it is gone, or from
//   a regenerated floor's hall); the panel closes
//
// Lockstep runs (--fixed-dt) wait for a job between frames. Rides, the editor, live updates and the directory's moves
// wait while a preview or a commit runs; a regeneration never starts while one of them is under way. A job's answer
// goes through a JobMailbox: a cancelled job's late answer never takes the next one's place. A commit that does not
// answer within kCommitPatience is given up (the panel says it failed; the floor follows the stored plan through a live
// update).

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/world_build.hpp>
#include <rv/app/world_edit_draft.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/app/world_regenerate.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/ui/regenerate_preview_model.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/regeneration.hpp>

namespace rv::app {

/// The inputs of the catalog as it is (RegenerationInputs), made once by the first job that needs them (a prewarm, a
/// preview) while the others wait for them. Shared with the jobs; a new slot for each refresh of the catalog.
struct RegenerationInputsSlot {
    std::mutex mutex;
    std::condition_variable done;
    bool running = false;
    std::optional<RegenerationInputsResult> result;
    /// How long they took (milliseconds), when made.
    double milliseconds = 0.0;
};

/// The slot's inputs: made by this call (`taken` set) unless they are made or being made (then waited for).
[[nodiscard]] RegenerationInputsResult regenerationInputsFor(RegenerationInputsSlot& slot, const WorldPaths& paths,
                                                             WorldCatalogReader* reader, bool& taken);

/// What asked for a preview: its kind, its scope (an undo's: the world), and how (the directory, the notice).
struct RegenerateRequest {
    ui::RegenerateKind kind = ui::RegenerateKind::Room;
    world::RegenerationScope scope;
    std::string via;
};

/// The floor shown when a commit's choice is made (I4b step 6's review), for its job to rebuild alone the rooms the
/// commit changed: what shows of its collections (ShownFloor), a build of it with its frame, elevator, arrival,
/// showcase and media roots but no room, plan or scene, and each room's place, doorway and doorway camera.
struct RegenerateShownFloor {
    struct Room {
        world::RoomPlacement placement;
        core::Aabb doorway;
        scene::CameraSetup doorwayCamera;
    };
    ShownFloor shown;
    WorldBuild frame;
    std::map<world::RoomId, Room> rooms;
};

/// The floor `floor` shows as a build (rebuildRoomsAlone()'s template): its frame with a collection per room shown
/// (their scene data unknown: the rooms rebuilt read theirs).
[[nodiscard]] inline WorldBuild templateOf(const RegenerateShownFloor& floor) {
    WorldBuild build = floor.frame;
    build.rooms.reserve(floor.shown.rooms.size());
    for (const ShownCollection& shown : floor.shown.rooms) {
        RoomCollection room;
        room.room = shown.room;
        room.scenePlacement = shown.scenePlacement;
        room.layout = shown.layout;
        room.covers = shown.covers;
        room.contentDigest = shown.contentDigest;
        room.screenGames = shown.screenGames;
        if (const auto known = floor.rooms.find(shown.room); known != floor.rooms.end()) {
            room.placement = known->second.placement;
            room.doorway = known->second.doorway;
            room.doorwayCamera = known->second.doorwayCamera;
        }
        build.rooms.push_back(std::move(room));
    }
    return build;
}

/// What a job answers.
struct RegenerateJobAnswer {
    enum class Kind : std::uint8_t { Preview, Commit };
    Kind kind = Kind::Preview;
    /// The inputs used (a preview's, made or waited for by it: `inputsTaken`), or why there are none.
    std::shared_ptr<const RegenerationInputs> inputs;
    bool inputsTaken = false;
    double inputsMs = 0.0;
    /// A preview's (a regeneration's or an undo's), a commit's (a regeneration's or an undo's); a failure is in them.
    std::optional<RegenerationPreview> preview;
    std::optional<UndoPreview> undoPreview;
    std::optional<RegenerationCommit> commit;
    std::optional<UndoCommit> undone;
    /// The failure of what the job could not even begin (the inputs: regenerate_failure's key), and why.
    std::string failure;
    std::string error;
    /// A commit's floor to show, or why it could not be built: the floor shown with the rooms the commit changed
    /// rebuilt alone (`alone`: their static parts in `statics`, the scene a stand-in), else the floor built from the
    /// plan stored now; `fallback` the sources a floor built here would read (a room's part that cannot be replaced in
    /// the scene).
    std::optional<WorldBuild> build;
    std::vector<RoomStaticPart> statics;
    bool alone = false;
    std::shared_ptr<const FloorSources> fallback;
    std::string buildError;
    /// The floor's (or the rooms') build in the job (milliseconds).
    double floorMs = 0.0;
    /// The job's own time (milliseconds).
    double milliseconds = 0.0;
};

/// Where the jobs post (shared with them: a job outliving the session posts into it harmlessly), and the host's wake.
struct RegenerateJobBox {
    JobMailbox<RegenerateJobAnswer> mailbox;
    /// Guards `wake` (let go of when the session ends).
    std::mutex mutex;
    std::function<void()> wake;
};

/// How long a commit may run before the panel gives it up (it says it failed; the floor follows the stored plan through
/// a live update): far beyond the reference library's world (seconds), never a lockstep run's wait.
inline constexpr std::chrono::seconds kCommitPatience{120};

enum class RegenerateJob : std::uint8_t { None, Prewarm, Preview, Commit };

/// The session's regenerations (see the file comment).
struct WorldRegenerations {
    std::shared_ptr<RegenerateJobBox> box;
    core::JobGeneration generation;
    /// The job in flight (a prewarm posts nothing: it fills the slot), when it was asked.
    RegenerateJob job = RegenerateJob::None;
    std::chrono::steady_clock::time_point asked;
    /// The catalog's inputs and the refresh they belong to.
    std::shared_ptr<RegenerationInputsSlot> slot;
    std::string slotKey;
    bool prewarmed = false;
    /// The preview under way or shown (none: no panel), its answer, the panel's state last seen, the record's index.
    std::optional<RegenerateRequest> request;
    std::shared_ptr<const RegenerationInputs> inputs;
    std::optional<RegenerationPreview> preview;
    std::optional<UndoPreview> undoPreview;
    ui::RegenerateState panelState = ui::RegenerateState::Computing;
    std::optional<std::size_t> record;
    std::chrono::steady_clock::time_point requested;
    std::chrono::steady_clock::time_point chosen;
    /// The room shown again after a commit, waiting for the next rendered frame (its backFrame).
    bool backPending = false;
    /// The rooms rebuilt alone so far (their synthetic scene digests).
    std::uint64_t scenesRebuilt = 0;
    /// The building's result last seen (WorldSession's count of recorded builds), the room its needs-confirmation
    /// notice offers to regenerate, and whether that notice waits for another notice to hide first.
    std::uint64_t confirmationBuild = 0;
    std::optional<world::RoomId> confirmationRoom;
    bool confirmationPending = false;
};

} // namespace rv::app
