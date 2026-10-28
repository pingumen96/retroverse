#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The editor's draft (I4b step 3; ADR-0015 §4, §6): an edit session's operations applied one by one to an in-memory
// copy of the stored plan and its overrides (world::applyEdit(), frozen at I4b step 2), each validated at once, with
// the session's own undo ("Annulla ultima azione": the draft as it was before its last operation), and its save: the
// session's operations replayed onto the stored plan under the world lock (world::applyEdits()), behind a snapshot
// (world::commitWorld(), action "edit", undoable with "Annulla ultima modifica" and `worldplan undo`). When the plan
// and the overrides read under the lock are those the session started from, the draft is saved as it is; when they
// changed meanwhile (the command line, a live update before the session, another app), the operations are replayed on
// them, and an operation that no longer validates writes nothing: the save is refused with its reasons and the session
// stays open. The application never writes `config/`: only the world's own files.
//
// Also the room data an edit reads (prepareEditData(): the configuration, the building, every room of the floor) and
// the edited floor's collections without a floor build (editedRoomCollections(): an operation that moves only
// placements changes no static part, ADR-0015 §4's "texture swap").

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/app/world_build.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_edit.hpp>
#include <rv/world/world_plan.hpp>
#include <rv/world/world_store.hpp>

namespace rv::app {

/// The world the draft starts from: the stored plan (its file's digest) and the overrides, as read.
struct EditBase {
    world::EditedWorld world;
    std::string planDigest;
};

class EditDraft {
public:
    /// A draft of `base`, edited with `data` (EditData::rooms must hold every room of the floors the session edits:
    /// prepareEditData()).
    EditDraft(EditBase base, std::shared_ptr<const world::EditData> data);

    /// `operation` applied to the draft (world::applyEdit with `provenance`): the result; a refusal leaves the draft as
    /// it was (EditResult::refused), an accepted operation is the draft's last (undo() takes it back). An operation
    /// that changes nothing (a pin that was there) is not kept. Errors: those of applyEdit() (the draft as it was).
    [[nodiscard]] Expected<world::EditResult> apply(const world::EditOperation& operation,
                                                    const world::PinProvenance& provenance);
    /// An operation applied elsewhere (the app's job on the pool: applyEdit() on a copy of current()) kept as the
    /// draft's last: `world` is what it made of current(). Ignored for a refused operation or one that changed nothing.
    void adopt(const world::EditOperation& operation, const world::EditResult& result, world::EditedWorld world);
    /// The draft as it was before its last operation; false when it holds none.
    bool undo();

    [[nodiscard]] const EditBase& base() const noexcept { return m_base; }
    [[nodiscard]] const world::EditedWorld& current() const noexcept {
        return m_steps.empty() ? m_base.world : m_steps.back().world;
    }
    /// The draft as undo() would leave it (the base when it holds one operation or none): what an undo shows before
    /// the draft takes it back.
    [[nodiscard]] const world::EditedWorld& previous() const noexcept {
        return m_steps.size() < 2 ? m_base.world : m_steps[m_steps.size() - 2].world;
    }
    /// The operations kept, in order, and what each did.
    [[nodiscard]] std::vector<world::EditOperation> operations() const;
    [[nodiscard]] std::vector<world::EditResult> results() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_steps.size(); }
    [[nodiscard]] bool pending() const noexcept { return !m_steps.empty(); }
    [[nodiscard]] const std::shared_ptr<const world::EditData>& data() const noexcept { return m_data; }
    /// The game is pinned in the draft's overrides (edit_pin's choice between pin and unpin).
    [[nodiscard]] bool pinned(const catalog::GameId& game) const noexcept;

private:
    struct Step {
        world::EditOperation operation;
        world::EditResult result;
        world::EditedWorld world;
    };

    EditBase m_base;
    std::shared_ptr<const world::EditData> m_data;
    std::vector<Step> m_steps;
};

/// How the draft is saved.
struct EditSaveOptions {
    WorldPaths paths;
    catalog::LibraryId library;
    /// The plan's meta sidecar's and the snapshot's writer.
    std::string writer;
    /// The pins' provenance if the operations are replayed (origin editor).
    world::PinProvenance provenance;
    std::chrono::milliseconds lockWait = world::kWorldLockWait;
};

/// What a save did.
struct EditSaveOutcome {
    /// The plan and the overrides were written behind a snapshot (false: nothing to write, or refused).
    bool written = false;
    /// The stored world had changed since the session began: the operations were replayed on it.
    bool replayed = false;
    /// A replayed operation no longer validates: nothing written; the operation's index and its refusal.
    std::optional<std::size_t> refusedAt;
    std::optional<world::EditRefusal> refused;
    /// The snapshot taken (its folder's name), if any.
    std::string snapshot;
    /// The world as stored after the save (the saved draft, or the stored one when refused), and its plan's digest.
    world::EditedWorld saved;
    std::string planDigest;
    double milliseconds = 0.0;
};

/// The draft saved (see the file comment): the world lock taken (an interrupted action rolled back first), the plan
/// (lifted from schema 1 as the app reads it) and the overrides read under it, the draft's world or its replay
/// committed through world::commitWorld() (action "edit", scope the room edited, or "<n> rooms", or "pins"). Errors
/// (nothing written): the lock, the plan or the overrides cannot be read, a room's data cannot be loaded for the
/// replay, the commit fails; NotFound when no plan is stored.
[[nodiscard]] Expected<EditSaveOutcome> saveDraft(const EditDraft& draft, const EditSaveOptions& options);

/// The data an edit of `floor` of `plan` reads (world::EditData): the configuration (world.json over its preset, its
/// issues logged), the building, and every room of the floor (world::loadRoomData() as `worldplan edit` reads it).
/// Errors: those of the readers.
[[nodiscard]] Expected<world::EditData> prepareEditData(const WorldPaths& paths, const world::WorldPlan& plan,
                                                        std::string_view floor);

/// `shown`'s floor with the collections of `rooms` laid out again from `plan` (an edited plan whose static part is
/// `shown`'s: only placements moved, world::RebuildLevel::NoRebuild): each room's layout from its scene data and the
/// titles its items had (world::collectionLayout(), moved into the scene's frame), its cover requests the ones its
/// games had (in the new layout's order), its content digest cleared (unknown); the plan and its digest `plan` and
/// `planDigest`. Errors: a room `shown` lacks, those of the layout.
[[nodiscard]] Expected<WorldBuild> editedRoomCollections(const WorldBuild& shown, const world::WorldPlan& plan,
                                                         std::string planDigest,
                                                         std::span<const world::RoomId> rooms);

} // namespace rv::app
