// The editor in the app (I4b step 3; ADR-0015 §4): WorldSession's side of an edit session (world_edit.hpp's file
// comment): its beginning ("Modifica stanza" in the directory, "Sposta" on the card), its frames (EditInteraction over
// the edited room's targets), its jobs on the worker pool (the data, each operation applied to the draft and the floor
// to show, an undo, a discard, the save), what it shows (each edit through the live update's room-by-room path, the
// held game in the hand, where it would stand, the bar) and its end.

#include "world_session.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/core/time.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/world_store.hpp>

#include "collection_instance.hpp"
#include "product_ui.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// What the session's jobs read besides their own input.
struct EditJobContext {
    WorldPaths paths;
    catalog::LibraryId library;
    std::string floor;
    std::optional<world::RoomId> startRoom;
    FloorSources sources;
    world::RoomId room;
    std::string writer;
};

/// The floor shown kept for the fast path: `build` without its scene, the scene data of every room but `room` dropped
/// (only the edited room is laid out again).
[[nodiscard]] std::shared_ptr<const WorldBuild> templateOf(WorldBuild build, const world::RoomId& room) {
    build.scene.reset();
    for (RoomCollection& collection : build.rooms) {
        if (collection.room != room) {
            collection.sceneData = world::SceneData{};
        }
    }
    return std::make_shared<const WorldBuild>(std::move(build));
}

/// `plan`'s floor built as the session's floor shows it (the scene's frame: the start room's on its floor).
[[nodiscard]] WorldBuild floorOf(const EditJobContext& context, world::WorldPlan plan, std::string digest) {
    WorldBuild build;
    build.library = context.library;
    build.startRoom = context.startRoom;
    build.requestedFloor = context.floor;
    build.outcome = WorldOutcome::Kept;
    build.plan = std::move(plan);
    build.planDigest = std::move(digest);
    build.reason = "the editor's floor";
    buildFloor(context.paths, build, context.sources);
    return build;
}

/// The floor to show for `plan`: the edited rooms laid out again on the floor shown when only placements moved, else the
/// floor built again (dressing gave way, a fixture moved: the static part changed).
void displayInto(EditJobAnswer& answer, const EditJobContext& context, const std::shared_ptr<const WorldBuild>& shown,
                 const world::WorldPlan& plan, const std::string& digest, world::RebuildLevel rebuild,
                 std::span<const world::RoomId> rooms) {
    if (rebuild == world::RebuildLevel::NoRebuild && shown) {
        std::vector<world::RoomId> onFloor;
        for (const world::RoomId& room : rooms) {
            if (shown->findRoom(room) != nullptr) {
                onFloor.push_back(room);
            }
        }
        if (auto fast = editedRoomCollections(*shown, plan, digest, onFloor)) {
            answer.build = std::move(*fast);
            answer.fullFloor = false;
            return;
        } else {
            spdlog::warn("Editor: the edited rooms cannot be laid out again ({}); the floor is built again",
                         fast.error().toString());
        }
    }
    WorldBuild build = floorOf(context, plan, digest);
    if (!build.plan || !build.scene) {
        answer.error = "the floor cannot be built: " + build.reason;
        return;
    }
    answer.build = std::move(build);
    answer.fullFloor = true;
}

[[nodiscard]] world::RebuildLevel higher(world::RebuildLevel a, world::RebuildLevel b) noexcept {
    return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

[[nodiscard]] geometry::SlotFacing facingOf(world::ItemStance stance, world::SpineFacing spine) noexcept {
    if (stance == world::ItemStance::FaceOut || spine == world::SpineFacing::Front) {
        return geometry::SlotFacing::FaceOut;
    }
    return spine == world::SpineFacing::Side ? geometry::SlotFacing::SideOut : geometry::SlotFacing::SpineOut;
}

/// The held game in the hand: as Inspect holds an item (heldItemTransform() at its full lift), turned a little to show
/// its side, nearer and smaller (ADR-0015 §4).
constexpr float kHeldDistance = 0.5f;
constexpr float kHeldYaw = 0.35f;
constexpr float kHeldScale = 0.7f;

} // namespace

