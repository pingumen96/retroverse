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
//
// I4b step 4 (furniture): a fixture held (EditMode::Moving) is drawn where it would stand (its scene items and games
// left out of their places, drawn moved and outlined, warm white where it may drop, red otherwise); each new place is
// judged by a probe on the pool (applyEdit() on a copy of the draft: its own job box, beside the session's job). An
// edit whose static part changed (a fixture moved, dressing giving way) rebuilds the edited room's static part alone
// (roomStaticPart(): its scene objects, dressing, wall-washers, lamps' lights and colliders, in place on the floor
// shown through the host's replaceObjects()), then lays its collection out again in place: never the whole floor.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/app/edit_bar_ui.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_edit_draft.hpp>
#include <rv/app/world_edit_interaction.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/light.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_edit.hpp>

namespace rv::app {

/// The synthetic scene digest of a floor shown with a room's static part rebuilt alone: "edit-scene:<session>:<n>" by
/// the editor, "edit-scene:regenerate:<n>" by a regeneration or an undo (I4b step 6's review). The scene shown is its
/// plan's: the next session takes it; the next live update commits the floor once.
inline constexpr std::string_view kEditSceneDigest = "edit-scene:";

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
    /// I4b step 4: the rooms whose static part is rebuilt alone (with `build` the shown floor's collections laid out
    /// again).
    std::vector<RoomStaticPart> statics;
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

/// A held fixture's place judged (I4b step 4): the probe and the rule that refuses it ("" none; "failed": it could not
/// be judged).
struct EditProbeAnswer {
    world::EditOperation operation;
    std::string refusal;
    double milliseconds = 0.0;
    /// What applyEdit() said and the draft's world after it (an accepted place): what a drop there shows.
    std::optional<world::EditResult> result;
    std::shared_ptr<const world::EditedWorld> world;
    /// The draft the probe judged.
    std::shared_ptr<const world::EditedWorld> base;
};

/// Where the probes post (as EditJobBox).
struct EditProbeBox {
    std::mutex mutex;
    std::optional<EditProbeAnswer> answer;
    std::uint64_t generation = 0;
    std::function<void()> wake;
};

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
    /// The collection's flags the editor set: the held items' static records hidden (a game, or a held fixture's
    /// games), the outlined target.
    std::vector<world::PickId> hidden;
    world::PickId outlined = world::kNoPick;
    /// I4b step 4: the floor's scene digest when the session began (the stored plan's static part: shown again by an
    /// undo to the base or a discard; a rebuilt room's floor has a synthetic one, `edit-scene:<session>:<n>`).
    std::string baseSceneDigest;
    /// The plan's derived state (the plan state, its where-is index, the card's versions, the launch's placements)
    /// waits for the session's end (WorldSession::keepShown(), endEdit()).
    bool planStatePending = false;
    /// The last probe's answer, whose edited world a drop of the same place takes instead of applying it again.
    std::optional<EditProbeAnswer> lastProbe;
    /// The probes: their box and generation, the one in flight, the next asked meanwhile, the draft they judge.
    std::shared_ptr<EditProbeBox> probeBox;
    core::JobGeneration probeGeneration;
    std::optional<world::EditOperation> probing;
    std::optional<world::EditOperation> probeNext;
    std::shared_ptr<const world::EditedWorld> probeWorld;
    /// The fixture drawn held: its id, the scene's render items of it (left out of the view), its games, its move.
    std::optional<world::FixtureId> heldFixture;
    std::vector<std::size_t> heldSceneItems;
    std::vector<world::PickId> heldGames;
    /// Its lights (a lamp's) as they stood, by their indices into the scene's lights, and the place they were moved to
    /// last (moveHeldLights()).
    std::vector<std::pair<std::size_t, scene::Light>> heldLights;
    std::optional<world::FixtureTransform> heldLightsAt;
    /// The device used last (the bar's glyphs).
    input::DeviceKind device = input::DeviceKind::KeyboardMouse;
    /// The session's record (also pushed into the run report's world.edits at its end, or the run's).
    WorldEditRecord record;
};

} // namespace rv::app
