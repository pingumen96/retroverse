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
#include <condition_variable>
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

/// The inputs of the catalog as it is (RegenerationInputs), made once by the first job that needs them (a prewarm, a
/// preview, an undo's commit) while the others wait for them. Shared with the jobs (a job that took the inputs holds
/// them through their shared_ptr); a new slot for each refresh of the catalog, emptied while nothing needs them
/// (releaseIdleInputs()).
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

/// Reads the data of `plan`'s standing rooms into `inputs`' caching loader (world::standingRoomRequests()), as the
/// directory's tab reads the inputs ahead (m3/preview-time: a regeneration reads every room outside its scope; cold,
/// about half a second of a room's first preview on the reference library). A room whose data cannot be read is left
/// to the preview, which reports it. Milliseconds taken.
double warmRoomData(const RegenerationInputs& inputs, const world::WorldPlan& plan);

/// Lets go of `slot`'s inputs when nothing needs them now (`wanted` false: the directory closed, no notice offering a
/// preview, no preview or commit asked or running; I4b step 6's review: they hold about 224 MiB of private bytes on the
/// reference library), unless a job is making them. A job or a preview that took them keeps them through its
/// shared_ptr; the next one that needs them makes them again. True when they were let go of.
bool releaseIdleInputs(RegenerationInputsSlot& slot, bool wanted);

/// The plan the app shows and its digest (the SHA-256 of its canonical text: the file it was read from or written as).
/// A preview whose plan file has that digest takes it instead of parsing the file (m3/preview-time: the parse was about
/// a third of a room's preview on the reference library); its lists are sorted first, so that it is the file's plan.
struct KnownPlan {
    std::shared_ptr<const world::WorldPlan> plan;
    std::string digest;
};

/// The world as a preview reads it (world::readWorldUnlocked(): no lock, nothing written): the plan file's digest, its
/// plan (`known`'s when that is its digest, else the file parsed; not lifted), the overrides.
struct RegenerationWorld {
    /// Why it could not be read ("" it was: regenerate_failure's key), and the error for the log.
    std::string failure;
    std::string error;
    catalog::LibraryId library;
    std::string planDigest;
    std::shared_ptr<const world::WorldPlan> plan;
    world::WorldOverrides overrides;
    /// The plan was `known`'s (the file not parsed).
    bool knownPlan = false;
    double readMs = 0.0;
};

/// Reads `library`'s world for a preview. Failures: busy (an action in progress or interrupted, the world changing
/// while it is read), no-plan, failed (the plan or overrides.json refused).
[[nodiscard]] RegenerationWorld readRegenerationWorld(const WorldPaths& paths, const catalog::LibraryId& library,
                                                      const KnownPlan* known = nullptr);

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
    /// The world's plan was the one the app shows (KnownPlan): not parsed.
    bool knownPlan = false;
    /// Milliseconds: the world read, the regeneration (its rooms' data included), the whole preview (the read and
    /// what follows it, not a wait between them).
    double readMs = 0.0;
    double regenerateMs = 0.0;
    double totalMs = 0.0;
};

/// The dry run of `scope` on the world as world::readWorldUnlocked() reads it (writing nothing), with `inputs`:
/// readRegenerationWorld() of the inputs' library, then the preview of that world. Failures: those of the read; failed
/// (the plan refused, the room or floor not in the plan, a scope the plan needs the world for, the regeneration's
/// errors); a preview whose regenerated plan breaks a hard rule is invalid-plan (nothing could be committed).
[[nodiscard]] RegenerationPreview previewRegeneration(const WorldPaths& paths, const world::RegenerationScope& scope,
                                                      const RegenerationInputs& inputs,
                                                      const KnownPlan* known = nullptr);
/// The dry run of `scope` on `world` (readRegenerationWorld(), which the app's job reads while the inputs are made):
/// its failure if it has one; failed for a world of another library than the inputs'.
[[nodiscard]] RegenerationPreview previewRegeneration(const RegenerationWorld& world,
                                                      const world::RegenerationScope& scope,
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