std::optional<std::string> WorldSession::editableRoom(const ProductUi& product) const {
    if (m_edit || m_state != "room" || !m_shown || !m_planState || m_ride || m_retargetPending || m_retargetResult ||
        m_shownPreview || m_previewAwaitingFrame || product.launchActive() ||
        (m_flow && m_flow->phase() != WorldPhase::Done)) {
        return std::nullopt;
    }
    const std::optional<std::size_t> eye = eyeRoom();
    if (!eye || !m_rooms[*eye].collection || !m_rooms[*eye].interactionRoom) {
        return std::nullopt;
    }
    return m_rooms[*eye].id.toString();
}

void WorldSession::beginEdit(ProductUi& product, const world::RoomId& room, std::string_view via,
                             std::optional<catalog::GameId> held) {
    const std::uint64_t frame = m_context.frameIndex();
    const std::optional<std::string> editable = editableRoom(product);
    if (!editable || *editable != room.toString()) {
        spdlog::warn("Frame {}: the editor cannot edit {} now (not the eye's room, or the floor is on its way)", frame,
                     room.toString());
        setNotice(WorldNoticeContent{std::string(world_notice::kEditUnavailable), {}, noticeRoom()}, true);
        return;
    }
    m_interaction->bindNow(interactionRoomOf(room.toString()));
    if (!m_interaction->enterEdit()) {
        spdlog::warn("Frame {}: the editor cannot begin now (a launch)", frame);
        return;
    }
    releaseHero();
    InteractionSettings settings;
    m_edit = std::make_unique<WorldEditState>(settings.timings, m_reduceMotion);
    WorldEditState& edit = *m_edit;
    edit.room = room;
    edit.session = ++m_editSessions;
    edit.box = std::make_shared<EditJobBox>();
    edit.box->wake = m_context.wakeCallback();
    edit.interaction.begin(held);
    edit.record.room = room.toString();
    edit.record.via = std::string(via);
    edit.record.enterFrame = frame;
    edit.record.heldAtStart = held ? held->toString() : std::string();
    spdlog::info("Frame {}: the editor begins in {} ({}{})", frame, room.toString(), via,
                 held ? ", " + held->toString() + " held" : std::string());

    // The world's data for the floor, the overrides, the floor of the plan shown (the fast path's template).
    EditJobContext context;
    context.paths = m_options.paths;
    context.library = m_planState->library;
    context.floor = m_floor;
    context.startRoom = m_options.startRoom;
    context.sources.mediaRoot = m_planState->mediaRoot;
    context.sources.mediaRootPath = m_planState->mediaRootPath;
    context.sources.screenMediaRoot = m_planState->screenMediaRoot;
    context.room = room;
    submitEditJob(EditJob::Prepare, [context, plan = m_planState->plan,
                                     digest = m_planState->digest](const core::JobContext& job) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Prepared;
        auto data = prepareEditData(context.paths, *plan, context.floor);
        if (!data) {
            answer.error = "the world's data: " + data.error().toString();
            return answer;
        }
        answer.data = std::make_shared<const world::EditData>(std::move(*data));
        const world::WorldFiles files = world::worldFiles(context.paths.userDataRoot, context.library);
        auto overrides = world::loadOverrides(files.overrides);
        if (!overrides) {
            // A newer schema (ADR-0015 §2): the app edits nothing and says so.
            answer.error = "overrides.json: " + overrides.error().toString();
            return answer;
        }
        answer.overrides = std::move(*overrides);
        if (job.cancelled()) {
            return answer;
        }
        displayInto(answer, context, nullptr, *plan, digest, world::RebuildLevel::Full, {});
        return answer;
    });
}

