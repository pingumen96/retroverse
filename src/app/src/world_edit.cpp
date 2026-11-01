// The editor in the app (I4b step 3; ADR-0015 §4): WorldSession's side of an edit session (world_edit.hpp's file
// comment): its beginning ("Modifica stanza" in the directory, "Sposta" on the card), its frames (EditInteraction over
// the edited room's targets), its jobs on the worker pool (the data, each operation applied to the draft and the floor
// to show, an undo, a discard, the save), what it shows (each edit through the live update's room-by-room path, the
// held game in the hand, where it would stand, the bar) and its end.

#include "world_session.hpp"

#include <algorithm>
#include <array>
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
#include <rv/world/building.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_store.hpp>

#include "collection_instance.hpp"
#include "product_ui.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// `plan` as the app shows it: a stored building's ground-floor rooms that stand at I4's lobby drawn on the hub, as
/// buildFloor() draws them (world::placeOnHub(); nothing written). The draft and every save keep the plan as stored.
[[nodiscard]] world::WorldPlan asShown(world::WorldPlan plan, const world::EditData* data) {
    if (data != nullptr) {
        static_cast<void>(world::placeOnHub(plan.rooms, data->building.layout));
    }
    return plan;
}

/// The floor shown kept for the fast path: `build` without its scene, the scene data of every room but the session's
/// chain's dropped (only the edited rooms are laid out again: I4b step 8, a move across the chain lays out two).
[[nodiscard]] std::shared_ptr<const WorldBuild> templateOf(WorldBuild build, std::span<const world::RoomId> chain) {
    build.scene.reset();
    for (RoomCollection& collection : build.rooms) {
        if (std::find(chain.begin(), chain.end(), collection.room) == chain.end()) {
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
    // The CRTs' games with the catalog's sessions now, as a ride's floor (a game played since the items files were
    // written stays the room's CRT's choice after an edit).
    FloorSources sources = context.sources;
    if (context.reader) {
        sources.lastPlayed = catalogLastPlayed(*context.reader);
    }
    buildFloor(context.paths, build, sources);
    return build;
}

/// The floor to show for `plan`: the edited rooms laid out again on the floor shown when only placements moved; when
/// their static part changed too (dressing gave way, a fixture moved: LocalReflow), each one's static part rebuilt
/// alone (I4b step 4: roomStaticPart(), shown with `sceneDigest`) and its collection laid out again; else (or when a
/// room's part cannot be made) the floor built again.
void displayInto(EditJobAnswer& answer, const EditJobContext& context, const std::shared_ptr<const WorldBuild>& shown,
                 const world::WorldPlan& plan, const std::string& digest, world::RebuildLevel rebuild,
                 std::span<const world::RoomId> rooms, const std::string& sceneDigest = {}) {
    if ((rebuild == world::RebuildLevel::NoRebuild || rebuild == world::RebuildLevel::LocalReflow) && shown) {
        // Each room once (a move or a swap within a room names it twice; an undo or a discard gathers several).
        std::vector<world::RoomId> onFloor;
        for (const world::RoomId& room : rooms) {
            if (shown->findRoom(room) != nullptr &&
                std::find(onFloor.begin(), onFloor.end(), room) == onFloor.end()) {
                onFloor.push_back(room);
            }
        }
        const world::WorldPlan shownPlan = asShown(plan, context.data.get());
        std::vector<RoomStaticPart> statics;
        bool alone = rebuild == world::RebuildLevel::NoRebuild || !onFloor.empty();
        if (rebuild == world::RebuildLevel::LocalReflow) {
            for (const world::RoomId& room : onFloor) {
                auto part = roomStaticPart(*shown, shownPlan, room);
                if (!part) {
                    spdlog::warn("Editor: the static part of {} cannot be rebuilt alone ({}); the floor is built again",
                                 room.toString(), part.error().toString());
                    alone = false;
                    break;
                }
                for (const world::UndressedSpan& span : part->undressed) {
                    spdlog::info("World scene: {} slot(s) of {} from slot {} left bare: {}", span.count,
                                 span.level.toString(), span.firstSlot, span.reason);
                }
                statics.push_back(std::move(*part));
            }
        }
        if (alone) {
            if (auto fast = editedRoomCollections(*shown, shownPlan, digest, onFloor)) {
                if (!statics.empty()) {
                    fast->sceneDigest = sceneDigest;
                }
                answer.build = std::move(*fast);
                answer.statics = std::move(statics);
                answer.fullFloor = false;
                return;
            } else {
                spdlog::warn("Editor: the edited rooms cannot be laid out again ({}); the floor is built again",
                             fast.error().toString());
            }
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

/// The scene's objects of `fixture` (its room's prefix, then its own id, its pieces ".2", its dressed spans and the
/// picture hung over it): what a held fixture takes with it.
[[nodiscard]] bool objectOfFixture(std::string_view id, const world::FixtureId& fixture) {
    const std::string room = fixture.room().toString() + "/";
    if (!id.starts_with(room)) {
        return false;
    }
    const std::string_view local = id.substr(room.size());
    const std::string own = fixture.localText();
    return local == own || local.starts_with(own + ".") || local.starts_with("dress:" + own + "/") ||
           local == "dressing:picture:" + own;
}

/// The outline over a held fixture (I4b step 4's review): wider than the hover's 2 px, so that it reads on a dark
/// cabinet; over a refused place a lighter red of the bar's verdict colour's hue (edit_bar.rcss: #C8645A), which reads
/// on dark wood; the rest of the view dimmed a little (the inspection dims by half).
constexpr float kHeldOutlineWidthAt1080 = 3.5f;
constexpr std::array<std::uint8_t, 3> kRefusedOutlineSrgb{0xE8, 0x80, 0x6E};
constexpr float kHeldDimOutside = 0.2f;

/// The held game in the hand: as Inspect holds an item (heldItemTransform() at its full lift), turned a little to show
/// its side, nearer and smaller (ADR-0015 §4).
constexpr float kHeldDistance = 0.5f;
constexpr float kHeldYaw = 0.35f;
constexpr float kHeldScale = 0.7f;

} // namespace

std::optional<std::string> WorldSession::editableRoom(const ProductUi& product) const {
    if (m_edit || regenerating() || m_state != "room" || !m_shown || !m_planState || m_ride || m_retargetPending ||
        m_retargetResult || m_shownPreview || m_previewAwaitingFrame || launchHolds(product) ||
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
    // I4b step 8 (ADR-0015 §4, §7): the session edits the room's chain, its rooms on the floor shown (one floor).
    edit.chain.push_back(room);
    for (const FloorRoom& floorRoom : m_rooms) {
        if (floorRoom.id != room && floorRoom.id.head() == room.head() && floorRoom.collection) {
            edit.chain.push_back(floorRoom.id);
        }
    }
    edit.session = ++m_editSessions;
    edit.box = std::make_shared<EditJobBox>();
    edit.box->wake = m_context.wakeCallback();
    edit.probeBox = std::make_shared<EditProbeBox>();
    edit.probeBox->wake = m_context.wakeCallback();
    edit.reader = product.catalog().worldReader();
    edit.interaction.begin(held);
    edit.record.room = room.toString();
    edit.record.rooms.push_back(room.toString());
    edit.record.via = std::string(via);
    edit.record.enterFrame = frame;
    edit.record.heldAtStart = held ? held->toString() : std::string();
    spdlog::info("Frame {}: the editor begins in {} ({}{}{})", frame, room.toString(), via,
                 held ? ", " + held->toString() + " held" : std::string(),
                 edit.chain.size() > 1 ? fmt::format(", its chain's {} rooms", edit.chain.size()) : std::string());

    // The plan as stored (the draft's base), the world's data for the floor, the overrides, the floor of the stored
    // plan (the fast path's template). The base is never the plan as shown: a stored building's lobby rooms are drawn
    // on the hub with the stored digest kept (buildFloor()), and a save must not write that re-placement.
    const EditJobContext context = editJobContext();
    submitEditJob(EditJob::Prepare, [context, shownDigest = m_planState->digest](const core::JobContext& job) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Prepared;
        StoredWorldPlan read = readWorldPlan(context.paths, context.library);
        if (read.error || !read.stored) {
            answer.error = "the stored plan: " + (read.error ? read.error->toString() : std::string("none"));
            return answer;
        }
        world::StoredPlan& stored = *read.stored;
        auto data = prepareEditData(context.paths, stored.plan, context.floor);
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
        if (job.cancelled()) {
            return answer;
        }
        answer.storedChanged = stored.digest != shownDigest;
        displayInto(answer, context, nullptr, stored.plan, stored.digest, world::RebuildLevel::Full, {});
        answer.base = EditBase{world::EditedWorld{std::move(stored.plan), std::move(*overrides)}, stored.digest};
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

EditJobContext WorldSession::editJobContext() const {
    EditJobContext context;
    context.paths = m_options.paths;
    context.library = m_planState->library;
    context.floor = m_floor;
    context.startRoom = m_options.startRoom;
    context.sources.mediaRoot = m_planState->mediaRoot;
    context.sources.mediaRootPath = m_planState->mediaRootPath;
    context.sources.screenMediaRoot = m_planState->screenMediaRoot;
    if (m_edit) {
        context.room = m_edit->room;
        context.data = m_edit->draft ? m_edit->draft->data() : nullptr;
        context.reader = m_edit->reader;
    }
    return context;
}

bool WorldSession::editHoldsWalker() const noexcept {
    return m_edit && (m_edit->interaction.mode() == EditMode::Menu || m_edit->interaction.mode() == EditMode::Saving);
}

bool WorldSession::editWaits() const noexcept {
    return m_edit && (m_edit->job != EditJob::None || m_edit->probing.has_value());
}

CollectionInstance* WorldSession::editCollection() const {
    return m_edit ? editCollectionOf(m_edit->room) : nullptr;
}

CollectionInstance* WorldSession::editCollectionOf(const world::RoomId& id) const {
    for (const FloorRoom& room : m_rooms) {
        if (room.id == id) {
            return room.collection.get();
        }
    }
    return nullptr;
}

void WorldSession::enterEditRoom(const world::RoomId& room) {
    WorldEditState& edit = *m_edit;
    // The outline is the room left's; the held game stays hidden in its own room's collection (syncEditFlags()).
    if (edit.outlined != world::kNoPick) {
        if (CollectionInstance* collection = editCollectionOf(edit.outlinedRoom)) {
            static_cast<void>(collection->setHovered(world::kNoPick));
        }
        edit.outlined = world::kNoPick;
    }
    const world::RoomId left = std::exchange(edit.room, room);
    if (std::find(edit.record.rooms.begin(), edit.record.rooms.end(), room.toString()) == edit.record.rooms.end()) {
        edit.record.rooms.push_back(room.toString());
    }
    bindEditRoom(true);
    spdlog::info("Frame {}: the editor edits {} of its chain (from {}){}", m_context.frameIndex(), room.toString(),
                 left.toString(),
                 edit.interaction.held() ? ", " + edit.interaction.held()->toString() + " carried" : std::string());
}

InteractionResult WorldSession::editFrame(const WorldHover::Frame& frame) {
    WorldEditState& edit = *m_edit;
    InteractionResult result;
    EditFrame input;
    input.actions = frame.actions;
    input.deltaSeconds = frame.deltaSeconds;
    input.walker = frame.walker;
    input.back = frame.back;
    input.viewer = frame.viewer;
    // The edited room's targets only from inside it (its walls occlude them; its doorway would not): outside, nothing
    // under the reticle and no step.
    const std::optional<std::size_t> eye = eyeRoom();
    // I4b step 8 (ADR-0015 §4, §7): the walker entering another room of the session's chain edits that room, a held game
    // carried along (never while an edit is applied, the menu shows or a fixture is held).
    if (eye && m_rooms[*eye].id != edit.room && edit.draft && edit.job == EditJob::None &&
        (edit.interaction.mode() == EditMode::Browse || edit.interaction.mode() == EditMode::Holding) &&
        std::find(edit.chain.begin(), edit.chain.end(), m_rooms[*eye].id) != edit.chain.end()) {
        enterEditRoom(m_rooms[*eye].id);
    }
    input.inRoom = eye && m_rooms[*eye].id == edit.room;
    if (edit.editRoom && frame.ray) {
        // A held game's ray passes the dressing that gives way where it lands (I4b step 4: dressed free slots from
        // afar).
        const world::PickIndex& index =
            edit.interaction.mode() == EditMode::Holding ? edit.editRoom->heldIndex() : edit.editRoom->index();
        input.hover = input.inRoom ? index.sample(*frame.ray, edit.interaction.hovered()) : world::HoverSample{};
        if (input.inRoom) {
            // I4b step 4: the furniture where no game or slot is; where a held fixture would follow the reticle.
            if (input.hover->hit == world::kNoPick && edit.interaction.mode() == EditMode::Browse) {
                input.fixture = edit.editRoom->fixtureAt(*frame.ray);
            }
            input.floorAim = edit.editRoom->floorAim(*frame.ray);
            input.wallAim = edit.editRoom->wallAim(*frame.ray);
            input.roomForward = edit.editRoom->roomDirection(frame.ray->direction);
        }
    }
    const EditMode before = edit.interaction.mode();
    const EditFrameResult answer = edit.interaction.update(input);
    if (edit.interaction.mode() == EditMode::Menu && before != EditMode::Menu) {
        ++edit.record.menus;
    }
    if (edit.interaction.mode() == EditMode::Moving && before == EditMode::Browse) {
        // A fixture picked up (its places judged against the draft as it is now: syncHeldFixture()).
        edit.refusal.clear();
    }
    if (!answer.refused.empty()) {
        edit.refusal = answer.refused;
    }
    if (answer.probe) {
        requestEditProbe(*answer.probe);
    }
    result.shellBack = answer.shellBack;
    result.walker = answer.walker;
    if (answer.apply) {
        requestEditOperation(*answer.apply);
    } else if (answer.undo) {
        if (edit.draft && edit.draft->pending()) {
            // The draft takes its last operation back once the room shows it without (takeEditAnswer()).
            const world::EditResult last = edit.draft->results().back();
            const bool toBase = edit.draft->size() == 1;
            const std::string digest = toBase ? edit.draft->base().planDigest
                                              : fmt::format("edit:{}:{}", edit.session, ++edit.shownCount);
            const std::string sceneDigest =
                toBase ? edit.baseSceneDigest : fmt::format("{}{}:{}", kEditSceneDigest, edit.session, edit.shownCount);
            requestEditDisplay(EditJob::Undo, edit.draft->previous(), digest, last.rebuild, last.rooms, sceneDigest);
        } else {
            // Nothing to undo: the interaction never waits for an answer that will not come.
            edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
            edit.interaction.undone();
        }
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
        edit.discardRetried = false;
        requestEditDisplay(EditJob::Discard, edit.draft->base().world, edit.draft->base().planDigest, rebuild,
                           std::vector<world::RoomId>(rooms.begin(), rooms.end()), edit.baseSceneDigest);
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
    const EditJobContext context = editJobContext();
    const world::PinProvenance provenance{world::PinOrigin::Editor, core::utcNow()};
    const std::string digest = fmt::format("edit:{}:{}", edit.session, ++edit.shownCount);
    const std::string sceneDigest = fmt::format("{}{}:{}", kEditSceneDigest, edit.session, edit.shownCount);
    // A fixture or a game dropped where its last probe judged it, on the same draft (the probes' copy of it, taken again
    // whenever the draft changes): the probe's edited world (applyEdit() of the same operation on the same world) is
    // shown, not applied again.
    std::optional<EditProbeAnswer> probed;
    if (edit.lastProbe && edit.lastProbe->operation == operation && edit.lastProbe->world && edit.lastProbe->result &&
        edit.lastProbe->base && edit.lastProbe->base == edit.probeWorld) {
        probed = std::move(edit.lastProbe);
        spdlog::info("Editor: {} shows what its probe made", world::editText(operation));
    }
    edit.lastProbe.reset();
    std::optional<world::EditedWorld> current;
    if (!probed) {
        current = edit.draft->current();
    }
    submitEditJob(EditJob::Operation, [context, operation, provenance, digest, sceneDigest,
                                       current = std::move(current), probed = std::move(probed),
                                       data = edit.draft->data(), shown = edit.shown](const core::JobContext&) {
        EditJobAnswer answer;
        answer.kind = EditJobAnswer::Kind::Shown;
        answer.operation = operation;
        if (probed) {
            answer.result = *probed->result;
            displayInto(answer, context, shown, probed->world->plan, digest, probed->result->rebuild,
                        probed->result->rooms, sceneDigest);
            answer.world = *probed->world;
            return answer;
        }
        world::EditedWorld edited = *current;
        auto result = world::applyEdit(edited, operation, *data, provenance);
        if (!result) {
            answer.error = result.error().toString();
            return answer;
        }
        answer.result = *result;
        if (result->refused || !result->changed) {
            return answer;
        }
        displayInto(answer, context, shown, edited.plan, digest, result->rebuild, result->rooms, sceneDigest);
        answer.world = std::move(edited);
        return answer;
    });
}

void WorldSession::requestEditDisplay(EditJob job, world::EditedWorld world, std::string digest,
                                      world::RebuildLevel rebuild, std::vector<world::RoomId> rooms,
                                      std::string sceneDigest) {
    WorldEditState& edit = *m_edit;
    const EditJobContext context = editJobContext();
    submitEditJob(job,
                  [context, world = std::move(world), digest = std::move(digest), rebuild, rooms = std::move(rooms),
                   sceneDigest = std::move(sceneDigest), shown = edit.shown](const core::JobContext&) {
                      EditJobAnswer answer;
                      answer.kind = EditJobAnswer::Kind::Shown;
                      displayInto(answer, context, shown, world.plan, digest, rebuild, rooms, sceneDigest);
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
    const EditJobContext context = editJobContext();
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
        // Replayed over a changed plan, the plan saved is not the draft shown: its floor is built for the session's end
        // (a floor that cannot be built leaves the save as it stands: the outcome is the save's, the error the
        // display's).
        if (saved->replayed && !saved->refused) {
            displayInto(answer, context, nullptr, saved->saved.plan, saved->planDigest, world::RebuildLevel::Full, {});
            answer.displayError = std::exchange(answer.error, std::string());
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
    if (m_edit) {
        followEditProbe();
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
    bar.roomNumber = edit.room.index(); // I4b step 8: the room of the chain the walker is in
    if (edit.room.kind() == world::RoomIdKind::System) {
        const std::string fullName = product.catalog().systemFullName(edit.room.system());
        bar.roomTitle = fullName.empty() ? edit.room.system().str() : fullName;
    }
    const auto fixtureKind = [&](const world::FixtureId& id) -> const world::EditableFixture* {
        const world::EditableFixture* fixture = edit.editRoom ? edit.editRoom->fixture(id) : nullptr;
        if (fixture != nullptr) {
            bar.fixtureKind = std::string(edit_bar::fixtureKindOf(fixture->kind));
            bar.fixtureHung = fixture->hung;
        }
        return fixture;
    };
    const world::EditableFixture* lockedTarget = nullptr;
    if (const std::optional<EditTarget>& target = edit.interaction.target(); target && target->fixture) {
        bar.target = "fixture";
        bar.targetPinned = edit.interaction.targetPinned();
        if (const world::EditableFixture* fixture = fixtureKind(*target->fixture);
            fixture != nullptr && !fixture->refusal.empty()) {
            // It cannot move (a kept room's): why, at once, instead of "Prendi".
            bar.fixtureLocked = true;
            lockedTarget = fixture;
        }
    } else if (target) {
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
        if (bar.heldTitle.empty() && edit.heldItem) {
            bar.heldTitle = edit.heldItem->title; // carried from another room of the chain
        }
        if (bar.heldTitle.empty()) {
            bar.heldTitle = held->toString();
        }
    }
    bar.ending = edit.ending;
    bar.pending = edit.draft ? static_cast<std::uint32_t>(edit.draft->size()) : 0;
    bar.refusal = edit.refusal;
    if (lockedTarget != nullptr) {
        bar.refusal = lockedTarget->refusal;
    }
    if (const std::optional<HeldFixture>& held = edit.interaction.heldFixture()) {
        fixtureKind(held->id);
        bar.placeMoved = held->moved();
        const std::optional<std::string> verdict = edit.interaction.verdict();
        bar.verdict = !verdict ? "checking" : verdict->empty() ? "ok" : "refused";
        if (verdict) {
            // The place's own verdict (a refusal of a drop elsewhere no longer applies).
            bar.refusal = *verdict;
        }
    }
    bar.saveRefused = edit.saveRefused;
    bar.menuFocus = std::string(toString(edit.interaction.menuFocus()));
    product.setEditBar(std::move(bar));
    m_editBarShown = true;
}

void WorldSession::takeEditAnswer(EditJobAnswer answer) {
    WorldEditState& edit = *m_edit;
    const EditJob job = std::exchange(edit.job, EditJob::None);
    const std::uint64_t frame = m_context.frameIndex();
    switch (answer.kind) {
    case EditJobAnswer::Kind::Prepared: {
        if (!answer.error.empty() || !answer.build || !answer.base || !m_planState) {
            spdlog::error("Frame {}: the editor cannot edit {}: {}", frame, edit.room.toString(),
                          answer.error.empty() ? std::string("no floor") : answer.error);
            edit.record.failed = answer.error.empty() ? std::string("no floor") : answer.error;
            setNotice(WorldNoticeContent{std::string(world_notice::kEditUnavailable), {}, noticeRoom()}, true);
            endEdit("failed");
            return;
        }
        edit.draft = std::make_unique<EditDraft>(std::move(*answer.base), std::move(answer.data));
        if (answer.storedChanged) {
            // Another writer changed the plan since the floor was shown: the session edits, and shows, the stored one.
            spdlog::warn("Frame {}: the stored plan is not the one shown; the editor shows it", frame);
            clearEditFlags();
            if (!showEditBuild(*answer.build, true)) {
                spdlog::error("Frame {}: the editor cannot show the stored plan of {}", frame, edit.room.toString());
                edit.record.failed = "the stored plan cannot be shown";
                setNotice(WorldNoticeContent{std::string(world_notice::kEditUnavailable), {}, noticeRoom()}, true);
                endEdit("failed");
                return;
            }
        } else {
            if (m_shown && answer.build->sceneDigest != m_shown->sceneDigest) {
                if (m_shown->sceneDigest.starts_with(kEditSceneDigest)) {
                    // An earlier session rebuilt a room's static part alone from this plan (the floor shown carries
                    // the editor's synthetic scene digest): the scene shown is this plan's, the session takes its
                    // digest, so that its edits are shown in place.
                    answer.build->sceneDigest = m_shown->sceneDigest;
                } else {
                    spdlog::warn("Editor: the floor read for the session is not the floor shown (its scene differs)");
                }
            }
            edit.shown = templateOf(std::move(*answer.build), edit.chain);
            bindEditRoom();
        }
        edit.baseSceneDigest = edit.shown ? edit.shown->sceneDigest : std::string();
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
                if (answer.operation) {
                    // A fixture's drop refused: the place's verdict too (it stays in the hand, outlined red).
                    edit.interaction.setVerdict(*answer.operation, edit.refusal);
                }
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
        // What shows could not change: the draft stays as it was (an operation not adopted, an undo not taken), and the
        // session never ends with the draft in view.
        const auto notShown = [&] {
            edit.refusal = "failed";
            edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
            if (job == EditJob::Operation) {
                edit.interaction.applied(false);
            } else if (job == EditJob::Undo) {
                edit.interaction.undone();
            } else if (job == EditJob::Discard && !edit.discardRetried && edit.draft) {
                // The stored plan's whole floor, once.
                spdlog::warn("Frame {}: the stored plan is asked again as a whole floor", frame);
                edit.discardRetried = true;
                edit.refusal.clear();
                requestEditDisplay(EditJob::Discard, edit.draft->base().world, edit.draft->base().planDigest,
                                   world::RebuildLevel::Full, {}, edit.baseSceneDigest);
            } else {
                // The session stays open (its draft shown): Esci senza salvare can be asked again.
                edit.ending.clear();
                edit.endAsked.reset();
                edit.interaction.saved(false);
            }
        };
        if (!answer.error.empty() || !answer.build) {
            spdlog::error("Frame {}: the editor cannot show the room: {}", frame, answer.error);
            notShown();
            return;
        }
        clearEditFlags();
        // The plan the answer shows, as stored: what the floor is built from should the rooms rebuilt alone not show in
        // place (showEditBuild()).
        const world::WorldPlan* floorPlan = nullptr;
        if (job == EditJob::Operation && answer.world) {
            floorPlan = &answer.world->plan;
        } else if (job == EditJob::Undo && edit.draft && edit.draft->pending()) {
            floorPlan = &edit.draft->previous().plan;
        } else if (job == EditJob::Discard && edit.draft) {
            floorPlan = &edit.draft->base().world.plan;
        }
        bool committed = false;
        if (!showEditBuild(*answer.build, answer.fullFloor, answer.statics, floorPlan, &committed)) {
            spdlog::error("Frame {}: the editor's room cannot be shown", frame);
            notShown();
            return;
        }
        const bool full = answer.fullFloor || committed;
        const bool alone = !full && !answer.statics.empty();
        if (job == EditJob::Operation || job == EditJob::Undo) {
            // The draft changes: the probes judged the one before (taken again at the next probe).
            m_pool.cancel(edit.probeGeneration);
            edit.probing.reset();
            edit.probeNext.reset();
            edit.lastProbe.reset();
            edit.probeWorld.reset();
        }
        if (job == EditJob::Operation) {
            edit.draft->adopt(*answer.operation, *answer.result, std::move(*answer.world));
            edit.interaction.setPending(edit.draft->size());
            if (record != nullptr) {
                record->path = full ? "floor" : alone ? "room" : "in-place";
                edit.shownPending = edit.record.operations.size() - 1;
                edit.shownAt = Clock::now();
            }
            edit.interaction.applied(true);
            spdlog::info("Frame {}: the edit {} shown ({}, {:.0f} ms)", frame, world::editText(*answer.operation),
                         full    ? "the floor built again"
                         : alone ? "the room's static part rebuilt alone"
                                 : "in place",
                         millisecondsSince(edit.asked));
        } else if (job == EditJob::Undo) {
            if (edit.draft) {
                edit.draft->undo();
                ++edit.record.undone;
            }
            edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
            edit.interaction.undone();
        } else if (job == EditJob::Discard) {
            edit.record.backMs = millisecondsSince(edit.endAsked.value_or(edit.asked));
            endEdit("discarded");
        }
        return;
    }
    case EditJobAnswer::Kind::Saved: {
        if (!answer.saved) {
            // Nothing written (the lock, the files, the commit): the session stays open, the bar saying so.
            spdlog::error("Frame {}: the edit session cannot be saved: {}", frame, answer.error);
            edit.refusal = "save-failed";
            edit.saveRefused = false;
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
        bool shown = false;
        if (answer.build) {
            clearEditFlags();
            shown = showEditBuild(*answer.build, answer.fullFloor);
            if (!shown) {
                spdlog::error("Frame {}: the saved plan's floor cannot be shown; the draft stays in view", frame);
            }
        } else if (saved.replayed) {
            spdlog::error("Frame {}: the saved plan's floor cannot be built ({}); the draft stays in view", frame,
                          answer.displayError);
        }
        if (!shown && m_planState) {
            // The draft shown is the plan saved (or, when a replay's floor could not be shown, the nearest to it): the
            // floor shown takes its plan and digest (the next live update compares them).
            m_planState->plan = std::make_shared<const world::WorldPlan>(
                asShown(saved.saved.plan, edit.draft ? edit.draft->data().get() : nullptr));
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

bool WorldSession::showEditBuild(WorldBuild& build, bool fullFloor, std::span<const RoomStaticPart> statics,
                                 const world::WorldPlan* floorPlan, bool* committedFloor) {
    if (!m_shown || !build.plan) {
        return false;
    }
    if (m_edit && (fullFloor || !statics.empty())) {
        // A held fixture's scene items and lights are the scene's before: its lights put back, all taken again
        // (syncHeldFixture()) from the scene shown after.
        releaseHeldFixture(true);
    }
    if (!fullFloor && !build.scene) {
        // The static part is the one shown (only placements moved), or a room's rebuilt alone below: an empty stand-in
        // for the live update's decision.
        build.scene.emplace();
    }
    const bool alone = !fullFloor && !statics.empty();
    LiveUpdatePlan plan;
    if (alone) {
        // I4b step 4 (ADR-0015 §4 as amended): each room's static part rebuilt alone, in place on the floor shown (its
        // scene objects, dressing, wall-washers, lamps' lights and colliders), the walker where it stands. Planned as
        // the floor will be once the parts are replaced (its scene digest the build's), before anything changes.
        const std::string shownDigest = std::exchange(m_shown->sceneDigest, build.sceneDigest);
        plan = planLiveUpdate(*m_shown, build, true);
        m_shown->sceneDigest = shownDigest;
        if (plan.kind == LiveUpdateKind::Rebuild || plan.kind == LiveUpdateKind::Kept || !m_host.replaceObjects) {
            spdlog::warn("Editor: the rooms rebuilt alone cannot be shown in place ({}); the floor is committed again",
                         m_host.replaceObjects ? plan.reason : std::string("no scene to replace them in"));
            return commitEditFloor(floorPlan, build.planDigest, committedFloor);
        }
        // The rebuilt rooms' collections applied again even when their items stand as they did: their occluders and
        // their fixtures' pick targets moved with the furniture.
        plan.kind = LiveUpdateKind::Items;
        plan.reloadCovers = true;
        for (RoomUpdate& update : plan.rooms) {
            const bool rebuilt = std::any_of(statics.begin(), statics.end(),
                                             [&](const RoomStaticPart& part) { return part.room == update.room; });
            if (rebuilt && (update.kind == RoomUpdateKind::Unchanged || update.kind == RoomUpdateKind::Textures)) {
                update.kind = RoomUpdateKind::Items;
            }
        }
        std::size_t replaced = 0;
        for (const RoomStaticPart& part : statics) {
            SceneObjectsChange change;
            change.prefix = part.room.toString() + "/";
            change.objects = part.objects;
            change.lights = part.lights;
            if (!m_host.replaceObjects(change)) {
                spdlog::error("Editor: the static part of {} cannot be replaced in the scene", part.room.toString());
                if (replaced == 0) {
                    return false; // nothing changed
                }
                // Some rooms show their new part, others not: the floor of the plan shown, whole.
                return commitEditFloor(floorPlan, build.planDigest, committedFloor);
            }
            ++replaced;
        }
    } else {
        plan = planLiveUpdate(*m_shown, build, true);
    }
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
        if (alone) {
            // The rooms' new parts stand in the scene, their collections not: the floor of the plan shown, whole.
            return commitEditFloor(floorPlan, build.planDigest, committedFloor);
        }
        return false;
    }
    if (alone) {
        // The floor shown's static part is the build's from now on (keepShown() took it with the build).
        m_shown->sceneDigest = build.sceneDigest;
    }
    if (m_edit) {
        m_edit->shown = templateOf(std::move(build), m_edit->chain);
        bindEditRoom();
    }
    return true;
}

bool WorldSession::commitEditFloor(const world::WorldPlan* plan, const std::string& digest, bool* committedFloor) {
    if (plan == nullptr || !m_edit) {
        spdlog::error("Editor: the floor cannot be committed again (no plan to build it from); the scene may show a "
                      "room's static part the draft does not hold");
        return false;
    }
    const Clock::time_point start = Clock::now();
    WorldBuild floor = floorOf(editJobContext(), *plan, digest);
    if (!floor.plan || !floor.scene) {
        spdlog::error("Editor: the floor cannot be built again: {}", floor.reason);
        return false;
    }
    if (!showEditBuild(floor, true)) {
        spdlog::error("Editor: the floor built again cannot be shown");
        return false;
    }
    spdlog::warn("Editor: the floor built again on the main thread and committed ({:.0f} ms)", millisecondsSince(start));
    if (committedFloor != nullptr) {
        *committedFloor = true;
    }
    return true;
}

void WorldSession::bindEditRoom(bool entered) {
    WorldEditState& edit = *m_edit;
    const RoomCollection* room = edit.shown ? edit.shown->findRoom(edit.room) : nullptr;
    if (room == nullptr) {
        edit.editRoom.reset();
        edit.interaction.setRoom(nullptr);
        return;
    }
    // I4b step 4: the room's movable fixtures, its frame and interior (the plan shown's).
    std::vector<world::EditableFixture> fixtures;
    std::vector<world::FixtureBox> fixed;
    EditRoomFrame roomFrame;
    roomFrame.toScene = world::roomToFloorMatrix(room->scenePlacement);
    if (edit.shown->plan) {
        if (edit.draft && edit.draft->data()) {
            fixtures = world::editableFixtures(*edit.shown->plan, edit.room, *edit.draft->data());
        }
        // Each named by what it is drawn as (a pack's variant of a decoration: the bar's words). A secondary TV (I5
        // step 3's review) stands taller than its planned sideboard: its pick box reaches up to the CRT's top, so that
        // aiming at the set picks its fixture (its footprint, the moves' rules, stay the plan's).
        if (!room->sceneData.roomTemplate.id.empty()) {
            if (auto drawn = world::drawnFixtures(*edit.shown->plan, edit.room, room->sceneData)) {
                for (const world::Fixture& shown : *drawn) {
                    for (world::EditableFixture& fixture : fixtures) {
                        if (shown.id == fixture.id) {
                            fixture.kind = shown.prefab.kind;
                            if (shown.prefab.kind == world::kSecondaryTvKind) {
                                fixture.box.maxYMm = std::max(fixture.box.maxYMm, world::fixtureBox(shown).maxYMm);
                            }
                        }
                    }
                }
            }
        }
        fixed = fixedFixtureBoxes(*edit.shown->plan, edit.room);
        for (const world::Room& planned : edit.shown->plan->rooms) {
            if (planned.id == edit.room) {
                roomFrame.minXMm = planned.minXMm;
                roomFrame.maxXMm = planned.maxXMm;
                roomFrame.minZMm = planned.minZMm;
                roomFrame.maxZMm = planned.maxZMm;
                roomFrame.heightMm = planned.heightMm;
            }
        }
    }
    auto built = EditRoom::build(std::make_shared<const world::CollectionLayout>(room->layout), std::move(fixtures),
                                 roomFrame, fixed);
    if (!built) {
        spdlog::error("Editor: the targets of {} cannot be built: {}", edit.room.toString(), built.error().toString());
        edit.editRoom.reset();
        edit.interaction.setRoom(nullptr);
        return;
    }
    // The interaction lets go of the old room before it goes.
    std::unique_ptr<EditRoom> previous = std::move(edit.editRoom);
    edit.editRoom = std::move(*built);
    if (entered) {
        edit.interaction.enterRoom(edit.editRoom.get());
    } else {
        edit.interaction.setRoom(edit.editRoom.get());
    }
    previous.reset();
    edit.itemSize = room->layout.groups.empty() ? glm::vec3(0.0f) : room->layout.groups.front().itemSize;
    edit.spineFacing = room->sceneData.mediaFamily.spineFacing;
    edit.interaction.setPending(edit.draft ? edit.draft->size() : 0);
    const EditDraft* draft = edit.draft.get();
    edit.interaction.setPinned([draft](const catalog::GameId& game) { return draft != nullptr && draft->pinned(game); });
    // I4b step 4: a held fixture snapped to its grid and turn step by the draft (world::snapFixture()), its pin.
    edit.interaction.setPlacer(
        [draft](const world::FixtureId& fixture,
                const world::FixtureTransform& wanted) -> std::optional<world::FixtureTransform> {
            if (draft == nullptr || !draft->data()) {
                return std::nullopt;
            }
            return world::snapFixture(draft->current(), fixture, wanted, *draft->data());
        });
    edit.interaction.setFixturePinned([draft](const world::FixtureId& fixture) {
        if (draft == nullptr) {
            return false;
        }
        const std::vector<world::FixturePin>& pins = draft->current().overrides.fixturePins;
        return std::any_of(pins.begin(), pins.end(),
                           [&](const world::FixturePin& pin) { return pin.fixture == fixture; });
    });
}

void WorldSession::clearEditFlags() {
    if (!m_edit) {
        return;
    }
    for (const auto& [room, pick] : m_edit->hidden) {
        if (CollectionInstance* collection = editCollectionOf(room)) {
            static_cast<void>(collection->setHidden(pick, false));
        }
    }
    if (m_edit->outlined != world::kNoPick) {
        if (CollectionInstance* collection = editCollectionOf(m_edit->outlinedRoom)) {
            static_cast<void>(collection->setHovered(world::kNoPick));
        }
    }
    m_edit->hidden.clear();
    m_edit->outlined = world::kNoPick;
}

void WorldSession::syncEditFlags() {
    if (!m_edit) {
        return;
    }
    WorldEditState& edit = *m_edit;
    CollectionInstance* collection = editCollection();
    if (collection == nullptr) {
        edit.hidden.clear();
        edit.outlined = world::kNoPick;
        return;
    }
    // The held game leaves its slot (drawn in the hand: editViewItems()), a held fixture's games theirs (drawn where it
    // would stand); a game targeted is outlined (the swap's other, the game to pick up). I4b step 8: the held game's
    // item and room as it stands, so that it stays hidden there and in the hand while it is carried across the chain.
    syncHeldFixture();
    if (!edit.interaction.held()) {
        edit.heldItem.reset();
        edit.heldRoom = world::RoomId{};
    } else if (edit.interaction.heldFrom() != world::kNoPick && edit.editRoom) {
        if (const world::CollectionItem* item = edit.editRoom->item(edit.interaction.heldFrom())) {
            edit.heldItem = *item;
            edit.heldRoom = edit.room;
        }
    }
    std::vector<std::pair<world::RoomId, world::PickId>> hidden;
    if (edit.interaction.held() && edit.heldItem) {
        hidden.emplace_back(edit.heldRoom, edit.heldItem->pickId);
    }
    for (const world::PickId pick : edit.heldGames) {
        hidden.emplace_back(edit.room, pick);
    }
    if (hidden != edit.hidden) {
        for (const auto& entry : edit.hidden) {
            if (std::find(hidden.begin(), hidden.end(), entry) == hidden.end()) {
                if (CollectionInstance* owner = editCollectionOf(entry.first)) {
                    static_cast<void>(owner->setHidden(entry.second, false));
                }
            }
        }
        for (const auto& entry : hidden) {
            if (std::find(edit.hidden.begin(), edit.hidden.end(), entry) == edit.hidden.end()) {
                if (CollectionInstance* owner = editCollectionOf(entry.first)) {
                    static_cast<void>(owner->setHidden(entry.second, true));
                }
            }
        }
        edit.hidden = std::move(hidden);
    }
    const world::PickId heldGame = edit.interaction.held() ? edit.interaction.heldFrom() : world::kNoPick;
    const std::optional<EditTarget>& target = edit.interaction.target();
    const bool browsing = edit.interaction.mode() == EditMode::Browse || edit.interaction.mode() == EditMode::Holding;
    const world::PickId outlined =
        browsing && target && target->game && target->pick != heldGame ? target->pick : world::kNoPick;
    if (outlined != edit.outlined || (outlined != world::kNoPick && edit.outlinedRoom != edit.room)) {
        if (Status set = collection->setHovered(outlined); !set) {
            spdlog::error("Editor: the target's outline: {}", set.error().toString());
        }
        edit.outlined = outlined;
        edit.outlinedRoom = edit.room;
    }
}

std::span<const scene::RenderItem> WorldSession::editViewItems(std::span<const scene::RenderItem> sceneItems,
                                                               const scene::Camera& camera) {
    WorldEditState& edit = *m_edit;
    CollectionInstance* collection = editCollection();
    if (collection != nullptr && edit.editRoom && edit.heldFixture && edit.interaction.heldFixture()) {
        appendHeldFixture(sceneItems, *collection);
        return m_viewItems;
    }
    if (collection == nullptr || !edit.editRoom || !edit.interaction.held()) {
        return sceneItems;
    }
    // In its own room, the edited room's item; carried into another room of the chain (I4b step 8), the item as it
    // stood, drawn from its own room's collection.
    const bool carried = edit.interaction.heldFrom() == world::kNoPick;
    const world::CollectionItem* held = carried ? (edit.heldItem ? &*edit.heldItem : nullptr)
                                                : edit.editRoom->item(edit.interaction.heldFrom());
    CollectionInstance* source = carried ? editCollectionOf(edit.heldRoom) : collection;
    if (held == nullptr || source == nullptr) {
        return sceneItems;
    }
    m_viewItems.assign(sceneItems.begin(), sceneItems.end());
    // In the hand, smaller: as Inspect holds an item, turned a little.
    const InspectView view{1.0f, kHeldYaw, 0.0f, kHeldDistance};
    const glm::mat4 hand = heldItemTransform(held->transform, view, camera, kHeldDistance) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(kHeldScale));
    std::size_t first = m_viewItems.size();
    source->appendRenderItems(held->pickId, hand, m_viewItems);
    for (std::size_t index = first; index < m_viewItems.size(); ++index) {
        m_viewItems[index].attributes.flags &= ~scene::instance_flags::kHighlighted;
        m_viewItems[index].castsShadow = false;
    }
    // Where it would stand: in the empty slot targeted, outlined in the theme's warm white.
    const std::optional<EditTarget>& target = edit.interaction.target();
    if (target && target->free() && target->pick != edit.interaction.heldFrom()) {
        if (const world::FreeSlot* free = edit.editRoom->freeSlot(target->pick)) {
            const glm::mat4 standing =
                free->slotFrame * geometry::slotItemTransform(facingOf(free->stance, edit.spineFacing), edit.itemSize);
            first = m_viewItems.size();
            source->appendRenderItems(held->pickId, standing, m_viewItems);
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

void WorldSession::requestEditProbe(const world::EditOperation& probe) {
    WorldEditState& edit = *m_edit;
    if (edit.probing) {
        // The latest place waits for the one in flight.
        edit.probeNext = probe;
        return;
    }
    if (!edit.draft || !edit.draft->data()) {
        return;
    }
    if (!edit.probeWorld) {
        edit.probeWorld = std::make_shared<const world::EditedWorld>(edit.draft->current());
    }
    const std::uint64_t generation = edit.probeGeneration.current();
    {
        const std::lock_guard lock(edit.probeBox->mutex);
        edit.probeBox->answer.reset();
    }
    edit.probing = probe;
    core::JobOptions options;
    options.priority = core::WorkerPool::kHighestPriority;
    options.generation = &edit.probeGeneration;
    const world::PinProvenance provenance{world::PinOrigin::Editor, core::utcNow()};
    const Status submitted = m_pool.submit(
        [box = edit.probeBox, generation, probe, provenance, world = edit.probeWorld,
         data = edit.draft->data()](const core::JobContext& context) {
            if (context.cancelled()) {
                return;
            }
            const Clock::time_point start = Clock::now();
            EditProbeAnswer answer;
            answer.operation = probe;
            answer.base = world;
            // The place judged as the drop would be: applyEdit() on a copy of the draft (kept for a drop there).
            world::EditedWorld trial = *world;
            auto result = world::applyEdit(trial, probe, *data, provenance);
            answer.refusal = !result ? std::string("failed") : result->refused ? result->refused->rule : std::string();
            if (result && !result->refused && result->changed) {
                answer.result = *result;
                answer.world = std::make_shared<const world::EditedWorld>(std::move(trial));
            }
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
        spdlog::error("Editor: a fixture's place cannot be judged: {}", submitted.error().toString());
        edit.probing.reset();
    }
}

void WorldSession::followEditProbe() {
    WorldEditState& edit = *m_edit;
    std::optional<EditProbeAnswer> answer;
    {
        const std::lock_guard lock(edit.probeBox->mutex);
        if (edit.probeBox->answer && edit.probeBox->generation == edit.probeGeneration.current()) {
            answer = std::exchange(edit.probeBox->answer, std::nullopt);
        }
    }
    if (!answer) {
        return;
    }
    edit.probing.reset();
    ++edit.record.probes;
    edit.interaction.setVerdict(answer->operation, answer->refusal);
    edit.lastProbe = std::move(answer);
    if (edit.probeNext) {
        const world::EditOperation next = *edit.probeNext;
        edit.probeNext.reset();
        requestEditProbe(next);
    }
}

void WorldSession::syncHeldFixture() {
    WorldEditState& edit = *m_edit;
    const std::optional<HeldFixture>& held = edit.interaction.heldFixture();
    const std::optional<world::FixtureId> wanted = held ? std::optional<world::FixtureId>(held->id) : std::nullopt;
    if (wanted == edit.heldFixture) {
        moveHeldLights();
        return;
    }
    releaseHeldFixture(true);
    edit.heldFixture = wanted;
    if (!wanted) {
        return;
    }
    // Picked up: the probes judge the draft as it is now (an answer about another draft is dropped).
    m_pool.cancel(edit.probeGeneration);
    edit.probing.reset();
    edit.probeNext.reset();
    edit.lastProbe.reset();
    if (edit.draft) {
        edit.probeWorld = std::make_shared<const world::EditedWorld>(edit.draft->current());
    }
    // What it takes with it: its scene objects (its pieces, its dressing, the picture over it) and its games.
    if (m_host.sceneItemsOf) {
        edit.heldSceneItems =
            m_host.sceneItemsOf([id = *wanted](std::string_view object) { return objectOfFixture(object, id); });
    }
    if (edit.editRoom) {
        for (const world::CollectionItem& item : edit.editRoom->layout().items) {
            if (item.slot.fixture() == *wanted) {
                edit.heldGames.push_back(item.pickId);
            }
        }
    }
    // Its lights (a lamp's bulb: "<room>/fx:<socket>.light"), which follow it while it is held; the room's wall-washers
    // are placed again at the drop.
    if (m_host.sceneLightsOf) {
        edit.heldLights =
            m_host.sceneLightsOf([id = *wanted](std::string_view light) { return objectOfFixture(light, id); });
    }
    moveHeldLights();
}

void WorldSession::releaseHeldFixture(bool restore) {
    WorldEditState& edit = *m_edit;
    if (restore && edit.heldLightsAt && !edit.heldLights.empty() && m_host.setSceneLights) {
        m_host.setSceneLights(edit.heldLights);
    }
    edit.heldFixture.reset();
    edit.heldSceneItems.clear();
    edit.heldGames.clear();
    edit.heldLights.clear();
    edit.heldLightsAt.reset();
}

void WorldSession::moveHeldLights() {
    WorldEditState& edit = *m_edit;
    const std::optional<HeldFixture>& held = edit.interaction.heldFixture();
    if (!held || edit.heldLights.empty() || !edit.editRoom || !m_host.setSceneLights ||
        edit.heldLightsAt == held->place) {
        return;
    }
    const glm::mat4 move = fixtureMove(held->from, held->place, edit.editRoom->frame().toScene);
    std::vector<std::pair<std::size_t, scene::Light>> moved = edit.heldLights;
    for (auto& [index, light] : moved) {
        light.position = glm::vec3(move * glm::vec4(light.position, 1.0f));
        const glm::vec3 direction = glm::vec3(move * glm::vec4(light.direction, 0.0f));
        if (glm::length(direction) > 0.0f) {
            light.direction = glm::normalize(direction);
        }
    }
    m_host.setSceneLights(moved);
    edit.heldLightsAt = held->place;
}

void WorldSession::appendHeldFixture(std::span<const scene::RenderItem> sceneItems, CollectionInstance& collection) {
    WorldEditState& edit = *m_edit;
    const HeldFixture& held = *edit.interaction.heldFixture();
    // Where it would stand, relative to where it stands in the scene: its items and its games drawn there, outlined.
    const glm::mat4 move = fixtureMove(held.from, held.place, edit.editRoom->frame().toScene);
    m_viewItems.clear();
    m_viewItems.reserve(sceneItems.size() + edit.heldSceneItems.size() + 8 * edit.heldGames.size());
    std::size_t next = 0;
    for (std::size_t index = 0; index < sceneItems.size(); ++index) {
        if (next < edit.heldSceneItems.size() && edit.heldSceneItems[next] == index) {
            ++next;
            continue;
        }
        m_viewItems.push_back(sceneItems[index]);
    }
    for (const std::size_t index : edit.heldSceneItems) {
        if (index >= sceneItems.size()) {
            continue;
        }
        scene::RenderItem item = sceneItems[index];
        item.model = move * item.model;
        item.attributes.flags |= scene::instance_flags::kHighlighted;
        m_viewItems.push_back(item);
    }
    for (const world::PickId pick : edit.heldGames) {
        const world::CollectionItem* item = edit.editRoom->item(pick);
        if (item == nullptr) {
            continue;
        }
        const std::size_t first = m_viewItems.size();
        collection.appendRenderItems(pick, move * item->transform, m_viewItems);
        for (std::size_t index = first; index < m_viewItems.size(); ++index) {
            m_viewItems[index].attributes.flags |= scene::instance_flags::kHighlighted;
        }
    }
}

void WorldSession::applyEditOutline(gl::RenderView& view) const {
    if (!m_edit || !m_edit->interaction.heldFixture()) {
        return;
    }
    // Warm white where it may drop (the hover's colour), red where a rule refuses it (the bar names the rule); wider
    // than the hover's, the rest of the view dimmed a little, whatever the verdict.
    view.outline.widthAt1080 = kHeldOutlineWidthAt1080;
    view.outline.dimOutside = std::max(view.outline.dimOutside, kHeldDimOutside);
    const std::optional<std::string> verdict = m_edit->interaction.verdict();
    if (verdict && !verdict->empty()) {
        view.outline.colorSrgb = kRefusedOutlineSrgb;
    }
}

void WorldSession::endEdit(std::string_view end) {
    if (!m_edit) {
        return;
    }
    clearEditFlags();
    releaseHeldFixture(true);
    WorldEditState& edit = *m_edit;
    m_pool.cancel(edit.generation);
    m_pool.cancel(edit.probeGeneration);
    {
        const std::lock_guard lock(edit.box->mutex);
        edit.box->wake = nullptr;
    }
    {
        const std::lock_guard lock(edit.probeBox->mutex);
        edit.probeBox->wake = nullptr;
    }
    edit.interaction.end();
    m_interaction->leaveEdit();
    edit.record.end = std::string(end);
    edit.record.endFrame = m_context.frameIndex();
    // The plan the floor shows as the session ends (the saved one after Salva, the stored one again after Esci senza
    // salvare: never a draft's synthetic digest once it is over).
    if (m_shown) {
        edit.record.planDigest = m_shown->planDigest;
    }
    spdlog::info("Frame {}: the edit session in {} ends ({}): {} operation(s), {} undone", m_context.frameIndex(),
                 edit.room.toString(), end, edit.record.operations.size(), edit.record.undone);
    m_record.edits.push_back(std::move(edit.record));
    // The plan's derived state the session's edits left waiting (keepShown()), from the floor shown now (its plan and
    // the digest of the plan it shows: the saved one, the stored one).
    std::optional<WorldBuild> pending;
    if (edit.planStatePending && edit.shown && edit.shown->plan && m_shown) {
        pending = *edit.shown;
        pending->planDigest = m_shown->planDigest;
    }
    m_edit.reset();
    if (pending) {
        keepShown(*pending);
    }
}

} // namespace rv::app
