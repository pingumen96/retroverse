// The regenerations and the undo in the app (I4b step 6; ADR-0015 §5, §6): WorldSession's side
// (world_regenerate_state.hpp's file comment): the directory's "Rigenera stanza / piano / mondo" and "Annulla ultima
// modifica" and the update's needs-confirmation notice opening the preview (regenerate_preview.rml), its dry run and
// the commit of the choice on the worker pool (world_regenerate.hpp), the floor shown again from the plan stored after
// it (room by room when its static part stands, else committed again with the walker kept), the walker of a regenerated
// room at its doorway.

#include "world_session.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/core/version.hpp>

#include "product_ui.hpp"
#include "world_regenerate_state.hpp"

namespace rv::app {

RegenerationInputsResult regenerationInputsFor(RegenerationInputsSlot& slot, const WorldPaths& paths,
                                               WorldCatalogReader* reader, bool& taken) {
    taken = false;
    {
        std::unique_lock lock(slot.mutex);
        slot.done.wait(lock, [&] { return !slot.running; });
        if (slot.result) {
            return *slot.result;
        }
        slot.running = true;
    }
    const auto start = std::chrono::steady_clock::now();
    RegenerationInputsResult result;
    if (reader == nullptr) {
        result.failure = regenerate_failure::kFailed;
        result.error = "no catalog to read the selections from";
    } else {
        result = takeRegenerationInputs(paths, *reader);
    }
    taken = true;
    {
        const std::lock_guard lock(slot.mutex);
        slot.running = false;
        slot.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // A failure is not kept: the next preview tries again (the media may be back).
        if (result.inputs) {
            slot.result = result;
        }
    }
    slot.done.notify_all();
    return result;
}

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The panel's name of `room` (its kind and, for a console room, its system's full name).
[[nodiscard]] ui::RegenerateScopeTitle roomTitleOf(const world::RoomId& room, AppCatalog& catalog) {
    ui::RegenerateScopeTitle title;
    switch (room.kind()) {
    case world::RoomIdKind::System: {
        title.roomKind = "console";
        const std::string fullName = catalog.systemFullName(room.system());
        title.roomTitle = fullName.empty() ? room.system().str() : fullName;
        break;
    }
    case world::RoomIdKind::Shared:
        title.roomKind = "shared";
        break;
    case world::RoomIdKind::Arcade:
        title.roomKind = "arcade";
        break;
    }
    return title;
}

void copyCounts(const ui::RegeneratePreviewContent& content, WorldRegenerationRecord& record) {
    record.roomsRegenerated = content.roomsRegenerated;
    record.roomsAdded = content.roomsAdded;
    record.roomsRemoved = content.roomsRemoved;
    record.roomsMoved = content.roomsMoved;
    record.gamesMoved = content.gamesMoved;
    record.faceOutEntering = content.faceOutEntering;
    record.faceOutLeaving = content.faceOutLeaving;
    record.newlyPlaced = content.newlyPlaced;
    record.newlyUnplaced = content.newlyUnplaced;
    record.fixturesAdded = content.fixturesAdded;
    record.fixturesRemoved = content.fixturesRemoved;
    record.fixturesMoved = content.fixturesMoved;
    record.pinsKept = content.pinsKept;
    record.conflicts = content.conflictCount;
    record.overrides = content.overrides;
    record.changed = content.changed;
}

} // namespace

bool WorldSession::regenerating() const noexcept {
    return m_regenerations &&
           (m_regenerations->request.has_value() ||
            (m_regenerations->job != RegenerateJob::None && m_regenerations->job != RegenerateJob::Prewarm));
}

bool WorldSession::regenerationWaits() const noexcept {
    // A lockstep run waits for the preview's and the commit's jobs (a prewarm is no reason to wait).
    return m_regenerations &&
           (m_regenerations->job == RegenerateJob::Preview || m_regenerations->job == RegenerateJob::Commit);
}