void WorldSession::submitEditJob(EditJob job, std::function<EditJobAnswer(const core::JobContext&)> work) {
    WorldEditState& edit = *m_edit;
    m_pool.cancel(edit.generation);
    const std::uint64_t generation = edit.generation.current();
    {
        const std::lock_guard lock(edit.box->mutex);
        edit.box->answer.reset();
    }
    edit.job = job;
    edit.asked = Clock::now();
    core::JobOptions options;
    options.priority = core::WorkerPool::kHighestPriority;
    options.generation = &edit.generation;
    const Status submitted = m_pool.submit(
        [box = edit.box, generation, work = std::move(work)](const core::JobContext& context) {
            if (context.cancelled()) {
                return;
            }
            const Clock::time_point start = Clock::now();
            EditJobAnswer answer = work(context);
            answer.milliseconds = millisecondsSince(start);
            if (context.cancelled()) {
                return;
            }
            std::function<void()> wake;
            {
                const std::lock_guard lock(box->mutex);
                box->answer = std::move(answer);
                box->generation = generation;
                wake = box->wake;
            }
            if (wake) {
                wake();
            }
        },
        options);
    if (!submitted) {
        spdlog::error("Editor: a job cannot be submitted: {}", submitted.error().toString());
        EditJobAnswer failed;
        failed.kind = job == EditJob::Prepare ? EditJobAnswer::Kind::Prepared
                      : job == EditJob::Save  ? EditJobAnswer::Kind::Saved
                                              : EditJobAnswer::Kind::Shown;
        failed.error = submitted.error().toString();
        const std::lock_guard lock(edit.box->mutex);
        edit.box->answer = std::move(failed);
        edit.box->generation = generation;
    }
}

bool WorldSession::editWaits() const noexcept {
    return m_edit && m_edit->job != EditJob::None;
}

CollectionInstance* WorldSession::editCollection() const {
    if (!m_edit) {
        return nullptr;
    }
    for (const FloorRoom& room : m_rooms) {
        if (room.id == m_edit->room) {
            return room.collection.get();
        }
    }
    return nullptr;
}

InteractionResult WorldSession::editFrame(const WorldHover::Frame& frame) {
    WorldEditState& edit = *m_edit;
    InteractionResult result;
    if (frame.actions != nullptr) {
        edit.device = frame.actions->activeDevice();
    }
    EditFrame input;
    input.actions = frame.actions;
    input.deltaSeconds = frame.deltaSeconds;
    input.walker = frame.walker;
    input.back = frame.back;
    input.viewer = frame.viewer;
    if (edit.editRoom && frame.ray) {
        input.hover = edit.editRoom->index().sample(*frame.ray, edit.interaction.hovered());
    }
    const EditMode before = edit.interaction.mode();
    const EditFrameResult answer = edit.interaction.update(input);
    if (edit.interaction.mode() == EditMode::Menu && before != EditMode::Menu) {
        ++edit.record.menus;
    }
    result.shellBack = answer.shellBack;
    result.walker = answer.walker;
    if (answer.apply) {
        requestEditOperation(*answer.apply);
    } else if (answer.undo && edit.draft && edit.draft->pending()) {
        const world::EditResult last = edit.draft->results().back();
        edit.draft->undo();
        ++edit.record.undone;
        const bool atBase = !edit.draft->pending();
        const std::string digest = atBase ? edit.draft->base().planDigest
                                          : fmt::format("edit:{}:{}", edit.session, ++edit.shownCount);
        requestEditDisplay(EditJob::Undo, edit.draft->current(), digest, last.rebuild, last.rooms);
    } else if (answer.save) {
        requestEditSave();
    } else if (answer.discard && edit.draft) {
        world::RebuildLevel rebuild = world::RebuildLevel::NoRebuild;
        std::set<world::RoomId> rooms;
        for (const world::EditResult& step : edit.draft->results()) {
            rebuild = higher(rebuild, step.rebuild);
            rooms.insert(step.rooms.begin(), step.rooms.end());
        }
        edit.ending = "discard";
        edit.endAsked = Clock::now();
        requestEditDisplay(EditJob::Discard, edit.draft->base().world, edit.draft->base().planDigest, rebuild,
                           std::vector<world::RoomId>(rooms.begin(), rooms.end()));
    } else if (answer.exit) {
        endEdit("exited");
        return result;
    }
    syncEditFlags();
    return result;
}

