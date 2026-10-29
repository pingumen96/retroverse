#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The regenerations and the undo of the app (I4b step 6; ADR-0015 §5, §6): what the jobs on the worker pool do for
// "Rigenera stanza / piano / mondo" and "Annulla ultima modifica", from the impact preview to the commit. The app's
// side (the panel, the jobs, the floor shown again, the walker at its room's doorway) is world_regenerate.cpp's
// (app::WorldSession); this header is what a test can run without a window.
//
//   the preview (a dry run, writing nothing, not even world.lock: world::readWorldUnlocked())
//     the inputs: the catalog's building snapshot (takeBuildingSnapshot(), kept while the catalog is not refreshed:
//     RegenerationInputs), each system's selection (world::selectRoom(): nothing is decided from unknown media), the
//     world's configuration and data, one caching room-data loader (world::assetRootLoader(): every preview of the
//     session reads a room's data once);
//     world::regenerate() of the scope, the overrides kept: its impact as the panel's content (previewContentOf()).
//   the commit (under the world lock)
//     the plan and the overrides read again: the preview's own result committed when they are what it read and the
//     overrides are kept; the drop refused (changed) when either differs from what the preview showed (the consent to
//     drop is to the customizations it named); else the scope regenerated again with the overrides kept; a plan that
//     breaks a hard rule is never written; world::commitWorld() behind a snapshot (action "regenerate", the scope's
//     text), which "Annulla ultima modifica" and `worldplan undo` restore; the plan committed is the regeneration's own
//     (its digest the commit's: nothing read back); when it changed anything, the rooms' items files written again for
//     the new plan (a game placed anew has its title and cover for the floors' builds) and the building stamp removed
//     (the next start runs the building job: the plan is not the one the stamp names).
//   the undo
//     its preview reads the snapshots (world::scanSnapshots(), no lock): the snapshot undo restores next, or why there
//     is none (nothing to undo, a damaged folder, an action in progress); its commit, under the lock, undoes it
//     (world::undoLastAction()) when it is still that one, then the items files and the stamp as above.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/app/room_items.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/ui/regenerate_preview_model.hpp>
#include <rv/world/building.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_snapshots.hpp>

