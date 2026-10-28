#pragma once

// The editor in the app (I4b step 3; ADR-0015 §4): an edit session's state, kept by app::WorldSession (m_edit) and
// driven by its methods in world_edit.cpp. GPU-free itself: the session's methods do what touches the scene.
//
//   main thread (WorldSession)                         worker pool (one job at a time, the session's JobGeneration)
//   ------------------------------------------------   ---------------------------------------------------------------
//   beginEdit(): the state Edit, the bar ----------->   prepare: the world's data for the floor (prepareEditData()),
//   ("Preparazione della stanza...")                    overrides.json, the floor of the plan shown (buildFloor())
//   Prepared: the draft, the edited room's targets <-
//   an operation (interact, edit_pin) ------------->   applyEdit() on a copy of the draft; the floor to show: the
//   (Busy: input waits)                                 edited rooms laid out again (editedRoomCollections(): only
//                                                       placements moved), or the floor built again (dressing gave way)
//   Shown: applied in place, room by room (the live  <-
//   update's path), or the floor committed again
//   "Annulla ultima azione", "Esci senza salvare" -->   the floor of the draft's previous state, or of the stored plan
//   "Salva" -------------------------------------->    saveDraft() under the world lock (replayed over a changed plan)
//   Saved: the session ends, or stays with reasons <-
//
// Lockstep runs (--fixed-dt) wait for a job between frames (WorldSession::waits()), so a scripted session shows each
// edit on the frame after the one that asked it. Live updates (a refresh's building job) wait for the session's end.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/app/edit_bar_ui.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_edit_draft.hpp>
#include <rv/app/world_edit_interaction.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_edit.hpp>

namespace rv::app {

/// What the session's jobs read besides their own input (WorldSession::editJobContext()).
struct EditJobContext {
    WorldPaths paths;
    catalog::LibraryId library;
    std::string floor;
    std::optional<world::RoomId> startRoom;
    FloorSources sources;
    world::RoomId room;
    /// The session's edit data (none while it is prepared): the building's layout for the plan as shown.
    std::shared_ptr<const world::EditData> data;
    /// The catalog's reader for the CRTs' last plays (catalogLastPlayed()), as a ride's floor reads them; none: the
    /// items files'.
    std::shared_ptr<WorldCatalogReader> reader;
};

/// What an editor's job answers.
struct EditJobAnswer {
    enum class Kind : std::uint8_t {
        Prepared, ///< the session's data and the floor of the plan shown
        Shown,    ///< an operation applied (or refused), an undo or a discard: the floor to show
        Saved,    ///< the draft saved (or refused), with the floor to show when it was replayed
    };
    Kind kind = Kind::Prepared;
    /// Why the job failed ("" none): the session cannot go on with it (logged).
    std::string error;
    /// Prepared: the edit data, the draft's base (the plan as stored, its file's digest, the overrides read) and
    /// whether the stored plan is not the one shown (its digest differs: the session shows the stored one).
    std::shared_ptr<const world::EditData> data;
    std::optional<EditBase> base;
    bool storedChanged = false;
    /// The floor to show (Prepared: the plan shown's; none for a refused operation, a save that needs no new floor),
    /// whether it is a whole floor built again (its scene new: committed again) or the shown floor with its edited rooms
    /// laid out again.
    std::optional<WorldBuild> build;
    bool fullFloor = false;
    /// Shown of an operation: the operation, what applyEdit() said, the draft's world after it.
    std::optional<world::EditOperation> operation;
    std::optional<world::EditResult> result;
    std::optional<world::EditedWorld> world;
    /// Saved: what saveDraft() did, and why the floor of a replayed save cannot be shown ("" none: the save stands
    /// all the same).
    std::optional<EditSaveOutcome> saved;
    std::string displayError;
    /// The job's own time (milliseconds).
    double milliseconds = 0.0;
};

/// Where the editor's jobs post (shared with them: a job outliving the session posts into it harmlessly).
struct EditJobBox {
    std::mutex mutex;
    std::optional<EditJobAnswer> answer;
    std::uint64_t generation = 0;
    std::function<void()> wake;
};

/// What the job in flight is for.
enum class EditJob : std::uint8_t { None, Prepare, Operation, Undo, Save, Discard };

/// An edit session (see the file comment).
struct WorldEditState {
    explicit WorldEditState(InteractionTimings timings, bool reduceMotion) : interaction(timings, reduceMotion) {}

    world::RoomId room;
    EditInteraction interaction;
    /// The edited room's targets (from the floor shown), the draft (once prepared), the floor as shown now without its
    /// scene (the template the fast path lays rooms out on).
    std::unique_ptr<EditRoom> editRoom;
    std::unique_ptr<EditDraft> draft;
    std::shared_ptr<const WorldBuild> shown;
    /// The room's family's item size and spine facing (where a held game would stand in a free slot).
    glm::vec3 itemSize{0.0f};
    world::SpineFacing spineFacing = world::SpineFacing::End;
    /// The catalog's reader the session's floors read the CRTs' last plays with.
    std::shared_ptr<WorldCatalogReader> reader;
    /// The job in flight, its mailbox and generation, when it was asked.
    EditJob job = EditJob::None;
    std::shared_ptr<EditJobBox> box;
    core::JobGeneration generation;
    std::chrono::steady_clock::time_point asked;
    /// The record's operation shown by the last answer, waiting for the next rendered frame (its shownMs).
    std::optional<std::size_t> shownPending;
    std::optional<std::chrono::steady_clock::time_point> shownAt;
    /// The save's end waiting for the room back (backMs).
    std::optional<std::chrono::steady_clock::time_point> endAsked;
    /// "Esci senza salvare" could not show the stored plan once and asked its whole floor again.
    bool discardRetried = false;
    /// Synthetic digests of the drafts shown ("edit:<session>:<n>": never a file's).
    std::uint64_t session = 0;
    std::uint64_t shownCount = 0;
    /// The last refusal (its rule; "" none), whether it was the save's.
    std::string refusal;
    bool saveRefused = false;
    std::string ending;
    /// The collection's flags the editor set: the held item's static records hidden, the outlined target.
    world::PickId hidden = world::kNoPick;
    world::PickId outlined = world::kNoPick;
    /// The device used last (the bar's glyphs).
    input::DeviceKind device = input::DeviceKind::KeyboardMouse;
    /// The session's record (also pushed into the run report's world.edits at its end, or the run's).
    WorldEditRecord record;
};

} // namespace rv::app