void WorldSession::requestEditOperation(const world::EditOperation& operation) {
    WorldEditState& edit = *m_edit;
    if (!edit.draft) {
        edit.interaction.applied(false);
        return;
    }
    EditOperationRecord record;
    record.frame = m_context.frameIndex();
    record.op = std::string(world::toString(operation.kind));
    record.operation = world::editText(operation);
    edit.record.operations.push_back(std::move(record));
    edit.refusal.clear();
    edit.saveRefused = false;
    EditJobContext context;
    context.paths = m_options.paths;
    context.library = m_planState->library;
    context.floor = m_floor;
    context.startRoom = m_options.startRoom;
    context.sources.mediaRoot = m_planState->mediaRoot;
    context.sources.mediaRootPath = m_planState->mediaRootPath;
    context.sources.screenMediaRoot = m_planState->screenMediaRoot;
    context.room = edit.room;
    const world::PinProvenance provenance{world::PinOrigin::Editor, core::utcNow()};
    const std::string digest = fmt::format("edit:{}:{}", edit.session, ++edit.shownCount);
    submitEditJob(EditJob::Operation, [context, operation, provenance, digest, world = edit.draft->current(),
                                       data = edit.draft->data(), shown = edit.shown](const core::JobContext&) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Shown;
        answer.operation = operation;
        world::EditedWorld edited = world;
        auto result = world::applyEdit(edited, operation, *data, provenance);
        if (!result) {
            answer.error = result.error().toString();
            return answer;
        }
        answer.result = *result;
        if (result->refused || !result->changed) {
            return answer;
        }
        displayInto(answer, context, shown, edited.plan, digest, result->rebuild, result->rooms);
        answer.world = std::move(edited);
        return answer;
    });
}

void WorldSession::requestEditDisplay(EditJob job, world::EditedWorld world, std::string digest,
                                      world::RebuildLevel rebuild, std::vector<world::RoomId> rooms) {
    WorldEditState& edit = *m_edit;
    EditJobContext context;
    context.paths = m_options.paths;
    context.library = m_planState->library;
    context.floor = m_floor;
    context.startRoom = m_options.startRoom;
    context.sources.mediaRoot = m_planState->mediaRoot;
    context.sources.mediaRootPath = m_planState->mediaRootPath;
    context.sources.screenMediaRoot = m_planState->screenMediaRoot;
    context.room = edit.room;
    submitEditJob(job, [context, world = std::move(world), digest = std::move(digest), rebuild,
                        rooms = std::move(rooms), shown = edit.shown](const core::JobContext&) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Shown;
        displayInto(answer, context, shown, world.plan, digest, rebuild, rooms);
        return answer;
    });
}

void WorldSession::requestEditSave() {
    WorldEditState& edit = *m_edit;
    if (!edit.draft) {
        edit.interaction.saved(false);
        return;
    }
    edit.ending = "save";
    edit.endAsked = Clock::now();
    edit.refusal.clear();
    edit.saveRefused = false;
    EditJobContext context;
    context.paths = m_options.paths;
    context.library = m_planState->library;
    context.floor = m_floor;
    context.startRoom = m_options.startRoom;
    context.sources.mediaRoot = m_planState->mediaRoot;
    context.sources.mediaRootPath = m_planState->mediaRootPath;
    context.sources.screenMediaRoot = m_planState->screenMediaRoot;
    context.room = edit.room;
    EditSaveOptions options;
    options.paths = m_options.paths;
    options.library = m_planState->library;
    options.writer = m_options.writer.empty() ? std::string("retroverse (editor)") : m_options.writer + " (editor)";
    options.provenance = world::PinProvenance{world::PinOrigin::Editor, core::utcNow()};
    submitEditJob(EditJob::Save, [context, options, draft = *edit.draft](const core::JobContext&) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Saved;
        auto saved = saveDraft(draft, options);
        if (!saved) {
            answer.error = saved.error().toString();
            return answer;
        }
        // Replayed over a changed plan, the plan saved is not the draft shown: its floor is built for the session's end.
        if (saved->replayed && !saved->refused) {
            displayInto(answer, context, nullptr, saved->saved.plan, saved->planDigest, world::RebuildLevel::Full, {});
        }
        answer.saved = std::move(*saved);
        return answer;
    });
}