bool WorldSession::regenerationAllowed(const ProductUi& product) const {
    return m_state == "room" && m_shown && m_planState && !m_ride && !m_edit && !m_retargetPending &&
           !m_retargetResult && !m_shownPreview && !m_previewAwaitingFrame && !product.launchActive() &&
           !(m_flow && m_flow->phase() != WorldPhase::Done) && !regenerating();
}

bool WorldSession::confirmationOffered() const {
    if (!m_regenerations || !m_regenerations->confirmationRoom || m_notice.notice != world_notice::kNeedsConfirmation) {
        return false;
    }
    // While the notice shows (it hides after its time).
    return !m_noticeTimed || !m_noticeSince || !m_lastTime ||
           *m_lastTime - *m_noticeSince < world_notice::kShownSeconds;
}

void WorldSession::openConfirmationPreview(ProductUi& product) {
    if (!confirmationOffered()) {
        return;
    }
    const world::RoomId room = *m_regenerations->confirmationRoom;
    spdlog::info("Frame {}: the notice's preview of Rigenera stanza for {}", m_context.frameIndex(), room.toString());
    beginRegeneration(product, ui::RegenerateKind::Room, world::RegenerationScope::ofRoom(room), "notice",
                      std::nullopt);
}

void WorldSession::submitRegenerateJob(RegenerateJob job,
                                       std::function<RegenerateJobAnswer(const core::JobContext&)> work) {
    WorldRegenerations& state = *m_regenerations;
    m_pool.cancel(state.generation);
    const std::uint64_t generation = state.generation.current();
    {
        const std::lock_guard lock(state.box->mutex);
        state.box->answer.reset();
    }
    state.job = job;
    state.asked = Clock::now();
    core::JobOptions options;
    options.priority = core::WorkerPool::kHighestPriority; // the world's jobs (ADR-0010 §12)
    options.generation = &state.generation;
    const Status submitted = m_pool.submit(
        [box = state.box, generation, work = std::move(work)](const core::JobContext& context) {
            if (context.cancelled()) {
                return;
            }
            const Clock::time_point start = Clock::now();
            RegenerateJobAnswer answer = work(context);
            answer.milliseconds = millisecondsSince(start);
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
        spdlog::error("Regeneration: a job cannot be submitted: {}", submitted.error().toString());
        RegenerateJobAnswer failed;
        failed.kind =
            job == RegenerateJob::Commit ? RegenerateJobAnswer::Kind::Commit : RegenerateJobAnswer::Kind::Preview;
        failed.failure = std::string(regenerate_failure::kFailed);
        failed.error = submitted.error().toString();
        const std::lock_guard lock(state.box->mutex);
        state.box->answer = std::move(failed);
        state.box->generation = generation;
    }
}

void WorldSession::beginRegeneration(ProductUi& product, ui::RegenerateKind kind, world::RegenerationScope scope,
                                     std::string_view via, const std::optional<ui::DirectoryCommand>& command) {
    const std::uint64_t frame = m_context.frameIndex();
    if (!m_regenerations || !m_regenerations->slot) {
        followRegeneration(product); // the state and the catalog's slot made
    }
    WorldRegenerations& state = *m_regenerations;
    if (!regenerationAllowed(product)) {
        spdlog::info("Frame {}: {} of {} refused for now (a floor on its way, a ride, a launch, the editor, another "
                     "regeneration)",
                     frame, ui::toString(kind), scope.toString());
        if (command) {
            product.refuseDirectory(*command, "change-unavailable");
        }
        return;
    }
    if (state.job == RegenerateJob::Prewarm) {
        // The prewarm's job is the slot's: the preview waits for it there (cancelled only once it ran: the slot keeps
        // its inputs whatever happens to the job's answer).
        state.job = RegenerateJob::None;
    }
    // The panel's name of the scope.
    ui::RegenerateScopeTitle title;
    if (kind == ui::RegenerateKind::Room) {
        title = roomTitleOf(scope.room, product.catalog());
    } else if (kind == ui::RegenerateKind::Floor) {
        title.floor = scope.floor;
    }
    product.openRegeneratePreview(kind, title);
    if (!product.regeneratePreviewOpen()) {
        spdlog::error("Frame {}: the preview cannot show; nothing is regenerated", frame);
        return;
    }
    m_interaction->bindNow(m_interaction->activeRoom()); // Focus left, as for a ride
    state.request = RegenerateRequest{kind, scope, std::string(via)};
    state.preview.reset();
    state.undoPreview.reset();
    state.inputs.reset();
    state.panelState = ui::RegenerateState::Computing;
    state.requested = Clock::now();
    WorldRegenerationRecord record;
    record.kind = std::string(ui::toString(kind));
    record.scope = kind == ui::RegenerateKind::Undo ? std::string() : scope.toString();
    record.via = std::string(via);
    record.requestFrame = frame;
    m_record.regenerations.push_back(std::move(record));
    state.record = m_record.regenerations.size() - 1;
    spdlog::info("Frame {}: the preview of {} {} asked ({})", frame, ui::toString(kind),
                 kind == ui::RegenerateKind::Undo ? std::string() : scope.toString(), via);

    const WorldPaths paths = m_options.paths;
    const catalog::LibraryId library = m_planState->library;
    if (kind == ui::RegenerateKind::Undo) {
        submitRegenerateJob(RegenerateJob::Preview,
                            [paths, library, plan = m_planState->plan](const core::JobContext&) {
                                RegenerateJobAnswer answer;
                                answer.kind = RegenerateJobAnswer::Kind::Preview;
                                answer.undoPreview = previewUndo(paths, library, plan.get());
                                return answer;
                            });
        return;
    }
    submitRegenerateJob(RegenerateJob::Preview, [paths, scope, slot = state.slot,
                                                 reader =
                                                     product.catalog().worldReader()](const core::JobContext& job) {
        RegenerateJobAnswer answer;
        answer.kind = RegenerateJobAnswer::Kind::Preview;
        const Clock::time_point start = Clock::now();
        const RegenerationInputsResult inputs = regenerationInputsFor(*slot, paths, reader.get(), answer.inputsTaken);
        answer.inputsMs = millisecondsSince(start);
        if (!inputs.inputs) {
            answer.failure = inputs.failure;
            answer.error = inputs.error;
            return answer;
        }
        answer.inputs = inputs.inputs;
        if (job.cancelled()) {
            return answer;
        }
        answer.preview = previewRegeneration(paths, scope, *inputs.inputs);
        return answer;
    });
}

void WorldSession::prewarmRegeneration(ProductUi& product) {
    WorldRegenerations& state = *m_regenerations;
    if (state.prewarmed || state.job != RegenerateJob::None || !m_planState || m_state != "room") {
        return;
    }
    const std::shared_ptr<WorldCatalogReader> reader = product.catalog().worldReader();
    if (!reader) {
        return;
    }
    state.prewarmed = true;
    spdlog::info("Frame {}: the regenerations' inputs are read ahead (the directory's Modifiche)",
                 m_context.frameIndex());
    submitRegenerateJob(RegenerateJob::Prewarm,
                        [paths = m_options.paths, slot = state.slot, reader](const core::JobContext&) {
                            RegenerateJobAnswer answer;
                            bool taken = false;
                            static_cast<void>(regenerationInputsFor(*slot, paths, reader.get(), taken));
                            answer.inputsTaken = taken;
                            return answer;
                        });
}

void WorldSession::followConfirmations() {
    WorldRegenerations& state = *m_regenerations;
    if (m_record.needsConfirmation == state.confirmationsSeen) {
        return;
    }
    state.confirmationsSeen = m_record.needsConfirmation;
    state.confirmationRoom.reset();
    if (state.confirmationsSeen.empty() || !m_planState || !m_planState->plan) {
        return;
    }
    // ADR-0015 §3: the update left something as it stands because it would have moved what the user pinned; the
    // notice offers "Rigenera stanza" of the room it names.
    state.confirmationRoom = confirmationRoomOf(state.confirmationsSeen, *m_planState->plan);
    if (!state.confirmationRoom) {
        spdlog::warn("Frame {}: the update's needsConfirmation names no room of the plan; no preview offered",
                     m_context.frameIndex());
        return;
    }
    spdlog::info("Frame {}: the notice offers the preview of Rigenera stanza for {} ({} sentence(s) of the update)",
                 m_context.frameIndex(), state.confirmationRoom->toString(), state.confirmationsSeen.size());
    setNotice(WorldNoticeContent{std::string(world_notice::kNeedsConfirmation), {}, noticeRoom()}, true);
}

void WorldSession::followRegeneration(ProductUi& product) {
    if (!m_regenerations) {
        m_regenerations = std::make_unique<WorldRegenerations>();
        m_regenerations->box = std::make_shared<RegenerateJobBox>();
        m_regenerations->box->wake = m_context.wakeCallback();
    }
    WorldRegenerations& state = *m_regenerations;
    // The catalog's inputs belong to its refresh: a refresh that ended makes them again.
    const std::string key = fmt::format("{}/{}", m_planState ? m_planState->library.str() : std::string(),
                                        m_lastRefresh.value_or(std::string()));
    if (!state.slot || key != state.slotKey) {
        state.slot = std::make_shared<RegenerationInputsSlot>();
        state.slotKey = key;
        state.prewarmed = false;
    }
    followConfirmations();
    // The job's answer.
    std::optional<RegenerateJobAnswer> answer;
    {
        const std::lock_guard lock(state.box->mutex);
        if (state.box->answer && state.box->generation == state.generation.current()) {
            answer = std::exchange(state.box->answer, std::nullopt);
        }
    }
    if (answer) {
        if (state.job == RegenerateJob::Prewarm) {
            state.job = RegenerateJob::None;
        } else if (state.job != RegenerateJob::None) {
            takeRegenerateAnswer(product, std::move(*answer));
        }
    }
    // The directory's "Modifiche" shown: the inputs read ahead.
    if (!state.request && product.directoryTab() == ui::DirectoryTab::Changes) {
        prewarmRegeneration(product);
    }
    if (!state.request) {
        return;
    }
    // The panel's choices.
    WorldRegenerationRecord* record = state.record && *state.record < m_record.regenerations.size()
                                          ? &m_record.regenerations[*state.record]
                                          : nullptr;
    const ui::RegenerateState panel = product.regeneratePreview().state();
    if (product.regeneratePreviewOpen() && panel != state.panelState) {
        if (panel == ui::RegenerateState::Confirm && record != nullptr) {
            ++record->confirmations;
        }
        state.panelState = panel;
    }
    for (const ui::RegenerateCommandKind command : product.takeRegenerateCommands()) {
        const std::uint64_t frame = m_context.frameIndex();
        if (record != nullptr) {
            record->choice = std::string(ui::toString(command));
            record->choiceFrame = frame;
        }
        if (command == ui::RegenerateCommandKind::Cancel) {
            spdlog::info("Frame {}: the preview of {} closed without a choice", frame,
                         ui::toString(state.request->kind));
            if (state.job == RegenerateJob::Preview) {
                m_pool.cancel(state.generation);
                state.job = RegenerateJob::None;
            }
            if (record != nullptr && record->end.empty()) {
                record->end = record->failure.empty() ? "cancelled" : "failed";
            }
            state.request.reset();
            return;
        }
        state.chosen = Clock::now();
        const WorldPaths paths = m_options.paths;
        const catalog::LibraryId library = m_planState->library;
        const std::string writer =
            m_options.writer.empty() ? std::string("retroverse (regeneration)") : m_options.writer + " (regeneration)";
        WorldBuild skeleton;
        skeleton.library = library;
        skeleton.startRoom = m_options.startRoom;
        skeleton.requestedFloor = m_floor;
        skeleton.outcome = WorldOutcome::Kept;
        FloorSources sources;
        sources.mediaRoot = m_planState->mediaRoot;
        sources.mediaRootPath = m_planState->mediaRootPath;
        sources.screenMediaRoot = m_planState->screenMediaRoot;
        const std::shared_ptr<WorldCatalogReader> reader = product.catalog().worldReader();
        // The floor shown, built from the plan stored now (on the pool, as a ride's destination).
        auto floorOf = [paths, skeleton, sources,
                        reader](std::shared_ptr<const world::WorldPlan> plan, std::string digest,
                                std::map<catalog::SystemName, RoomItems> items, RegenerateJobAnswer& answer) {
            if (!plan) {
                return;
            }
            WorldBuild build = skeleton;
            build.plan = *plan;
            build.planDigest = std::move(digest);
            build.reason = "a regeneration's floor";
            FloorSources floorSources = sources;
            floorSources.items = std::move(items);
            if (reader) {
                floorSources.lastPlayed = catalogLastPlayed(*reader);
            }
            buildFloor(paths, build, floorSources);
            if (!build.plan || !build.scene) {
                answer.buildError = "the floor cannot be built: " + build.reason;
                return;
            }
            answer.build = std::move(build);
        };
        if (command == ui::RegenerateCommandKind::Undo) {
            if (!state.undoPreview || !state.undoPreview->snapshot) {
                continue;
            }
            spdlog::info("Frame {}: Annulla ultima modifica: {} asked", frame, state.undoPreview->snapshot->name);
            submitRegenerateJob(RegenerateJob::Commit,
                                [paths, library, writer, expected = state.undoPreview->snapshot->name,
                                 slot = state.slot, floorOf](const core::JobContext&) {
                                    RegenerateJobAnswer answer;
                                    answer.kind = RegenerateJobAnswer::Kind::Commit;
                                    // The catalog's snapshot, when the inputs were read (the items files written again
                                    // from it).
                                    std::shared_ptr<const BuildingSnapshot> snapshot;
                                    {
                                        const std::lock_guard lock(slot->mutex);
                                        if (slot->result && slot->result->inputs) {
                                            snapshot = slot->result->inputs->snapshot;
                                        }
                                    }
                                    UndoCommit undone = commitUndo(paths, library, expected, writer, snapshot.get());
                                    if (undone.failure.empty() && undone.plan) {
                                        floorOf(undone.plan, undone.planDigest, undone.items, answer);
                                    }
                                    answer.undone = std::move(undone);
                                    return answer;
                                });
            continue;
        }
        if (!state.preview || !state.inputs) {
            continue;
        }
        const bool drop = command == ui::RegenerateCommandKind::RegenerateDrop;
        spdlog::info("Frame {}: {} of {} asked{}", frame, drop ? "Rigenera e scarta le personalizzazioni" : "Rigenera",
                     state.preview->scope.toString(), drop ? " (the second confirmation given)" : "");
        submitRegenerateJob(RegenerateJob::Commit, [paths, writer, drop, preview = *state.preview,
                                                    inputs = state.inputs, floorOf](const core::JobContext&) {
            RegenerateJobAnswer answer;
            answer.kind = RegenerateJobAnswer::Kind::Commit;
            RegenerationCommit commit = commitRegeneration(paths, preview, drop, *inputs, writer);
            if (commit.failure.empty() && commit.plan) {
                floorOf(commit.plan, commit.planDigest, commit.items, answer);
            }
            answer.commit = std::move(commit);
            return answer;
        });
    }
}

void WorldSession::takeRegenerateAnswer(ProductUi& product, RegenerateJobAnswer answer) {
    WorldRegenerations& state = *m_regenerations;
    state.job = RegenerateJob::None;
    const std::uint64_t frame = m_context.frameIndex();
    WorldRegenerationRecord* record = state.record && *state.record < m_record.regenerations.size()
                                          ? &m_record.regenerations[*state.record]
                                          : nullptr;
    if (!state.request) {
        return; // the panel closed meanwhile
    }
    const auto fail = [&](const std::string& failure, const std::string& error) {
        spdlog::warn("Frame {}: {} failed ({}): {}", frame, ui::toString(state.request->kind), failure, error);
        product.failRegeneratePreview(failure);
        if (record != nullptr) {
            record->failure = failure;
            record->end = "failed";
        }
    };
    if (answer.kind == RegenerateJobAnswer::Kind::Preview) {
        if (record != nullptr) {
            record->previewFrame = frame;
            record->previewMs = millisecondsSince(state.requested);
            record->previewJobMs = answer.milliseconds;
            record->inputsTaken = answer.inputsTaken;
            if (answer.inputsTaken) {
                record->inputsMs = answer.inputsMs;
            }
        }
        if (!answer.failure.empty()) {
            fail(answer.failure, answer.error);
            return;
        }
        if (answer.undoPreview) {
            UndoPreview& undo = *answer.undoPreview;
            if (!undo.failure.empty()) {
                fail(undo.failure, undo.error);
                return;
            }
            // The undone room named as the panel names rooms.
            if (const auto room = world::RoomId::parse(undo.content.undoScope)) {
                undo.content.undoScopeTitle = roomTitleOf(*room, product.catalog());
            }
            if (record != nullptr) {
                record->undoSnapshot = undo.snapshot ? undo.snapshot->name : std::string();
                record->scope = undo.content.undoScope;
            }
            product.setRegeneratePreview(undo.content);
            state.undoPreview = std::move(undo);
            spdlog::info("Frame {}: the preview of Annulla ultima modifica shown ({:.0f} ms)", frame,
                         record != nullptr ? record->previewMs.value_or(0.0) : 0.0);
            return;
        }
        if (!answer.preview) {
            fail(std::string(regenerate_failure::kFailed), "no preview");
            return;
        }
        RegenerationPreview& preview = *answer.preview;
        if (!preview.failure.empty()) {
            fail(preview.failure, preview.error);
            return;
        }
        if (record != nullptr) {
            copyCounts(preview.content, *record);
            record->regenerateMs = preview.regenerateMs;
        }
        product.setRegeneratePreview(preview.content);
        state.inputs = answer.inputs;
        state.preview = std::move(preview);
        spdlog::info("Frame {}: the preview of {} shown ({:.0f} ms after the request; the job {:.0f} ms, its inputs "
                     "{})",
                     frame, state.preview->scope.toString(), record != nullptr ? record->previewMs.value_or(0.0) : 0.0,
                     answer.milliseconds,
                     answer.inputsTaken ? fmt::format("taken in {:.0f} ms", answer.inputsMs) : std::string("ready"));
        return;
    }

    // A commit's answer.
    if (record != nullptr) {
        record->commitMs = answer.milliseconds;
    }
    std::string failure = answer.failure;
    std::string error = answer.error;
    std::shared_ptr<const world::WorldPlan> plan;
    std::string digest;
    std::string scopeText;
    if (answer.commit) {
        failure = answer.commit->failure;
        error = answer.commit->error;
        plan = answer.commit->plan;
        digest = answer.commit->planDigest;
        scopeText = state.preview ? state.preview->scope.toString() : std::string();
        if (record != nullptr) {
            record->written = answer.commit->written;
            record->reused = answer.commit->reused;
            record->snapshot = answer.commit->snapshot;
        }
    } else if (answer.undone) {
        failure = answer.undone->failure;
        error = answer.undone->error;
        plan = answer.undone->plan;
        digest = answer.undone->planDigest;
        scopeText = state.undoPreview ? state.undoPreview->content.undoScope : std::string();
        if (record != nullptr) {
            record->written = failure.empty();
            record->snapshot = answer.undone->kept;
        }
    }
    if (!failure.empty()) {
        fail(failure, error);
        return;
    }
    if (record != nullptr) {
        record->planDigest = digest;
    }
    m_record.planDigest = digest.empty() ? m_record.planDigest : digest;
    m_record.planWritten = m_record.planWritten || (record != nullptr && record->written);
    // The panel closes; the floor shown follows the plan stored now.
    product.closeRegeneratePreview();
    std::string path = "none";
    std::string walker = "kept";
    if (answer.build && m_shown) {
        const std::shared_ptr<const world::WorldPlan> before = m_planState ? m_planState->plan : nullptr;
        const std::string area = m_host.eyeArea ? m_host.eyeArea() : std::string();
        WorldLiveUpdateRecord live;
        releaseHero();
        const LiveUpdatePlan update = planLiveUpdate(*m_shown, *answer.build);
        bool shown = false;
        if (update.kind == LiveUpdateKind::Unchanged || update.kind == LiveUpdateKind::Textures ||
            update.kind == LiveUpdateKind::Items) {
            shown = applyRooms(*answer.build, update, live);
            path = "in-place";
        }
        if (!shown) {
            // The floor's static part changed (a room's fixtures, a room moved): committed again, the walker kept in
            // the frame of the area that holds it (ADR-0013 §7).
            shown = showFloor(*answer.build, true, &live.walker);
            path = shown ? "floor" : "none";
        }
        if (!shown) {
            spdlog::error("Frame {}: the floor after the regeneration cannot be shown; it shows at the next start",
                          frame);
        } else if (regenerationCovers(area, scopeText, before.get())) {
            // ADR-0015 §5: a walker in a regenerated room stands at its doorway, facing in (the elevator when the room
            // is gone from the floor).
            const bool standing = m_doorways.contains(area);
            const scene::CameraSetup& camera = standing ? m_doorways.at(area) : m_arrival;
            if (m_host.teleport) {
                m_host.teleport(camera);
            }
            m_interaction->bindNow(standing ? interactionRoomOf(area) : std::nullopt);
            TeleportRecord teleport;
            teleport.frame = frame;
            teleport.to = standing ? area : std::string("elevator");
            teleport.reason = "regeneration";
            if (m_host.walker) {
                teleport.walker = spawnOf(m_host.walker());
            }
            m_record.teleports.push_back(std::move(teleport));
            walker = standing ? "doorway:" + area : std::string("elevator");
            spdlog::info("Frame {}: the walker stands at {} (its room was regenerated)", frame,
                         standing ? area + "'s doorway" : std::string("the elevator"));
        }
        m_record.state = m_state;
    } else if (!answer.buildError.empty()) {
        spdlog::error("Frame {}: the floor after the regeneration: {}; it shows at the next start", frame,
                      answer.buildError);
    }
    if (record != nullptr) {
        record->path = path;
        record->walker = walker;
        record->backMs = millisecondsSince(state.chosen);
        record->end = "committed";
    }
    state.backPending = true;
    // The notice's offer is answered (the next update that needs a confirmation offers it again).
    state.confirmationRoom.reset();
    if (m_notice.notice == world_notice::kNeedsConfirmation) {
        setNotice(WorldNoticeContent{}, false);
    }
    spdlog::info("Frame {}: {} committed and shown ({}; {:.0f} ms after the choice)", frame,
                 ui::toString(state.request->kind), path, millisecondsSince(state.chosen));
    state.request.reset();
}

void WorldSession::regenerationFrameRendered() {
    if (!m_regenerations || !std::exchange(m_regenerations->backPending, false) || !m_regenerations->record) {
        return;
    }
    if (*m_regenerations->record < m_record.regenerations.size()) {
        WorldRegenerationRecord& record = m_record.regenerations[*m_regenerations->record];
        record.backFrame = m_context.frameIndex();
        record.backMs = millisecondsSince(m_regenerations->chosen);
    }
}

} // namespace rv::app