namespace rv::app {

/// The panel's failure keys (ui::regenerate_preview::kFailures), as the jobs name them.
namespace regenerate_failure {
inline constexpr std::string_view kBusy = "busy";
inline constexpr std::string_view kChanged = "changed";
inline constexpr std::string_view kMediaUnknown = "media-unknown";
inline constexpr std::string_view kInvalidPlan = "invalid-plan";
inline constexpr std::string_view kNothingToUndo = "nothing-to-undo";
inline constexpr std::string_view kDamaged = "damaged";
inline constexpr std::string_view kNoPlan = "no-plan";
inline constexpr std::string_view kFailed = "failed";
} // namespace regenerate_failure

/// What a regeneration reads besides the world's files: the catalog's building snapshot and each system's selection,
/// the world's configuration and data, the rooms' data through one caching loader. Immutable once made; the loader
/// keeps its own cache behind a mutex (a cancelled job may still run beside the next one).
struct RegenerationInputs {
    catalog::LibraryId library;
    std::shared_ptr<const BuildingSnapshot> snapshot;
    /// One per system of the snapshot with entries (a system that gives no room is not in the snapshot).
    std::vector<world::RoomSelection> selections;
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    world::WorldConfig config;
    world::BuildingData building;
    world::SystemsData systems;
    world::RoomDataLoader load;
    /// Milliseconds: the snapshot (taken by takeRegenerationInputs()), the selections.
    double snapshotMs = 0.0;
    double selectMs = 0.0;
};

/// The inputs, or why they could not be made (`failure`: regenerate_failure's key; `error`: English, for the log).
struct RegenerationInputsResult {
    std::shared_ptr<const RegenerationInputs> inputs;
    std::string failure;
    std::string error;
};

/// The inputs of `snapshot` (the catalog's systems as the building job reads them), with the world's configuration
/// and data read from `paths`. Fails (media-unknown) when a system's media are unknown (ADR-0010 §3), or (failed) when
/// the snapshot, the configuration or the data cannot be read.
[[nodiscard]] RegenerationInputsResult regenerationInputsOf(const WorldPaths& paths,
                                                            std::shared_ptr<const BuildingSnapshot> snapshot);
/// The catalog's building snapshot taken on `reader` (its mutex held while it reads), then regenerationInputsOf().
[[nodiscard]] RegenerationInputsResult takeRegenerationInputs(const WorldPaths& paths, WorldCatalogReader& reader);

/// A preview: the scope's dry run on the world as it was read, and the panel's content.
struct RegenerationPreview {
    world::RegenerationScope scope;
    /// Why there is no preview ("" there is one: regenerate_failure's key) and, for the log, the error.
    std::string failure;
    std::string error;
    /// The dry run (overrides kept), and what it ran on: the plan, its file's digest, the overrides as read.
    std::shared_ptr<const world::Regeneration> regeneration;
    std::shared_ptr<const world::WorldPlan> previous;
    std::string planDigest;
    world::WorldOverrides overrides;
    /// The systems whose selections were left out (no data yet for a room they would enter), as the CLI leaves them.
    std::vector<std::string> leftOut;
    ui::RegeneratePreviewContent content;
    /// Milliseconds: the world read, the regeneration (its rooms' data included), the whole preview.
    double readMs = 0.0;
    double regenerateMs = 0.0;
    double totalMs = 0.0;
};

/// The dry run of `scope` on the world as world::readWorldUnlocked() reads it (writing nothing), with `inputs`.
/// Failures: busy (an action in progress or interrupted, the world changing while it is read), no-plan, failed (the
/// plan refused, the room or floor not in the plan, a scope the plan needs the world for, the regeneration's errors);
/// a preview whose regenerated plan breaks a hard rule is invalid-plan (nothing could be committed).
[[nodiscard]] RegenerationPreview previewRegeneration(const WorldPaths& paths, const world::RegenerationScope& scope,
                                                      const RegenerationInputs& inputs);

/// The panel's content of a regeneration (its impact's counts, its conflicts with their games' titles from `inputs`'
/// snapshot, at most ui::regenerate_preview::kConflictsListed of them, a fixture's by its role in `previous`; the
/// scope's customizations the drop would remove: the pins kept and the conflicts).
[[nodiscard]] ui::RegeneratePreviewContent previewContentOf(const world::Regeneration& regeneration,
                                                            const world::WorldPlan& previous,
                                                            const RegenerationInputs* inputs);

/// What a commit did.
struct RegenerationCommit {
    /// Why nothing was written ("" it went through: regenerate_failure's key), and the error for the log.
    std::string failure;
    std::string error;
    /// The regeneration changed the plan or the overrides (false: nothing to write, no items files written, no stamp
    /// removed); they were written (false: the folder held them already), behind the snapshot named.
    bool changed = false;
    bool written = false;
    std::string snapshot;
    /// The preview's own result was committed (the world as it read it, the overrides kept); else the scope was
    /// regenerated again under the lock.
    bool reused = false;
    /// The plan stored now and its digest; the impact of what was committed.
    std::shared_ptr<const world::WorldPlan> plan;
    std::string planDigest;
    world::RegenerationImpact impact;
    /// The rooms' items written again (for the floor's build), by system.
    std::map<catalog::SystemName, RoomItems> items;
    /// Milliseconds: the lock and the reads under it, a second regeneration, the commit, the items files, all.
    double lockMs = 0.0;
    double regenerateMs = 0.0;
    double commitMs = 0.0;
    double itemsMs = 0.0;
    double totalMs = 0.0;
};

/// Commits `preview`'s scope with the consent `dropOverrides` (ADR-0015 §5), as the file comment says; `writer` names
/// the program in the snapshot and the meta sidecar. Failures: busy (the lock), no-plan, changed (the drop asked over a
/// plan or overrides other than the preview's), invalid-plan, failed.
[[nodiscard]] RegenerationCommit commitRegeneration(const WorldPaths& paths, const RegenerationPreview& preview,
                                                    bool dropOverrides, const RegenerationInputs& inputs,
                                                    std::string_view writer,
                                                    std::chrono::milliseconds lockWait = world::kWorldLockWait);

/// An undo's preview: the snapshot it would restore and the panel's content.
struct UndoPreview {
    std::string failure;
    std::string error;
    std::optional<world::WorldSnapshot> snapshot;
    ui::RegeneratePreviewContent content;
    double totalMs = 0.0;
};

/// The snapshot "Annulla ultima modifica" restores next (world::nextUndo() of the snapshots as they are, no lock), with
/// its action, scope and time; `plan` (the plan shown) names a room or a floor of its scope. Failures: busy (a journal:
/// an action in progress or interrupted), damaged (a snapshot folder that cannot be read: undo is refused while one is
/// there), nothing-to-undo.
[[nodiscard]] UndoPreview previewUndo(const WorldPaths& paths, const catalog::LibraryId& library,
                                      const world::WorldPlan* plan);

/// What an undo did.
struct UndoCommit {
    /// Why nothing was restored ("" it was: regenerate_failure's key); for the log, the error (also of a plan restored
    /// that cannot be read back: `written` set, no plan).
    std::string failure;
    std::string error;
    /// The snapshot was restored (the plan and the overrides written).
    bool written = false;
    /// The snapshot restored and the one kept of the state it replaced.
    std::string restored;
    std::string kept;
    /// The plan stored now (lifted when the snapshot held a schema-1 plan) and its digest; none when it cannot be read
    /// back (the floor then follows the stored plan through a live update).
    std::shared_ptr<const world::WorldPlan> plan;
    std::string planDigest;
    std::map<catalog::SystemName, RoomItems> items;
    double lockMs = 0.0;
    double undoMs = 0.0;
    double itemsMs = 0.0;
    double totalMs = 0.0;
};

/// Undoes the last explicit action under the world lock when the snapshot it restores is still `expected` (else
/// changed, nothing written), reads the plan restored, then writes the rooms' items again from `snapshot` (the
/// catalog's: the app's inputs, waited for) and removes the building stamp (whenever the undo wrote, even when the plan
/// cannot be read back).
/// `before`, when given, is the plan the undo replaces as the app shows it: only the items of the systems whose rooms
/// changed are written again (keepRoomItems()).
[[nodiscard]] UndoCommit commitUndo(const WorldPaths& paths, const catalog::LibraryId& library,
                                    std::string_view expected, std::string_view writer,
                                    const BuildingSnapshot* snapshot, const world::WorldPlan* before = nullptr,
                                    std::chrono::milliseconds lockWait = world::kWorldLockWait);

/// The rooms' items of every system of `snapshot` that a room of `plan` holds, written again into their files (those
/// that changed) for the floors' builds and the next start, and the building stamp removed (the plan is no longer the
/// one it names). With `before` (the plan `plan` replaces), only the systems of the rooms whose record, placements or
/// unplaced games differ (roomsWithOtherItems(); I4b step 6's review: every system's items cost about 0.4 s on the
/// reference library). The items by system; a file that cannot be written is logged.
[[nodiscard]] std::map<catalog::SystemName, RoomItems> keepRoomItems(const WorldPaths& paths,
                                                                     const catalog::LibraryId& library,
                                                                     const BuildingSnapshot& snapshot,
                                                                     const world::WorldPlan& plan,
                                                                     const world::WorldPlan* before = nullptr);

/// The rooms whose record, placements or unplaced games differ between `before` and `after` (a room in one plan only
/// included): the rooms whose systems' items may change.
[[nodiscard]] std::set<world::RoomId> roomsWithOtherItems(const world::WorldPlan& before,
                                                          const world::WorldPlan& after);

/// The area that holds the walker (`area`: a room's id, "hall:<floor>") is one an action of `scope` changes (a room
/// scope covers its chain; "floor:<id>" the rooms `before` holds on that floor and that floor's hall, whose rooms are
/// placed again; "world" every room and every hall), so that the walker stands at its doorway, or at the elevator
/// (ADR-0015 §5). A room scope never covers a hall.
[[nodiscard]] bool regenerationCovers(std::string_view area, std::string_view scope, const world::WorldPlan* before);

/// The room whose "Rigenera stanza" the update's needs-confirmation notice offers: the first of `rooms` (the change
/// report's needsConfirmationRooms, in its sentences' order) that `plan` holds; none when it holds none of them.
[[nodiscard]] std::optional<world::RoomId> confirmationRoomOf(const std::vector<world::RoomId>& rooms,
                                                              const world::WorldPlan& plan);

/// The rooms of `floor` a commit changed when it changed nothing else of the floor (I4b step 6's review: shown alone,
/// step 4's path): the floor's rooms in `before` and `after` are the same, each record the same but its generator
/// version (its id, kind, sections, place, interior, template and its version, family and pack: the hall is the same),
/// and the rooms returned are those whose content digest differs (world::roomContentDigests()). None when the floor's
/// rooms changed otherwise (added, removed, moved, grown: the floor is built again) or a digest cannot be computed.
[[nodiscard]] std::optional<std::vector<world::RoomId>> roomsChangedAlone(const world::WorldPlan& before,
                                                                          const world::WorldPlan& after,
                                                                          std::string_view floor);

/// A job's mailbox (I4b step 6's review): the job asked last wins. reset() before a job is submitted names its
/// generation and empties the box; post() keeps an answer only from that generation or a later one (a cancelled job's
/// late answer never overwrites the next job's); take() gives the answer of the generation asked for, once.
template <class Answer>
class JobMailbox {
public:
    void reset(std::uint64_t generation) {
        const std::lock_guard lock(m_mutex);
        m_answer.reset();
        m_generation = generation;
    }
    /// Whether the answer was kept.
    bool post(std::uint64_t generation, Answer answer) {
        const std::lock_guard lock(m_mutex);
        if (generation < m_generation) {
            return false;
        }
        m_answer = std::move(answer);
        m_generation = generation;
        return true;
    }
    [[nodiscard]] std::optional<Answer> take(std::uint64_t generation) {
        const std::lock_guard lock(m_mutex);
        if (!m_answer || m_generation != generation) {
            return std::nullopt;
        }
        return std::exchange(m_answer, std::nullopt);
    }

private:
    std::mutex m_mutex;
    std::optional<Answer> m_answer;
    std::uint64_t m_generation = 0;
};

/// "dd/mm/yyyy hh:mm" of a snapshot's UTC time ("2026-10-29T10:15:30.123Z") in the machine's local time; empty for a
/// time that does not read.
[[nodiscard]] std::string localSnapshotTime(std::string_view utc);

} // namespace rv::app