void WorldSession::followEdit(ProductUi& product) {
    if (m_edit) {
        std::optional<EditJobAnswer> answer;
        {
            const std::lock_guard lock(m_edit->box->mutex);
            if (m_edit->box->answer && m_edit->box->generation == m_edit->generation.current()) {
                answer = std::exchange(m_edit->box->answer, std::nullopt);
            }
        }
        if (answer) {
            takeEditAnswer(std::move(*answer));
        }
    }
    if (!m_edit) {
        if (m_editBarShown) {
            product.setEditBar(std::nullopt);
            m_editBarShown = false;
        }
        return;
    }
    // The bar (edit_bar.rml).
    WorldEditState& edit = *m_edit;
    EditBarContent bar;
    bar.mode = std::string(toString(edit.interaction.mode()));
    switch (edit.room.kind()) {
    case world::RoomIdKind::System:
        bar.roomKind = "console";
        break;
    case world::RoomIdKind::Shared:
        bar.roomKind = "shared";
        break;
    case world::RoomIdKind::Arcade:
        bar.roomKind = "arcade";
        break;
    }
    bar.roomTitle = edit.room.toString();
    if (edit.room.kind() == world::RoomIdKind::System) {
        const std::string fullName = product.catalog().systemFullName(edit.room.system());
        bar.roomTitle = fullName.empty() ? edit.room.system().str() : fullName;
    }
    if (const std::optional<EditTarget>& target = edit.interaction.target()) {
        bar.target = target->game ? "game" : (target->dressed ? "dressed" : "slot");
        bar.targetFaceOut = target->faceOut;
        bar.targetPinned = edit.interaction.targetPinned();
        if (target->game && edit.editRoom) {
            if (const world::CollectionItem* item = edit.editRoom->item(target->pick)) {
                bar.targetTitle = item->title;
            }
        }
        bar.targetOwn = edit.interaction.held().has_value() && target->pick == edit.interaction.heldFrom();
    }
    if (const std::optional<catalog::GameId>& held = edit.interaction.held()) {
        bar.holding = true;
        if (edit.editRoom) {
            if (const world::CollectionItem* item = edit.editRoom->item(edit.interaction.heldFrom())) {
                bar.heldTitle = item->title;
            }
        }
        if (bar.heldTitle.empty()) {
            bar.heldTitle = held->toString();
        }
    }
    bar.ending = edit.ending;
    bar.pending = edit.draft ? static_cast<std::uint32_t>(edit.draft->size()) : 0;
    bar.refusal = edit.refusal;
    bar.saveRefused = edit.saveRefused;
    bar.menuFocus = std::string(toString(edit.interaction.menuFocus()));
    bar.device = edit.device == input::DeviceKind::Gamepad ? "pad" : "keys";
    product.setEditBar(std::move(bar));
    m_editBarShown = true;
}

void WorldSession::takeEditAnswer(EditJobAnswer answer) {
    WorldEditState& edit = *m_edit;
    const EditJob job = std::exchange(edit.job, EditJob::None);
    const std::uint64_t frame = m_context.frameIndex();
    switch (answer.kind) {
    case EditJobAnswer::Kind::Prepared: {
        if (!answer.error.empty() || !answer.build || !m_planState) {
            spdlog::error("Frame {}: the editor cannot edit {}: {}", frame, edit.room.toString(),
                          answer.error.empty() ? std::string("no floor") : answer.error);
            edit.record.failed = answer.error.empty() ? std::string("no floor") : answer.error;
            setNotice(WorldNoticeContent{std::string(world_notice::kEditUnavailable), {}, noticeRoom()}, true);
            endEdit("failed");
            return;
        }
        if (m_shown && answer.build->sceneDigest != m_shown->sceneDigest) {
            spdlog::warn("Editor: the floor read for the session is not the floor shown (its scene differs)");
        }
        edit.draft = std::make_unique<EditDraft>(
            EditBase{world::EditedWorld{*m_planState->plan, std::move(answer.overrides)}, m_planState->digest},
            std::move(answer.data));
        edit.shown = templateOf(std::move(*answer.build), edit.room);
        bindEditRoom();
        edit.interaction.ready();
        edit.record.readyFrame = frame;
        edit.record.prepareMs = millisecondsSince(edit.asked);
        spdlog::info("Frame {}: the editor is ready in {} ({:.0f} ms; the job {:.0f} ms)", frame, edit.room.toString(),
                     *edit.record.prepareMs, answer.milliseconds);
        return;
    }
    case EditJobAnswer::Kind::Shown: {
        EditOperationRecord* record = job == EditJob::Operation && !edit.record.operations.empty()
                                          ? &edit.record.operations.back()
                                          : nullptr;
        if (record != nullptr) {
            record->editMs = answer.milliseconds;
        }
        if (job == EditJob::Operation) {
            if (!answer.error.empty() || !answer.result) {
                spdlog::error("Frame {}: the edit failed: {}", frame, answer.error);
                edit.refusal = "failed";
                if (record != nullptr) {
                    record->refused = "failed";
                    record->path = "none";
                }
                edit.interaction.applied(false);
                return;
            }
            if (answer.result->refused) {
                edit.refusal = answer.result->refused->rule;
                spdlog::info("Frame {}: the edit {} is refused by {}: {}", frame,
                             answer.operation ? world::editText(*answer.operation) : std::string(), edit.refusal,
                             answer.result->refused->message);
                if (record != nullptr) {
                    record->refused = edit.refusal;
                    record->path = "none";
                }
                edit.interaction.applied(false);
                return;
            }
            if (record != nullptr) {
                record->rebuild = std::string(world::toString(answer.result->rebuild));
            }
            if (!answer.result->changed || !answer.world) {
                if (record != nullptr) {
                    record->path = "none";
                }
                edit.interaction.applied(true);
                return;
            }
        }
        if (!answer.error.empty() || !answer.build) {
            spdlog::error("Frame {}: the editor cannot show the room: {}", frame, answer.error);
            edit.refusal = "failed";
            if (job == EditJob::Operation) {
                edit.interaction.applied(false);
            } else if (job == EditJob::Undo) {
                edit.interaction.undone();
            } else {
                endEdit("discarded");
            }
            return;
        }
        clearEditFlags();
        const bool full = answer.fullFloor;
        if (!showEditBuild(*answer.build, full)) {
            spdlog::error("Frame {}: the editor's room cannot be shown", frame);
            edit.refusal = "failed";
            if (job == EditJob::Operation) {
                edit.interaction.applied(false);
            } else if (job == EditJob::Undo) {
                edit.interaction.undone();
            } else {
                endEdit("discarded");
            }
            return;
        }
        if (job == EditJob::Operation) {
            edit.draft->adopt(*answer.operation, *answer.result, std::move(*answer.world));
            edit.interaction.setPending(edit.draft->size());
            if (record != nullptr) {
                record->path = full ? "floor" : "in-place";
                edit.shownPending = edit.record.operations.size() - 1;
                edit.shownAt = Clock::now();
            }
            edit.interaction.applied(true);
            spdlog::info("Frame {}: the edit {} shown ({}, {:.0f} ms)", frame, world::editText(*answer.operation),
                         full ? "the floor built again" : "in place", millisecondsSince(edit.asked));
        } else if (job == EditJob::Undo) {
            edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
            edit.interaction.undone();
        } else if (job == EditJob::Discard) {
            edit.record.backMs = millisecondsSince(edit.endAsked.value_or(edit.asked));
            endEdit("discarded");
        }
        return;
    }
    case EditJobAnswer::Kind::Saved: {
        if (!answer.error.empty() || !answer.saved) {
            spdlog::error("Frame {}: the edit session cannot be saved: {}", frame, answer.error);
            edit.refusal = "failed";
            edit.saveRefused = true;
            edit.record.saveRefused = "failed";
            edit.interaction.saved(false);
            return;
        }
        EditSaveOutcome& saved = *answer.saved;
        edit.record.replayed = saved.replayed;
        edit.record.saveMs = saved.milliseconds;
        if (saved.refused) {
            edit.refusal = saved.refused->rule;
            edit.saveRefused = true;
            edit.record.saveRefused = saved.refused->rule;
            edit.interaction.saved(false);
            return;
        }
        edit.record.written = saved.written;
        edit.record.snapshot = saved.snapshot;
        edit.record.planDigest = saved.planDigest;
        // The plan shown is the one stored now.
        m_record.planDigest = saved.planDigest;
        m_record.planWritten = m_record.planWritten || saved.written;
        if (answer.build) {
            clearEditFlags();
            if (!showEditBuild(*answer.build, answer.fullFloor)) {
                spdlog::error("Frame {}: the saved plan's floor cannot be shown", frame);
            }
        } else if (m_planState) {
            // The draft shown is the plan saved: the floor shown takes its digest (the next live update compares it).
            m_planState->plan = std::make_shared<const world::WorldPlan>(saved.saved.plan);
            m_planState->digest = saved.planDigest;
            m_planIndex.emplace(*m_planState->plan);
            if (m_shown) {
                m_shown->planDigest = saved.planDigest;
            }
        }
        edit.record.backMs = millisecondsSince(edit.endAsked.value_or(edit.asked));
        spdlog::info("Frame {}: the edit session saved ({}{}{}; {:.0f} ms)", frame,
                     saved.written ? "written" : "nothing to write", saved.replayed ? ", replayed" : "",
                     saved.snapshot.empty() ? std::string() : ", snapshot " + saved.snapshot, saved.milliseconds);
        edit.interaction.saved(true);
        endEdit("saved");
        return;
    }
    }
}

bool WorldSession::showEditBuild(WorldBuild& build, bool fullFloor) {
    if (!m_shown || !build.plan) {
        return false;
    }
    if (!fullFloor && !build.scene) {
        // The static part is the one shown (only placements moved): an empty stand-in for the live update's decision.
        build.scene.emplace();
    }
    const LiveUpdatePlan plan = planLiveUpdate(*m_shown, build, true);
    WorldLiveUpdateRecord record;
    bool shown = false;
    switch (plan.kind) {
    case LiveUpdateKind::Unchanged:
    case LiveUpdateKind::Textures:
    case LiveUpdateKind::Items:
        shown = applyRooms(build, plan, record);
        if (!shown && fullFloor) {
            shown = showFloor(build, true);
        }
        break;
    case LiveUpdateKind::Rebuild:
    case LiveUpdateKind::Kept:
        if (fullFloor) {
            shown = showFloor(build, true);
        } else {
            spdlog::warn("Editor: the room laid out again needs the floor committed again ({})", plan.reason);
        }
        break;
    }
    if (!shown) {
        return false;
    }
    if (m_edit) {
        m_edit->shown = templateOf(std::move(build), m_edit->room);
        bindEditRoom();
    }
    return true;
}

void WorldSession::bindEditRoom() {
    WorldEditState& edit = *m_edit;
    const RoomCollection* room = edit.shown ? edit.shown->findRoom(edit.room) : nullptr;
    if (room == nullptr) {
        edit.editRoom.reset();
        edit.interaction.setRoom(nullptr);
        return;
    }
    auto built = EditRoom::build(std::make_shared<const world::CollectionLayout>(room->layout));
    if (!built) {
        spdlog::error("Editor: the targets of {} cannot be built: {}", edit.room.toString(), built.error().toString());
        edit.editRoom.reset();
        edit.interaction.setRoom(nullptr);
        return;
    }
    // The interaction lets go of the old room before it goes.
    std::unique_ptr<EditRoom> previous = std::move(edit.editRoom);
    edit.editRoom = std::move(*built);
    edit.interaction.setRoom(edit.editRoom.get());
    previous.reset();
    edit.itemSize = room->layout.groups.empty() ? glm::vec3(0.0f) : room->layout.groups.front().itemSize;
    edit.spineFacing = room->sceneData.mediaFamily.spineFacing;
    edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
    const EditDraft* draft = edit.draft.get();
    edit.interaction.setPinned([draft](const catalog::GameId& game) { return draft != nullptr && draft->pinned(game); });
}

void WorldSession::clearEditFlags() {
    if (!m_edit) {
        return;
    }
    if (CollectionInstance* collection = editCollection()) {
        if (m_edit->hidden != world::kNoPick) {
            static_cast<void>(collection->setHidden(m_edit->hidden, false));
        }
        if (m_edit->outlined != world::kNoPick) {
            static_cast<void>(collection->setHovered(world::kNoPick));
        }
    }
    m_edit->hidden = world::kNoPick;
    m_edit->outlined = world::kNoPick;
}

void WorldSession::syncEditFlags() {
    if (!m_edit) {
        return;
    }
    WorldEditState& edit = *m_edit;
    CollectionInstance* collection = editCollection();
    if (collection == nullptr) {
        edit.hidden = world::kNoPick;
        edit.outlined = world::kNoPick;
        return;
    }
    // The held game leaves its slot (drawn in the hand: editViewItems()); a game targeted is outlined (the swap's other,
    // the game to pick up).
    const world::PickId hidden = edit.interaction.held() ? edit.interaction.heldFrom() : world::kNoPick;
    if (hidden != edit.hidden) {
        if (edit.hidden != world::kNoPick) {
            static_cast<void>(collection->setHidden(edit.hidden, false));
        }
        if (hidden != world::kNoPick) {
            static_cast<void>(collection->setHidden(hidden, true));
        }
        edit.hidden = hidden;
    }
    const std::optional<EditTarget>& target = edit.interaction.target();
    const bool browsing = edit.interaction.mode() == EditMode::Browse || edit.interaction.mode() == EditMode::Holding;
    const world::PickId outlined =
        browsing && target && target->game && target->pick != hidden ? target->pick : world::kNoPick;
    if (outlined != edit.outlined) {
        if (Status set = collection->setHovered(outlined); !set) {
            spdlog::error("Editor: the target's outline: {}", set.error().toString());
        }
        edit.outlined = outlined;
    }
}

std::span<const scene::RenderItem> WorldSession::editViewItems(std::span<const scene::RenderItem> sceneItems,
                                                               const scene::Camera& camera) {
    WorldEditState& edit = *m_edit;
    CollectionInstance* collection = editCollection();
    if (collection == nullptr || !edit.editRoom || !edit.interaction.held() ||
        edit.interaction.heldFrom() == world::kNoPick) {
        return sceneItems;
    }
    const world::CollectionItem* held = edit.editRoom->item(edit.interaction.heldFrom());
    if (held == nullptr) {
        return sceneItems;
    }
    m_viewItems.assign(sceneItems.begin(), sceneItems.end());
    // In the hand, smaller: as Inspect holds an item, turned a little.
    const InspectView view{1.0f, kHeldYaw, 0.0f, kHeldDistance};
    const glm::mat4 hand = heldItemTransform(held->transform, view, camera, kHeldDistance) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(kHeldScale));
    std::size_t first = m_viewItems.size();
    collection->appendRenderItems(held->pickId, hand, m_viewItems);
    for (std::size_t index = first; index < m_viewItems.size(); ++index) {
        m_viewItems[index].attributes.flags &= ~scene::instance_flags::kHighlighted;
        m_viewItems[index].castsShadow = false;
    }
    // Where it would stand: in the empty slot targeted, outlined in the theme's warm white.
    const std::optional<EditTarget>& target = edit.interaction.target();
    if (target && target->free() && target->pick != held->pickId) {
        if (const world::FreeSlot* free = edit.editRoom->freeSlot(target->pick)) {
            const glm::mat4 standing =
                free->slotFrame * geometry::slotItemTransform(facingOf(free->stance, edit.spineFacing), edit.itemSize);
            first = m_viewItems.size();
            collection->appendRenderItems(held->pickId, standing, m_viewItems);
            for (std::size_t index = first; index < m_viewItems.size(); ++index) {
                m_viewItems[index].attributes.flags |= scene::instance_flags::kHighlighted;
            }
        }
    }
    return m_viewItems;
}

void WorldSession::editFrameRendered() {
    if (!m_edit || !m_edit->shownPending || !m_edit->shownAt) {
        return;
    }
    WorldEditState& edit = *m_edit;
    if (*edit.shownPending < edit.record.operations.size()) {
        EditOperationRecord& record = edit.record.operations[*edit.shownPending];
        record.shownFrame = m_context.frameIndex();
        // From the request (the job's start) to the frame that shows it.
        record.shownMs = millisecondsSince(edit.asked);
    }
    edit.shownPending.reset();
    edit.shownAt.reset();
}

void WorldSession::endEdit(std::string_view end) {
    if (!m_edit) {
        return;
    }
    clearEditFlags();
    WorldEditState& edit = *m_edit;
    m_pool.cancel(edit.generation);
    {
        const std::lock_guard lock(edit.box->mutex);
        edit.box->wake = nullptr;
    }
    edit.interaction.end();
    m_interaction->leaveEdit();
    edit.record.end = std::string(end);
    edit.record.endFrame = m_context.frameIndex();
    spdlog::info("Frame {}: the edit session in {} ends ({}): {} operation(s), {} undone", m_context.frameIndex(),
                 edit.room.toString(), end, edit.record.operations.size(), edit.record.undone);
    m_record.edits.push_back(std::move(edit.record));
    m_edit.reset();
}

} // namespace rv::app
